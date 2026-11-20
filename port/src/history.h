// X-Men: Destiny - the history: what the game did in the last five minutes, always being recorded, written out by
// an F4 capture (capture.cpp) - the port's black box.
//
//   history\frames.csv         every frame of the last 5 minutes: how long it took, the renderer's share, what it
//                              waited for, how many calls of each kind
//   history\events.txt         the things that are rare enough to keep in words for 5 minutes: memory given and
//                              freed, files opened / read / closed, threads made, any kernel call that took long,
//                              pipelines built, stutters
//   history\render_calls.txt   every graphics call the game made, newest last (as many as the ring holds: 64 MB,
//                              2 million calls - seconds in a fight, minutes in a menu; XMD_HISTORY_CALLS_MB=<n>)
//   history\kernel_calls.txt   every kernel call of every thread (the ones made thousands of times a frame -
//                              critical sections, TLS - are only counted), newest last, 16,384 a thread
//   history\kernel_summary.txt per kernel function: calls and time since the start
//
// Every file's times are seconds before the capture ("-12.345"), so they can be laid side by side.
// Cost: a few stores per call. XMD_NO_HISTORY=1: off.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace xmd::history {

// Once, at setup: the kernel calls start being recorded. `guest_base`: host address of guest address 0.
void Install(const uint8_t* guest_base);

// The names of the graphics calls' kinds (RenderCall's `type`), for the files.
void SetRenderCallNames(const char* const* names, uint32_t count);
// A graphics call as the game made it (one thread at a time: the renderer's hand-over).
void RenderCall(uint8_t type, bool flag, uint32_t a, uint32_t b, uint32_t c, uint32_t d);

struct Frame {
  float renderer_ms = 0, waited_ms = 0, slow_ms = 0;  // (the renderer's thread; the game's wait for it; its slowest call)
  const char* slow = "";
  uint32_t draws = 0, device_copies = 0;
};
// A frame ended (at present, the game's render thread - marked as such: its long waits become events).
void FrameEnd(const Frame& frame);
// Time the frame under way spent building something (a pipeline).
void FrameBuilt(double ms);

// Something rare, in words.
void Event(std::string_view category, std::string_view text);

// Writes the files into `dir`; a line for the capture's info.txt.
std::string Write(const std::filesystem::path& dir);

}  // namespace xmd::history
