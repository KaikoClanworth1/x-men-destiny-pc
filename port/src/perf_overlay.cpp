// X-Men: Destiny - the performance overlay (see perf_overlay.h).

#include "perf_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include "native/native_renderer.h"

#if defined(_WIN32)
#include <windows.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#endif

REXCVAR_DEFINE_INT32(perf_overlay, 2, "UI",
                     "The performance overlay: 0 off, 1 the frame rate, 2 everything (CPU and GPU time a frame). "
                     "F2 cycles it");

namespace xmd {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
PerfNumbers g_numbers;
std::atomic<int> g_level{0};
std::atomic<double> g_gpu_percent{-1.0};

std::mutex g_notice_mutex;
std::string g_notice;
Clock::time_point g_notice_until;

#if defined(_WIN32)
uint64_t FileTime(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// The game's share of the GPU: Windows' "GPU Engine" counters (what Task Manager shows), this process's 3D
// engines added up, once a second.
void WatchGpu() {
  PDH_HQUERY query = nullptr;
  if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) return;
  wchar_t path[128];
  swprintf(path, 128, L"\\GPU Engine(pid_%lu_*engtype_3D)\\Utilization Percentage", GetCurrentProcessId());
  PDH_HCOUNTER counter = nullptr;
  // (the engines appear once the game has drawn: added again until they are there)
  for (;;) {
    if (!counter && PdhAddEnglishCounterW(query, path, 0, &counter) != ERROR_SUCCESS) counter = nullptr;
    PdhCollectQueryData(query);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!counter) continue;
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) continue;
    DWORD size = 0, count = 0;
    PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (!size) {
      // (no instance yet: the counter is made again - a wildcard is expanded when it is added)
      PdhRemoveCounter(counter);
      counter = nullptr;
      continue;
    }
    std::vector<uint8_t> buffer(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) continue;
    double sum = 0;
    for (DWORD i = 0; i < count; ++i) sum += items[i].FmtValue.doubleValue;
    g_gpu_percent = std::min(100.0, sum);
  }
}
#endif

class PerfOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit PerfOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;
};

PerfOverlay* g_overlay = nullptr;  // (lives for the whole run)

void DrawNotice(ImGuiIO& io) {
  std::string text;
  {
    std::lock_guard lock(g_notice_mutex);
    if (g_notice.empty()) return;
    if (Clock::now() > g_notice_until) {
      g_notice.clear();
      return;
    }
    text = g_notice;
  }
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.85f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.8f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##xmd_notice", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.4f * std::max(1.0f, io.DisplaySize.y / 720.0f));
    ImGui::PushTextWrapPos(io.DisplaySize.x * 0.88f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
}

void PerfOverlay::OnDraw(ImGuiIO& io) {
  DrawNotice(io);
  const int level = g_level.load(std::memory_order_relaxed);
  if (!level) return;
  const PerfNumbers n = Perf();
  ImGui::SetNextWindowPos(ImVec2(6.0f, 6.0f), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(level >= 2 ? 0.6f : 0.45f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  // (ImGui's font is small on a 1440p / 4K window: 720p is 1x)
  const float k = std::max(1.0f, io.DisplaySize.y / 720.0f);
  if (ImGui::Begin("##xmd_perf", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.5f * k);
    const ImVec4 green(0.45f, 1.0f, 0.45f, 1.0f), yellow(1.0f, 0.85f, 0.3f, 1.0f), red(1.0f, 0.4f, 0.4f, 1.0f);
    ImGui::TextColored(n.fps >= 60 ? green : n.fps >= 30 ? yellow : red, "FPS %.0f", n.fps);
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f ms  1%% low %.0f  |  %s", n.frame_ms, n.low_1pct_fps, n.renderer.c_str());
    if (level >= 2) {
      ImGui::SetWindowFontScale(1.15f * k);
      // (green: fits 240 frames a second - 4.2 ms; yellow: 60 - 16.7 ms)
      const auto colour = [&](double ms) { return ms <= 4.2 ? green : ms <= 16.7 ? yellow : red; };
      ImGui::TextColored(colour(n.render_cpu_ms), "CPU  render thread %.1f ms", n.render_cpu_ms);
      ImGui::SameLine();
      ImGui::TextDisabled("(draws %.1f ms)   all threads %.1f ms a frame   %.0f%% of the PC", n.draw_ms, n.all_cpu_ms,
                          n.cpu_percent);
      if (n.gpu_percent >= 0) {
        ImGui::TextColored(colour(n.gpu_ms), "GPU  %.0f%%  %.1f ms a frame", n.gpu_percent, n.gpu_ms);
      } else {
        ImGui::TextDisabled("GPU  (no counter yet)");
      }
      ImGui::SameLine();
      ImGui::TextDisabled("  waited for it %.1f ms", n.gpu_wait_ms);
      ImGui::TextDisabled("Draws %.0f   textures %.0f MB   render targets %.0f MB", n.draws, n.texture_mb,
                          n.targets_mb);
    }
  }
  ImGui::End();
}

}  // namespace

PerfNumbers Perf() {
  std::lock_guard lock(g_mutex);
  return g_numbers;
}

void PerfFrameShown() {
  // (the render thread only)
  static std::array<float, 512> history{};
  static size_t history_at = 0, history_count = 0;
  static Clock::time_point last{}, window_start{};
  static uint32_t window_frames = 0;
#if defined(_WIN32)
  static uint64_t thread_before = 0, process_before = 0;
  static const DWORD cpus = [] {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return std::max<DWORD>(1, info.dwNumberOfProcessors);
  }();
#endif
  const auto now = Clock::now();
  if (last.time_since_epoch().count()) {
    history[history_at] = std::chrono::duration<float, std::milli>(now - last).count();
    history_at = (history_at + 1) % history.size();
    history_count = std::min(history_count + 1, history.size());
  } else {
    window_start = now;
  }
  last = now;
  ++window_frames;
  const double secs = std::chrono::duration<double>(now - window_start).count();
  if (secs < 0.5) return;

  PerfNumbers n;
  n.fps = window_frames / secs;
  n.frame_ms = 1000.0 * secs / window_frames;
  {  // (the slowest 1 % of the last frames, as a frame rate)
    std::vector<float> sorted(history.begin(), history.begin() + history_count);
    if (!sorted.empty()) {
      const size_t worst = std::max<size_t>(1, sorted.size() / 100);
      std::partial_sort(sorted.begin(), sorted.begin() + worst, sorted.end(), std::greater<float>());
      double sum = 0;
      for (size_t i = 0; i < worst; ++i) sum += sorted[i];
      n.low_1pct_fps = sum > 0 ? 1000.0 * worst / sum : 0;
    }
  }
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
    const uint64_t t = FileTime(kernel) + FileTime(user);
    if (thread_before) n.render_cpu_ms = double(t - thread_before) / 10000.0 / window_frames;
    thread_before = t;
  }
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    const uint64_t t = FileTime(kernel) + FileTime(user);
    if (process_before) {
      const double ms = double(t - process_before) / 10000.0;
      n.all_cpu_ms = ms / window_frames;
      n.cpu_percent = 100.0 * ms / (secs * 1000.0 * cpus);
    }
    process_before = t;
  }
#endif
  n.gpu_percent = g_gpu_percent.load();
  n.gpu_ms = n.gpu_percent >= 0 ? n.gpu_percent / 100.0 * n.frame_ms : 0;
  const native::FrameCosts c = native::LastFrameCosts();
  n.gpu_wait_ms = c.gpu_wait_ms, n.draw_ms = c.draw_ms, n.draws = c.draws;
  n.texture_mb = c.texture_mb, n.targets_mb = c.targets_mb;
  n.renderer = native::RendererLabel();
  {
    std::lock_guard lock(g_mutex);
    g_numbers = n;
  }
  window_start = now;
  window_frames = 0;
}

void InstallPerfOverlay(rex::ui::ImGuiDrawer* drawer) {
  g_level = std::clamp(REXCVAR_GET(perf_overlay), 0, 2);
  g_overlay = new PerfOverlay(drawer);
  rex::ui::RegisterBind("bind_perf_overlay", "F2", "Performance overlay: off, the frame rate, everything",
                        [] { g_level = (g_level.load() + 1) % 3; });
#if defined(_WIN32)
  std::thread(WatchGpu).detach();
#endif
}

void ShowNotice(const std::string& text, float seconds) {
  std::lock_guard lock(g_notice_mutex);
  if (text != g_notice || Clock::now() > g_notice_until) REXLOG_INFO("notice on screen: {}", text);
  g_notice = text;
  g_notice_until = Clock::now() + std::chrono::milliseconds(int(seconds * 1000));
}

}  // namespace xmd
