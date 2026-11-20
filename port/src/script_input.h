// X-Men: Destiny - scripted controller for automated testing.
//
// With XMD_INPUT_FILE=<path> set, the game's only controller is a virtual pad
// driven by commands appended to that file (one per line), so a test can play
// the game while it runs in the background, without touching the real mouse,
// keyboard or window focus:
//
//   press <buttons> [ms]        tap buttons (default 120 ms), then release
//   hold <buttons> <ms>         same, explicit duration
//   stick <L|R> <x> <y> <ms> [buttons|-] [RT] [LT]
//                               hold a stick at x,y (-32768..32767)
//   trigger <L|R> <0-255> <ms>  hold a trigger
//   wait <ms>                   idle
//   until <text>                wait for a log line containing text (180 s at most)
//   pressuntil <buttons> <text> press the buttons every 1.5 s until a log line contains text
//                               (<buttons>/<buttons>: the two in turn - START/A skips what either skips)
//   pad <ms> <hex buttons> <lx> <ly> <rx> <ry> <lt> <rt>
//                               a recorded state, held until <ms> after the run of pad lines began
//                               (written by the recorder: XMD_RECORD_FILE, tools\record_play.ps1)
//   sync <text>                 the recorder's "until": the line may also have come since the sync before it
//   clear                       drop everything still queued and the step under way
//
// <buttons> is one or more of A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT
// joined with '+'. Lines starting with '#' are ignored.

#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>

namespace rex::input {
class InputSystem;
}

namespace xmd {

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;  // X_INPUT_* types and button bits

// The "until" steps' log sink - once, at setup, before any game thread
// (adding a sink later races the threads' logging).
void InstallScriptLogSink();

// Records what the player does on controller 1 as "pad" lines in `file`, from the first log line that holds
// `from` (empty: from now). InstallScriptLogSink first.
void InstallPadRecorder(rex::input::InputSystem* input, const std::filesystem::path& file, std::string from);

class ScriptInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptInputDriver(std::filesystem::path command_file);

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  using Clock = std::chrono::steady_clock;

  struct Step {
    uint16_t buttons = 0;
    uint16_t other_buttons = 0;  // (pressuntil a/b: pressed in turn with `buttons`)
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int ms = 0;
    long long end_at = -1;  // ("pad": ms after the run's first step began; -1 otherwise)
    enum Kind : uint8_t { kPlain, kUntil, kPressUntil } kind = kPlain;
    std::string arg;  // the text an until step waits for
    bool sync = false;  // ("sync": an until whose line may have come while the steps before it played)
  };

  void PollFile();
  void ParseLine(const std::string& line);
  Step CurrentStep();

  std::filesystem::path file_;
  std::mutex mutex_;
  std::deque<Step> queue_;
  bool running_ = false;
  Step current_{};
  Clock::time_point step_end_{}, smart_start_{}, next_press_{}, press_end_{};
  uint32_t packet_ = 0;
  Clock::time_point pad_base_{};
  bool pad_base_set_ = false;
};

}  // namespace xmd
