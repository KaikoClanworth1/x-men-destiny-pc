// X-Men: Destiny - the port's own entries in the game's menus: "Exit" on the main menu and "Graphics" under
// Options. They are entries of the game's own Flash (Scaleform) menus, not an overlay.
//
// XMD_MENU_TRACE=1: every call between the game and its Flash movies, and every movie load, in the log.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include <rex/ppc.h>

namespace xmd::pcmenu {

// Once, before the game runs. `guest_block`: GuestBlockSize() bytes of guest memory for the port's texts and the
// changed movies; `exit_game` closes the game (any thread); `on_ui_thread` runs a function on the window's thread;
// `config_file`: the settings file the Graphics page's settings are written to.
void Install(uint32_t guest_block, std::function<void()> exit_game,
             std::function<void(std::function<void()>)> on_ui_thread, std::filesystem::path config_file);
uint32_t GuestBlockSize();

// Before the settings file is read: a Graphics setting the last session chose but did not live to run with (the
// GPU hung, a crash) goes back to what it was before - pc_menu.cpp, "settings on trial".
void RevertUnconfirmed(const std::filesystem::path& config_file);

// From the Flash call dispatcher's hook (lua_debug.cpp, sub_8271A058: r4 = the movie, r5 = the name, r6 = the
// arguments, r7 = their count). True: the call was the port's own and is done - the game must not see it.
bool OnFlashCall(PPCContext& ctx, uint8_t* base);

}  // namespace xmd::pcmenu
