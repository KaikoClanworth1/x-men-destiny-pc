// X-Men: Destiny - texture dumps and packs (texture_packs.h): not in this
// port yet. The texture cache finds no pack and dumps nothing.

#include "native/texture_packs.h"

namespace xmd::native::texture_packs {

void Start() {}
bool Active() { return false; }
bool Dumping() { return false; }
Lookup Find(uint64_t, std::shared_ptr<const Replacement>*) { return Lookup::kNone; }
bool WantDump(uint64_t) { return false; }
void Dump(uint64_t, uint32_t, uint32_t, std::vector<uint8_t>, const char*) {}
std::vector<uint8_t> EncodePng(const uint8_t*, uint32_t, uint32_t) { return {}; }

}  // namespace xmd::native::texture_packs