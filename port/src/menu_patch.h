// X-Men: Destiny - the game's Flash (Scaleform .gfx) menu movies, changed as they are loaded: a fifth button
// on the main menu ("Exit") and a sixth entry under Options with its own page ("Graphics"). The disc's files
// stay as they are: the movie's bytes are rewritten in memory (pc_menu.cpp hands them over).
//
// A .gfx movie is a Flash 8 .swf without its pictures: a header, then tags (2-byte code+length, 6 bytes when
// the length is 63 or more). The menus' logic is ActionScript 2 byte code in DoAction tags and in the key
// handlers of an invisible button on each page. tools\gfx_dis.py lists a movie in this file's terms.
//
// No game or runtime headers here: tools\menu_patch_test.cpp builds this file alone to check its output.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xmd::menupatch {

// The movie's own name (its exporter tag): "XMDMenuMain", "XMDMenuOptions", ... Empty: not a .gfx movie.
std::string MovieName(const uint8_t* data, size_t size);

// Where the Options list's buttons sit after the change (the n-th, from 0), in the list's own coordinates.
double OptionsButtonY(int index);

// How many rows the Graphics page has (pc_menu.cpp fills that many or fewer).
constexpr int kGraphicsRows = 8;

// The changed movie, or an empty vector when the movie is not the one expected (another version of the game:
// the menu then stays as the disc has it). `why` gets the reason.
std::vector<uint8_t> PatchMainMenu(const uint8_t* data, size_t size, std::string* why);
// (parts: 8 = the key bindings page, frame "Bindings", and the X handler on the game's Controls page)
// `parts` (a debugging aid, tools\menu_patch_test.cpp): 1 the list's sixth button, 2 the Graphics page's pieces,
// 4 the Graphics frame.
std::vector<uint8_t> PatchOptions(const uint8_t* data, size_t size, std::string* why, unsigned parts = 15);

}  // namespace xmd::menupatch
