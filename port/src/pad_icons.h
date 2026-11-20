// X-Men: Destiny - controller-specific button pictures (the texture cache's
// hooks for them). Not in this port yet: every texture is the game's own.
#pragma once

#include <cstdint>
#include <vector>

namespace xmd {

struct PadIconsPicture {
  uint32_t width = 0, height = 0;
  const std::vector<uint8_t>* rgba = nullptr;
};
inline PadIconsPicture PadIconsAtlas(uint32_t, uint32_t, uint64_t) { return {}; }
inline bool PadIconsCandidate(uint32_t, uint32_t) { return false; }
inline void PadIconsTextureUploaded(uint32_t, uint32_t, uint32_t, uint64_t) {}

}  // namespace xmd