// X-Men: Destiny - the performance overlay: the frame rate, and where a frame's time goes.
//
// In the top left corner of the window; F2 cycles it: off, the frame rate, everything (setting perf_overlay).
//   FPS 143  7.0 ms  1% low 110            the frames shown a second, the average frame, the slowest 1 %
//   CPU  render 6.1 ms  all threads 15 ms  the render thread's own CPU time a frame (what limits the frame rate
//                                          when the GPU waits for it), every thread's together, the PC's share
//   GPU  38 %  2.7 ms  wait 0.0 ms         this game's share of the GPU (Windows' own counter), that share of a
//                                          frame, and how long the renderer waited for the GPU to finish a frame
//   Draws, textures, render targets
// 240 frames a second needs CPU render and GPU both under about 4 ms.

#pragma once

#include <string>

#include <rex/ui/imgui_dialog.h>

namespace xmd {

// The numbers, measured as the game runs (any thread reads them).
struct PerfNumbers {
  double fps = 0, frame_ms = 0, low_1pct_fps = 0;
  double render_cpu_ms = 0;  // the render thread's CPU time a frame
  double all_cpu_ms = 0;     // every thread's CPU time a frame
  double cpu_percent = 0;    // the game's share of the whole PC's CPUs
  double gpu_percent = 0;    // the game's share of the GPU (3D engine); < 0: not known
  double gpu_ms = 0;         // gpu_percent of a frame
  double gpu_wait_ms = 0, draw_ms = 0, draws = 0, texture_mb = 0, targets_mb = 0;
  std::string renderer;
};
PerfNumbers Perf();

// A frame was shown (the render thread, at present).
void PerfFrameShown();

// Creates the overlay and registers F2.
void InstallPerfOverlay(rex::ui::ImGuiDrawer* drawer);

// A short notice at the bottom centre for `seconds` (any thread).
void ShowNotice(const std::string& text, float seconds);

}  // namespace xmd
