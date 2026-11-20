// Checks src\menu_patch.cpp without the game: reads a .gfx movie carved from the disc
// (tools\gfx_dis.py --carve), writes the changed movie; tools\gfx_dis.py then lists it.
//
//   clang++ -std=c++20 -O1 -I ..\src menu_patch_test.cpp ..\src\menu_patch.cpp -o menu_patch_test.exe
//   menu_patch_test.exe <in.gfx> <out.gfx> [parts]     (parts: see PatchOptions)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "menu_patch.h"

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string name = xmd::menupatch::MovieName(data.data(), data.size());
  std::string why;
  std::vector<uint8_t> out;
  if (name == "XMDMenuMain") out = xmd::menupatch::PatchMainMenu(data.data(), data.size(), &why);
  else if (name == "XMDMenuOptions") out = xmd::menupatch::PatchOptions(data.data(), data.size(), &why, argc > 3 ? unsigned(std::atoi(argv[3])) : 7u);
  else why = "no change for this movie";
  std::printf("%s: %zu bytes -> %zu bytes %s\n", name.c_str(), data.size(), out.size(), why.c_str());
  if (out.empty()) return 1;
  std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return 0;
}
