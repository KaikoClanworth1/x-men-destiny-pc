// X-Men: Destiny - scripted controller (see script_input.h).

#include "script_input.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <spdlog/sinks/base_sink.h>

namespace xmd {

namespace {

constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x53435254);  // 'SCRT'
constexpr auto kPollInterval = std::chrono::milliseconds(100);

std::mutex g_file_mutex;
std::vector<std::string> g_file_lines;

uint16_t ParseButtons(const std::string& spec) {
  static const struct {
    const char* name;
    uint16_t bit;
  } kNames[] = {
      {"UP", X_INPUT_GAMEPAD_DPAD_UP},       {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},   {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"START", X_INPUT_GAMEPAD_START},      {"BACK", X_INPUT_GAMEPAD_BACK},
      {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},    {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"A", X_INPUT_GAMEPAD_A},              {"B", X_INPUT_GAMEPAD_B},
      {"X", X_INPUT_GAMEPAD_X},              {"Y", X_INPUT_GAMEPAD_Y},
  };
  uint16_t bits = 0;
  std::istringstream in(spec);
  for (std::string name; std::getline(in, name, '+');) {
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    bool found = false;
    for (const auto& n : kNames) {
      if (name == n.name) {
        bits |= n.bit;
        found = true;
      }
    }
    if (!found) REXLOG_WARN("script input: unknown button '{}'", name);
  }
  return bits;
}

// "until": the text a step waits for in the log, and whether it has come.
std::mutex g_until_mutex;
std::string g_until_text;
std::atomic<bool> g_until_hit{false};
// (the last second's lines: a step's text may be logged just before the step starts - the press before it brought it)
std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> g_recent_lines;

// The recorder (XMD_RECORD_FILE): starts at the first log line holding g_record_from.
std::string g_record_from;
std::atomic<bool> g_record_on{false};
std::chrono::steady_clock::time_point g_record_start;
std::mutex g_record_mutex;
std::ofstream g_record_out;
X_INPUT_GAMEPAD g_record_last{};
bool g_record_any = false;
// (the screens a session passes: the recorder writes "sync <name>" at each and counts its times from it again,
// the playback waits there for the same line - loading and cut-scene times differ from run to run)
const char* const kSyncNames[] = {"flash call: OnCharacterSelection (", "flash call: IntroComplete",
                                  "flash call: OnPowerSelection (", "flash call: ShowHudComplete"};

// "sync" steps: the text of the next one is watched from the moment the one before it passed, so a line that
// comes while the pad lines before it still play is not missed.
std::string g_sync_text;
bool g_sync_hit = false;

void WritePad(std::ostream& out, long long end_ms, const X_INPUT_GAMEPAD& g) {
  char line[128];
  std::snprintf(line, sizeof(line), "pad %lld %04X %d %d %d %d %d %d\n", end_ms, unsigned(uint16_t(g.buttons)),
                int(int16_t(g.thumb_lx)), int(int16_t(g.thumb_ly)), int(int16_t(g.thumb_rx)), int(int16_t(g.thumb_ry)),
                int(uint8_t(g.left_trigger)), int(uint8_t(g.right_trigger)));
  out << line;
  out.flush();
}

bool SamePad(const X_INPUT_GAMEPAD& a, const X_INPUT_GAMEPAD& b) {
  auto close_to = [](int x, int y) { return std::abs(x - y) < 400; };
  return uint16_t(a.buttons) == uint16_t(b.buttons) && uint8_t(a.left_trigger) == uint8_t(b.left_trigger) &&
         uint8_t(a.right_trigger) == uint8_t(b.right_trigger) && close_to(int16_t(a.thumb_lx), int16_t(b.thumb_lx)) &&
         close_to(int16_t(a.thumb_ly), int16_t(b.thumb_ly)) && close_to(int16_t(a.thumb_rx), int16_t(b.thumb_rx)) &&
         close_to(int16_t(a.thumb_ry), int16_t(b.thumb_ry));
}

class UntilSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const std::string_view text(msg.payload.data(), msg.payload.size());
    if (text.find("script input:") != std::string_view::npos) return;  // (not the step's own line)
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(g_until_mutex);
    if (!g_until_text.empty() && text.find(g_until_text) != std::string_view::npos) g_until_hit = true;
    g_recent_lines.emplace_back(now, std::string(text.substr(0, 200)));
    while (g_recent_lines.size() > 400 || now - g_recent_lines.front().first > std::chrono::seconds(1)) {
      g_recent_lines.pop_front();
    }
    if (!g_sync_text.empty() && text.find(g_sync_text) != std::string_view::npos) g_sync_hit = true;
    if (!g_record_on && !g_record_from.empty() && text.find(g_record_from) != std::string_view::npos) {
      std::lock_guard record(g_record_mutex);
      g_record_start = now;
      g_record_on = true;
    } else if (g_record_on) {
      for (const char* name : kSyncNames) {
        if (text.find(name) == std::string_view::npos) continue;
        std::lock_guard record(g_record_mutex);
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_record_start).count();
        if (g_record_any) WritePad(g_record_out, ms, g_record_last);
        g_record_out << "sync " << name << '\n';
        g_record_out.flush();
        g_record_start = now;
        break;
      }
    }
  }
  void flush_() override {}
};

}  // namespace

void InstallScriptLogSink() {
  static bool done = false;
  if (done) return;
  done = true;
  rex::AddSink(std::make_shared<UntilSink>());
}

void InstallPadRecorder(rex::input::InputSystem* input, const std::filesystem::path& file, std::string from) {
  g_record_out.open(file, std::ios::binary | std::ios::trunc);
  g_record_from = std::move(from);
  if (g_record_from.empty()) {
    g_record_start = std::chrono::steady_clock::now();
    g_record_on = true;
  }
  REXLOG_INFO("pad recorder: writing {} from \"{}\"", file.string(), g_record_from);
  input->SetStateObserver([](uint32_t user, const X_INPUT_STATE& state) {
    if (user != 0 || !g_record_on) return;
    std::lock_guard lock(g_record_mutex);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - g_record_start).count();
    if (!g_record_any) {
      g_record_any = true;
      g_record_last = state.gamepad;
      return;
    }
    if (SamePad(state.gamepad, g_record_last)) return;
    WritePad(g_record_out, ms, g_record_last);  // (what was held until now)
    g_record_last = state.gamepad;
  });
}

ScriptInputDriver::ScriptInputDriver(std::filesystem::path command_file)
    : InputDriver(nullptr, 0), file_(std::move(command_file)) {}

X_STATUS ScriptInputDriver::Setup() {
  REXLOG_INFO("script input: reading commands from {}", file_.string());
  return X_STATUS_SUCCESS;
}

void ScriptInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  rex::input::DeviceInfo info;
  info.id = kDevice;
  info.name = "Script";
  out.push_back(info);
}

void ScriptInputDriver::ParseLine(const std::string& line) {
  std::istringstream in(line);
  std::string verb;
  if (!(in >> verb) || verb[0] == '#') return;
  std::transform(verb.begin(), verb.end(), verb.begin(), ::tolower);
  auto rest_of_line = [&in] {
    std::string text;
    std::getline(in, text);
    const size_t first = text.find_first_not_of(" \t");
    return first == std::string::npos ? std::string() : text.substr(first);
  };
  Step step;
  if (verb == "press" || verb == "hold") {
    std::string buttons;
    in >> buttons;
    step.buttons = ParseButtons(buttons);
    step.ms = 120;
    in >> step.ms;
    queue_.push_back(step);
    queue_.push_back(Step{.ms = 120});  // release, so repeated presses register
  } else if (verb == "stick") {
    std::string side;
    int x = 0, y = 0;
    in >> side >> x >> y >> step.ms;
    auto clamp = [](int v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); };
    (side == "R" || side == "r" ? step.rx : step.lx) = clamp(x);
    (side == "R" || side == "r" ? step.ry : step.ly) = clamp(y);
    std::string buttons;
    if (in >> buttons && buttons != "-") step.buttons = ParseButtons(buttons);
    int rt = 0;
    if (in >> rt) step.right_trigger = static_cast<uint8_t>(std::clamp(rt, 0, 255));
    int lt = 0;
    if (in >> lt) step.left_trigger = static_cast<uint8_t>(std::clamp(lt, 0, 255));
    queue_.push_back(step);
  } else if (verb == "trigger") {
    std::string side;
    int v = 0;
    in >> side >> v >> step.ms;
    (side == "R" || side == "r" ? step.right_trigger : step.left_trigger) =
        static_cast<uint8_t>(std::clamp(v, 0, 255));
    queue_.push_back(step);
  } else if (verb == "clear") {
    // (drops everything queued and the step under way: a boot route's "pressuntil" still waiting for a line
    // that came early would keep pressing its button in the level, ahead of whatever is appended)
    queue_.clear();
    current_ = Step{};
    running_ = false;
    pad_base_set_ = false;
    std::lock_guard until(g_until_mutex);
    g_until_text.clear();
    g_sync_text.clear();
  } else if (verb == "pad") {
    // (a recorded state: held until <ms> after the first of a run of "pad" lines started)
    unsigned buttons = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    in >> step.end_at >> std::hex >> buttons >> std::dec >> lx >> ly >> rx >> ry >> lt >> rt;
    step.buttons = static_cast<uint16_t>(buttons);
    step.lx = static_cast<int16_t>(lx), step.ly = static_cast<int16_t>(ly);
    step.rx = static_cast<int16_t>(rx), step.ry = static_cast<int16_t>(ry);
    step.left_trigger = static_cast<uint8_t>(lt), step.right_trigger = static_cast<uint8_t>(rt);
    queue_.push_back(step);
  } else if (verb == "wait") {
    in >> step.ms;
    queue_.push_back(step);
  } else if (verb == "until" || verb == "sync") {
    step.kind = Step::kUntil;
    step.arg = rest_of_line();
    step.sync = verb == "sync";
    if (step.sync) {
      std::lock_guard until(g_until_mutex);
      if (g_sync_text.empty()) g_sync_text = step.arg, g_sync_hit = false;
    }
    queue_.push_back(step);
  } else if (verb == "pressuntil") {
    std::string buttons;
    in >> buttons;
    step.kind = Step::kPressUntil;
    if (const size_t slash = buttons.find('/'); slash != std::string::npos) {
      step.other_buttons = ParseButtons(buttons.substr(slash + 1));
      buttons.resize(slash);
    }
    step.buttons = ParseButtons(buttons);
    step.arg = rest_of_line();
    queue_.push_back(step);
  } else {
    REXLOG_WARN("script input: unknown command '{}'", line);
  }
}

void ScriptInputDriver::PollFile() {
  static std::once_flag started;
  std::call_once(started, [this] {
    std::thread([file = file_] {
      size_t consumed = 0;
      for (;;) {
        std::this_thread::sleep_for(kPollInterval);
        std::ifstream f(file, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        const size_t size = static_cast<size_t>(f.tellg());
        if (size < consumed) consumed = 0;  // file was truncated: start over
        if (size == consumed) continue;
        f.seekg(static_cast<std::streamoff>(consumed));
        std::string text(size - consumed, '\0');
        f.read(text.data(), static_cast<std::streamsize>(text.size()));
        // Only whole lines; a partly written last line waits for the next poll.
        const size_t end = text.rfind('\n');
        if (end == std::string::npos) continue;
        consumed += end + 1;
        std::istringstream lines(text.substr(0, end));
        std::lock_guard lock(g_file_mutex);
        for (std::string line; std::getline(lines, line);) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          g_file_lines.push_back(std::move(line));
        }
      }
    }).detach();
  });
  std::vector<std::string> lines;
  {
    std::lock_guard lock(g_file_mutex);
    lines.swap(g_file_lines);
  }
  for (const std::string& line : lines) {
    if (line.compare(0, 4, "pad ") != 0) REXLOG_INFO("script input: {}", line);
    ParseLine(line);
  }
}

ScriptInputDriver::Step ScriptInputDriver::CurrentStep() {
  std::lock_guard lock(mutex_);
  PollFile();
  const auto now = Clock::now();
  for (;;) {
    if (running_ && current_.kind != Step::kPlain) {
      const double waited = std::chrono::duration<double>(now - smart_start_).count();
      const bool hit = g_until_hit;
      if (!hit && waited <= 180.0) {
        Step out;
        if (current_.kind == Step::kPressUntil) {
          if (now >= next_press_) {
            press_end_ = now + std::chrono::milliseconds(120);
            next_press_ = now + std::chrono::milliseconds(1500);
            if (current_.other_buttons) std::swap(current_.buttons, current_.other_buttons);
          }
          if (now < press_end_) out.buttons = current_.buttons;
        }
        return out;
      }
      if (hit) {
        REXLOG_INFO("script input: until \"{}\": seen after {:.1f} s", current_.arg, waited);
      } else {
        REXLOG_WARN("script input: until \"{}\": not seen in 180 s - going on", current_.arg);
      }
      {
        std::lock_guard until(g_until_mutex);
        g_until_text.clear();
      }
      running_ = false;
      current_ = Step{};
    }
    if (running_ && now < step_end_) return current_;
    running_ = !queue_.empty();
    if (!running_) {
      current_ = Step{};
      return current_;
    }
    current_ = queue_.front();
    queue_.pop_front();
    step_end_ = now + std::chrono::milliseconds(current_.ms);
    if (current_.end_at >= 0) {
      if (!pad_base_set_) pad_base_ = now, pad_base_set_ = true;
      step_end_ = pad_base_ + std::chrono::milliseconds(current_.end_at);
      // (already over - the game reads the pad less often than the recording changed: passed over, unless a
      // button would be lost with it)
      if (now >= step_end_ && !queue_.empty() && queue_.front().end_at >= 0 &&
          queue_.front().buttons == current_.buttons) {
        continue;
      }
      return current_;
    }
    pad_base_set_ = false;
    if (current_.kind != Step::kPlain) {
      smart_start_ = now;
      next_press_ = now;
      press_end_ = now;
      std::lock_guard until(g_until_mutex);
      g_until_text = current_.arg;
      g_until_hit = false;
      for (const auto& [when, text] : g_recent_lines) {
        if (now - when <= std::chrono::seconds(1) && text.find(g_until_text) != std::string::npos) g_until_hit = true;
      }
      if (current_.sync) {
        if (g_sync_text == current_.arg && g_sync_hit) g_until_hit = true;
        // (the next one is watched from now on)
        g_sync_text.clear();
        g_sync_hit = false;
        for (const Step& next : queue_) {
          if (next.sync) {
            g_sync_text = next.arg;
            break;
          }
        }
      }
      continue;
    }
    return current_;
  }
}

X_RESULT ScriptInputDriver::GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  // Test aid: how often the game reads the pad (XMD_TEST_PAD_RATE=1), every 2 s.
  static const bool rate = std::getenv("XMD_TEST_PAD_RATE") != nullptr;
  if (rate) {
    static std::atomic<uint32_t> polls{0};
    static std::atomic<int64_t> last{0};
    ++polls;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    int64_t before = last.load();
    if (now - before >= 2000 && last.compare_exchange_strong(before, now)) {
      REXLOG_INFO("script input: {} pad reads in the last {} ms", polls.exchange(0), before ? now - before : 0);
    }
  }
  const Step s = CurrentStep();
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = ++packet_;
    out_state->gamepad.buttons = s.buttons;
    out_state->gamepad.left_trigger = s.left_trigger;
    out_state->gamepad.right_trigger = s.right_trigger;
    out_state->gamepad.thumb_lx = s.lx;
    out_state->gamepad.thumb_ly = s.ly;
    out_state->gamepad.thumb_rx = s.rx;
    out_state->gamepad.thumb_ry = s.ry;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION*) {
  return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT ScriptInputDriver::GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                               X_INPUT_KEYSTROKE*) {
  return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace xmd

