// X-Men: Destiny - the whole state of the game at an F4 capture, for reading afterwards (tools\state.py).
//
// The game's threads are held still while it is taken, so what is written is one moment:
//   memory.bin + memory_map.txt  every committed page of the console's memory (all-zero 64 KB blocks left out)
//   threads.txt                  every game thread: its registers, its call stack (the recompiled functions it is
//                                in, sub_XXXXXXXX, from the PC side; and the PowerPC return addresses on its own
//                                stack), priority, stack range
//   objects.txt                  the kernel objects: files open (path, position), events, modules, threads...
//   lua_globals.txt              the game's script state (_G, without its functions): mission variables
//   save\                        the profile's saved games as they are now (what tools\resume.ps1 starts from)
//
// A recompiled game can't be put back into the middle of a frame the way an emulator's save state can: the
// threads run as PC code, and where each one is lives on the PC's own stacks next to the graphics driver's and
// the sound system's state. Getting back to the spot is the game's own save + the position (resume.ps1).
#pragma once

#include <filesystem>
#include <string>

namespace xmd {

// Writes the state into `dir`; `user_data` is the UserData folder (its <profile>\415608AA saves are copied).
// Returns a line for info.txt (sizes, how long the game was held).
std::string CaptureState(const std::filesystem::path& dir, const std::filesystem::path& user_data);

}  // namespace xmd
