// X-Men: Destiny - the problems log (see problem_log.h).

#include "problem_log.h"

#include "history.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <rex/logging.h>
#include <spdlog/sinks/base_sink.h>

namespace xmd::problems {

namespace {

using Clock = std::chrono::steady_clock;

struct Entry {
  uint64_t count = 0, written = 0;
  std::string category, last;
};

std::mutex g_mutex;
std::ofstream g_out;
bool g_open = false;
std::unordered_map<std::string, Entry> g_entries;
Clock::time_point g_start;

std::string Stamp() {
  char text[40];
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  const double up = std::chrono::duration<double>(Clock::now() - g_start).count();
  std::snprintf(text, sizeof(text), "%02d:%02d:%02d +%.0fs", local.tm_hour, local.tm_min, local.tm_sec, up);
  return text;
}

// The line without its numbers: what makes two lines the same problem. (A word with a letter in it - a
// shader's hash, a function's name - stays.)
std::string Shape(std::string_view category, std::string_view text) {
  std::string out(category);
  out.push_back('|');
  size_t i = 0;
  while (i < text.size()) {
    size_t end = i;
    bool digits_only = true, any = false;
    while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '.')) {
      if (!std::isdigit(static_cast<unsigned char>(text[end])) && text[end] != '.') digits_only = false;
      any = true;
      ++end;
    }
    if (!any) {
      out.push_back(text[i++]);
      continue;
    }
    // (an address: eight hex digits. A shader's hash has sixteen.)
    bool address = end - i == 8;
    for (size_t k = i; address && k < end; ++k) address = std::isxdigit(static_cast<unsigned char>(text[k])) != 0;
    if (digits_only || address) {
      out.push_back('#');
    } else {
      out.append(text.substr(i, end - i));
    }
    i = end;
  }
  if (out.size() > 240) out.resize(240);
  return out;
}

void WriteRepeats() {
  for (auto& [shape, e] : g_entries) {
    if (e.count == e.written) continue;
    g_out << Stamp() << "  [" << e.category << "] " << (e.count - e.written) << " more (" << e.count
          << " in all), the last: " << e.last << "\n";
    e.written = e.count;
  }
  g_out.flush();
}

int64_t NowMs();

std::mutex g_recent_mutex;
std::deque<std::pair<int64_t, std::string>> g_recent;  // (the log's last five minutes: RecentLog)

class Sink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const std::string_view text(msg.payload.data(), msg.payload.size());
    {
      std::lock_guard lock(g_recent_mutex);
      const auto level = spdlog::level::to_string_view(msg.level);
      std::string line = Stamp();
      line.append(" [").append(level.data(), level.size()).append("] ").append(text.substr(0, 600));
      const int64_t now = NowMs();
      g_recent.emplace_back(now, std::move(line));
      // (five minutes of it, never fewer than 1500 lines nor more than 200,000)
      while (g_recent.size() > 200000 || (g_recent.size() > 1500 && g_recent.front().first < now - 300000))
        g_recent.pop_front();
    }
    if (msg.level < spdlog::level::warn) return;
    const char* category = msg.level >= spdlog::level::err ? "error" : "warning";
    Report(category, text);
  }
  void flush_() override {}
};

// Hitches.
Clock::time_point g_last_frame{};
double g_usual_ms = 0;  // (a slow average of the frames that were not hitches)
std::string g_causes, g_split;
double g_cause_ms = 0;
uint64_t g_frames = 0;

// Sound.
std::atomic<int64_t> g_audio_last{0};      // (ms since g_start of the last frame; 0: none yet)
std::atomic<int64_t> g_audio_sound{0};     // (... of the last frame with sound in it)
std::atomic<bool> g_audio_silence_told{false};

int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count(); }

}  // namespace

void Install(const std::filesystem::path& file) {
  static bool done = false;
  if (done) return;
  done = true;
  g_start = Clock::now();
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  g_out.open(file, std::ios::binary | std::ios::trunc);
  g_open = g_out.is_open();
  if (!g_open) return;
  g_out << "X-Men: Destiny - problems of this session (the full log has the rest). A repeated problem is written\n"
           "once, then counted.\n\n";
  g_out.flush();
  rex::AddSink(std::make_shared<Sink>());
  REXLOG_INFO("problems log: {}", file.string());
  // Repeat counts, and the sound watch: the game hands over ~190 frames a second while it runs at all.
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      {
        std::lock_guard lock(g_mutex);
        WriteRepeats();
      }
      const int64_t now = NowMs(), last = g_audio_last.load(), sound = g_audio_sound.load();
      if (last && now - last > 3000) {
        Report("audio", "the game has handed over no sound for " + std::to_string((now - last) / 1000) +
                            " s (its audio thread stopped, or the output is stuck)");
      }
      if (last && sound && now - sound > 20000 && now - last < 1000 && !g_audio_silence_told.exchange(true)) {
        Report("audio", "sound has been silent for 20 s after playing (" + std::to_string(sound / 1000) +
                            " s into the session was the last of it)");
      }
    }
  }).detach();
}

void Report(std::string_view category, std::string_view text) {
  if (!g_open) return;
  std::lock_guard lock(g_mutex);
  Entry& e = g_entries[Shape(category, text)];
  e.last = text.substr(0, 400);
  if (++e.count > 1) return;
  e.written = 1;
  e.category = category;
  g_out << Stamp() << "  [" << category << "] " << text << "\n";
  g_out.flush();
}

std::vector<std::string> RecentLog() {
  std::lock_guard lock(g_recent_mutex);
  std::vector<std::string> lines;
  lines.reserve(g_recent.size());
  for (const auto& [when, line] : g_recent) lines.push_back(line);
  return lines;
}

void FrameCause(std::string_view what, double ms) {
  if (!g_open) return;
  char text[32];
  std::snprintf(text, sizeof(text), ": %.0f ms;", ms);
  if (g_causes.size() < 600) g_causes.append(what).append(text);
  history::FrameBuilt(ms);
  history::Event("built in a frame", std::string(what) + text);
  g_cause_ms += ms;
}

void FrameSplit(std::string_view text) {
  if (g_open) g_split = text;
}

void FrameEnd() {
  if (!g_open) return;
  const auto now = Clock::now();
  const double ms = g_last_frame.time_since_epoch().count()
                        ? std::chrono::duration<double, std::milli>(now - g_last_frame).count()
                        : 0;
  g_last_frame = now;
  ++g_frames;
  // (the first seconds, and frames after a pause of seconds - a loading screen's - are not counted)
  const bool hitch = g_frames > 300 && g_usual_ms > 0 && ms > 50.0 && ms > 2.5 * g_usual_ms && ms < 5000.0;
  if (hitch) {
    char text[900];
    if (!g_causes.empty()) {
      std::snprintf(text, sizeof(text), "a frame took %.0f ms (usual %.0f ms), %.0f ms of it for:%s", ms, g_usual_ms,
                    g_cause_ms, g_causes.c_str());
    } else {
      std::snprintf(text, sizeof(text), "a frame took %.0f ms (usual %.0f ms); nothing the renderer built in it",
                    ms, g_usual_ms);
    }
    // (where the time went: the game's side or the renderer's - FrameSplit. Its numbers don't make it another problem.)
    std::string line = text;
    if (!g_split.empty()) line += " | " + g_split;
    Report(g_causes.empty() ? "hitch" : "hitch-shader", line);
    history::Event("stutter", line);
  } else if (ms > 0 && ms < 5000.0) {
    g_usual_ms = g_usual_ms > 0 ? g_usual_ms * 0.98 + ms * 0.02 : ms;
  }
  g_causes.clear();
  g_split.clear();
  g_cause_ms = 0;
}

void AudioFrame(bool silent) {
  if (!g_open) return;
  const int64_t now = NowMs() + 1;
  g_audio_last = now;
  if (!silent) {
    g_audio_sound = now;
    g_audio_silence_told = false;
  }
}

}  // namespace xmd::problems
