// X-Men: Destiny - button prompts that match the device in use (controls chat).
//
// Every prompt in the game - the legends of the menus, the "[A]" style marks in tutorial and power texts, the
// HUD - is drawn from 15 pictures of one shared Flash movie (GameButtonIcons in engine2.zzz: Button1_img.dds ...
// Button15_img.dds, 128x128 DXT3; 1 A, 2 B, 3 Y, 4 X, 5 Back, 6 Start, 7 LB, 8 RB, 9 LT, 10 RT, 11 left stick,
// 12 right stick, 13 d-pad, 14 d-pad left/right, 15 d-pad up). The native renderer's texture cache recognizes
// them by their data as it uploads them and shows another picture in their place: a key or mouse button on the
// keyboard and mouse, a PlayStation or Nintendo pad's button on those pads, the pad's other button where the
// player has remapped it (controls.h decides which: PromptGlyph). When the device or a binding changes, the
// textures are uploaded again. The pictures: res\glyphs.bin (tools\make_glyphs.py), inside the exe.
// With the emulated renderer (--native_renderer=off) the prompts stay the game's own.
#pragma once

#include <cstdint>
#include <vector>

namespace xmd {

struct PadIconsPicture {
  uint32_t width = 0, height = 0;
  const std::vector<uint8_t>* rgba = nullptr;
};

// The pictures read from the exe (once, before the game draws).
void InstallPadIcons();

// --- the texture cache's side (native/textures.cpp, the renderer's thread) ---
// Whether a texture of that size could be a prompt picture (before hashing it).
bool PadIconsCandidate(uint32_t width, uint32_t height);
// The picture to show for a texture whose base level hashes to `hash` (FNV-1a 64), or none: the game's own.
PadIconsPicture PadIconsAtlas(uint32_t width, uint32_t height, uint64_t hash);
// A candidate texture was (re)uploaded from guest memory at physical_base.
void PadIconsTextureUploaded(uint32_t physical_base, uint32_t width, uint32_t height, uint64_t hash);
// Once a frame: true when the prompts' pictures have changed since the last call - `bases` gets the guest
// addresses of the prompt textures, to be uploaded again.
bool PadIconsTakeChanged(std::vector<uint32_t>* bases);

}  // namespace xmd
