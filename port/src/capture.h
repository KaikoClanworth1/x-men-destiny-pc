// X-Men: Destiny - F4: "I just saw something wrong". One key press while playing writes a folder with what is
// needed to find the fault and to get back to the same place:
//
//   UserData\captures\capture_<date>_<time>\
//     screenshot.png   the frame on screen (the game's picture, without the port's overlays)
//     info.txt         when, the build, the renderer, the frame's numbers (FPS, CPU / GPU time, draws), where the
//                      player is (x y z, read from the game) and the command that puts a test's player back there
//     draws.txt        every draw of the next frame: shaders, textures, render states, vertex layout
//     log_tail.txt     the last lines of the log before the key press
//     problems.txt     the session's problems so far (problem_log.h)
//
// A notice on screen says where it went.

#pragma once

#include <filesystem>

namespace xmd {

// Registers F4 (bind_capture). `user_data`: where captures\ goes; `problems`: the problems log to copy.
void InstallCapture(const std::filesystem::path& user_data, const std::filesystem::path& problems);

// Takes one now (any thread; returns at once, a worker writes the folder).
void TakeCapture();

}  // namespace xmd
