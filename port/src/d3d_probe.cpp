// X-Men: Destiny - a probe of the game's Direct3D library (developer aid).
//
// The Xbox 360 XDK's Direct3D is linked into the game without symbols; its
// functions were found by matching them against a game where they are known
// (tools/d3d_match.py). This checks those matches in the running game and
// collects what a native renderer needs:
//
//   XMD_D3D_PROBE=1         calls per frame of each function, the device object,
//                           the device's GPU register mirror layout, where the
//                           current shaders / declaration / index buffer are kept
//   XMD_SHADER_DUMP=<dir>   every shader container the game creates, once each
//
// Off (neither set), every hook only passes the call on.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <windows.h>

#include "generated/default/xmd_init.h"

namespace {

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

const bool g_probe = !Env("XMD_D3D_PROBE").empty();
const std::string g_shader_dir = Env("XMD_SHADER_DUMP");

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

enum Fn {
  kPresent, kDrawIndexed, kDrawUP, kClear, kResolve, kSetRenderTarget, kSetTexture, kSetStream,
  kSetIndices, kSetDecl, kSetPS, kSetVS, kCreatePS, kCreateVS, kPSConst, kVSConst, kRegGroup,
  kRegRange, kConstFlush, kShaderBind, kFnCount
};
const char* const kFnNames[kFnCount] = {
    "Present", "DrawIndexed", "DrawUP", "Clear", "Resolve", "SetRenderTarget", "SetTexture",
    "SetStreamSource", "SetIndices", "SetVertexDeclaration", "SetPixelShader", "SetVertexShader",
    "CreatePixelShader", "CreateVertexShader", "SetPixelShaderConstantF", "SetVertexShaderConstantF",
    "RegisterGroup", "RegisterRange", "ConstantFlush", "ShaderBind"};

std::atomic<uint64_t> g_calls[kFnCount];
std::atomic<uint32_t> g_logged[kFnCount];
std::atomic<uint32_t> g_device{0};
std::mutex g_mutex;
std::set<uint64_t> g_seen;  // (kind, a, b) facts already logged

// The first few calls of each function, with their arguments.
void Note(Fn fn, const PPCContext& ctx, uint32_t first = 4) {
  g_calls[fn].fetch_add(1, std::memory_order_relaxed);
  if (g_logged[fn].load(std::memory_order_relaxed) >= first) return;
  if (g_logged[fn].fetch_add(1) >= first) return;
  REXLOG_INFO("d3d probe: {} r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} r7 {:08X} r8 {:08X} (lr {:08X})",
              kFnNames[fn], ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
              uint32_t(ctx.lr));
}

bool Once(uint32_t kind, uint32_t a, uint32_t b) {
  std::lock_guard lock(g_mutex);
  return g_seen.insert(uint64_t(kind) << 56 | uint64_t(a) << 28 | b).second;
}

// Where in the device `value` is kept after a Set call (each place once).
void FindInDevice(const char* what, uint8_t* base, uint32_t device, uint32_t value) {
  if (!device || !value) return;
  const uint8_t* d = base + device;
  for (uint32_t off = 0; off < 0x5000; off += 4) {
    if (Be32(d + off) == value && Once(1, uint32_t(reinterpret_cast<uintptr_t>(what) & 0xFFFFFFF), off))
      REXLOG_INFO("d3d probe: {} keeps its object at device+0x{:04X}", what, off);
  }
}

void SaveShader(uint8_t* base, uint32_t container, bool pixel) {
  if (g_shader_dir.empty() || container < 0x10000) return;
  const uint8_t* p = base + container;
  const uint32_t flags = Be32(p);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return;
  const uint32_t size = Be32(p + 4) + Be32(p + 8);
  if (size <= 0x24 || size > 0x100000) return;
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  {
    std::lock_guard lock(g_mutex);
    if (!g_seen.insert(h | 1ull << 63).second) return;
  }
  std::error_code ec;
  std::filesystem::create_directories(g_shader_dir, ec);
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s.xsc", static_cast<unsigned long long>(h), pixel ? "ps" : "vs");
  if (FILE* f = std::fopen((std::filesystem::path(g_shader_dir) / name).string().c_str(), "wb")) {
    std::fwrite(p, 1, size, f);
    std::fclose(f);
  }
}

// Which device object and which thread the draws come from, and the shader
// objects in use (the first few shown).
std::map<uint64_t, uint64_t> g_draw_sources;  // device << 32 | host thread id
std::set<uint32_t> g_shader_objects[2];

void NoteDraw(uint32_t device) {
  static thread_local const uint32_t thread = uint32_t(std::hash<std::thread::id>()(std::this_thread::get_id()));
  std::lock_guard lock(g_mutex);
  ++g_draw_sources[uint64_t(device) << 32 | thread];
}

// XMD_SHADER_OBJECTS=<folder of the disc's containers, tools/find_shaders.py>:
// whether a pixel shader object's copy of its container (from +0x28) plus its
// microcode (physical address at +0x18) is byte for byte a container of the
// disc; and the first vertex shader objects written out whole for study.
const std::string g_disc_dir = Env("XMD_SHADER_OBJECTS");
std::set<uint64_t> g_disc_hashes;
uint32_t g_ps_match = 0, g_ps_miss = 0, g_vs_dumped = 0;
std::set<uint64_t> g_ps_distinct;

// Whether [p, p + size) is committed, readable memory (a shader object's
// microcode address is not always one).
bool Readable(const uint8_t* p, size_t size) {
  const uint8_t* end = p + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
      return false;
    p = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
}

void StudyShader(uint8_t* base, uint32_t object, bool pixel) {
  if (g_disc_dir.empty()) return;
  {
    const uint8_t* o = base + object;
    if (!Readable(o, 0x380)) return;
    const uint32_t head = pixel ? 0x28 : 0x368;
    const uint32_t vsize = Be32(o + head + 4), psize = Be32(o + head + 8), code = Be32(o + (pixel ? 0x18 : 0x20));
    if (vsize > 0x10000 || psize > 0x10000 || !Readable(o, head + vsize) ||
        !Readable(base + ((code & 0x1FFFFFFF) | 0xA0000000u), psize)) {
      static int bad = 0;
      if (bad++ < 6)
        REXLOG_INFO("d3d probe: {} shader object {:08X}: microcode {:08X} + {:X} not readable", pixel ? "pixel" : "vertex",
                    object, code, psize);
      return;
    }
  }
  if (g_disc_hashes.empty()) {
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(g_disc_dir, ec))
      g_disc_hashes.insert(std::strtoull(e.path().filename().string().substr(0, 16).c_str(), nullptr, 16));
    g_disc_hashes.insert(0);
  }
  const uint8_t* o = base + object;
  if (!pixel) {
    // The object up to the end of its container copy (from +0x368), then its
    // microcode (physical address at +0x20): "vsobj_<n>_<object>_<virtual size>_<physical size>.bin".
    if (g_vs_dumped >= 60 || (Be32(o + 0x368) & 0xFFFFFF00u) != 0x102A1100u) return;
    const uint32_t vsize = Be32(o + 0x36C), psize = Be32(o + 0x370), code = Be32(o + 0x20);
    if (vsize > 0x10000 || psize > 0x10000) return;
    char name[80];
    std::snprintf(name, sizeof(name), "vsobj_%02u_%08X_%X_%X.bin", g_vs_dumped++, object, vsize, psize);
    if (FILE* f = std::fopen((std::filesystem::path(g_disc_dir).parent_path() / name).string().c_str(), "wb")) {
      std::fwrite(o, 1, 0x368 + vsize, f);
      std::fwrite(base + ((code & 0x1FFFFFFF) | 0xA0000000u), 1, psize, f);
      std::fclose(f);
    }
    return;
  }
  if ((Be32(o + 0x28) & 0xFFFFFF00u) != 0x102A1100u) return;
  const uint32_t vsize = Be32(o + 0x2C), psize = Be32(o + 0x30), code = Be32(o + 0x18);
  if (vsize > 0x10000 || psize > 0x10000) return;
  // (physical addresses: the 0xA0000000 / 0xC0000000 / 0xE0000000 views all map them)
  const uint8_t* micro = base + ((code & 0x1FFFFFFF) | 0xA0000000u);
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < vsize; ++i) h = (h ^ o[0x28 + i]) * 1099511628211ull;
  for (uint32_t i = 0; i < psize; ++i) h = (h ^ micro[i]) * 1099511628211ull;
  g_ps_distinct.insert(h);
  const bool match = g_disc_hashes.count(h) != 0;
  ++(match ? g_ps_match : g_ps_miss);
  if (!match && g_ps_miss <= 3) {
    char name[64];
    std::snprintf(name, sizeof(name), "psobj_miss_%u_%08X.bin", g_ps_miss, object);
    if (FILE* f = std::fopen((std::filesystem::path(g_disc_dir).parent_path() / name).string().c_str(), "wb")) {
      std::fwrite(o + 0x28, 1, vsize, f);
      std::fwrite(micro, 1, psize, f);
      std::fclose(f);
    }
  }
}

void NoteShader(uint8_t* base, uint32_t object, bool pixel) {
  if (object < 0x10000) return;
  std::lock_guard lock(g_mutex);
  if (!g_shader_objects[pixel].insert(object).second) return;
  StudyShader(base, object, pixel);
  if (g_shader_objects[pixel].size() > 4) return;
  std::string hex;
  char part[12];
  for (uint32_t i = 0; i < 0x60; i += 4) {
    std::snprintf(part, sizeof(part), " %08X", Be32(base + object + i));
    hex += part;
  }
  REXLOG_INFO("d3d probe: {} shader object {:08X}:{}", pixel ? "pixel" : "vertex", object, hex);
}

void FrameEnd() {
  static uint64_t frames = 0;
  if (++frames % 300) return;
  {
    std::lock_guard lock(g_mutex);
    std::string sources;
    char part[64];
    for (auto& [key, n] : g_draw_sources) {
      std::snprintf(part, sizeof(part), " %08X/t%08X %.1f", uint32_t(key >> 32), uint32_t(key), n / 300.0);
      sources += part;
    }
    g_draw_sources.clear();
    REXLOG_INFO("d3d probe: draws per frame by device/thread:{}; shader objects seen: {} vertex, {} pixel", sources,
                g_shader_objects[0].size(), g_shader_objects[1].size());
    if (!g_disc_dir.empty())
      REXLOG_INFO("d3d probe: pixel shader objects: {} are disc containers, {} are not; {} distinct", g_ps_match,
                  g_ps_miss, g_ps_distinct.size());
  }
  std::string line;
  for (int i = 0; i < kFnCount; ++i) {
    const uint64_t n = g_calls[i].exchange(0);
    char part[64];
    std::snprintf(part, sizeof(part), " %s %.1f", kFnNames[i], n / 300.0);
    line += part;
  }
  REXLOG_INFO("d3d probe: frame {} per frame:{}", frames, line);
}

}  // namespace

#define XMD_PROBE(address, fn, before, after)   \
  REX_EXTERN(__imp__sub_##address);             \
  REX_HOOK_RAW(sub_##address) {                 \
    if (!g_probe) {                             \
      __imp__sub_##address(ctx, base);          \
      return;                                   \
    }                                           \
    Note(fn, ctx);                              \
    const PPCContext args = ctx;                \
    (void)args;                                 \
    before;                                     \
    __imp__sub_##address(ctx, base);            \
    after;                                      \
  }

// Present. XMD_FPS_LOG=1: every 5 s, the frames shown a second and how much of
// the game thread's time went by inside Present (waiting for the display's
// vblank or the GPU) - the rest is the game's own work.
REX_EXTERN(__imp__sub_82738208);
REX_HOOK_RAW(sub_82738208) {
  static const bool fps_log = !Env("XMD_FPS_LOG").empty();
  if (!g_probe && !fps_log) {
    __imp__sub_82738208(ctx, base);
    return;
  }
  using Clock = std::chrono::steady_clock;
  if (g_probe) Note(kPresent, ctx);
  g_device = ctx.r3.u32;
  const auto in = Clock::now();
  __imp__sub_82738208(ctx, base);
  const auto out = Clock::now();
  if (g_probe) FrameEnd();
  if (fps_log) {
    static Clock::time_point since = out;
    static double inside = 0, longest = 0;
    static Clock::time_point last = out;
    static uint32_t frames = 0;
    inside += std::chrono::duration<double>(out - in).count();
    longest = std::max(longest, std::chrono::duration<double>(out - last).count());
    last = out;
    ++frames;
    const double span = std::chrono::duration<double>(out - since).count();
    if (span >= 5.0) {
      REXLOG_INFO("fps: {:.1f} frames/s, {:.0f}% of the time in Present, longest frame {:.0f} ms", frames / span,
                  100.0 * inside / span, longest * 1000.0);
      since = out;
      inside = longest = 0;
      frames = 0;
    }
  }
}
XMD_PROBE(8273C300, kDrawIndexed, NoteDraw(ctx.r3.u32), )
XMD_PROBE(8273B4A8, kDrawUP, NoteDraw(ctx.r3.u32), )
XMD_PROBE(8273A690, kClear, , )
XMD_PROBE(827390F8, kResolve, , )
XMD_PROBE(827283C0, kSetRenderTarget, , )
XMD_PROBE(8272F0A8, kSetTexture, , )
XMD_PROBE(82728188, kSetStream, , )
XMD_PROBE(82728330, kSetIndices, , FindInDevice("SetIndices", base, args.r3.u32, args.r4.u32))
XMD_PROBE(827300C8, kSetDecl, , FindInDevice("SetVertexDeclaration", base, args.r3.u32, args.r4.u32))
XMD_PROBE(8272FB50, kSetPS, NoteShader(base, ctx.r4.u32, true),
          FindInDevice("SetPixelShader", base, args.r3.u32, args.r4.u32))
XMD_PROBE(8272FEF8, kSetVS, NoteShader(base, ctx.r4.u32, false),
          FindInDevice("SetVertexShader", base, args.r3.u32, args.r4.u32))
XMD_PROBE(8272F6B8, kPSConst, , )
XMD_PROBE(8272F790, kVSConst, , )
XMD_PROBE(8273E198, kConstFlush, , )
XMD_PROBE(8273ED38, kShaderBind, , )

// The register groups the device mirrors: (first GPU register, device offset).
REX_EXTERN(__imp__sub_8273DE00);
REX_HOOK_RAW(sub_8273DE00) {
  if (g_probe) {
    Note(kRegGroup, ctx);
    const uint32_t device = g_device.load();
    if (device && Once(2, ctx.r5.u32, ctx.r6.u32 - device))
      REXLOG_INFO("d3d probe: register group 0x{:04X} at device+0x{:04X} (r7 {:X})", ctx.r5.u32,
                  ctx.r6.u32 - device, ctx.r7.u32);
  }
  __imp__sub_8273DE00(ctx, base);
}

REX_EXTERN(__imp__sub_8273E040);
REX_HOOK_RAW(sub_8273E040) {
  if (g_probe) {
    Note(kRegRange, ctx);
    const uint32_t device = g_device.load();
    if (device && Once(3, ctx.r5.u32, ctx.r6.u32 - device))
      REXLOG_INFO("d3d probe: register range 0x{:04X} with mirror base device+0x{:04X}", ctx.r5.u32,
                  ctx.r6.u32 - device);
  }
  __imp__sub_8273E040(ctx, base);
}

// Shader creation: the container goes in (r3), the shader object comes out.
REX_EXTERN(__imp__sub_8272FA60);
REX_HOOK_RAW(sub_8272FA60) {
  if (g_probe) Note(kCreatePS, ctx);
  SaveShader(base, ctx.r3.u32, true);
  __imp__sub_8272FA60(ctx, base);
}

REX_EXTERN(__imp__sub_8272FE08);
REX_HOOK_RAW(sub_8272FE08) {
  if (g_probe) Note(kCreateVS, ctx);
  SaveShader(base, ctx.r3.u32, false);
  __imp__sub_8272FE08(ctx, base);
}
