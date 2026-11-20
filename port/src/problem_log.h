// X-Men: Destiny - the problems log: one short file that lists what went wrong in a session, apart from the
// full log - calls that failed (every warning and error the runtime logs), script errors, frames that took
// far too long and what they waited for (a shader pipeline built in the frame, ...), sound that stopped.
//
// A problem that repeats is written once, with a count of its repeats added every few seconds; numbers in a
// line do not make it a new problem.
//
//   UserData\logs\problems.log   (tests: XMD_PROBLEM_FILE=<file>, tools\run_test.ps1 sets runs\<name>_problems.log)

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xmd::problems {

// Opens the file and starts copying the log's warnings and errors into it. Once, at setup, before any game
// thread (adding a log sink later races the threads' logging).
void Install(const std::filesystem::path& file);

// One problem: `category` is a short word ("hitch", "shader", "audio", "script", "call").
void Report(std::string_view category, std::string_view text);

// A frame ended (the render thread, at present): reports the frame as a hitch when it took far longer than
// the frames before it, with what was noted for it.
void FrameEnd();
// Something the frame under way had to wait for (" pipeline vs .. ps ..: 43 ms") - named in its hitch line.
void FrameCause(std::string_view what, double ms);
// Where the frame's time went, in words (the game's side, the renderer's thread, its slowest call) - added to
// the frame's hitch line. Before FrameEnd.
void FrameSplit(std::string_view text);

// The game handed the sound output a frame (`silent`: all zero). Reports gaps and long silences.
void AudioFrame(bool silent);

// The log's last lines (every level), oldest first - an F4 capture's log_tail.txt (capture.h).
std::vector<std::string> RecentLog();

}  // namespace xmd::problems
