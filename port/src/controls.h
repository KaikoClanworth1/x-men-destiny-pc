// X-Men: Destiny - keyboard and mouse play, mouse look (controls chat).
//
// The game reads one Xbox 360 controller. The runtime's keyboard-and-mouse driver (rex::input::mnk) turns keys
// and mouse buttons into that controller's buttons and sticks; this file gives it a layout made for this game,
// switches it on for players, and moves the camera with the mouse past the game's stick dead zone and
// response curve (hook on the game's stick function, see controls.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace xmd::controls {

// Before the runtime is set up: the keyboard and mouse settings' defaults for this game (only where the player's
// settings file or the command line did not set them).
void Configure();

// After the game image is loaded. `overlay_open`: whether a window of the port that is worked with the mouse is
// open (the cursor is let go meanwhile).
void Install(uint8_t* membase, std::function<bool()> overlay_open);

// Debug lines (tools\debug_cmd.ps1): "controls ..." and "cam ..." - true when the verb was one of them.
bool Command(const std::string& verb, const std::string& rest);

}  // namespace xmd::controls
