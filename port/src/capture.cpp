// X-Men: Destiny - F4 captures (see capture.h).

#include "capture.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include "debug_commands.h"
#include "history.h"
#include "native/native_renderer.h"
#include "perf_overlay.h"
#include "problem_log.h"
#include "state_capture.h"

namespace xmd {

namespace {

std::filesystem::path g_user_data, g_problems;
std::atomic<bool> g_busy{false};

// A PNG without compression (stored deflate blocks): no library needed, any viewer reads it.
uint32_t Crc(const uint8_t* data, size_t size, uint32_t crc) {
  static uint32_t table[256];
  static bool made = false;
  if (!made) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    made = true;
  }
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

void Chunk(std::ofstream& out, const char type[4], const std::vector<uint8_t>& data) {
  const uint32_t size = static_cast<uint32_t>(data.size());
  const uint8_t head[8] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size),
                           uint8_t(type[0]), uint8_t(type[1]), uint8_t(type[2]), uint8_t(type[3])};
  out.write(reinterpret_cast<const char*>(head), 8);
  out.write(reinterpret_cast<const char*>(data.data()), data.size());
  uint32_t crc = Crc(head + 4, 4, 0xFFFFFFFFu);
  crc = Crc(data.data(), data.size(), crc) ^ 0xFFFFFFFFu;
  const uint8_t tail[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
  out.write(reinterpret_cast<const char*>(tail), 4);
}

bool WritePng(const std::filesystem::path& file, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
  std::ofstream out(file, std::ios::binary);
  if (!out) return false;
  out.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> header = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                 uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
                                 8, 2, 0, 0, 0};  // 8 bits, RGB
  Chunk(out, "IHDR", header);
  // (the rows, each with its filter byte 0, in stored blocks of up to 65535 bytes)
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;  // (Adler-32)
  for (size_t at = 0; at < raw.size();) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    for (size_t i = at; i < at + n; ++i) {
      a = (a + raw[i]) % 65521;
      b = (b + a) % 65521;
    }
    at += n;
  }
  const uint32_t adler = (b << 16) | a;
  z.push_back(uint8_t(adler >> 24)), z.push_back(uint8_t(adler >> 16));
  z.push_back(uint8_t(adler >> 8)), z.push_back(uint8_t(adler));
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  return bool(out);
}

void Work() {
  char stamp[48];
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::strftime(stamp, sizeof(stamp), "capture_%Y%m%d_%H%M%S", &local);
  const std::filesystem::path dir = g_user_data / "captures" / stamp;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);

  // (first, before anything of this capture is logged: what the log held when the key was pressed)
  {
    std::ofstream out(dir / "log_tail.txt", std::ios::binary);
    for (const std::string& line : problems::RecentLog()) out << line << "\n";
  }
  // (and what the game did up to it: the last five minutes of frames, calls and events - history.h)
  const std::string past = history::Write(dir / "history");
  const PerfNumbers n = Perf();

  // The picture, the draws of a frame, and the game's own word on where the player is - all asked for now,
  // collected below.
  native::RequestFrameCapture(false);
  native::RequestDrawDump((dir / "draws.txt").string());
  std::string where = "(the game's scripts did not answer within 3 s)";
  auto where_done = std::make_shared<std::atomic<bool>>(false);
  auto where_text = std::make_shared<std::string>();
  auto where_lock = std::make_shared<std::mutex>();
  debug::QueryPosition([where_done, where_text, where_lock](bool ok, const std::string& text) {
    std::lock_guard lock(*where_lock);
    *where_text = ok ? text : "(" + text + ")";
    *where_done = true;
  });

  bool shot = false;
  std::vector<uint8_t> rgb;
  uint32_t w = 0, h = 0;
  for (int i = 0; i < 60 && !(shot && *where_done); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!shot && native::TakeFrameCapture(rgb, w, h)) shot = WritePng(dir / "screenshot.png", rgb, w, h);
  }
  bool have_where = false;
  if (*where_done) {
    std::lock_guard lock(*where_lock);
    where = *where_text;
    have_where = !where.empty() && where[0] != '(';
  }

  // Everything that is loaded, from one held moment (state_capture.h), and how to get back to the spot.
  ShowNotice("Capturing the game's state...", 3.0f);
  const std::string state = CaptureState(dir / "state", g_user_data);
  {
    std::ofstream out(dir / "resume.txt", std::ios::binary);
    out << "# tools\\resume.ps1 -Capture <this folder> (or record_play.ps1 -Resume): the saves in state\\save,\n"
           "# Continue, then the player put here and the world frozen (F5 lets it go on)\n";
    out << "position " << (have_where ? where : std::string("none")) << "\n";
  }

  {
    std::ofstream out(dir / "info.txt", std::ios::binary);
    char text[1024];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    out << "X-Men: Destiny - capture (F4)\n";
    out << "when:      " << text << "\n";
    out << "build:     " << __DATE__ << " " << __TIME__ << "\n";
    out << "renderer:  " << n.renderer << ", picture " << w << " x " << h << (shot ? "" : " (no screenshot came)")
        << "\n\n";
    std::snprintf(text, sizeof(text),
                  "frame rate:        %.1f fps (%.2f ms a frame), 1%% low %.1f fps\n"
                  "CPU render thread: %.2f ms a frame (recording the draws %.2f ms)\n"
                  "CPU all threads:   %.2f ms a frame, %.0f%% of the PC\n"
                  "GPU:               %.0f%% of the GPU, %.2f ms a frame; waited for it %.2f ms\n"
                  "draws:             %.0f a frame\n"
                  "textures:          %.0f MB, render targets %.0f MB\n\n",
                  n.fps, n.frame_ms, n.low_1pct_fps, n.render_cpu_ms, n.draw_ms, n.all_cpu_ms, n.cpu_percent,
                  n.gpu_percent, n.gpu_ms, n.gpu_wait_ms, n.draws, n.texture_mb, n.targets_mb);
    out << text;
    out << "player position (x y z): " << where << "\n"
        << "to get back there in a test: tools\\debug_cmd.ps1 'goto " << where << "'\n\n";
    out << state << "\n" << past << "\n\n";
    out << "files: screenshot.png (the frame), draws.txt (every draw of the next frame), log_tail.txt (the log up\n"
           "to the key press), problems.txt (the session's problems so far), resume.txt (tools\\resume.ps1),\n"
           "state\\ (all memory, threads, kernel objects, script variables, the saves - tools\\state.py reads it),\n"
           "history\\ (the five minutes before: frames.csv, events.txt - memory, files, slow calls, stutters -,\n"
           "render_calls.txt, kernel_calls.txt, kernel_summary.txt; times are seconds before the capture)\n";
  }
  std::filesystem::copy_file(g_problems, dir / "problems.txt", std::filesystem::copy_options::overwrite_existing, ec);

  REXLOG_INFO("capture: {} (screenshot {}, where: {}; {})", dir.string(), shot ? "yes" : "no", where.substr(0, 200),
              state);
  ShowNotice("Capture saved: " + dir.string(), 5.0f);
  g_busy = false;
}

}  // namespace

void TakeCapture() {
  if (g_busy.exchange(true)) return;
  ShowNotice("Capturing...", 2.0f);
  std::thread(Work).detach();
}

void InstallCapture(const std::filesystem::path& user_data, const std::filesystem::path& problems) {
  g_user_data = user_data;
  g_problems = problems;
  // (F4 was the runtime's settings window: moved to F6)
  rex::cvar::SetFlagByName("bind_settings", "F6");
  rex::ui::RegisterBind("bind_capture", "F4",
                        "Capture: screenshot, the frame's draws, the log, where the player is and the game's whole "
                        "state (memory, threads, saves), into UserData\\captures", [] { TakeCapture(); });
  // (the runtime applies a change of "fullscreen" at once: a borderless window over the display, or back)
  rex::ui::RegisterBind("bind_fullscreen", "F11", "Borderless fullscreen on / off", [] {
    const bool on = rex::cvar::Query<bool>("fullscreen");
    rex::cvar::SetFlagByName("fullscreen", on ? "false" : "true");
  });
  rex::ui::RegisterBind("bind_freeze", "F5", "Freeze: the world held still while it goes on being drawn (again: on)",
                        [] {
                          const bool frozen = !debug::Frozen();
                          debug::SetFrozen(frozen);
                          ShowNotice(frozen ? "Frozen - F5 to go on" : "Running", 2.0f);
                        });
}

}  // namespace xmd
