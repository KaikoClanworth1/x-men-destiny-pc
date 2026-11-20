// X-Men: Destiny - debug commands (see debug_commands.cpp): lines of a command file run as Lua in the game's
// script state - teleport the player, spawn a fight, fire an event.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct PPCContext;

namespace xmd::debug {

// `scratch`: ScratchSize() bytes of guest memory (the text handed to the game's Lua loader). `file`: the command
// file to read ("": none - Query only).
void Install(const std::string& file, uint32_t scratch);
uint32_t ScratchSize();
bool Waiting();  // (commands read and not yet run)

// Runs `lua` (a chunk that returns one value) in the game's script state at its next chance and hands the value,
// as text, to `answer` on the game's thread (false: Lua's error message instead). Any thread.
using Answer = std::function<void(bool ok, const std::string& text)>;
void Query(std::string lua, Answer answer);
// The player's position ("x y z", the game's own units) to `answer`; false: no player in the world.
void QueryPosition(Answer answer);

// The mod menu's ways in (mod_menu.cpp): the player put at a position; Lua run with its result in the log ("debug
// command: ok / error" lines); host code run on the game's script thread, where the game's own functions may be
// called (ctx: a scratch register set with stack room below r1).
void GotoPosition(float x, float y, float z, Answer answer);
void RunLua(std::string lua);
using Native = std::function<void(::PPCContext& ctx, uint8_t* base)>;
void RunNative(Native work);

// The world held still (time almost stopped) while frames go on being drawn; F5 and the debug lines
// "freeze" / "unfreeze".
void SetFrozen(bool frozen);
bool Frozen();

}  // namespace xmd::debug
