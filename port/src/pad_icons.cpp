// X-Men: Destiny - button prompts that match the device in use (see pad_icons.h).

#include "pad_icons.h"

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/logging.h>

#include "controls.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace xmd {

namespace {

constexpr int kButtons = 15;
// FNV-1a 64 of Button1_img.dds ... Button15_img.dds (their 128x128 DXT3 data, as the disc has it).
constexpr uint64_t kHashes[kButtons] = {
    0x3700F9800E6876F5ull, 0xBA821260B99E3806ull, 0xA5446E97F16752E4ull, 0x58009BCB210FA90Eull,
    0x0A79F426F2E3FEC9ull, 0xB38FCAEE84BC866Eull, 0xDFBCEE3492C42155ull, 0xDFA2CBF987FDCA1Eull,
    0x5962F1C5E5D9E083ull, 0x9D7DB99FD012EF10ull, 0x0F66313D53B507ADull, 0xAE0A171A10058362ull,
    0xF9853A117D99EB89ull, 0x00530A7AC534A6D3ull, 0xF9EA42F597755EDEull,
};

std::unordered_map<std::string, std::vector<uint8_t>> g_pictures;  // (filled once, then only read)

std::mutex g_mutex;
std::array<std::string, kButtons> g_shown;   // (the picture each prompt has now; "": the game's own)
std::unordered_map<uint32_t, int> g_bases;   // (guest address of a prompt texture -> its button, 0-based)
bool g_logged_unknown = false;

// The picture a prompt shows now: its name in the pack, "" for the game's own.
std::string Resolve(int button) {
  std::string name = controls::PromptGlyph(button + 1);
  if (name.empty() || g_pictures.count(name)) return name;
  // (a key the pack has no picture of: the "?" key rather than an Xbox button)
  return name.rfind("key_", 0) == 0 && g_pictures.count("key_unknown") ? "key_unknown" : std::string();
}

int ButtonOf(uint64_t hash) {
  for (int i = 0; i < kButtons; ++i) {
    if (kHashes[i] == hash) return i;
  }
  return -1;
}

}  // namespace

void InstallPadIcons() {
#if defined(_WIN32)
  const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
  const HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
  const auto* data = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
  const size_t size = found ? SizeofResource(nullptr, found) : 0;
  uint32_t count = 0;
  if (data && size >= 8 && std::memcmp(data, "XMDG", 4) == 0) std::memcpy(&count, data + 4, 4);
  constexpr size_t kName = 24, kPicture = 128 * 128 * 4;
  if (!count || size < 8 + count * (kName + kPicture)) {
    REXLOG_WARN("prompts: no button pictures in the program - the prompts stay the game's own");
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = reinterpret_cast<const char*>(data + 8 + i * kName);
    const uint8_t* pixels = data + 8 + count * kName + i * kPicture;
    g_pictures[std::string(name, strnlen(name, kName))].assign(pixels, pixels + kPicture);
  }
  REXLOG_INFO("prompts: {} button pictures", g_pictures.size());
#endif
}

bool PadIconsCandidate(uint32_t width, uint32_t height) {
  return width == 128 && height == 128 && !g_pictures.empty();
}

PadIconsPicture PadIconsAtlas(uint32_t width, uint32_t height, uint64_t hash) {
  const int button = ButtonOf(hash);
  if (button < 0) return {};
  const std::string name = Resolve(button);
  {
    std::lock_guard lock(g_mutex);
    g_shown[button] = name;
  }
  if (name.empty()) return {};
  return {width, height, &g_pictures.find(name)->second};
}

void PadIconsTextureUploaded(uint32_t physical_base, uint32_t width, uint32_t height, uint64_t hash) {
  if (width != 128 || height != 128 || !hash) return;
  const int button = ButtonOf(hash);
  std::lock_guard lock(g_mutex);
  if (button >= 0) {
    if (g_bases.emplace(physical_base, button).second) {
      REXLOG_INFO("prompts: the game's picture of button {} is the texture at {:08X}", button + 1, physical_base);
    }
  } else {
    g_bases.erase(physical_base);  // (that memory holds another texture now)
  }
}

bool PadIconsTakeChanged(std::vector<uint32_t>* bases) {
  if (g_pictures.empty()) return false;
  // (a few times a second is soon enough, and the answer takes the bindings' lock)
  static uint32_t frame = 0;
  if (++frame % 8) return false;
  bool changed = false;
  std::lock_guard lock(g_mutex);
  if (g_bases.empty()) return false;
  for (const auto& [base, button] : g_bases) {
    // (noted as shown at once: the texture is uploaded again when the game next draws with it - which may be
    // much later, and until then this must not report the change again)
    if (std::string now = Resolve(button); now != g_shown[button]) {
      g_shown[button] = std::move(now);
      changed = true;
    }
  }
  if (!changed) return false;
  REXLOG_INFO("prompts: the pictures change ({} textures; button 1 is now \"{}\")", g_bases.size(), Resolve(0));
  bases->clear();
  for (const auto& [base, button] : g_bases) bases->push_back(base);
  return true;
}

}  // namespace xmd
