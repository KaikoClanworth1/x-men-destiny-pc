// X-Men: Destiny - the bindings: which key, mouse button or pad button does what (see controls.h).
//
// Keyboard and mouse: each action's key is written into the runtime driver's key settings (keybind_*), which the
// driver reads at every poll. Pad: a filter on the runtime's input system (InputSystem::SetDeviceStateFilter)
// rewrites every real pad's state through the table before the game sees it. The same filter does press-to-bind
// and holds the game's input back meanwhile. Stored in controls.toml beside the port's settings file:
//   key.<action> = "<key name>"     pad.<action> = "<pad control>"     mouse_sensitivity = 1.0   mouse_invert_y = false

#include "controls.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DECLARE(double, mouse_sensitivity);
REXCVAR_DECLARE(bool, mouse_invert_y);

namespace xmd::controls {

namespace {

using namespace rex::input;
using rex::ui::VirtualKey;

// The pad's controls an action can sit on (the order is the file's and the tables').
struct PadControl {
  const char* id;
  const char* text;
  uint16_t bit;  // (0: a trigger)
  int trigger;   // (1 left, 2 right)
};
const PadControl kPad[] = {
    {"a", "A", X_INPUT_GAMEPAD_A, 0},
    {"b", "B", X_INPUT_GAMEPAD_B, 0},
    {"x", "X", X_INPUT_GAMEPAD_X, 0},
    {"y", "Y", X_INPUT_GAMEPAD_Y, 0},
    {"lb", "LB", X_INPUT_GAMEPAD_LEFT_SHOULDER, 0},
    {"rb", "RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0},
    {"lt", "LT", 0, 1},
    {"rt", "RT", 0, 2},
    {"ls", "Left Stick Click", X_INPUT_GAMEPAD_LEFT_THUMB, 0},
    {"rs", "Right Stick Click", X_INPUT_GAMEPAD_RIGHT_THUMB, 0},
    {"start", "Start", X_INPUT_GAMEPAD_START, 0},
    {"back", "Back", X_INPUT_GAMEPAD_BACK, 0},
    {"dpad_up", "D-Pad Up", X_INPUT_GAMEPAD_DPAD_UP, 0},
    {"dpad_down", "D-Pad Down", X_INPUT_GAMEPAD_DPAD_DOWN, 0},
    {"dpad_left", "D-Pad Left", X_INPUT_GAMEPAD_DPAD_LEFT, 0},
    {"dpad_right", "D-Pad Right", X_INPUT_GAMEPAD_DPAD_RIGHT, 0},
};
constexpr int kPadControls = static_cast<int>(std::size(kPad));
constexpr uint8_t kTriggerPressed = 64;

// The actions: the game's names (its Options > Controls page), the driver setting that takes the key, the pad
// control the game reads for it (-1: the left stick), the default key and the helper key that always works too.
struct Action {
  const char* id;
  const char* name;
  const char* setting;
  int pad;
  const char* key;
  const char* helper;
};
const Action kActions[] = {
    {"move_forward", "Move Forward", "keybind_lstick_up", -1, "W", ""},
    {"move_back", "Move Back", "keybind_lstick_down", -1, "S", ""},
    {"move_left", "Move Left", "keybind_lstick_left", -1, "A", ""},
    {"move_right", "Move Right", "keybind_lstick_right", -1, "D", ""},
    {"light_attack", "Light Attack", "keybind_x", 2, "LMB", ""},
    {"heavy_attack", "Heavy Attack", "keybind_y", 3, "RMB", ""},
    {"jump", "Jump / Accept", "keybind_a", 0, "Space", "Return"},
    {"dodge", "Dodge / Back", "keybind_b", 1, "Shift", "Backspace"},
    {"block", "Block", "keybind_right_shoulder", 5, "Q", "MMB"},
    {"x_mode", "X-Mode", "keybind_left_shoulder", 4, "R", ""},
    {"powershift", "PowerShift", "keybind_right_trigger", 7, "E", ""},
    {"focus_camera", "Focus Camera", "keybind_left_trigger", 6, "F", ""},
    {"pause", "Pause", "keybind_start", 10, "Escape", "Escape"},
    {"back", "Back", "keybind_back", 11, "Tab", ""},
    {"dpad_up", "Change View Up", "keybind_dpad_up", 12, "Up", "Up"},
    {"dpad_down", "Change View Down", "keybind_dpad_down", 13, "Down", "Down"},
    {"dpad_left", "Change View Left", "keybind_dpad_left", 14, "Left", "Left"},
    {"dpad_right", "Change View Right", "keybind_dpad_right", 15, "Right", "Right"},
    {"left_stick_click", "Left Stick Click", "keybind_lstick_press", 8, "C", ""},
    {"right_stick_click", "Right Stick Click", "keybind_rstick_press", 9, "V", ""},
};
constexpr int kActionCount = static_cast<int>(std::size(kActions));

std::mutex g_mutex;
uint16_t g_key[kActionCount] = {};  // (virtual key of each action)
int g_pad[kActionCount] = {};       // (index into kPad; -1 where the action has none)
bool g_pad_identity = true;
std::filesystem::path g_file;
std::atomic<Device> g_last_device{Device::kKeyboardMouse};
std::atomic<uint32_t> g_pad_family{0};  // (of the pad used last: 0 Xbox-style, 1 PlayStation, 2 Nintendo)

// Press-to-bind.
using Clock = std::chrono::steady_clock;
Capture g_capture = Capture::kIdle;
int g_capture_action = -1;
Device g_capture_device = Device::kKeyboardMouse;
bool g_capture_armed = false;  // (everything was let go since it began)
Clock::time_point g_capture_start;
std::string g_capture_text;
bool g_swallow = false;        // (after a capture: nothing reaches the game until all is let go)
bool g_pad_held = false, g_keys_held = false;

uint16_t KeyOf(const char* name) {
  return static_cast<uint16_t>(rex::ui::ParseVirtualKey(name));
}

// The settings' name of a key ("" for one that has none).
std::string KeyName(uint16_t key) {
  if (key == static_cast<uint16_t>(VirtualKey::kReturn)) return "Return";
  return rex::ui::VirtualKeyToString(static_cast<VirtualKey>(key));
}

// What the player reads.
std::string KeyText(uint16_t key) {
  const std::string name = KeyName(key);
  static const struct {
    const char* name;
    const char* text;
  } kTexts[] = {{"LMB", "Left Mouse"},  {"RMB", "Right Mouse"}, {"MMB", "Middle Mouse"}, {"Mouse4", "Mouse 4"},
                {"Mouse5", "Mouse 5"},  {"Return", "Enter"},    {"Control", "Ctrl"},     {"Escape", "Esc"},
                {"PageUp", "Page Up"},  {"PageDown", "Page Down"}};
  for (const auto& t : kTexts) {
    if (name == t.name) return t.text;
  }
  return name.empty() ? "?" : name;
}

// Keys that stay the port's own or make no sense as a game key.
bool Bindable(uint16_t key) {
  const std::string name = KeyName(key);
  if (name.empty() || name == "Escape" || name == "Backtick") return false;
  if (name.size() >= 2 && name[0] == 'F' && std::isdigit(static_cast<unsigned char>(name[1]))) return false;  // F1-F24
  for (const char* no : {"PrintScreen", "Pause", "CapsLock", "NumLock", "ScrollLock"}) {
    if (name == no) return false;
  }
  return true;
}

void DefaultsLocked(Device device) {
  for (int i = 0; i < kActionCount; ++i) {
    if (device == Device::kKeyboardMouse) g_key[i] = KeyOf(kActions[i].key);
    if (device == Device::kPad) g_pad[i] = kActions[i].pad;
  }
}

// The table into the driver's key settings.
void ApplyLocked() {
  for (int i = 0; i < kActionCount; ++i) {
    std::string value = KeyName(g_key[i]);
    const uint16_t helper = *kActions[i].helper ? KeyOf(kActions[i].helper) : 0;
    bool helper_free = helper != 0 && helper != g_key[i];
    for (int j = 0; helper_free && j < kActionCount; ++j) helper_free = g_key[j] != helper;
    if (helper_free) value += std::string(",") + kActions[i].helper;
    rex::cvar::SetFlagByName(kActions[i].setting, value);
  }
  for (const char* unused : {"keybind_rstick_up", "keybind_rstick_down", "keybind_rstick_left", "keybind_rstick_right",
                             "keybind_guide"}) {
    rex::cvar::SetFlagByName(unused, "");  // (the camera is the mouse's; the guide button is not the game's)
  }
  g_pad_identity = true;
  for (int i = 0; i < kActionCount; ++i) g_pad_identity = g_pad_identity && g_pad[i] == kActions[i].pad;
}

void SaveLocked() {
  if (g_file.empty()) return;
  std::ostringstream out;
  out << "# X-Men: Destiny - controls (written by the game: Options > Controls). Delete the file for the defaults.\n";
  for (int i = 0; i < kActionCount; ++i) out << "key." << kActions[i].id << " = \"" << KeyName(g_key[i]) << "\"\n";
  for (int i = 0; i < kActionCount; ++i) {
    if (g_pad[i] >= 0) out << "pad." << kActions[i].id << " = \"" << kPad[g_pad[i]].id << "\"\n";
  }
  out << "mouse_sensitivity = " << REXCVAR_GET(mouse_sensitivity) << "\n";
  out << "mouse_invert_y = " << (REXCVAR_GET(mouse_invert_y) ? "true" : "false") << "\n";
  std::error_code error;
  std::filesystem::create_directories(g_file.parent_path(), error);
  const std::filesystem::path part = g_file.string() + ".part";
  {
    std::ofstream file(part, std::ios::binary | std::ios::trunc);
    file << out.str();
    if (!file) {
      REXLOG_WARN("controls: could not write {}", part.string());
      return;
    }
  }
  std::filesystem::rename(part, g_file, error);
  if (error) REXLOG_WARN("controls: could not write {}: {}", g_file.string(), error.message());
}

// A table that came from a file: every key and pad control at most once, none missing - else that side's defaults.
void RepairLocked() {
  bool keys_ok = true, pad_ok = true;
  for (int i = 0; i < kActionCount; ++i) {
    keys_ok = keys_ok && g_key[i] != 0 && !KeyName(g_key[i]).empty();
    for (int j = 0; j < i; ++j) {
      keys_ok = keys_ok && g_key[i] != g_key[j];
      pad_ok = pad_ok && (g_pad[i] < 0 || g_pad[i] != g_pad[j]);
    }
    pad_ok = pad_ok && (kActions[i].pad < 0 ? g_pad[i] < 0 : (g_pad[i] >= 0 && g_pad[i] < kPadControls));
  }
  if (!keys_ok) {
    REXLOG_WARN("controls: the stored keys are not a whole layout - the default keys are used");
    DefaultsLocked(Device::kKeyboardMouse);
  }
  if (!pad_ok) {
    REXLOG_WARN("controls: the stored pad buttons are not a whole layout - the default buttons are used");
    DefaultsLocked(Device::kPad);
  }
}

void LoadLocked() {
  if (g_file.empty()) return;
  std::ifstream file(g_file);
  if (!file) return;
  int count = 0;
  for (std::string line; std::getline(file, line);) {
    const size_t equals = line.find('=');
    if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
    auto trim = [](std::string text) {
      const char* blank = " \t\r\"";
      const size_t first = text.find_first_not_of(blank), last = text.find_last_not_of(blank);
      return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    const std::string name = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
    if (name == "mouse_sensitivity" || name == "mouse_invert_y") {
      // (a value given on the command line or in the settings file itself stays)
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      continue;
    }
    for (int i = 0; i < kActionCount; ++i) {
      if (name == std::string("key.") + kActions[i].id) {
        g_key[i] = KeyOf(value.c_str());
        ++count;
      } else if (name == std::string("pad.") + kActions[i].id) {
        g_pad[i] = -2;
        for (int c = 0; c < kPadControls; ++c) {
          if (value == kPad[c].id) g_pad[i] = c;
        }
        ++count;
      }
    }
  }
  RepairLocked();
  REXLOG_INFO("controls: {} binding(s) read from {}", count, g_file.string());
}

// Gives `action` the key; the action that had it gets `action`'s old one.
void BindKeyLocked(int action, uint16_t key) {
  for (int i = 0; i < kActionCount; ++i) {
    if (i != action && g_key[i] == key) g_key[i] = g_key[action];
  }
  g_key[action] = key;
  ApplyLocked();
  SaveLocked();
}

void BindPadLocked(int action, int control) {
  for (int i = 0; i < kActionCount; ++i) {
    if (i != action && g_pad[i] == control) g_pad[i] = g_pad[action];
  }
  g_pad[action] = control;
  ApplyLocked();
  SaveLocked();
}

std::string TextLocked(int action, Device device) {
  if (device == Device::kKeyboardMouse) return KeyText(g_key[action]);
  return g_pad[action] < 0 ? "Left Stick" : kPad[g_pad[action]].text;
}

void EndCaptureLocked(Capture result) {
  g_capture = result;
  g_swallow = true;
}

uint8_t PadValue(const X_INPUT_GAMEPAD& pad, int control) {
  const PadControl& c = kPad[control];
  if (c.trigger == 1) return pad.left_trigger;
  if (c.trigger == 2) return pad.right_trigger;
  return (static_cast<uint16_t>(pad.buttons) & c.bit) ? 255 : 0;
}

// Every device's state on its way to the game (the game's thread, at each of its controller reads).
void Filter(const DeviceInfo& info, X_INPUT_GAMEPAD& pad) {
  std::lock_guard lock(g_mutex);
  const bool waiting = g_capture == Capture::kWaiting;
  if (waiting && Clock::now() - g_capture_start > std::chrono::seconds(5)) EndCaptureLocked(Capture::kTimedOut);
  if (!info.synthetic) {
    const X_INPUT_GAMEPAD raw = pad;
    int pressed = -1;
    for (int c = 0; c < kPadControls; ++c) {
      if (PadValue(raw, c) >= kTriggerPressed) {
        pressed = c;
        break;
      }
    }
    g_pad_held = pressed >= 0;
    if (g_pad_held || !IsNeutral(raw)) {
      g_last_device = Device::kPad;
      g_pad_family = info.family;
    }
    if (g_capture == Capture::kWaiting) {
      if (!g_capture_armed) {
        // (armed by the keys' side below as well: both must be let go)
      } else if (pressed >= 0 && g_capture_device == Device::kPad) {
        BindPadLocked(g_capture_action, pressed);
        g_capture_text = kPad[pressed].text;
        EndCaptureLocked(Capture::kDone);
      } else if (pressed == 1 && g_capture_device == Device::kKeyboardMouse) {
        EndCaptureLocked(Capture::kCancelled);  // (the pad's B)
      }
    }
    if (!g_pad_identity) {
      uint16_t buttons = static_cast<uint16_t>(raw.buttons) & X_INPUT_GAMEPAD_GUIDE;
      uint8_t left = 0, right = 0;
      for (int i = 0; i < kActionCount; ++i) {
        if (kActions[i].pad < 0) continue;
        const uint8_t value = PadValue(raw, g_pad[i]);
        const PadControl& target = kPad[kActions[i].pad];
        if (target.trigger == 1) {
          left = value;
        } else if (target.trigger == 2) {
          right = value;
        } else if (value >= kTriggerPressed) {
          buttons |= target.bit;
        }
      }
      pad.buttons = buttons;
      pad.left_trigger = left;
      pad.right_trigger = right;
    }
  } else {
    bool keys[256];
    rex::input::mnk::KeysDown(keys);
    int pressed = -1;
    for (int k = 1; k < 256; ++k) {
      if (keys[k]) {
        pressed = k;
        break;
      }
    }
    g_keys_held = pressed >= 0;
    if (g_keys_held) g_last_device = Device::kKeyboardMouse;
    if (g_capture == Capture::kWaiting) {
      if (!g_capture_armed) {
        g_capture_armed = !g_keys_held && !g_pad_held;
      } else if (pressed == static_cast<int>(VirtualKey::kEscape)) {
        EndCaptureLocked(Capture::kCancelled);
      } else if (g_capture_device == Device::kKeyboardMouse) {
        // (the first key down that can be a game key; others are passed over)
        for (int k = 1; k < 256; ++k) {
          if (keys[k] && Bindable(static_cast<uint16_t>(k))) {
            BindKeyLocked(g_capture_action, static_cast<uint16_t>(k));
            g_capture_text = KeyText(static_cast<uint16_t>(k));
            EndCaptureLocked(Capture::kDone);
            break;
          }
        }
      }
    }
  }
  if (g_swallow && g_capture != Capture::kWaiting && !g_keys_held && !g_pad_held) g_swallow = false;
  if (g_capture == Capture::kWaiting || g_swallow) pad = X_INPUT_GAMEPAD{};
}

}  // namespace

void LoadBindings(const std::filesystem::path& settings_file) {
  std::lock_guard lock(g_mutex);
  g_file = settings_file.parent_path() / "controls.toml";
  // Tests (a scripted controller): bindings are neither read nor kept, so one test's remapping never reaches the
  // next test's route - unless the test names a file of its own (XMD_CONTROLS_FILE).
  if (std::getenv("XMD_INPUT_FILE")) g_file.clear();
  if (const char* own = std::getenv("XMD_CONTROLS_FILE"); own && *own) g_file = own;
  DefaultsLocked(Device::kKeyboardMouse);
  DefaultsLocked(Device::kPad);
  LoadLocked();
  ApplyLocked();
  rex::input::mnk::SetPlainKeys(true);
}

void AttachBindings(rex::input::InputSystem* input) {
  if (input) input->SetDeviceStateFilter(&Filter);
}

int ActionCount() { return kActionCount; }

const char* ActionName(int action) {
  return action >= 0 && action < kActionCount ? kActions[action].name : "";
}

bool CanBind(int action, Device device) {
  if (action < 0 || action >= kActionCount) return false;
  return device == Device::kKeyboardMouse || kActions[action].pad >= 0;
}

std::string BindingText(int action, Device device) {
  if (action < 0 || action >= kActionCount) return {};
  std::lock_guard lock(g_mutex);
  return TextLocked(action, device);
}

bool BeginCapture(int action, Device device) {
  if (!CanBind(action, device)) return false;
  std::lock_guard lock(g_mutex);
  g_capture = Capture::kWaiting;
  g_capture_action = action;
  g_capture_device = device;
  g_capture_armed = false;
  g_capture_start = Clock::now();
  g_capture_text.clear();
  return true;
}

Capture CaptureState() {
  std::lock_guard lock(g_mutex);
  // (the filter only runs while the game reads its controller: the time limit holds without it too)
  if (g_capture == Capture::kWaiting && Clock::now() - g_capture_start > std::chrono::seconds(5)) {
    EndCaptureLocked(Capture::kTimedOut);
  }
  return g_capture;
}

std::string CaptureText() {
  std::lock_guard lock(g_mutex);
  return g_capture_text;
}

void CancelCapture() {
  std::lock_guard lock(g_mutex);
  if (g_capture == Capture::kWaiting) EndCaptureLocked(Capture::kCancelled);
}

void ClearCapture() {
  std::lock_guard lock(g_mutex);
  if (g_capture == Capture::kWaiting) g_swallow = true;
  g_capture = Capture::kIdle;
}

void ResetDefaults(Device device) {
  std::lock_guard lock(g_mutex);
  DefaultsLocked(device);
  ApplyLocked();
  SaveLocked();
}

bool SetBindingByName(int action, Device device, const std::string& name) {
  if (!CanBind(action, device)) return false;
  std::lock_guard lock(g_mutex);
  if (device == Device::kKeyboardMouse) {
    const uint16_t key = KeyOf(name.c_str());
    if (!key || (!Bindable(key) && key != KeyOf(kActions[action].key))) return false;
    BindKeyLocked(action, key);
    return true;
  }
  for (int c = 0; c < kPadControls; ++c) {
    if (name == kPad[c].id || name == kPad[c].text) {
      BindPadLocked(action, c);
      return true;
    }
  }
  return false;
}

double MouseSensitivity() { return REXCVAR_GET(mouse_sensitivity); }

void SetMouseSensitivity(double value) {
  rex::cvar::SetFlagByName("mouse_sensitivity", std::to_string(std::clamp(value, 0.1, 5.0)));
  std::lock_guard lock(g_mutex);
  SaveLocked();
}

bool MouseInvertY() { return REXCVAR_GET(mouse_invert_y); }

void SetMouseInvertY(bool on) {
  rex::cvar::SetFlagByName("mouse_invert_y", on ? "true" : "false");
  std::lock_guard lock(g_mutex);
  SaveLocked();
}

Device LastUsedDevice() { return g_last_device.load(std::memory_order_relaxed); }

void NoteMouseUsed() { g_last_device = Device::kKeyboardMouse; }

std::string PromptGlyph(int button) {
  // The pad control each of the game's 15 prompt pictures stands for (index into kPad; -1: a stick or the d-pad
  // as a whole).
  static const int kControl[15] = {0, 1, 3, 2, 11, 10, 4, 5, 6, 7, -1, -1, -1, -1, 12};
  if (button < 1 || button > 15) return {};
  static const std::string forced = [] {
    const char* value = std::getenv("XMD_PROMPTS");
    return std::string(value ? value : "");
  }();
  Device device = LastUsedDevice();
  uint32_t family = g_pad_family.load(std::memory_order_relaxed);
  if (!forced.empty()) {
    device = forced == "keyboard" ? Device::kKeyboardMouse : Device::kPad;
    family = forced == "playstation" ? 1 : forced == "nintendo" ? 2 : 0;
  }
  const int control = kControl[button - 1];
  std::lock_guard lock(g_mutex);
  // (the action the game reads that control for)
  int action = -1;
  for (int i = 0; i < kActionCount; ++i) {
    if (control >= 0 && kActions[i].pad == control) action = i;
  }
  if (device == Device::kKeyboardMouse) {
    if (button == 12) return "kbm_camera";
    if (button == 11) {
      const bool wasd = g_key[0] == KeyOf("W") && g_key[1] == KeyOf("S") && g_key[2] == KeyOf("A") &&
                        g_key[3] == KeyOf("D");
      return wasd ? "kbm_move_wasd" : "kbm_move";
    }
    if (button == 13 || button == 14) return "kbm_move";  // (the arrow keys' block)
    return action >= 0 ? "key_" + KeyName(g_key[action]) : std::string();
  }
  const char* prefix = family == 1 ? "ps_" : family == 2 ? "nin_" : "xb_";
  if (button == 11 || button == 12) {
    return family == 0 ? std::string() : std::string(button == 11 ? "pad_lstick" : "pad_rstick");
  }
  if (action < 0) return {};  // (the d-pad as a whole: the game's picture fits every pad)
  const int physical = g_pad[action];
  if (family == 0 && physical == control) return {};  // (an Xbox-style pad, not remapped: the game's own)
  if (physical >= 12) return {};
  return std::string(prefix) + kPad[physical].id;
}

}  // namespace xmd::controls
