// X-Men: Destiny - the mod menu (F1; on a controller: both sticks pressed in together).
//
// An overlay list over the game: cheats, teleports (the game's checkpoints, challenge arenas, saved spots),
// cut-scenes, suits and characters, game speed. While it is open the game sees an idle controller. Everything
// it does goes through the game's own script functions (the developer console the game still carries) and a
// few of the game's own routines - see mod_menu.cpp.
//
// Every entry can also be run without the menu, for tests: debug lines (tools\debug_cmd.ps1)
//   mod list                 all entries with their ids, in the log
//   mod run <id>             what choosing that entry does
//   mod open | close         the menu shown / hidden
//   mod key <up|down|ok|back|left|right>   one step in the open menu
//   mod checkpoint <name>    straight to a checkpoint by the game's own name for it
//   mod lua <code>           Lua, as the "lua" line

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::ui {
class ImGuiDrawer;
}
namespace rex::input {
class InputSystem;
}

namespace xmd::modmenu {

// The overlay and its key. `user_data`: where the saved spots are kept (mod_spots.txt).
void Install(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& user_data);
// Before the game starts: its controller (held back while the menu is open) and `scratch`, ScratchSize() bytes of
// guest memory for text handed to the game's own routines.
void AttachGame(rex::input::InputSystem* input, uint32_t scratch);
uint32_t ScratchSize();
// A debug line's verb and the rest of the line; false: not one of the mod menu's.
bool Command(const std::string& verb, const std::string& rest);
bool IsOpen();

}  // namespace xmd::modmenu
