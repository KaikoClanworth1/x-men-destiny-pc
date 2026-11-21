// X-Men: Destiny - keyboard and mouse play, mouse look (controls chat).
//
// The game reads one Xbox 360 controller. The runtime's keyboard-and-mouse driver (rex::input::mnk) turns keys
// and mouse buttons into that controller's buttons and sticks; this file gives it a layout made for this game,
// switches it on for players, and moves the camera with the mouse past the game's stick dead zone and
// response curve (hook on the game's stick function, see controls.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace rex::input {
class InputSystem;
}

namespace xmd::controls {

// Before the runtime is set up: keyboard and mouse on, the stored bindings read (`settings_file`: the port's
// settings file - the bindings live beside it in controls.toml) and given to the keyboard-and-mouse driver.
void Configure(const std::filesystem::path& settings_file);

// After the game image is loaded. `overlay_open`: whether a window of the port that is worked with the mouse is
// open (the cursor is let go meanwhile). `input`: the runtime's input system (the pad remapping hangs on it).
void Install(uint8_t* membase, std::function<bool()> overlay_open, rex::input::InputSystem* input);

// ---------------------------------------------------------------------------------------------------------
// Bindings - for the Options menu's remap page (pc_menu.cpp). Every function may be called from any thread
// (the game's, inside a Flash call); all are cheap. Changes are saved at once (controls.toml).
//
// An action is one of the game's controller functions under the name the game's Controls page gives it. On
// the keyboard-and-mouse side each has one key or mouse button the player chooses (plus fixed helpers that
// never get lost: Enter = accept, Backspace = back, Escape = pause, the arrow keys = d-pad - a helper yields
// when its key is given to another action); on the pad side each has one pad button or trigger. Binding a
// key or button that another action has swaps the two, so no action is ever left without one.

enum class Device { kKeyboardMouse, kPad };

int ActionCount();
const char* ActionName(int action);           // "Light Attack", "Jump", "Move Forward" ...
bool CanBind(int action, Device device);      // (false: the four Move rows on the pad - the left stick)
std::string BindingText(int action, Device device);  // short: "Left Mouse", "Space", "RB", "Left Stick"

// Press-to-bind: the next key / mouse button (kKeyboardMouse) or pad button / trigger (kPad) pressed becomes
// the action's binding. While it waits, and until everything is let go again afterwards, the game sees no
// input at all. It ends by itself: kDone, kCancelled (Escape; on the keyboard side also the pad's B; a key
// that cannot be bound - F1-F12, the console key - is ignored) or kTimedOut (5 s). Poll CaptureState() - e.g.
// every frame; the result stays until the next BeginCapture or ClearCapture.
enum class Capture { kIdle, kWaiting, kDone, kCancelled, kTimedOut };
bool BeginCapture(int action, Device device);
Capture CaptureState();
std::string CaptureText();  // (kDone: what was bound, as BindingText gives it)
void CancelCapture();       // (-> kCancelled)
void ClearCapture();        // (-> kIdle)

void ResetDefaults(Device device);

// Mouse rows. Sensitivity: 0.1 ... 5.0, shown best in steps of 0.1 (1.0 = default).
double MouseSensitivity();
void SetMouseSensitivity(double value);
bool MouseInvertY();
void SetMouseInvertY(bool on);

// Binds directly, by name (a key as the key settings spell it - "J", "LMB", "Space" - or a pad control: a b x y lb
// rb lt rt ls rs start back dpad_up ...). False: no such action, key or control.
bool SetBindingByName(int action, Device device, const std::string& name);

// What the player used last (for the page's first view, and the button prompts).
Device LastUsedDevice();

// The picture (a name in res\glyphs.bin) a button prompt of the game shows now, "" for the game's own Xbox 360
// picture. `button`: the game's prompt picture 1-15 (1 A, 2 B, 3 Y, 4 X, 5 Back, 6 Start, 7 LB, 8 RB, 9 LT,
// 10 RT, 11 left stick, 12 right stick, 13 d-pad, 14 d-pad left/right, 15 d-pad up) - pad_icons.h.
// Tests: XMD_PROMPTS=keyboard|xbox|playstation|nintendo fixes the device kind.
std::string PromptGlyph(int button);
// (the mouse was moved: the keyboard and mouse are what the player uses)
void NoteMouseUsed();

// (controls.cpp's own use: the stored bindings read / the pad remapping hung on the input system)
void LoadBindings(const std::filesystem::path& settings_file);
void AttachBindings(rex::input::InputSystem* input);

// Debug lines (tools\debug_cmd.ps1): "controls ..." and "cam ..." - true when the verb was one of them.
//   controls                      the sticks as the game has them        controls trace <frames>
//   controls list                 every action with both bindings
//   controls bind <key|pad> <action number>     = BeginCapture (the next scripted key / pad button binds)
//   controls set <key|pad> <action number> <name>   bind directly       controls reset <key|pad>
//   cam | cam scan | cam diff | cam peek <offset> <count> | cam poke <offset> <value>   the camera object
bool Command(const std::string& verb, const std::string& rest);

}  // namespace xmd::controls
