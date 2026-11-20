// X-Men: Destiny - native renderer (see native_renderer.h).
//
// Every draw is made with the game's own (converted) shaders, vertex/index data,
// constants and render state, into host render targets standing in for the
// EDRAM surfaces; resolves copy them into the textures the game samples
// (textures.cpp loads the rest from guest memory), and the front buffer is
// shown at Present.

#include "android_perf.h"
#include "frame_rate.h"
#include "native/native_renderer.h"

#include <algorithm>
#include <array>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <cctype>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <dxgi.h>  // (VideoMemoryBytes)
#else
#include <sys/resource.h>
#endif

#include <fmt/format.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <tmmintrin.h>
#else
// (ARM: the SSSE3 shuffle through SIMDe, as the recompiled code does)
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/ssse3.h>
#endif

#include <plume_render_interface.h>
#if defined(_WIN32)
#include <plume_d3d11.h>  // (slot tables: a view's swizzle)
#endif

#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "crash_report.h"
#include "profile_sampler.h"
#include "problem_log.h"
#include "history.h"
#include "perf_overlay.h"
#include "native/gpu.h"
#include "native/recording_list.h"
#if XMD_FSR2
#include "native/fsr2.h"
#if XMD_DLSS
#include "native/dlss.h"
#endif
#endif
#include "native/guest_d3d.h"
#include "native/textures.h"
#include "free_cam.h"
#include "gpu_probe.h"
#include "../util.h"
#if XMD_DEBUG_SUITE
#include "debug_suite/debug_suite.h"
#endif

// Default "main" (as the launcher): settings files without the key use it.
#if defined(__ANDROID__)
constexpr bool kScaleEffectsDefault = false;  // (phones: shadows and effects at the console's size)
#else
constexpr bool kScaleEffectsDefault = true;
#endif
REXCVAR_DEFINE_BOOL(native_scale_effects, kScaleEffectsDefault, "GPU",
                    "Native renderer: render shadows, reflections and glow at the render scale too "
                    "(false: at the Xbox 360's size, faster on weak GPUs)");

REXCVAR_DEFINE_BOOL(native_shadows, true, "GPU",
                    "Native renderer: the characters' and the arena's real-time shadows (false: nothing "
                    "is drawn into the shadow map - no shadows, fewer draws). Applies at once.");

// On by default only on phones, where the driver's command recording is a
// large part of the render thread; on a PC (D3D12, Batista's entrance at
// 5,400 draws) handing the commands over cost more than recording them.
#if defined(__ANDROID__)
constexpr bool kRecordThreadDefault = true;
#else
constexpr bool kRecordThreadDefault = false;
#endif
REXCVAR_DEFINE_BOOL(native_render_thread, true, "GPU",
                    "Native renderer: its work for the game's graphics calls is done on a thread of its own, beside the "
                    "game's render thread instead of inside it (needs a restart)");

REXCVAR_DEFINE_BOOL(native_record_thread, kRecordThreadDefault, "GPU",
                    "Native renderer: record the GPU commands on a second thread (recording_list.h), "
                    "off the game's render thread");

REXCVAR_DEFINE_BOOL(native_widescreen, false, "GPU",
                    "Native renderer: matches as wide as a wider-than-16:9 window (the HUD and "
                    "menus stay 16:9)");

REXCVAR_DEFINE_BOOL(native_prepare_pipelines, true, "GPU",
                    "Native renderer: build the known pipelines in the background while in the menus, "
                    "so scenes don't stutter compiling them");

REXCVAR_DEFINE_BOOL(native_resolve_write_back, true, "GPU",
                    "Native renderer: copy one-off render-to-texture results back to the game's memory "
                    "(Superstar Threads attire baking reads them)");

REXCVAR_DEFINE_BOOL(native_constant_check, false, "GPU",
                    "Debug: also hash the shader constants and log when a reused upload was "
                    "stale (checks the D3D dirty tracking the native renderer relies on)");

REXCVAR_DEFINE_INT32(native_aa, 0, "GPU",
                     "Native renderer anti-aliasing (supersampling): 1 off, 2 / 3 / 4 = that many times the "
                     "screen's resolution per side (4 / 9 / 16 samples a pixel, within native_max_scale); "
                     "0: native_2x_msaa decides (on = 2)");

REXCVAR_DEFINE_DOUBLE(native_render_scale, 1.0, "GPU",
                      "Native renderer: the scene's resolution below the Xbox 360's 720p for weak GPUs "
                      "(0.5 = 640 x 360, scaled up to the screen; 0.25 - 1). The HUD and menus' 2D art "
                      "are drawn into the same scene, so they get softer too.");

REXCVAR_DEFINE_INT32(native_scene_height, 0, "GPU",
                     "Native renderer: the scene's height in pixels, between the 720p steps (900 / 1080: "
                     "2x scaled down to it). 0: native_max_scale and native_render_scale decide.");

REXCVAR_DEFINE_STRING(native_upscale, "off", "GPU",
                      "Native renderer upscaling: off (the scene at the window's resolution or above, averaged "
                      "down), fsr1 (AMD FSR 1: the scene rendered smaller - native_upscale_quality - and "
                      "scaled up to the window edge-adaptively, then sharpened; the HUD scaled with it) or fsr2 "
                      "(experimental, Direct3D 12 / Vulkan: AMD FSR 2 - the match scene gathered over frames from "
                      "its depth, motion and native_jitter; FSR 1 where it can't run).");

REXCVAR_DEFINE_BOOL(native_jitter, false, "GPU",
                    "Native renderer (temporal upscaling groundwork, experimental): the match scene's 3D draws "
                    "moved by a sub-pixel offset each frame (Halton 2,3; 8 frames). On its own it only shakes "
                    "the picture by under a pixel - for a temporal upscaler (FSR 2 / DLSS) to gather.");

REXCVAR_DEFINE_INT32(native_upscale_quality, 1, "GPU",
                     "FSR 1's render size: 0 Ultra Quality (77% of the window's height), 1 Quality (67%), "
                     "2 Balanced (59%), 3 Performance (50%)");

REXCVAR_DEFINE_DOUBLE(native_upscale_sharpness, 0.8, "GPU", "FSR 1's sharpening, 0 (none) - 1 (the most)");

REXCVAR_DEFINE_INT32(native_max_scale, 8, "GPU",
                     "Native renderer: the largest render scale (1 = the Xbox 360's 720p, up to 8 = 5760 lines; "
                     "4, AUTO before 8K, counts as 8). The scale follows the window; phones start at 1.");

REXCVAR_DEFINE_STRING(native_texture_quality, "high", "GPU",
                      "Native renderer texture quality: high - as the game has them; medium / low - big "
                      "mipmapped textures start at the game's own half / quarter size mipmap (a quarter / "
                      "a sixteenth of the memory and upload); menus, fonts and pictures stay sharp")
    .allowed({"high", "medium", "low"});

REXCVAR_DEFINE_STRING(native_renderer, "off", "GPU",
                      "Native renderer: main - the game's own Direct3D calls are drawn natively (the emulator only "
                      "runs the GPU command stream); off - the emulated Xbox GPU draws (the default while the "
                      "native renderer is being brought up for this game).")
    .allowed({"off", "main", "shadow"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace texture_util = rex::graphics::texture_util;

#if defined(__SWITCH__)
#include <plume_vulkan.h>  // (the GPU time per pass)
#endif
namespace plume {
extern std::atomic<uint32_t> g_svr_render_passes;  // (plume_vulkan.cpp: render passes begun)
extern std::atomic<uint64_t> g_svr_render_pass_pixels;
}  // namespace plume

namespace xmd::native {

std::atomic<bool> g_up_waiting{false};

using plume::RenderFormat;
using plume::RenderTextureLayout;
namespace RenderBarrierStage = plume::RenderBarrierStage;

namespace {

// ---------------------------------------------------------------------------
// constants and small helpers

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kFrames = 3;                   // frames in flight
// Frames are drawn into these and handed to the emulator's presentation;
// enough that one is never redrawn while the emulator's queue may still
// read it.
constexpr uint32_t kOutputs = 6;
constexpr uint64_t kRingSize = 128ull << 20;      // per-frame upload ring
// Mapped past the ring's end: shaders indexing constants may read up to a
// constant buffer view's 64 KB past an allocation near the end.
constexpr uint64_t kRingSlack = 64ull << 10;
constexpr uint32_t kSrvHeapSize = 16384;   // per texture table: 0-2 placeholders, then textures
constexpr uint32_t kSamplerHeapSize = 2000;  // 0 default linear wrap, then per fetch state
constexpr uint32_t kMaxColorTargets = 256;
constexpr uint32_t kMaxDepthTargets = 64;
constexpr uint32_t kGpuTimeoutMs = 3000;
constexpr uint32_t kGpuPatienceMs = 20000;  // (WaitForFrame: a late GPU that is not lost)
constexpr uint32_t kMaxEffectSide = 4160;   // (TargetScale: host pixels a side of targets other than the scene's)

// Shader constant sizes (XenosRecomp contract).
constexpr uint32_t kVertexConstantsBytes = 256 * 16;
constexpr uint32_t kPixelConstantsBytes = 256 * 16;  // all 256 (the skin shaders use c224+)
constexpr uint32_t kSharedConstantsBytes = 34 * 16;

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline float BeFloat(const uint8_t* p) {
  const uint32_t u = Be32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Byte-swapped copies of 32-bit / 16-bit words, 16 bytes at a time (NEON's
// vrev on ARM - SIMDe's emulated SSSE3 shuffle cost ~3% of the phone's render
// thread - and SSSE3 on x86). Whole 16-byte stores also suit the upload
// ring, which on phones is uncached memory.
void Swap32(uint8_t* dst, const uint8_t* src, size_t bytes) {
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= bytes; i += 16) vst1q_u8(dst + i, vrev32q_u8(vld1q_u8(src + i)));
#else
  const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  for (; i + 16 <= bytes; i += 16) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_shuffle_epi8(v, swap));
  }
#endif
  for (; i + 4 <= bytes; i += 4) {
    uint32_t w;
    std::memcpy(&w, src + i, 4);
    w = _byteswap_ulong(w);
    std::memcpy(dst + i, &w, 4);
  }
}

void Swap16(uint8_t* dst, const uint8_t* src, size_t bytes) {
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= bytes; i += 16) vst1q_u8(dst + i, vrev16q_u8(vld1q_u8(src + i)));
#else
  const __m128i swap = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
  for (; i + 16 <= bytes; i += 16) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_shuffle_epi8(v, swap));
  }
#endif
  for (; i + 2 <= bytes; i += 2) {
    dst[i] = src[i + 1];
    dst[i + 1] = src[i];
  }
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// ---------------------------------------------------------------------------
// guest memory

rex::memory::Memory* g_memory = nullptr;
// TranslateVirtual without its out-of-line heap lookup (~3% of a phone's
// render thread in heavy entrances): the host offset of each 1 MB of the
// guest address space (the heaps' bounds are 1 MB multiples), set at Attach.
uint8_t* g_virtual_base = nullptr;
uint32_t g_virtual_offset[4096];

inline const uint8_t* Virtual(uint32_t address) {
  return g_virtual_base + address + g_virtual_offset[address >> 20];
}
const uint8_t* Physical(uint32_t address) { return g_memory->TranslatePhysical(address); }
// Addresses stored in the game's D3D objects (vertex/index buffers, textures)
// are guest *virtual* (0xA0000000+ / 0xE0000000+ physical-memory views); the
// 0xE... view is offset by a page on the host, so they must be translated as
// virtual. The fetch constants in the device's register mirror hold true
// physical addresses instead (Physical()).
const uint8_t* ObjectData(uint32_t address) { return Virtual(address); }
// Whether guest memory [address, address + size) can be read (the emulator's page tables: cheap - asking
// Windows about the host memory at every draw cost over a second a frame). A shader or buffer object's data
// address isn't always good (the game sets objects it is still building, or has freed): reading on from one
// ended the game while a level loaded.
bool GuestReadable(uint32_t address, uint32_t size) {
  if (!size) return true;
  if (!g_memory || address + size - 1 < address) return false;
  auto* heap = g_memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

// A view of physical memory as the physical address (the 0xE... view starts a page in).
uint32_t PhysicalOf(uint32_t view) { return view >= 0xE0000000u ? (view & 0x1FFFFFFFu) + 0x1000 : view & 0x1FFFFFFFu; }
// A physical address as one that can be read through (the view its memory was allocated in; 0: none).
uint32_t PhysicalView(uint32_t physical, uint32_t size) {
  physical &= 0x1FFFFFFFu;
  for (uint32_t view : {0xA0000000u + physical, 0xC0000000u + physical, 0xE0000000u + physical - 0x1000})
    if (GuestReadable(view, size)) return view;
  return 0;
}

// Test aid XMD_NATIVE_DRAW_TIMES=1: where a draw's time goes, on a "native draw times" line every 5 s
// (ms a frame: finding the shaders, the vertex layout, the pipeline, vertex data, indices, state, constants and
// textures, the rest).
const bool g_draw_times = std::getenv("XMD_NATIVE_DRAW_TIMES") != nullptr;
double g_draw_time[8] = {};
struct DrawClock {
  std::chrono::steady_clock::time_point last =
      g_draw_times ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  void Mark(int part) {
    if (!g_draw_times) return;
    const auto now = std::chrono::steady_clock::now();
    g_draw_time[part] += std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
  }
};

// The game's D3D device: r3 of every device call (the hooks pass it on - its
// address differs between builds of the port).
std::atomic<uint32_t> g_device_address{0};
// The vertex shader microcode the D3D library last gave the GPU (OnShaderBound).
uint32_t g_bound_vertex_container = 0, g_bound_vertex_microcode = 0;
// While a command buffer is played: the vertex shader code its IM_LOAD names (a view of physical memory; 0: none).
// The vertex fetches - the vertex format - are read from exactly that: the game keeps copies of a shader's code
// patched for different formats (a mesh whose lightmap coordinates are 16-bit integers instead of half floats),
// and the copy a buffer loads is not always the shader object's own.
uint32_t g_replay_vertex_code = 0;
// While a recorded command buffer is played (RunCommands): a copy of the device with the buffer's register
// writes made in it, its shaders as the current ones - what the draw code reads the state from.
const uint8_t* g_replay_device = nullptr;
// ... its vertex streams, as buffer objects (the two words of a vertex fetch constant where an object has
// them), and the draw's indices.
constexpr uint32_t kReplayStream = 0xFFFFFF00u;
uint8_t g_replay_streams[16][0x20];
uint32_t g_replay_index_address = 0, g_replay_index_size = 0;
// ... and while a queued call is made on the render thread (render thread, below): the device as it was at the call.
const uint8_t* g_job_device = nullptr;
const uint8_t* LiveDevice() { return g_job_device ? g_job_device : Virtual(g_device_address.load(std::memory_order_relaxed)); }
const uint8_t* Device() { return g_replay_device ? g_replay_device : LiveDevice(); }
// What Clear and Resolve read of their call: the registers, and what their pointers and the stack held then.
struct HookArgs {
  uint32_t r4 = 0, r8 = 0, stencil = 0;
  float z = 0;
  bool has_rect = false, has_colour = false;
  uint8_t rect[16] = {}, colour[16] = {};
};
// A stream's vertex buffer object.
const uint8_t* BufferObject(uint32_t buffer) {
  return (buffer & 0xFFFFFFF0u) == kReplayStream ? g_replay_streams[buffer & 15] : Virtual(buffer);
}
uint32_t Reg(uint32_t reg) { return Be32(Device() + guest::RegisterOffset(reg)); }
float RegFloat(uint32_t reg) { return BeFloat(Device() + guest::RegisterOffset(reg)); }

// Xenos register numbers used here.
enum : uint32_t {
  RB_SURFACE_INFO = 0x2000,
  PA_SC_WINDOW_SCISSOR_TL = 0x2011,  // (D3D's mirror slots; see Draw)
  PA_SC_WINDOW_SCISSOR_BR = 0x2012,
  RB_COLOR_INFO = 0x2001,
  RB_DEPTH_INFO = 0x2002,
  RB_COLOR_MASK = 0x2104,
  RB_BLEND_RED = 0x2105,
  RB_STENCILREFMASK = 0x210D,
  RB_ALPHA_REF = 0x210E,
  PA_CL_VPORT_XSCALE = 0x210F,
  PA_CL_VPORT_XOFFSET = 0x2110,
  PA_CL_VPORT_YSCALE = 0x2111,
  PA_CL_VPORT_YOFFSET = 0x2112,
  PA_CL_VPORT_ZSCALE = 0x2113,
  PA_CL_VPORT_ZOFFSET = 0x2114,
  RB_DEPTHCONTROL = 0x2200,
  RB_BLENDCONTROL0 = 0x2201,
  RB_COLORCONTROL = 0x2202,
  PA_SU_SC_MODE_CNTL = 0x2205,
  PA_CL_VTE_CNTL = 0x2206,
  PA_SU_VTX_CNTL = 0x2302,
  PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380,  // then FRONT_OFFSET, BACK_SCALE, BACK_OFFSET
  RB_DEPTH_CLEAR = 0x231D,
  RB_COLOR_CLEAR = 0x231E,
};

// ---------------------------------------------------------------------------
// shaders

struct ShaderInput {
  std::string semantic;
  uint32_t index;
  bool is_uint;
  int location;  // Vulkan input location (-1: in declaration order)
  // The register its fetch writes in the shader as converted and which components (bits 0-3 = x-w); -1: not
  // known. See Declaration().
  int reg = -1;
  uint32_t mask = 0;
  // The fetch's component order in the shader as converted (3 bits a component, 0x688: as they come). The game's
  // graphics library rewrites it in the bound code for the vertex format (a D3DCOLOR element: ZYXW).
  uint32_t swizzle = 0x688;
};

struct Shader {
  uint64_t hash = 0;
  bool pixel = false;
  bool loaded = false;
  bool missing = false;
  // [0] base, [1] packed 11:11:10 normals (vs), [2] alpha test (ps), [3] normals as signed bytes (vs),
  // [4] packed 10:11:11 normals (vs)
  std::vector<uint8_t> code[5];
  std::unique_ptr<plume::RenderShader> compiled[5];
  std::vector<ShaderInput> inputs;
  std::vector<std::pair<uint32_t, uint32_t>> textures;  // (fetch slot, 0 2D / 1 3D / 2 cube)
  // Vertex elements the shader fetches by a computed index (a skinned model's bone matrix rows, kept in a
  // vertex stream): not inputs - the renderer passes the stream's rows as constants (IndexedRows), these
  // elements of each stream entry in this order.
  std::vector<std::pair<std::string, uint32_t>> indexed;
  // How many float4 constants of its stage it declares (the highest register + 1; "<name>.consts" of
  // tools/convert_shaders.py): a draw uploads that many, not all 256 (GpuConstants).
  uint32_t constants = 256;
};

// The game builds most of its shader objects in place from its archives (few
// go through CreateVertexShader / CreatePixelShader), thousands of them for a
// few hundred shaders, at addresses it uses again for others. So a shader is
// known by its contents, read from the object when a draw uses it:
//   pixel   the object's copy of its container (from +0x28) and its microcode
//           (physical address at +0x18) - together the container as on the disc;
//   vertex  the container copy's virtual part (from +0x368; flag 0x40, set on a
//           shader bound to a vertex format, cleared) - binding rewrites the
//           microcode's vertex fetches, the virtual part stays the disc's.
// The converted shaders (tools/convert_shaders.py) are named by the same hashes.
struct ShaderObject {
  uint64_t signature = 0;  // of the object's fields the hash was read through
  Shader* shader = nullptr;
  uint64_t frame = 0;      // (g_shader_frame when the signature was last compared)
  uint32_t code = 0;       // (the microcode address then)
};
// The frame being drawn (counted at each present): an object's contents are compared once in a frame, not at
// every draw that uses it (two hashes of a few KB a draw - 2 ms a frame of the first level's 4900 draws).
// XMD_NATIVE_SHADER_CHECK_ALWAYS=1: at every draw.
uint64_t g_shader_frame = 1;
std::mutex g_shader_mutex;
std::unordered_map<uint64_t, Shader> g_shaders;  // hash (and stage) -> shader
std::unordered_map<uint32_t, ShaderObject> g_shader_objects;  // guest shader object -> shader

std::filesystem::path ShaderDirectory() {
  char* v = nullptr;
  size_t n = 0;
  std::filesystem::path dir;
  if (_dupenv_s(&v, &n, "XMD_NATIVE_SHADERS") == 0 && v) {
    dir = v;
    free(v);
  } else {
    dir = rex::filesystem::GetExecutableFolder() / "native_shaders";
  }
  return dir;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// A converted shader: "<name><backend extension>". Installed, the DXIL and
// SPIR-V files share native_shaders\; in the converter's output (a dev
// XMD_NATIVE_SHADERS pointing at .../dxil) SPIR-V is in ../spirv.
std::filesystem::path ShaderFile(const std::string& name) {
  static const std::filesystem::path dir = ShaderDirectory();
  const std::string file = name + backend::ShaderExtension();
  std::error_code ec;
  if (!std::filesystem::exists(dir / file, ec) && dir.filename() == "dxil")
    return dir.parent_path() / "spirv" / file;
  return dir / file;
}

// The shaders' folder packed in one file (tools/pack_shaders.py):
// native_shaders\shaders<extension>.pak - read once at start instead of a
// thousand small files (on a hard disk each one is a seek: a new scene's
// shaders froze it for hundreds of ms). "SVRPAK1\0", u32 count, then per
// file u16 name length, name, u32 offset, u32 size; then the files.
// Without one (a dev shader folder), the loose files.
struct ShaderPack {
  std::vector<uint8_t> data;
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> files;  // name -> offset, size
};

const ShaderPack& Pack() {
  static const ShaderPack pack = [] {
    ShaderPack p;
    const std::filesystem::path path =
        ShaderDirectory() / (std::string("shaders") + backend::ShaderExtension() + ".pak");
    p.data = ReadFile(path);
    if (p.data.empty()) return p;
    auto u16 = [&](size_t at) { return uint32_t(p.data[at]) | uint32_t(p.data[at + 1]) << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    bool ok = p.data.size() >= 12 && std::memcmp(p.data.data(), "SVRPAK1", 8) == 0;
    size_t at = 12;
    for (uint32_t i = 0, n = ok ? u32(8) : 0; ok && i < n; ++i) {
      if (at + 2 > p.data.size()) { ok = false; break; }
      const uint32_t len = u16(at);
      if (at + 2 + len + 8 > p.data.size()) { ok = false; break; }
      std::string name(reinterpret_cast<const char*>(&p.data[at + 2]), len);
      const uint32_t offset = u32(at + 2 + len), size = u32(at + 6 + len);
      if (uint64_t(offset) + size > p.data.size()) { ok = false; break; }
      p.files.emplace(std::move(name), std::make_pair(offset, size));
      at += 2 + len + 8;
    }
    if (!ok) {
      REXLOG_WARN("native renderer: {} is damaged - using the loose shader files", path.string());
      p.files.clear();
      p.data.clear();
    } else {
      REXLOG_INFO("native renderer: shader pack {} ({} files, {} KB)", path.filename().string(), p.files.size(),
                  p.data.size() / 1024);
    }
    return p;
  }();
  return pack;
}

// A file of the shaders' folder by name ("<hash>.vs.inputs").
std::vector<uint8_t> ShaderFolderFile(const std::string& file) {
  const ShaderPack& pack = Pack();
  if (auto it = pack.files.find(file); it != pack.files.end()) {
    const uint8_t* b = pack.data.data() + it->second.first;
    return std::vector<uint8_t>(b, b + it->second.second);
  }
  static const std::filesystem::path dir = ShaderDirectory();
  return ReadFile(dir / file);
}

// A converted shader ("<name>" + the backend's extension).
std::vector<uint8_t> ReadShader(const std::string& name) {
  const ShaderPack& pack = Pack();
  if (auto it = pack.files.find(name + backend::ShaderExtension()); it != pack.files.end()) {
    const uint8_t* b = pack.data.data() + it->second.first;
    return std::vector<uint8_t>(b, b + it->second.second);
  }
  return ReadFile(ShaderFile(name));
}

bool ShaderExists(const std::string& name) {
  if (Pack().files.count(name + backend::ShaderExtension())) return true;
  std::error_code ec;
  return std::filesystem::exists(ShaderFile(name), ec);
}

void LoadShader(Shader& s) {
  if (s.loaded) return;
  s.loaded = true;
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s", static_cast<unsigned long long>(s.hash),
                s.pixel ? "ps" : "vs");
  const std::string ext = backend::ShaderExtension();
  s.code[0] = ReadShader(name);
  s.code[s.pixel ? 2 : 1] = ReadShader(std::string(name) + (s.pixel ? ".s2" : ".s1"));
  if (!s.pixel) s.code[3] = ReadShader(std::string(name) + ".s32");
  if (!s.pixel) s.code[4] = ReadShader(std::string(name) + ".s64");
  auto text = [](const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); };
  if (!s.pixel) {
    // SEMANTIC INDEX TYPE [LOCATION]
    std::istringstream in(text(ShaderFolderFile(std::string(name) + ".inputs")));
    std::string line, type;
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      ShaderInput i;
      if (!(fields >> i.semantic >> i.index >> type)) continue;
      i.is_uint = type == "uint4";
      if (!(fields >> i.location)) i.location = -1;
      if (!(fields >> i.reg >> i.mask)) i.reg = -1, i.mask = 0;
      if (!(fields >> i.swizzle)) i.swizzle = 0x688;
      s.inputs.push_back(i);
    }
    std::istringstream rows(text(ShaderFolderFile(std::string(name) + ".indexed")));
    std::string semantic;
    uint32_t index;
    while (rows >> semantic >> index) s.indexed.emplace_back(semantic, index);
  }
  {
    std::istringstream in(text(ShaderFolderFile(std::string(name) + ".textures")));
    uint32_t slot, dimension;
    while (in >> slot >> dimension) {
      if (slot < 32 && dimension < 3) s.textures.emplace_back(slot, dimension);
    }
  }
  {
    std::istringstream in(text(ShaderFolderFile(std::string(name) + ".consts")));
    uint32_t count;
    if (in >> count && count <= 256) s.constants = count;
  }
  s.missing = s.code[0].empty();
  if (s.missing) REXLOG_WARN("native renderer: no converted shader {}{}", name, ext);
}

Shader* FindShader(uint32_t object, bool pixel) {
  if (object < 0x10000) return nullptr;
  const uint8_t* o = Virtual(object);
  if ((Be32(o) & 0xF) != (pixel ? 7u : 6u)) return nullptr;
  const uint8_t* c = o + (pixel ? guest::kPixelShaderContainer : guest::kVertexShaderContainer);
  const uint32_t code = Be32(o + (pixel ? guest::kPixelShaderMicrocode : guest::kVertexShaderMicrocode));
  // (this frame's answers by the object's address, without the lock or the map: only draws ask - one thread)
  struct Front {
    uint32_t object = 0, code = 0;
    uint64_t frame = 0;
    Shader* shader = nullptr;
  };
  static Front fronts[2][1024];
  static const bool always = std::getenv("XMD_NATIVE_SHADER_CHECK_ALWAYS") != nullptr;
  Front& front = fronts[pixel ? 1 : 0][(object >> 3) & 1023];
  if (!always && front.object == object && front.code == code && front.frame == g_shader_frame) return front.shader;
  const uint32_t flags = Be32(c), vsize = Be32(c + 4), psize = Be32(c + 8);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || vsize < 0x24 || vsize > 0x10000 || psize > 0x10000) return nullptr;
  // (what would differ had the address been given to another shader)
  // (the whole container copy: shaders that differ only in their vertex elements have the same sizes and the
  // same constant table, and the game's allocator hands the same addresses out again)
  const uint64_t signature = XXH3_64bits(c, vsize) ^ (uint64_t(code) * 0x9E3779B97F4A7C15ull) ^ psize;
  std::lock_guard lock(g_shader_mutex);
  ShaderObject& known = g_shader_objects[object];
  if (!known.shader || known.signature != signature) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < vsize; ++i) h = (h ^ (i == 3 ? uint8_t(c[i] & 0xBF) : c[i])) * 1099511628211ull;
    if (pixel) {
      const uint8_t* micro = Virtual(code);  // (an address in one of the physical memory views)
      if (!GuestReadable(code, psize)) {
        known.shader = nullptr;
        return nullptr;
      }
      for (uint32_t i = 0; i < psize; ++i) h = (h ^ micro[i]) * 1099511628211ull;
    }
    auto [it, added] = g_shaders.try_emplace(h * 2 + (pixel ? 1 : 0));
    if (added) {
      it->second.hash = h;
      it->second.pixel = pixel;
    }
    known.signature = signature;
    known.shader = &it->second;
  }
  known.frame = g_shader_frame;
  known.code = code;
  LoadShader(*known.shader);
  if (!known.shader->missing) front = {object, code, g_shader_frame, known.shader};
  return known.shader->missing ? nullptr : known.shader;
}

// ---------------------------------------------------------------------------
// renderer state

using plume::RenderBufferReference;

struct UploadRing {
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint8_t* cpu = nullptr;
  uint64_t offset = 0;
};

struct CachedBuffer {
  std::shared_ptr<plume::RenderBuffer> resource;
  uint64_t hash = 0;
  uint64_t checked_frame = ~0ull;
  uint32_t size = 0;
  uint32_t changes = 0;  // times the contents changed
  uint64_t seen_frame = 0;  // (last asked for: buffers not used for a while are let go)
  bool dynamic = false;  // changes often: converted into the ring when changed
  // The dynamic buffer's latest conversion in this frame's ring.
  uint64_t ring_frame = ~0ull, ring_hash = 0;
  bool ring_hashed = false;  // (ring_hash known: only when used again, below)
  RenderBufferReference ring_ref;
  // A played command buffer's small buffer: its converted copy's place in the shared pool (BufferPool) while
  // pool_generation is the pool's.
  RenderBufferReference pool_ref;
  uint32_t pool_generation = 0;
  // (a played command buffer's draw used it: a ReplaySite may hold its buffer - replacing or dropping it counts
  // g_site_epoch. The game's own short-lived buffers change all the time and must not.)
  bool sited = false;
  // Its uses in this frame and the last: a buffer drawn once a frame is just
  // converted (hashing it to spot a repeat would be wasted).
  uint64_t use_frame = ~0ull;
  uint32_t uses = 0, last_uses = 0;
  // Buffers drawn many times a frame (Batista's pyro: dozens of 3 KB
  // particle buffers, ~90 draws each) that the game only rewrites between
  // frames are checked once a frame - and at every use every 8th frame; a
  // change seen within a frame puts them back to checks at every use.
  bool changed_in_frame = false;
  bool per_frame = false;
  uint16_t stable_frames = 0;  // frames in a row without a change within them
  uint8_t demotions = 0;       // (twice: always checked at every use)
};

// The converted copies of the small vertex and index buffers that played command buffers draw from - a level's
// static meshes, thousands of them of a few KB each: kept side by side in a few big buffers. (Each in a buffer
// of its own is 64 KB of memory a piece; converted into the frame's upload ring at every draw - as the game's
// own short-lived buffers are - they were 3 ms a frame in the first level.) Space is only ever taken: what
// changed or went unused is counted in `dead`, and past a limit the pool starts again (everything is converted
// anew at its next use). XMD_NATIVE_NO_BUFFER_POOL=1: as before.
struct BufferPool {
  struct Chunk {
    std::shared_ptr<plume::RenderBuffer> buffer;
    uint8_t* cpu = nullptr;
    uint64_t used = 0;
  };
  std::vector<Chunk> chunks;
  uint64_t dead = 0;
  uint32_t generation = 1;
};
constexpr uint64_t kPoolChunk = 8ull << 20;
constexpr uint32_t kPoolLargest = 64u << 10;

struct Stream {
  uint32_t buffer = 0;  // guest vertex buffer object
  uint32_t offset = 0;
  uint32_t stride = 0;
};

struct Stats {
  uint32_t draws = 0, drawn = 0, skipped_target = 0, skipped_shader = 0, skipped_prim = 0;
  uint32_t targets = 0, resolves = 0;
  uint32_t wide_3d = 0;  // depth-tested draws into wide targets (none: a 2D screen)
};

// A render target: the host image of one EDRAM colour surface
// (base tile, pitch, format, height) - EDRAM itself isn't emulated.
struct DepthTarget;

struct ColorTarget {
  std::shared_ptr<plume::RenderTexture> resource;  // typeless family of `format`
  std::unique_ptr<plume::RenderTextureView> rtv;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // With each depth target (or none) it was drawn with.
  std::unordered_map<const DepthTarget*, std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
  RenderFormat resource_format = RenderFormat::UNKNOWN;
  RenderFormat format = RenderFormat::UNKNOWN;  // the render target view format
  RenderFormat view_format = RenderFormat::UNKNOWN, view_gamma_format = RenderFormat::UNKNOWN;
  uint32_t components = 4;
  uint32_t width = 0, height = 0;
  uint32_t scale = 1;  // its host pixels per guest pixel (TargetScale)
  uint32_t host_w = 0;  // its host width (wider than width * scale on wide screens: HostWidth)
  uint32_t host_h = 0;  // and height (taller on screens narrower than 16:9: HostHeight)
  uint64_t used_frame = 0;
};

struct DepthTarget {
  std::shared_ptr<plume::RenderTexture> resource;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t scale = 1;
  uint32_t host_w = 0, host_h = 0;
};

// The copy a resolve made of a target (what the game samples as a texture).
struct ResolvedTexture {
  std::shared_ptr<plume::RenderTexture> resource;
  uint64_t last_frame = ~0ull;  // the frame it was last resolved in (WriteBackResolve)
  uint32_t streak = 0;          // consecutive frames it was resolved in
  uint32_t host_w = 0, host_h = 0;  // (larger than the guest size * scale when copied from a wide / tall target)
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t scale = 1;  // (the resolved target's)
  bool swap_rb = false;  // (red and blue swapped in the copy: its texture view swaps them back)
  RenderFormat view_format = RenderFormat::UNKNOWN;  // (colour: the format it is read as - `format` is typeless)
};

// A DrawVerticesUP waiting for its vertices (the game writes them into the
// space the call returns, then calls EndVertices).
struct PendingUP {
  bool active = false;
  uint32_t primitive = 0, count = 0, stride = 0, data = 0;
  // (BeginIndexedVertices: the indices' space and how many - 16-bit; 0: not indexed)
  uint32_t index_data = 0, index_count = 0;
  int32_t base_vertex = 0;
};

struct Renderer {
  std::unique_ptr<plume::RenderInterface> api;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandList> lists[kFrames];
  std::unique_ptr<plume::RenderCommandFence> fences[kFrames];
  bool submitted[kFrames] = {};  // the slot's fence has a submission to wait for
  plume::RenderCommandList* list = nullptr;  // this frame's
  std::unique_ptr<plume::RenderPipelineLayout> layout;
  // The shaders' texture tables (2D, 3D, cube) and samplers. Vulkan: the
  // three tables are one set (texture_sets[0], bindings 0-2 - with the
  // samplers and the constants three sets, within the four every GPU has);
  // texture_set(i) / texture_base[i] address a table either way.
  std::unique_ptr<plume::RenderDescriptorSet> texture_sets[3], sampler_set;
  plume::RenderDescriptorSet* texture_set[3] = {};
  uint32_t texture_base[3] = {};
  // Vulkan: each upload ring bound as the shaders' constant buffer (set 2):
  // the push constants hold the offsets of the draw's constants in it - no
  // buffer addresses or 64-bit integers in the shaders (shader_common.h).
  std::unique_ptr<plume::RenderDescriptorSet> constant_sets[kFrames];
  // Compact tables (backend::CompactTables(): Vulkan GPUs without descriptor
  // indexing): texture_sets[0] and sampler_set only record what each index
  // holds (TableRecord), and each draw binds two small sets of its own
  // textures and samplers (draw_sets), made once per contents and kept while
  // used (CompactSet).
  struct CompactSet {
    std::unique_ptr<plume::RenderDescriptorSet> set;
    uint64_t used = 0;  // (frame)
  };
  // (keyed by the places' stamps - no allocation or string hash per draw;
  // 16 places, or 64 with slot tables)
  struct CompactKey {
    std::array<uint32_t, 64> stamps;
    bool operator==(const CompactKey& o) const { return stamps == o.stamps; }
  };
  struct CompactKeyHash {
    size_t operator()(const CompactKey& k) const {
      uint64_t h = 1469598103934665603ull;
      for (uint32_t v : k.stamps) h = (h ^ v) * 1099511628211ull;
      return size_t(h ^ (h >> 32));
    }
  };
  std::unordered_map<CompactKey, CompactSet, CompactKeyHash> compact_sets[2];  // textures, samplers
  // The last draw's places and set (CompactSet's fast path).
  struct LastCompact {
    uint32_t indices[64] = {};
    uint32_t table_stamps = 0;
    CompactSet* entry = nullptr;
  };
  LastCompact last_compact[2];
  plume::RenderDescriptorSet* draw_sets[2] = {};
  plume::RenderDescriptorSet* default_sets[2] = {};  // (placeholders only)
  uint64_t compact_swept = 0;
  std::unique_ptr<plume::RenderSampler> default_sampler;
  std::shared_ptr<plume::RenderTexture> outputs[kOutputs];
  std::unique_ptr<plume::RenderFramebuffer> output_framebuffers[kOutputs];
  // FSR 1's intermediate (the frame's size: EASU's result, RCAS's source), in the 2D table at kFsrSrv.
  std::shared_ptr<plume::RenderTexture> fsr_image;
  std::unique_ptr<plume::RenderTextureView> fsr_view;
  std::unique_ptr<plume::RenderFramebuffer> fsr_framebuffer;
  bool fsr_image_fresh = true;  // (not drawn into since it was made)
#if XMD_FSR2
  // FSR 2 (experimental): its backend (made at the first use, again when the sizes change), the scene depth copied
  // at the scene's resolve (fsr2_depth_frame: the frame it was), the motion vectors (zero: the camera's come with
  // step 4).
  std::unique_ptr<xmd::fsr2::Upscaler> fsr2;
  bool fsr2_failed = false;
  std::shared_ptr<plume::RenderTexture> fsr2_depth, fsr2_motion;
  std::shared_ptr<plume::RenderTexture> fsr2_depth32;  // (the depth as R32_FLOAT, from the motion pass: DLSS's)
#if XMD_DLSS
  // DLSS (native_upscale dlss, experimental): FSR 2's inputs into NVIDIA's upscaler instead of FSR 2's.
  std::unique_ptr<xmd::dlss::Upscaler> dlss;
  bool dlss_failed = false;
#endif
  RenderTextureLayout fsr2_depth_layout = RenderTextureLayout::UNKNOWN, fsr2_motion_layout = RenderTextureLayout::UNKNOWN;
  uint32_t fsr2_depth_w = 0, fsr2_depth_h = 0;
  uint64_t fsr2_depth_frame = ~0ull, fsr2_last_frame = ~0ull;
  // (the scene camera's view x projection at the scene's resolve: this frame's [0], the one before [1])
  float fsr2_vp[2][16] = {};
  uint64_t fsr2_vp_frame[2] = {~0ull, ~0ull};
  // (test aid XMD_TEST_FSR2_DUMP: FSR 2's inputs read back this frame, written once it is done)
  struct Fsr2Dump {
    std::shared_ptr<plume::RenderBuffer> buffer;
    std::string file;
    uint32_t w = 0, h = 0, row = 0, bytes = 4;  // (row: pixels, `bytes` each)
  };
  std::vector<Fsr2Dump> fsr2_dumps;
  // FSR 2's masks: the scene draws that move without motion vectors or blend over others (the wrestlers, the
  // blended crowd / particles, the HUD) noted as drawn and drawn again at the scene's resolve, depth tested, into
  // the reactive (R8: trust the history less) and transparency / composition (R8) targets - fsr2_mask.ps.
  struct Fsr2MaskDraw {
    const plume::RenderPipeline* pso = nullptr;
    RenderBufferReference constants[4];
    plume::RenderVertexBufferView views[16] = {};
    plume::RenderInputSlot slots[16] = {};
    uint32_t view_count = 0;
    plume::RenderIndexBufferView ibv = {};
    bool indexed = false;
    uint32_t count = 0, start = 0;
    int32_t base = 0;
    plume::RenderViewport viewport;
    plume::RenderRect scissor;
    uint32_t stencil_ref = 0;
    bool hud = false;  // (no depth test: kept only after the frame's last 3D draw - not a background)
    // (a skinned draw: its own motion too - fsr2_skin: its key across frames, its bones a vertex, its NDC scale /
    // offset as drawn; 0: none)
    uint64_t skin_key = 0;
    uint32_t skin_bones = 0;
    float skin_ndc[4] = {};
    const plume::RenderPipeline* skin_pso = nullptr;
  };
  std::vector<Fsr2MaskDraw> fsr2_mask_draws;
  size_t fsr2_mask_after_3d = 0;  // (the noted draws before the frame's last 3D scene draw)
  uint64_t fsr2_mask_draws_frame = ~0ull, fsr2_mask_frame = ~0ull;  // (noted in / drawn in)
  std::shared_ptr<plume::RenderTexture> fsr2_reactive, fsr2_composition;
  RenderTextureLayout fsr2_reactive_layout = RenderTextureLayout::UNKNOWN,
                      fsr2_composition_layout = RenderTextureLayout::UNKNOWN;
  uint32_t fsr2_mask_w = 0, fsr2_mask_h = 0;
  struct Fsr2MaskFramebuffer {
    std::shared_ptr<plume::RenderTexture> depth;  // (kept: the key)
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  };
  std::vector<Fsr2MaskFramebuffer> fsr2_mask_framebuffers;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> fsr2_mask_pipelines;
  std::vector<uint8_t> fsr2_mask_code;
  std::unique_ptr<plume::RenderShader> fsr2_mask_ps;
  // FSR 2 / DLSS: the skinned draws' own motion (fsr2_skin.vs / .ps): each one's view x projection and bones
  // (c54-c57, c74-c137) this frame and the last, by draw key.
  struct SkinRows {
    float rows[(4 + 64) * 4];
  };
  std::unordered_map<uint64_t, SkinRows> fsr2_skin_now, fsr2_skin_prev;
  uint64_t fsr2_skin_frame = ~0ull;
  std::vector<uint8_t> fsr2_skin_vs_code, fsr2_skin_ps_code;
  std::unique_ptr<plume::RenderShader> fsr2_skin_vs, fsr2_skin_ps;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> fsr2_skin_pipelines;
  std::vector<Fsr2MaskFramebuffer> fsr2_skin_framebuffers;
  bool fsr2_skin_off = false;
  RenderTextureLayout fsr_image_layout = RenderTextureLayout::UNKNOWN;
#endif
  uint32_t output_index = 0;
  std::unordered_map<uint64_t, ColorTarget> color_targets;
  std::unordered_map<uint64_t, DepthTarget> depth_targets;
  std::unordered_map<uint32_t, ResolvedTexture> resolved;  // destination base address ->
  // Resolve textures set aside when another shape was resolved to their address, by shape, to be used again:
  // X-Men: Destiny resolves targets of several sizes to the same memory in turn within every frame (its
  // post-processing chain), and making a new texture each time cost tens of ms a frame and ended in a
  // failed allocation.
  std::unordered_map<uint64_t, std::vector<ResolvedTexture>> resolved_spares;
  // Rectangles of depth resolves as float textures (CropDepth), by destination address and size.
  struct DepthCrop {
    std::shared_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  };
  std::unordered_map<uint64_t, DepthCrop> depth_crops;
  uint32_t present_source = 0;  // the last full-screen colour resolve (the front buffer)
  ColorTarget* main_target = nullptr;  // the 1280-pitch target at tile 0
  PendingUP pending_up;
  UploadRing rings[kFrames];
  std::shared_ptr<plume::RenderTexture> null_2d, null_3d, null_cube;  // (0, 0, 0, 0) placeholders
  std::vector<std::shared_ptr<void>> null_views;
  std::unique_ptr<plume::RenderBuffer> zero_buffer;  // for vertex inputs the declaration lacks

  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
  std::unordered_map<uint64_t, CachedBuffer> vertex_buffers;  // (address << 32 | size)
  std::unordered_map<uint64_t, CachedBuffer> index_buffers;   // (address << 32 | size)
  BufferPool pool;
  std::deque<std::pair<uint64_t, std::shared_ptr<void>>> garbage;  // (frame, object)

  bool frame_open = false;
  bool hidden_frame = false;  // (30 fps at 60 Hz: SetHalfFrames - this frame isn't shown, its draws are skipped)
  uint32_t back_index = 0;  // frame slot (frames % kFrames)
  uint64_t frames = 0;

  std::chrono::steady_clock::time_point stat_start = std::chrono::steady_clock::now();
  uint32_t stat_frames = 0;
  Stats frame_stats, window_stats;
  // Performance log (every 5 s): time in the renderer's draw translation,
  // waiting for the GPU (frames in flight), and frame span.
  double perf_draw_ms = 0, perf_wait_ms = 0, perf_span_ms = 0, perf_submit_ms = 0;
  uint64_t perf_draws = 0, perf_drawn = 0;
  uint32_t perf_frames = 0;
  std::chrono::steady_clock::time_point perf_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point frame_begin;
  // The command list's state as Draw last set it, so unchanged state is not
  // recorded again (reset at every frame and by the other code that sets it).
  struct ListState {
    bool bound = false;  // pipeline layout and descriptor sets
    const plume::RenderPipeline* pso = nullptr;
    const plume::RenderFramebuffer* framebuffer = nullptr;
    uint32_t stencil_ref = UINT32_MAX;
    float blend[4] = {-1, -1, -1, -1};
    plume::RenderViewport viewport = {-1, -1, -1, -1, -1, -1};
    plume::RenderRect scissor = {-1, -1, -1, -1};
    plume::RenderIndexBufferView ibv = {};
    // The vertex buffers bound (consecutive draws of a mesh share them).
    plume::RenderVertexBufferView views[16] = {};
    plume::RenderInputSlot slots[16] = {};
    uint32_t view_count = 0;
    const plume::RenderDescriptorSet* compact[2] = {};  // (compact tables: the bound sets 0, 1)
  } list_state;
  // native_record_thread: stands in for the frame's list (r->list), replaying
  // onto it on its own thread.
  std::unique_ptr<RecordingList> recorder;
  textures::Context texture_context;  // this frame's (BeginFrame)
  // The last upload of each constant range (vertex, pixel): reused while the
  // mirror's range is unchanged (consecutive draws of a model share them).
  struct ConstantUpload {
    uint64_t hash = 0;
    RenderBufferReference ref;
  } constant_uploads[2];
  // (the frame's last shared constants, and its one buffer for draws without constants of their own)
  RenderBufferReference shared_upload, own_upload;
  uint64_t perf_const_uploads = 0, perf_const_reused = 0;
};

// Keeps `object` alive until the GPU has finished the frame being recorded.
void Retire(Renderer* r, std::shared_ptr<void> object) {
  if (object) r->garbage.emplace_back(r->frames, std::move(object));
}

// Accumulates the scope's duration (milliseconds) into `ms`.
#if defined(__x86_64__) || defined(_M_X64)
// (the processor's time stamp counter: two clock readings a draw were 0.4 ms of a 5000-draw frame.
// g_ms_per_tick is measured as the game runs - UpdateTitle.)
double g_ms_per_tick = 1.0 / 3.0e6;
struct ScopeTimer {
  double& ms;
  uint64_t t0 = __rdtsc();
  explicit ScopeTimer(double& m) : ms(m) {}
  ~ScopeTimer() { ms += double(__rdtsc() - t0) * g_ms_per_tick; }
};
#else
struct ScopeTimer {
  double& ms;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit ScopeTimer(double& m) : ms(m) {}
  ~ScopeTimer() {
    ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
};
#endif

Renderer* g_r = nullptr;
Stream g_streams[16];
bool g_stream_words_valid = false;  // (StreamsFromFetch)
uint32_t g_site_epoch = 1;          // (ReplaySite: counted whenever a kept buffer is replaced or let go)

// The GPU's ALU constant file as the game programs it: float4 0-255 vertex,
// 256-511 pixel (little-endian floats), plus dirty flags for mirror ranges.
float g_gpu_constants[512][4];
// Constants loaded from memory (a shader's literal values - LOAD_ALU_CONSTANT at its bind, or in a played
// command buffer) stay in the GPU's constant file until something writes over them: g_constants_loaded says
// which, g_gpu_constants holds them, and every draw's upload takes them over the mirror's. (Found by comparing
// a frame with the emulated GPU's - tools/frame_diff.py: they were applied to the one draw after a bind only,
// and every later draw with the same shader lost its lightmap basis: black ground, a dark level.)
uint64_t g_literal_version[2] = {1, 1}, g_literal_uploaded[2] = {0, 0};
bool g_constants_loaded[512];
// Constants loaded from memory since the last draw (OnLoadShaderConstants),
// and those of the draw in progress (taken at its start).
uint16_t g_loaded_pending[512], g_loaded_draw[512];
uint32_t g_loaded_pending_count = 0, g_loaded_draw_count = 0;  // loaded from memory for the draw in progress  // set by SetStreamSource (also before the first frame)
std::once_flag g_init_once;
bool g_failed = false;
// Main mode, switched to the emulated renderer from the GRAPHICS page: the
// hooks do nothing and the emulator draws and presents again.
std::atomic<bool> g_suspended{false};
std::recursive_mutex g_mutex;
// native_renderer=main: frames go to the main window through the emulator's
// presentation (rex/external_frame.h) instead of this renderer's own window.
bool g_main = false;
// Main mode: every target (see TargetScale), resolve copy and frame image has g_scale x g_scale
// times the guest's pixels (the emulator's resolution_scale setting); sizes the
// game sees and computes with stay the guest's.
uint32_t g_scale = 1;
// [xmd] The render scale's ceiling after memory ran out (OutOfVideoMemory): a step lower each time a target or
// the frame images can't be made above 1x.
uint32_t g_memory_scale_cap = UINT32_MAX;
// native_scale_effects off: only the main scene's targets (and their resolves
// and the frame images) are scaled; the effect buffers (the 1120 x 1120 shadow
// map, reflections, glow and blur chains) keep the guest's size.
bool g_scale_effects = true;
// native_shadows off: draws into the shadow map (the 1120 x 1120 32-bit float
// target) are skipped; its clear stays, so nothing reads as shadowed.
bool g_shadows = true;

// native_render_scale: the main scene's targets (and their resolves and the
// frame images) have this fraction of the scale's pixels per side - weak GPUs.
float g_res = 1.0f;
// native_upscale fsr1, while the scene is smaller than the window: Present scales it up with FSR 1 (EASU into the
// frame-size intermediate, RCAS into the frame image) - g_fsr_sharpness RCAS's strength (0-1).
bool g_fsr = false;
float g_fsr_sharpness = 0.8f;
// native_jitter: this frame's sub-pixel offset of the match scene's 3D draws (scene pixels, -0.5 to 0.5; y down)
// - what a temporal upscaler needs to undo; zero when off.
float g_jitter[2] = {0.0f, 0.0f};
// native_upscale fsr2 (experimental): FSR 2 wanted - Present uses it in frames whose scene depth was caught at the
// scene's resolve (fsr2_depth_frame), FSR 1 otherwise; the jitter on while it is.
bool g_fsr2 = false;
// native_upscale dlss (experimental): FSR 2's path with DLSS's upscaler (g_fsr2 on with it).
bool g_dlss = false;
// The intermediate's place in the 2D texture table: after the three placeholders, before the texture cache's.
constexpr uint32_t kFsrSrv = 3;

// Whether a target of this guest size is the main scene (1280 x 720). Not Superstar Threads' attire bake
// (2080 x 1024, read back by the game's CPU: WriteBackResolve): sized as the scene - native_render_scale /
// native_scene_height's fraction, the wide factor, the upscalers' jitter - its write-back took the wrong host pixels
// and every Threads attire came out with another superstar's face and parts (2.1.8 report b43b9e: render scale 2x,
// scene at 87%).
bool SceneTarget(uint32_t width, uint32_t height) {
  return width >= 1152 && height >= 640 && !(width == 2080 && height == 1024);
}

// Host pixels per guest pixel for a target of this guest size.
uint32_t TargetScale(uint32_t width, uint32_t height) {
  // [xmd] The engine's filter buffers - the scene at a half, a quarter and an eighth (640 x 360, 320 x 180,
  // 160 x 90: depth of field and bloom) - keep the console's size at every render scale. Their blur shaders step
  // by one texel of the console's buffer (13 taps, offsets 1/320 and 1/180 in the constants): on a buffer with
  // 2 x 2 host pixels a texel the taps land two host pixels apart and skip the ones between, so the blur came
  // out as a lattice of streaks (period 8 screen pixels at 2x) that shimmers as the picture moves - the main
  // menu's and the cut-scenes' blurred background, 2026-11-19. XMD_NATIVE_SCALE_FILTERS=1: scaled as before.
  static const bool scale_filters = std::getenv("XMD_NATIVE_SCALE_FILTERS") != nullptr;
  if (!scale_filters && width <= 640 && height <= 360 && width * 9 == height * 16) return 1;
  uint32_t s = g_scale_effects || (width >= 1152 && height >= 640) ? g_scale : 1;  // (the scene: 1280 x 720)
  // [xmd] The engine's big square buffers (2080 x 2080: shadows) stay within 4160 host pixels a side: at 4x that
  // one alone was 8320 x 8320 - 1.1 GB with its depth, and four times the scene's pixels to fill every frame.
  static const bool no_cap = std::getenv("XMD_TEST_NO_EFFECT_CAP") != nullptr;  // (test: as before)
  if (!no_cap && (width != kWidth || height != kHeight))
    while (s > 1 && std::max(width, height) * s > kMaxEffectSide) --s;
  return s;
}

// Wide screens (native_widescreen): in matches the scene is as wide as the
// window (up to 32:9). The camera's aspect ratio is patched to match - a
// 16:9 constant only the projection (sub_825818B8) reads, so the game also
// culls for the wider view - and the full-screen targets (and their resolves)
// get g_wide times the width. Screen-space passes follow on their own; the
// 2D HUD's sprites are drawn into the 16:9 middle (Draw). Menus stay 16:9.
float g_wide = 1.0f;
// Screens narrower than 16:9 (a foldable's inner screen, 16:10): the same
// the other way - g_tall times the height, the camera shows more above and
// below (its vertical field of view widened: CameraFovScale), the HUD in
// the vertical middle.
float g_tall = 1.0f;
std::atomic<float> g_fov_scale{1.0f};
std::atomic<bool> g_match_scene{false};
std::atomic<bool> g_capture_wanted{false};  // (RequestFrameCapture: the next shown frame into g_capture)
std::atomic<int> g_capture_output{-1};      // (... or this frame image: the one shown before - previous)
std::atomic<int> g_last_shown_output{-1};   // (the frame image last shown)
std::mutex g_capture_mutex;
std::vector<uint8_t> g_capture;  // (RGB)
uint32_t g_capture_w = 0, g_capture_h = 0;
// The last frame drew nothing in 3D (a pause menu, a loading or transition
// screen): 2D art filling the screen - all of it goes in the 16:9 middle,
// with black sides.
bool g_frame_2d = false;
bool g_flat_frame_seen = false;  // (a match frame without the scene came: XMD_TEST_FRAME_SHOT_ON_2D)
constexpr float kAspect16x9 = 16.0f / 9.0f;
constexpr float kMaxWide = 2.0f;  // (32:9)

// The host width of a target of this guest size.
uint32_t HostWidth(uint32_t width, uint32_t height, uint32_t scale) {
  const uint32_t w = width * scale;
  const float f = SceneTarget(width, height) ? std::max(g_wide, 1.0f) * g_res : 1.0f;
  if (f == 1.0f) return w;
  return (uint32_t(std::lround(float(w) * f)) + 1) & ~1u;
}

// The host height of a target of this guest size.
uint32_t HostHeight(uint32_t width, uint32_t height, uint32_t scale) {
  const uint32_t h = height * scale;
  const float f = SceneTarget(width, height) ? std::max(g_tall, 1.0f) * g_res : 1.0f;
  if (f == 1.0f) return h;
  return (uint32_t(std::lround(float(h) * f)) + 1) & ~1u;
}

// (This game's camera aspect isn't found yet: wide screens stay 16:9, native_widescreen off.)
void SetCameraAspect(float) {}
// Main mode: the frame images' size - the window's 16:9 area, so the
// emulator's presentation shows them 1:1 (Present averages the scaled image
// down to it). Both follow the window and the anti-aliasing setting live
// (ApplyOutputSettings).
uint32_t g_out_w = kWidth, g_out_h = kHeight;
std::function<std::pair<uint32_t, uint32_t>()> g_window_size;

// The native renderer couldn't start or stopped working. The game draws only
// with it - no emulated fallback: the screen stays black with the reason on
// it (FailureReason, fps_overlay.cpp), and on Android the launcher shows it
// next time (native_failed.txt in the app's cache).
std::mutex g_fail_mutex;
std::string g_fail_reason;

void Fail(const char* reason = "it stopped working") {
  g_failed = true;
  {
    std::lock_guard lock(g_fail_mutex);
    if (g_fail_reason.empty()) g_fail_reason = reason;
  }
  REXLOG_ERROR("native renderer: can't draw the game - {} (no emulated fallback)", reason);
#if defined(__ANDROID__)
  if (const char* cache = std::getenv("XMD_CACHE_DIR")) {
    if (FILE* f = std::fopen((std::string(cache) + "/native_failed.txt").c_str(), "w")) {
      std::fprintf(f, "%s\n", reason);
      std::fclose(f);
    }
  }
#endif
}

using xmd::EnvFlag;

int EnvInt(const char* name, int fallback) {
  char* v = nullptr;
  size_t n = 0;
  int value = fallback;
  if (_dupenv_s(&v, &n, name) == 0 && v) {
    value = std::atoi(v);
    free(v);
  }
  return value;
}

// Debug: XMD_NATIVE_NOCACHE=1 - buffers and textures re-checked at every use.
bool NoCache() {
  static const bool on = EnvFlag("XMD_NATIVE_NOCACHE");
  return on;
}

// Debug aids (environment): XMD_NATIVE_STOP_AT_RESOLVE=1 draws
// only up to the first Resolve of each frame (shows the scene before
// post-processing).
bool g_stop_at_resolve = false;
bool g_resolved_this_frame = false;
// Debug: XMD_NATIVE_STOP_AFTER_RESOLVE=<n> - each frame ends at its n-th
// resolve (1: the first; any target, depth too): nothing is drawn after it and
// that resolve's texture is shown instead of the front buffer. Steps through
// the frame's passes (the game reuses its targets' memory, so a pass's
// texture can't be shown once the frame went on).
uint32_t g_stop_after = 0, g_resolve_count = 0, g_stop_texture = 0;
bool g_stopped = false;
// ... and, with it, <stop file>.draws: only the frame's first n draws are made (0: all) - what the scene looks
// like part way through its draws, at the resolve the frame is stopped at.
uint32_t g_stop_draws = 0;
// ... and <stop file>.base "<hex physical address>": the frame stopped at its first resolve into the texture there.
uint32_t g_stop_base = 0;
// ... and <stop file>.ps "<first 8 hex digits of a pixel shader's hash> <0|1>": the frame's draws end at the first
// draw with that pixel shader - before it (0) or after it (1): what one pass does to its target.
uint32_t g_stop_ps = 0;
bool g_stop_ps_after = false, g_stop_ps_hit = false;
uint32_t g_stop_ps_extra = 0, g_stop_ps_left = 0;  // (a third number: that many more draws after it)
// XMD_NATIVE_DEBUG_SOLID=1: every draw with a flat per-draw colour, no
// depth test, no culling (checks geometry independently of pixel shading).
bool g_debug_solid = false;
bool g_debug_solid_state = false;  // (=2: the shader only - depth, culling and blending as the game set them)
bool g_no_depth = false;  // XMD_NATIVE_NO_DEPTH=1
double g_pipeline_ms = 0;  // pipeline builds since the last perf line
// Bytes hashed since the last perf line: static / dynamic vertex buffers, constants.
uint64_t g_hash_vb_static = 0, g_hash_vb_dynamic = 0, g_hash_const = 0;
// Test aid XMD_TEST_CONST_TIMES=1: the constants path's time a frame on the native perf line - SharedConstants
// (its texture lookups, its sets), GpuConstants - and the textures looked up a draw.
const bool g_const_times = std::getenv("XMD_TEST_CONST_TIMES") != nullptr;
uint64_t g_ct_shared_ns = 0, g_ct_tex_ns = 0, g_ct_sets_ns = 0, g_ct_gpu_ns = 0, g_ct_textures = 0, g_ct_draws = 0;
uint64_t g_ct_tex_same = 0, g_ct_memo_hits = 0;  // (a slot's fetch as its last draw's; SharedConstants' kept answers)
uint32_t g_ct_last_fetch[2][32][6] = {};
struct ConstTimer {
  uint64_t& sum;
  std::chrono::steady_clock::time_point t0;
  explicit ConstTimer(uint64_t& s) : sum(s) {
    if (g_const_times) t0 = std::chrono::steady_clock::now();
  }
  ~ConstTimer() {
    if (g_const_times)
      sum += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  }
};
uint32_t g_vb_demotions = 0;
uint64_t g_repeat_same = 0, g_repeat_changed = 0;  // (total) dynamic buffers seen changing within a frame after trusting them
// Debug (XMD_NATIVE_VB_STATS=1): hashed uses per dynamic buffer (address << 32 | size).
std::unordered_map<uint64_t, uint32_t> g_vb_stats;
// Vertex / pixel constant blocks written since their last upload: D3D's
// constant flush (sub_829251D8) reports which 4-constant blocks it sends at
// each draw (bit 63 - n = block n, its order). A draw compares only those
// blocks with the last upload's copy (g_const_shadow, as in the mirror) and
// reuses the upload when they're unchanged - instead of hashing both 4 KB
// constant files every draw (~17-26 MB a frame in entrances: the render
// thread's largest cost on phones).
uint64_t g_const_dirty[2] = {~0ull, ~0ull};
uint32_t g_draw_float_uint_inputs = 0;  // (the draw's InputLayout::float_uint_inputs, for SharedConstants)
bool g_const_force[2] = {true, true};  // (the last upload had loaded constants)
alignas(16) uint8_t g_const_shadow[2][256 * 16];
uint32_t g_const_stale = 0;  // (native_constant_check: reuses found stale)
// Pipelines built since the pipeline cache was saved, and when the last was.
std::atomic<bool> g_pipeline_cache_dirty{false};
std::atomic<int64_t> g_last_pipeline_build{0};  // (steady_clock ticks)

// The pipeline cache (gpu.h): the user data's cache folder, one per API.
std::filesystem::path PipelineCacheFile() {
  std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "UserData";
  char* v = nullptr;
  size_t n = 0;
  if (_dupenv_s(&v, &n, "XMD_USER_DATA") == 0 && v) {  // (tests)
    if (*v) dir = v;
    free(v);
  }
  return dir / "cache" /
         (backend::ActiveApi() == backend::Api::kVulkan ? "native_vulkan_pipelines.bin" : "native_d3d12_pipelines.bin");
}
#if XMD_DLSS
xmd::dlss::Host DlssHost(Renderer* r) {
  xmd::dlss::Host h;
  h.device = r->device.get();
  h.vulkan = backend::ActiveApi() == backend::Api::kVulkan;
  h.dll_dir = rex::filesystem::GetExecutableFolder().wstring();
  h.data_path = (PipelineCacheFile().parent_path()).wstring();
  return h;
}
#endif
bool g_no_blend = false;  // XMD_NATIVE_NO_BLEND=1
// XMD_NATIVE_DUMP_FRAME=<frame>: describe the first 40 draws of that
// native frame in native_draws.txt (next to the log).
uint64_t g_dump_frame = ~0ull;
uint32_t g_dump_first = 0, g_dump_count = 40;  // XMD_NATIVE_DUMP_FIRST / _COUNT
FILE* g_dump = nullptr;
std::mutex g_dump_request_mutex;
std::string g_dump_request_file;          // (RequestDrawDump: where the next dump goes)
std::atomic<bool> g_dump_requested{false};
// Debug: XMD_DRAWLOG=<file>: when <file>.go appears, one line per draw of
// the next frame goes to <file> (screen-space or not, depth, textures, the
// positions' extent) - for telling the HUD from the scene.
uint64_t g_drawlog_frame = ~0ull, g_drawlog_end = 0;  // (XMD_DRAWLOG_FRAMES: frames logged)
FILE* g_drawlog = nullptr;
std::vector<uint8_t> g_debug_ps;

// ---------------------------------------------------------------------------
// resources

// An upload-heap buffer (vertex / index / constant data written by the CPU).
std::shared_ptr<plume::RenderBuffer> CreateBuffer(Renderer* r, uint64_t size) {
  std::shared_ptr<plume::RenderBuffer> b = r->device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
      size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::CONSTANT));
  backend::LogBuffer(b.get(), size);
  return b;
}

// Space in the pool (BufferPool; null: none - the caller's other way).
std::pair<uint8_t*, RenderBufferReference> PoolAllocate(Renderer* r, uint32_t size) {
  static const bool off = EnvFlag("XMD_NATIVE_NO_BUFFER_POOL");
  if (off || backend::ActiveApi() == backend::Api::kD3D11 || !size || size > kPoolLargest) return {nullptr, {}};
  BufferPool& p = r->pool;
  const uint64_t need = (uint64_t(size) + 15) & ~15ull;
  if (p.chunks.empty() || p.chunks.back().used + need > kPoolChunk) {
    BufferPool::Chunk c;
    c.buffer = CreateBuffer(r, kPoolChunk);
    if (!c.buffer) return {nullptr, {}};
    c.buffer->setName("pool of small vertex and index buffers");
    c.cpu = static_cast<uint8_t*>(c.buffer->map());
    if (!c.cpu) return {nullptr, {}};
    p.chunks.push_back(std::move(c));
  }
  BufferPool::Chunk& c = p.chunks.back();
  auto result = std::make_pair(c.cpu + c.used, c.buffer->at(c.used));
  c.used += need;
  return result;
}
// A played command buffer's buffers are a level's own: looked at again every 16-23 frames (not all in one frame).
// The game's own draws' buffers: at every frame they are used in. Their memory is the game's per-frame vertex
// memory as often as a model's - handed out anew every frame from the same addresses (sprites, trails, decals,
// the HUD), so an address and size seen before says nothing about what is there now. Looked at every 4th frame,
// a draw got the vertices that were there up to 3 frames earlier - another effect's, or another stream's
// (positions read as colours): sprites jumping, wrong colours and big stray quads for a frame or two.
// XMD_NATIVE_VB_RECHECK=<frames>: as before with 4.
uint32_t RecheckEvery(uint32_t address) {
  static const uint32_t own = [] {
    const char* v = std::getenv("XMD_NATIVE_VB_RECHECK");
    return v ? uint32_t(std::max(1, std::atoi(v))) : 1u;
  }();
  return g_replay_device ? 16 + ((address >> 8) & 7) : own;
}

// A 1x1 transparent black texture in texture table `dimension` (0 2D, 1 3D,
// 2 cube) at index `dimension`: what Xenia (and so the reference) samples for
// unbound / invalid fetch constants and unsupported textures.
std::shared_ptr<plume::RenderTexture> CreatePlaceholder(Renderer* r, uint32_t dimension) {
  const uint32_t array = dimension == 2 ? 6 : 1;
  const plume::RenderTextureDesc desc =
      dimension == 1 ? plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM)
                     : plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1,
                                                         array, RenderFormat::R8G8B8A8_UNORM,
                                                         dimension == 2 ? plume::RenderTextureFlag::CUBE
                                                                        : plume::RenderTextureFlag::NONE);
  std::shared_ptr<plume::RenderTexture> tex = r->device->createTexture(desc);
  // Upload the texels through a temporary buffer.
  std::shared_ptr<plume::RenderBuffer> upload =
      r->device->createBuffer(plume::RenderBufferDesc::UploadBuffer(512ull * array));
  std::memset(upload->map(), 0x00, 512 * array);
  upload->unmap();
  r->list->barriers(RenderBarrierStage::COPY, plume::RenderTextureBarrier(tex.get(), RenderTextureLayout::COPY_DEST));
  for (uint32_t i = 0; i < array; ++i) {
    r->list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(tex.get(), 0, i),
        plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                                                          64, 512ull * i));  // (256-byte rows)
  }
  r->list->barriers(RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                    plume::RenderTextureBarrier(tex.get(), RenderTextureLayout::SHADER_READ));
  Retire(r, upload);
  plume::RenderTextureViewDesc vd;
  vd.format = RenderFormat::R8G8B8A8_UNORM;
  vd.dimension = dimension == 1 ? plume::RenderTextureViewDimension::TEXTURE_3D
                 : dimension == 2 ? plume::RenderTextureViewDimension::TEXTURE_CUBE
                                  : plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipLevels = 1;
  std::shared_ptr<plume::RenderTextureView> view = tex->createTextureView(vd);
  r->texture_set[dimension]->setTexture(r->texture_base[dimension] + dimension, tex.get(),
                                        RenderTextureLayout::SHADER_READ, view.get());
  r->null_views.push_back(view);
  return tex;
}

// The texture tables' layout (register spaces 0-3) - the same for the
// pipeline layout and the tables themselves.
plume::RenderDescriptorRange g_table_ranges[4];
plume::RenderDescriptorSetDesc g_table_descs[4];

// Vulkan: textures (set 0: 2D, 3D, cube - bindings 0-2), samplers (set 1)
// and this frame's upload ring as the constant buffer (set 2).
plume::RenderDescriptorRange g_vk_texture_ranges[3], g_vk_sampler_range, g_vk_constant_range;
plume::RenderDescriptorSetDesc g_vk_set_descs[3];

// Compact tables (backend::CompactTables()): set 0 is 13 2D textures, one 3D
// and two cube (bindings 0-2, 16 in all - old Mali drivers allow 16 a stage),
// set 1 16 samplers; the shaders' indices are places in them (.spvc shaders,
// shader_common.h). The game's shaders use at most 12 textures a draw.
constexpr uint32_t kCompactPlaces[3] = {13, 1, 2}, kCompactFirst[3] = {0, 13, 14};
constexpr uint32_t kCompactSamplers = 16;
plume::RenderDescriptorRange g_compact_texture_ranges[3], g_compact_sampler_range;
// Compact tables' constants (set 2): the upload ring as a storage buffer (the
// renderer's own shaders), and the vertex, pixel and shared constants as
// uniform buffers on it, at the draw's offsets (dynamic) - old Mali drivers
// read uniform buffers much faster than storage buffers (shader_common.h).
plume::RenderDescriptorRange g_compact_constant_ranges[4];
plume::RenderDescriptorSetDesc g_compact_descs[2];

// Slot tables (Direct3D 11, SlotTables()): no descriptor tables in the
// shaders - each fetch slot has its own register (shader_common.h, SVR_D3D11):
// set 0 is 64 textures (2D slots 0-31, 3D 0-15, cube 0-15), set 1 16 samplers
// (slots 0-15; the vertex shaders' 16 / 17 in 14 / 15), made per draw like
// the compact tables'. The constants are root constant buffers b0-b3.
constexpr uint32_t kSlotTextures = 64, kSlotSamplers = 16;
constexpr uint32_t kSlotFirst[3] = {0, 32, 48}, kSlotCount[3] = {32, 16, 16};
plume::RenderDescriptorRange g_slot_ranges[2];
plume::RenderDescriptorSetDesc g_slot_descs[2];

bool SlotTables() { return backend::ActiveApi() == backend::Api::kD3D11; }

// The sampler register of a fetch slot (slot tables).
uint32_t SlotSampler(uint32_t slot) { return slot >= 16 ? slot - 2 : slot; }

// Compact tables: stands in for the big tables - remembers what each index
// holds (and a stamp, new at every write), for the draws' own sets.
class TableRecord final : public plume::RenderDescriptorSet {
 public:
  struct Entry {
    const plume::RenderTexture* texture = nullptr;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::SHADER_READ;
    const plume::RenderTextureView* view = nullptr;
    const plume::RenderSampler* sampler = nullptr;
    uint32_t stamp = 0;
  };
  explicit TableRecord(uint32_t size) : entries(size) {}
  void setBuffer(uint32_t, const plume::RenderBuffer*, uint64_t, const plume::RenderBufferStructuredView*,
                 const plume::RenderBufferFormattedView*) override {}
  void setTexture(uint32_t index, const plume::RenderTexture* texture, plume::RenderTextureLayout layout,
                  const plume::RenderTextureView* view) override {
    if (index >= entries.size() || !texture) return;
    Entry& e = entries[index];
    e.texture = texture;
    e.layout = layout;
    e.view = view;
    e.stamp = ++stamps;
  }
  void setSampler(uint32_t index, const plume::RenderSampler* sampler) override {
    if (index >= entries.size() || !sampler) return;
    entries[index].sampler = sampler;
    entries[index].stamp = ++stamps;
  }
  void setAccelerationStructure(uint32_t, const plume::RenderAccelerationStructure*) override {}
  std::vector<Entry> entries;
  uint32_t stamps = 0;
};

// Compact tables: the set holding these table indices (16 places, slot
// tables 64 textures / 16 samplers; UINT32_MAX: a placeholder) - made the
// first time, then reused. Sets not used for a while are dropped (well after
// the GPU is done with them).
plume::RenderDescriptorSet* CompactSet(Renderer* r, int samplers, const uint32_t* indices) {
  auto* table = static_cast<TableRecord*>(samplers ? r->sampler_set.get() : r->texture_sets[0].get());
  const uint32_t places = SlotTables() && !samplers ? kSlotTextures : 16;
  // The same places as the last draw's, and the table unchanged since: its set
  // (most draws - no key to build and look up).
  Renderer::LastCompact& last = r->last_compact[samplers];
  if (last.entry && last.table_stamps == table->stamps &&
      std::memcmp(last.indices, indices, places * sizeof(uint32_t)) == 0) {
    last.entry->used = r->frames;
    return last.entry->set.get();
  }
  Renderer::CompactKey key{};
  for (uint32_t i = 0; i < places; ++i)
    key.stamps[size_t(i)] = indices[i] < table->entries.size() ? table->entries[indices[i]].stamp : 0;
  auto& cache = r->compact_sets[samplers];
  if (r->frames - r->compact_swept > 600) {
    r->compact_swept = r->frames;
    r->last_compact[0].entry = r->last_compact[1].entry = nullptr;
    for (int k = 0; k < 2; ++k)
      for (auto it = r->compact_sets[k].begin(); it != r->compact_sets[k].end();) {
        const bool keep = it->second.used + 2 * kFrames + 4 >= r->frames ||
                          it->second.set.get() == r->default_sets[0] || it->second.set.get() == r->default_sets[1];
        it = keep ? std::next(it) : r->compact_sets[k].erase(it);
      }
  }
  Renderer::CompactSet& c = cache[key];
  c.used = r->frames;
  last.entry = &c;  // (map elements stay put until erased)
  last.table_stamps = table->stamps;
  std::memcpy(last.indices, indices, places * sizeof(uint32_t));
  if (c.set) return c.set.get();
  c.set = r->device->createDescriptorSet(SlotTables() ? g_slot_descs[samplers] : g_compact_descs[samplers]);
  if (!c.set) return nullptr;
  for (uint32_t i = 0; i < places; ++i) {
    if (samplers) {
      const uint32_t index = indices[i] < table->entries.size() ? indices[i] : 0;  // (0: linear wrap)
      c.set->setSampler(i, table->entries[index].sampler);
      continue;
    }
    const uint32_t dimension = SlotTables() ? (i < kSlotFirst[1] ? 0 : i < kSlotFirst[2] ? 1 : 2)
                               : i < kCompactFirst[1]          ? 0
                               : i < kCompactFirst[2]          ? 1
                                                               : 2;
    const uint32_t index =
        indices[i] < table->entries.size() ? indices[i] : r->texture_base[dimension] + dimension;
    const TableRecord::Entry& e = table->entries[index];
    c.set->setTexture(i, e.texture, e.layout, e.view);
  }
  return c.set.get();
}

// Compact tables: binds the draw's sets (r->draw_sets) where they changed.
void BindCompactSets(Renderer* r, plume::RenderCommandList* list) {
  auto& ls = r->list_state;
  for (uint32_t k = 0; k < 2; ++k) {
    plume::RenderDescriptorSet* set = r->draw_sets[k] ? r->draw_sets[k] : r->default_sets[k];
    if (set && ls.compact[k] != set) {
      list->setGraphicsDescriptorSet(set, k);
      ls.compact[k] = set;
    }
  }
}

bool CreateVulkanCompactLayout(Renderer* r) {
  for (uint32_t i = 0; i < 3; ++i)
    g_compact_texture_ranges[i] =
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, i, kCompactPlaces[i]);
  g_compact_sampler_range =
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 0, kCompactSamplers);
  g_compact_constant_ranges[0] =
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1);
  for (uint32_t i = 1; i < 4; ++i)
    g_compact_constant_ranges[i] =
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC, i, 1);
  g_compact_descs[0] = plume::RenderDescriptorSetDesc(g_compact_texture_ranges, 3);
  g_compact_descs[1] = plume::RenderDescriptorSetDesc(&g_compact_sampler_range, 1);
  g_vk_set_descs[0] = g_compact_descs[0];
  g_vk_set_descs[1] = g_compact_descs[1];
  g_vk_set_descs[2] = plume::RenderDescriptorSetDesc(g_compact_constant_ranges, 4);
  const plume::RenderPushConstantRange push(0, 0, 0, 4 * sizeof(uint32_t),
                                            plume::RenderShaderStageFlag::VERTEX |
                                                plume::RenderShaderStageFlag::PIXEL);
  plume::RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = g_vk_set_descs;
  desc.descriptorSetDescsCount = 3;
  desc.pushConstantRanges = &push;
  desc.pushConstantRangesCount = 1;
  desc.allowInputLayout = true;
  r->layout = r->device->createPipelineLayout(desc);
  if (!r->layout) {
    REXLOG_ERROR("native renderer: could not create the pipeline layout (compact tables)");
    return false;
  }
  r->texture_sets[0] = std::make_unique<TableRecord>(3 * kSrvHeapSize);
  r->sampler_set = std::make_unique<TableRecord>(kSamplerHeapSize);
  for (uint32_t i = 0; i < 3; ++i) {
    r->texture_set[i] = r->texture_sets[0].get();
    r->texture_base[i] = i * kSrvHeapSize;
  }
  for (uint32_t i = 0; i < kFrames; ++i) {
    r->constant_sets[i] = r->device->createDescriptorSet(g_vk_set_descs[2]);
    if (!r->constant_sets[i] || !r->rings[i].buffer) return false;
    r->constant_sets[i]->setBuffer(0, r->rings[i].buffer.get(), kRingSize + kRingSlack);
    r->constant_sets[i]->setBuffer(1, r->rings[i].buffer.get(), kVertexConstantsBytes);
    r->constant_sets[i]->setBuffer(2, r->rings[i].buffer.get(), kPixelConstantsBytes);
    r->constant_sets[i]->setBuffer(3, r->rings[i].buffer.get(), kSharedConstantsBytes);
  }
  REXLOG_INFO("native renderer: compact tables ({} 2D / {} 3D / {} cube textures and {} samplers a draw)",
              kCompactPlaces[0], kCompactPlaces[1], kCompactPlaces[2], kCompactSamplers);
  return true;
}

bool CreateVulkanPipelineLayout(Renderer* r) {
  if (backend::CompactTables()) return CreateVulkanCompactLayout(r);
  for (uint32_t i = 0; i < 3; ++i)
    g_vk_texture_ranges[i] = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, i, kSrvHeapSize);
  g_vk_sampler_range = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 0, kSamplerHeapSize);
  g_vk_constant_range = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1);
  g_vk_set_descs[0] = plume::RenderDescriptorSetDesc(g_vk_texture_ranges, 3, true, kSrvHeapSize);
  g_vk_set_descs[1] = plume::RenderDescriptorSetDesc(&g_vk_sampler_range, 1, true, kSamplerHeapSize);
  g_vk_set_descs[2] = plume::RenderDescriptorSetDesc(&g_vk_constant_range, 1);
  const plume::RenderPushConstantRange push(0, 0, 0, 4 * sizeof(uint32_t),
                                            plume::RenderShaderStageFlag::VERTEX |
                                                plume::RenderShaderStageFlag::PIXEL);
  plume::RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = g_vk_set_descs;
  desc.descriptorSetDescsCount = 3;
  desc.pushConstantRanges = &push;
  desc.pushConstantRangesCount = 1;
  desc.allowInputLayout = true;
  r->layout = r->device->createPipelineLayout(desc);
  if (!r->layout) {
    REXLOG_ERROR("native renderer: could not create the pipeline layout");
    return false;
  }
  r->texture_sets[0] = r->device->createDescriptorSet(g_vk_set_descs[0]);
  r->sampler_set = r->device->createDescriptorSet(g_vk_set_descs[1]);
  for (uint32_t i = 0; i < 3; ++i) {
    r->texture_set[i] = r->texture_sets[0].get();
    r->texture_base[i] = i * kSrvHeapSize;
  }
  for (uint32_t i = 0; i < kFrames; ++i) {
    r->constant_sets[i] = r->device->createDescriptorSet(g_vk_set_descs[2]);
    if (!r->constant_sets[i] || !r->rings[i].buffer) return false;
    r->constant_sets[i]->setBuffer(0, r->rings[i].buffer.get(), kRingSize + kRingSlack);
  }
  return r->texture_sets[0] && r->sampler_set;
}

// Binds the texture tables, samplers and (Vulkan) this frame's constant buffer.
void BindTables(Renderer* r, plume::RenderCommandList* list) {
  if (backend::CompactTables() || SlotTables()) {
    // (the placeholders' sets until a draw binds its own)
    if (!r->default_sets[0]) {
      uint32_t none[kSlotTextures];
      std::fill(std::begin(none), std::end(none), UINT32_MAX);
      for (int k = 0; k < 2; ++k) r->default_sets[k] = CompactSet(r, k, none);
    }
    r->list_state.compact[0] = r->list_state.compact[1] = nullptr;
    r->draw_sets[0] = r->draw_sets[1] = nullptr;
    BindCompactSets(r, list);
    if (SlotTables()) return;  // (constants: root constant buffers, BindConstants)
    const uint32_t zero[3] = {};
    list->setGraphicsDescriptorSetDynamic(r->constant_sets[r->back_index].get(), 2, zero, 3);
    return;
  }
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    list->setGraphicsDescriptorSet(r->texture_sets[0].get(), 0);
    list->setGraphicsDescriptorSet(r->sampler_set.get(), 1);
    list->setGraphicsDescriptorSet(r->constant_sets[r->back_index].get(), 2);
    return;
  }
  for (uint32_t i = 0; i < 3; ++i) list->setGraphicsDescriptorSet(r->texture_sets[i].get(), i);
  list->setGraphicsDescriptorSet(r->sampler_set.get(), 3);
}

// Slot tables (Direct3D 11): see g_slot_descs.
bool CreateSlotPipelineLayout(Renderer* r) {
  g_slot_ranges[0] = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, kSlotTextures);
  g_slot_ranges[1] = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 0, kSlotSamplers);
  g_slot_descs[0] = plume::RenderDescriptorSetDesc(&g_slot_ranges[0], 1);
  g_slot_descs[1] = plume::RenderDescriptorSetDesc(&g_slot_ranges[1], 1);
  plume::RenderRootDescriptorDesc roots[4];
  for (uint32_t i = 0; i < 4; ++i)
    roots[i] = plume::RenderRootDescriptorDesc(i, 0, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  plume::RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = g_slot_descs;
  desc.descriptorSetDescsCount = 2;
  desc.rootDescriptorDescs = roots;
  desc.rootDescriptorDescsCount = 4;
  desc.allowInputLayout = true;
  r->layout = r->device->createPipelineLayout(desc);
  if (!r->layout) {
    REXLOG_ERROR("native renderer: could not create the pipeline layout (slot tables)");
    return false;
  }
  r->texture_sets[0] = std::make_unique<TableRecord>(3 * kSrvHeapSize);
  r->sampler_set = std::make_unique<TableRecord>(kSamplerHeapSize);
  for (uint32_t i = 0; i < 3; ++i) {
    r->texture_set[i] = r->texture_sets[0].get();
    r->texture_base[i] = i * kSrvHeapSize;
  }
  REXLOG_INFO("native renderer: slot tables ({} textures, {} samplers a draw, each fetch slot its own register)",
              kSlotTextures, kSlotSamplers);
  return true;
}

bool CreatePipelineLayout(Renderer* r) {
  if (backend::ActiveApi() == backend::Api::kVulkan) return CreateVulkanPipelineLayout(r);
  if (SlotTables()) return CreateSlotPipelineLayout(r);
  for (int i = 0; i < 4; ++i) {
    // (the count: Vulkan's upper bound for the table; D3D12 ignores it)
    g_table_ranges[i] = plume::RenderDescriptorRange(
        i == 3 ? plume::RenderDescriptorRangeType::SAMPLER : plume::RenderDescriptorRangeType::TEXTURE, 0,
        i == 3 ? kSamplerHeapSize : kSrvHeapSize);
    g_table_descs[i] = plume::RenderDescriptorSetDesc(&g_table_ranges[i], 1, true,
                                                      i == 3 ? kSamplerHeapSize : kSrvHeapSize);
  }
  // The constants: vertex, pixel, shared, the renderer's own (debug / present).
  // D3D12: root CBVs b0-b3 space4. Vulkan: their buffer addresses in the
  // push constants (shader_common.h, shaders/own_constants.hlsli).
  plume::RenderRootDescriptorDesc roots[4];
  for (uint32_t i = 0; i < 4; ++i)
    roots[i] = plume::RenderRootDescriptorDesc(i, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  const plume::RenderPushConstantRange push(0, 0, 0, 4 * sizeof(uint64_t),
                                            plume::RenderShaderStageFlag::VERTEX |
                                                plume::RenderShaderStageFlag::PIXEL);
  plume::RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = g_table_descs;
  desc.descriptorSetDescsCount = 4;
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    desc.pushConstantRanges = &push;
    desc.pushConstantRangesCount = 1;
  } else {
    desc.rootDescriptorDescs = roots;
    desc.rootDescriptorDescsCount = 4;
  }
  desc.allowInputLayout = true;
  r->layout = r->device->createPipelineLayout(desc);
  if (!r->layout) {
    REXLOG_ERROR("native renderer: could not create the pipeline layout");
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    r->texture_sets[i] = r->device->createDescriptorSet(g_table_descs[i]);
    r->texture_set[i] = r->texture_sets[i].get();
  }
  r->sampler_set = r->device->createDescriptorSet(g_table_descs[3]);
  return r->texture_sets[0] && r->texture_sets[1] && r->texture_sets[2] && r->sampler_set;
}

// A target that can't be made is asked for again at each draw: the first 16 logged, then every 1000th (the GT 710
// report's log would have been all of them - the start rotated away).
bool TargetFailureLogged() {
  static std::atomic<uint64_t> n{0};
  const uint64_t i = n++;
  return i < 16 || i % 1000 == 0;
}

// createTexture, with a C++ out-of-memory as no texture: Direct3D 11 fills a new target from a buffer of zeros
// (plume) - a 2.1.8 report (GeForce GT 710, the PC's memory full) crashed there making a 2x depth target.
// (test: XMD_TEST_TEXTURE_LIMIT=<pixels> - a target or frame image bigger fails as out of memory)
std::unique_ptr<plume::RenderTexture> TryCreateTexture(Renderer* r, const plume::RenderTextureDesc& d) {
  static const uint64_t test_limit = uint64_t(EnvInt("XMD_TEST_TEXTURE_LIMIT", 0));
  if (test_limit && uint64_t(d.width) * d.height > test_limit) {
    if (TargetFailureLogged()) REXLOG_WARN("native renderer: test texture limit - {}x{} not made", d.width, d.height);
    return nullptr;
  }
  try {
    return r->device->createTexture(d);
  } catch (const std::bad_alloc&) {
    REXLOG_ERROR("native renderer: out of memory making a {}x{} texture", d.width, d.height);
    return nullptr;
  }
}

void OutOfVideoMemory(Renderer* r, const char* what);

// The frame images, g_out_w x g_out_h, handed to the emulator's presentation
// (committed, so the reference the presentation holds keeps one alive).
// False (out of memory): none are left - BeginFrame draws nothing until they're made.
bool CreateOutputs(Renderer* r) {
  for (uint32_t i = 0; i < kOutputs; ++i) {
    plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
        g_out_w, g_out_h, 1, RenderFormat::R8G8B8A8_UNORM, plume::RenderTextureFlag::RENDER_TARGET);
    d.committed = true;
    r->output_framebuffers[i].reset();
    r->outputs[i] = TryCreateTexture(r, d);
    if (!r->outputs[i]) {
      REXLOG_ERROR("native renderer: could not create the frame images ({}x{})", g_out_w, g_out_h);
      for (uint32_t j = 0; j < kOutputs; ++j) r->output_framebuffers[j].reset(), r->outputs[j].reset();
      return false;
    }
    r->outputs[i]->setName(fmt::format("frame image {}", i));
    const plume::RenderTexture* color = r->outputs[i].get();
    r->output_framebuffers[i] = r->device->createFramebuffer(plume::RenderFramebufferDesc(&color, 1));
  }
  // FSR 1's intermediate, the frames' size (made with them: the GPU is idle), sampled from the 2D table at kFsrSrv.
  r->fsr_framebuffer.reset();
  r->fsr_view.reset();
  // (FSR 2 writes it too: a storage texture where there is compute - not Direct3D 11)
  r->fsr_image = TryCreateTexture(r, plume::RenderTextureDesc::Texture2D(
      g_out_w, g_out_h, 1, RenderFormat::R8G8B8A8_UNORM,
      plume::RenderTextureFlag::RENDER_TARGET |
          (SlotTables() ? 0u : plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS)));
  r->fsr_image_fresh = true;
#if XMD_FSR2
  r->fsr_image_layout = RenderTextureLayout::UNKNOWN;
  r->fsr2.reset();  // (its output size changed)
#endif
  if (r->fsr_image) {
    r->fsr_image->setName("FSR 1 intermediate");
    const plume::RenderTexture* color = r->fsr_image.get();
    r->fsr_framebuffer = r->device->createFramebuffer(plume::RenderFramebufferDesc(&color, 1));
    plume::RenderTextureViewDesc vd;
    vd.format = RenderFormat::R8G8B8A8_UNORM;
    vd.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    vd.mipLevels = 1;
    r->fsr_view = r->fsr_image->createTextureView(vd);
    if (r->fsr_view)
      r->texture_set[0]->setTexture(r->texture_base[0] + kFsrSrv, r->fsr_image.get(), RenderTextureLayout::SHADER_READ,
                                    r->fsr_view.get());
  }
  return true;
}

void ReportDeviceRemoved(Renderer* r);
void StartPrebuildingPipelines(Renderer* r);
uint64_t VideoMemoryBytes();

bool Initialize() {
  if (!g_memory) {
    REXLOG_ERROR("native renderer: guest memory not attached");
    return false;
  }
  auto* r = new Renderer();
  g_stop_at_resolve = EnvFlag("XMD_NATIVE_STOP_AT_RESOLVE");
  g_stop_after = uint32_t(std::max(0, EnvInt("XMD_NATIVE_STOP_AFTER_RESOLVE", 0)));
  if (const char* v = std::getenv("XMD_NATIVE_DEBUG_SOLID")) g_debug_solid = !std::strcmp(v, "1") || !std::strcmp(v, "2");
  if (const char* v = std::getenv("XMD_NATIVE_DEBUG_SOLID")) g_debug_solid_state = g_debug_solid && std::strcmp(v, "2") != 0;
  g_no_depth = EnvFlag("XMD_NATIVE_NO_DEPTH");
  g_no_blend = EnvFlag("XMD_NATIVE_NO_BLEND");
  {
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, "XMD_NATIVE_DUMP_FRAME") == 0 && v) {
      g_dump_frame = std::strtoull(v, nullptr, 10);
      free(v);
    }
    if (_dupenv_s(&v, &n, "XMD_NATIVE_DUMP_FIRST") == 0 && v) {
      g_dump_first = std::strtoul(v, nullptr, 10);
      free(v);
    }
    if (_dupenv_s(&v, &n, "XMD_NATIVE_DUMP_COUNT") == 0 && v) {
      g_dump_count = std::strtoul(v, nullptr, 10);
      free(v);
    }
  }
  std::string device_name;
  r->api = backend::CreateInterface(&device_name);
  if (!r->api) {
    REXLOG_ERROR("native renderer: no graphics API");
    return false;
  }
  r->device = r->api->createDevice(device_name);
  if (!r->device) {
    REXLOG_ERROR("native renderer: could not create the device");
    return false;
  }
  REXLOG_INFO("native renderer: GPU {}", r->device->getDescription().name);
#if defined(_WIN32)
  {  // (for reports: what a GPU that runs out of memory had)
    MEMORYSTATUSEX m = {sizeof(m)};
    GlobalMemoryStatusEx(&m);
    REXLOG_INFO("native renderer: video memory {} MB; system memory {} MB free of {} MB, page file {} MB free of {} MB",
                VideoMemoryBytes() >> 20, m.ullAvailPhys >> 20, m.ullTotalPhys >> 20, m.ullAvailPageFile >> 20,
                m.ullTotalPageFile >> 20);
  }
#endif
  backend::LoadPipelineCache(r->device.get(), PipelineCacheFile());
  if (g_debug_solid) g_debug_ps = ReadShader("debug_solid.ps");
  r->queue = r->device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  for (uint32_t i = 0; i < kFrames; ++i) {
    r->lists[i] = r->queue->createCommandList();
    r->fences[i] = r->device->createCommandFence();
    r->rings[i].buffer = r->device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        kRingSize + kRingSlack,
        plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::CONSTANT |
            plume::RenderBufferFlag::STORAGE));
    if (!r->lists[i] || !r->fences[i] || !r->rings[i].buffer) {
      REXLOG_ERROR("native renderer: could not create the frame resources");
      return false;
    }
    r->rings[i].cpu = static_cast<uint8_t*>(r->rings[i].buffer->map());
    r->rings[i].buffer->setName(fmt::format("upload ring {}", i));
    backend::LogBuffer(r->rings[i].buffer.get(), kRingSize + kRingSlack);
  }
  if (!CreatePipelineLayout(r)) return false;
  if (!CreateOutputs(r)) return false;
  // Sampler 0: linear wrap (for fetch slots without a texture).
  r->default_sampler = r->device->createSampler(plume::RenderSamplerDesc());
  r->sampler_set->setSampler(0, r->default_sampler.get());

  // The setup work (placeholder uploads), submitted and waited for.
  r->list = r->lists[0].get();
  r->list->begin();
  r->null_2d = CreatePlaceholder(r, 0);
  r->null_3d = CreatePlaceholder(r, 1);
  r->null_cube = CreatePlaceholder(r, 2);
  r->list->end();
  r->queue->executeCommandLists(r->list, r->fences[0].get());
  r->queue->waitForCommandFence(r->fences[0].get());
  r->garbage.clear();
  r->zero_buffer = r->device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256, plume::RenderBufferFlag::VERTEX));
  std::memset(r->zero_buffer->map(), 0, 256);
  r->zero_buffer->unmap();

  // (the texture cache's places: after the three placeholders and FSR 1's intermediate - kFsrSrv)
  textures::Initialize(g_memory, kFsrSrv + 1, kSrvHeapSize - (kFsrSrv + 1), 1, kSamplerHeapSize - 1);
  g_r = r;
#if XMD_DLSS
  // (DLSS where it can run: NGX started on this device once - the GRAPHICS page asks UpscalerAvailable)
  if (backend::ActiveApi() == backend::Api::kD3D12 || backend::ActiveApi() == backend::Api::kVulkan)
    xmd::dlss::Probe(DlssHost(r), nullptr);
#endif
  if (REXCVAR_GET(native_record_thread)) {
    r->recorder = std::make_unique<RecordingList>();
    REXLOG_INFO("native renderer: GPU commands recorded on a second thread");
  }
  REXLOG_INFO("native renderer: ready; shaders from {}", ShaderDirectory().string());
  StartPrebuildingPipelines(r);
  // Without its converted shaders (native_shaders\ beside the exe, installed
  // by the launcher) nothing could be drawn: a black screen. Let the emulated
  // renderer draw instead.
  {
    std::error_code ec;
    if (!ShaderExists("present.vs")) {
      // (what is there: a missing folder, an empty one or one without the pack tells copying apart from
      // something removing files - a 2.1.3 player had the game in Pictures and no pack)
      auto utf8 = [](const std::filesystem::path& p) {  // (string() throws on names outside the code page)
        const auto u = p.u8string();
        return std::string(u.begin(), u.end());
      };
      const std::filesystem::path dir = ShaderDirectory();
      const std::filesystem::path pack = dir / (std::string("shaders") + backend::ShaderExtension() + ".pak");
      uint32_t files = 0;
      for (const auto& e : std::filesystem::directory_iterator(dir, ec)) files += e.is_regular_file() ? 1 : 0;
      const bool pack_there = std::filesystem::exists(pack, ec);
      REXLOG_ERROR("native renderer: its shaders are missing ({} has no present.vs{}; folder {}, {} files, {} {}) - "
                   "reinstall with the launcher to get the native_shaders folder",
                   utf8(dir), backend::ShaderExtension(), std::filesystem::exists(dir, ec) ? "there" : "missing",
                   files, utf8(pack.filename()),
                   pack_there ? std::to_string(std::filesystem::file_size(pack, ec)) + " bytes (unreadable)"
                              : std::string("missing"));
      std::string reason = "its shaders are missing (" + utf8(dir) + ")";
#ifdef _WIN32
      // A game folder in the user's Pictures / Documents / Desktop or OneDrive: Windows Controlled folder access
      // blocks programs writing there and OneDrive can keep files online-only or move them.
      std::string lower = utf8(rex::filesystem::GetExecutableFolder());
      for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
      for (const char* place : {"\\pictures", "\\documents", "\\desktop", "\\onedrive"}) {
        if (lower.find(place) != std::string::npos) {
          reason += " - the game is in " + utf8(rex::filesystem::GetExecutableFolder()) +
                    ", where Windows Controlled folder access or OneDrive can block or remove its files";
          break;
        }
      }
#endif
      Fail(reason.c_str());
    }
  }
  return true;
}

// The device lost: in the first minute on the API gpu_backend "any" picked (a 2.1.8 report: GeForce GT 710, driver
// 456.71, Direct3D 12 removed - DXGI_ERROR_DRIVER_INTERNAL_ERROR - before the first frame, every start), the next
// start uses the next one (gpu_probe.h), and the message says so.
std::string DeviceLostReason(Renderer* r) {
  std::string reason = "the GPU stopped (device lost)";
  if (r->frames < 3600) {
    if (const std::string next = xmd::AvoidPickedGraphicsApi(fmt::format("device lost at frame {}", r->frames));
        !next.empty()) {
      reason += " - restart the game: it will use " + next + " on this GPU; updating the graphics driver can also help";
      REXLOG_WARN("native renderer: the next start uses {} (gpu_backend any)", next);
    }
  }
  return reason;
}

// The GPU faulted (device removed): logs why (and the faulting allocation,
// when the driver tells) and turns the native renderer off for the rest of
// the run.
void ReportDeviceRemoved(Renderer* r) {
  backend::ReportDeviceLost(r->device.get());
  Fail(DeviceLostReason(r).c_str());
}

// Waits (CPU) for frame slot `slot`'s last submission. A GPU that stops
// answering must not hang the game: under Proton (vkd3d-proton) a stuck GPU
// job can leave the fence short for good, where Windows would remove the
// device. After kGpuTimeoutMs the native renderer gives up - the emulated
// renderer takes over, and the fence is set from the CPU so the emulator's
// queue, which waits on it for the frame, runs on.
bool WaitForFrame(Renderer* r, uint32_t slot, const char* what) {
  if (!r->submitted[slot]) return true;
  if (r->queue->waitForCommandFenceTimeout(r->fences[slot].get(), kGpuTimeoutMs)) {
    r->submitted[slot] = false;
    return true;
  }
  bool lost = backend::DeviceLost(r->device.get());
  // [xmd] Late but alive (no device lost): a frame too heavy for this GPU - the first one after the render scale
  // went up, mostly - is waited for (kGpuPatienceMs in all), and the render scale comes down a step for the
  // next frames, instead of the renderer giving up for the rest of the session on one slow frame.
  if (!lost) {
    REXLOG_WARN("native renderer: the GPU has not finished ({}) after {} ms at render scale {}x - waiting on", what,
                kGpuTimeoutMs, g_scale);
    for (uint32_t waited = kGpuTimeoutMs; !lost && waited < kGpuPatienceMs; waited += 1000) {
      if (r->queue->waitForCommandFenceTimeout(r->fences[slot].get(), 1000)) {
        r->submitted[slot] = false;
        if (g_scale > 1 && g_memory_scale_cap >= g_scale) {
          g_memory_scale_cap = g_scale - 1;
          REXLOG_WARN("native renderer: that frame took the GPU over {} ms - render scale lowered to {}x", waited,
                      g_memory_scale_cap);
        }
        return true;
      }
      lost = backend::DeviceLost(r->device.get());
    }
  }
  REXLOG_ERROR("native renderer: GPU did not finish ({}) in {} ms{}", what, lost ? kGpuTimeoutMs : kGpuPatienceMs,
               lost ? " - device lost" : "");
  if (lost) backend::ReportDeviceLost(r->device.get());
  Fail(lost ? DeviceLostReason(r).c_str() : "the GPU stopped answering");
  // Releases the emulator's waits for the frames in flight (each slot's).
  for (auto& fence : r->fences) backend::ReleaseFence(fence.get());
  return false;
}

// Waits for every frame in flight.
bool WaitIdle(Renderer* r, const char* what) {
  for (uint32_t i = 0; i < kFrames; ++i)
    if (!WaitForFrame(r, i, what)) return false;
  return true;
}

Renderer* Get() {
  std::call_once(g_init_once, [] {
    if (!Initialize()) {
      const std::string reason = backend::FailReason();
      Fail(!reason.empty() ? reason.c_str()
                           : "the GPU or its graphics driver lacks what it needs (the log's GPU report says what)");
    }
  });
  return g_failed ? nullptr : g_r;
}

void Barrier(Renderer* r, plume::RenderTexture* texture, RenderTextureLayout layout) {
  const RenderBarrierStage::Bits stages =
      layout == RenderTextureLayout::SHADER_READ ? RenderBarrierStage::GRAPHICS_AND_COMPUTE
      : layout == RenderTextureLayout::COPY_SOURCE || layout == RenderTextureLayout::COPY_DEST
          ? RenderBarrierStage::COPY
          : RenderBarrierStage::GRAPHICS;
  r->list->barriers(stages, plume::RenderTextureBarrier(texture, layout));
}

// Allocates `size` bytes of this frame's upload ring.
std::pair<uint8_t*, RenderBufferReference> Allocate(Renderer* r, uint64_t size, uint64_t align = 256) {
  UploadRing& ring = r->rings[r->back_index];
  ring.offset = (ring.offset + align - 1) & ~(align - 1);
  if (ring.offset + size > kRingSize) return {nullptr, {}};
  auto result = std::make_pair(ring.cpu + ring.offset, ring.buffer->at(ring.offset));
  ring.offset += size;
  return result;
}

textures::Context TextureContext(Renderer* r) {
  textures::Context ctx;
  ctx.device = r->device.get();
  ctx.list = r->list;
  for (int i = 0; i < 3; ++i) {
    ctx.texture_sets[i] = r->texture_set[i];
    ctx.texture_base[i] = r->texture_base[i];
  }
  ctx.sampler_set = r->sampler_set.get();
  ctx.frame = r->frames;
  ctx.retire = [r](std::shared_ptr<void> object) { Retire(r, std::move(object)); };
  ctx.allocate = [r](uint64_t size, uint64_t align) -> textures::Context::UploadSpace {
    auto [cpu, ref] = Allocate(r, size, align);
    if (!cpu) return {nullptr, nullptr, 0};
    return {cpu, const_cast<plume::RenderBuffer*>(ref.ref), ref.offset};
  };
  return ctx;
}

// Main mode, at a frame's start: the frame images follow the window (its
// 16:9 area) and the render scale is the smallest that covers them - twice
// over with anti-aliasing (supersampling, averaged down by Present), at
// most 4x. A change rebuilds the scaled resources once the GPU is idle.
// [xmd] Render scales above 4 (5K / 8K screens): the largest scaled
// target must stay within 16384 a side (D3D11 / D3D12 / Vulkan alike), and
// the scaled targets' memory within half the GPU's own memory.
constexpr uint32_t kMaxRenderScale = 8;
constexpr float kMaxTargetSide = 16384.0f;
// (bytes a scene pixel costs at a scale: the scene's colour and depth
// targets, their resolve copies and the frame images - measured: see the
// 8K commit)
constexpr uint64_t kBytesPerScenePixel = 48;

// The GPU's own memory (the largest hardware adapter's; 0: unknown - no guard).
uint64_t VideoMemoryBytes() {
#if defined(_WIN32)
  static const uint64_t bytes = [] {
    uint64_t best = 0;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dxgi ? reinterpret_cast<Create>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
      IDXGIAdapter1* adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
          best = std::max<uint64_t>(best, desc.DedicatedVideoMemory);
        adapter->Release();
      }
      factory->Release();
    }
    return best;
  }();
  return bytes;
#elif defined(__ANDROID__)
  // (phones: the memory the driver gives the GPU - VK_EXT_memory_budget, shared with the game - at the start)
  static const uint64_t bytes = [] {
    uint64_t used = 0, budget = 0;
    return backend::VideoMemory(&used, &budget) ? budget : uint64_t(0);
  }();
  return bytes;
#else
  return 0;
#endif
}

// The highest scale up to `s` that fits (`why`: what lowered it).
// [xmd] Phones (supersampling above the screen): the GPU's own image limit
// (Mali / Adreno: 8192-16384) and a quarter of the GPU memory budget - the
// game's own textures and buffers share it - from 2x up.
uint32_t FitRenderScale(uint32_t s, float wide, float tall, const char*& why) {
  const float tw = float(kWidth) * wide, th = float(kHeight) * tall;
  float side = kMaxTargetSide;
  if (const uint32_t gpu_side = backend::VulkanMaxImageSide()) side = std::min(side, float(gpu_side));
  while (s > 1 && (tw * float(s) > side || th * float(s) > side)) --s, why = "the GPU's texture size limit";
  if (s > g_memory_scale_cap) s = g_memory_scale_cap, why = "video memory full or a frame too slow for the GPU";
#if defined(__ANDROID__)
  constexpr uint32_t kMemoryFrom = 1, kShare = 4;
#else
  // [xmd] From 2x up (was: above 4x only - a chosen 1440 lines on a 2 GB card was never checked), and with the
  // engine's other buffers counted: up to 4160 x 4160 (kMaxEffectSide) of colour, depth and resolve copy.
  constexpr uint32_t kMemoryFrom = 1, kShare = 2;
#endif
  auto effects = [](uint32_t scale) {
    const uint64_t side = std::min<uint64_t>(2080ull * scale, kMaxEffectSide);
    return side * side * 24;
  };
  if (const uint64_t vram = VideoMemoryBytes())
    while (s > kMemoryFrom &&
           uint64_t(tw * float(s)) * uint64_t(th * float(s)) * kBytesPerScenePixel + effects(s) > vram / kShare)
      --s, why = "video memory";
  return s;
}

// A target or the frame images couldn't be made (out of memory - a 2.1.8 report, GeForce GT 710: the 2x targets of
// a 1366x768 window, then a crash): above 1x the next frame lowers the render scale a step and makes them again; the
// frame images at 1x: nothing can be shown - the message says why instead of a crash.
void OutOfVideoMemory(Renderer* r, const char* what) {
  if (g_failed) return;
  if (g_scale > 1) {
    if (g_memory_scale_cap < g_scale) return;  // (lowered already: the next frame)
    g_memory_scale_cap = g_scale - 1;
    REXLOG_WARN("native renderer: {} could not be made at render scale {}x (out of memory; video memory {} MB) - "
                "lowered to {}x", what, g_scale, VideoMemoryBytes() >> 20, g_memory_scale_cap);
    return;
  }
  if (std::strcmp(what, "the frame images") != 0) return;  // (a target at 1x: its draws skipped, as before)
  Fail(fmt::format("not enough memory for the frame images on {} ({} MB video memory) - close other programs, play "
                   "in a smaller window, set texture quality to low or update the graphics driver",
                   r->device->getDescription().name, VideoMemoryBytes() >> 20)
           .c_str());
}

void ApplyOutputSettings(Renderer* r) {
  uint32_t win_w = kWidth, win_h = kHeight;
  if (g_window_size) {
    const auto [w, h] = g_window_size();
    if (w >= 64 && h >= 64) win_w = w, win_h = h;
  }
  // (test: XMD_TEST_OUTPUT_SIZE=7680x4320 - render for a window that size
  // on any screen: Windows keeps a window within about the desktop's size)
  static const std::pair<uint32_t, uint32_t> test_size = [] {
    unsigned w = 0, h = 0;
    const char* v = std::getenv("XMD_TEST_OUTPUT_SIZE");
    return v && std::sscanf(v, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 ? std::pair<uint32_t, uint32_t>{w, h}
                                                                         : std::pair<uint32_t, uint32_t>{0, 0};
  }();
  if (test_size.first) win_w = test_size.first, win_h = test_size.second;
  static bool widescreen = REXCVAR_GET(native_widescreen);
  // A match on a window wider than 16:9: as wide as the window.
  float wide = 1.0f;
  float tall = 1.0f;
  if (widescreen && g_match_scene.load() && uint64_t(win_w) * 9 > uint64_t(win_h) * 16 + 16) {
    wide = std::min(float(win_w) / float(win_h) / kAspect16x9, kMaxWide);
  } else if (widescreen && g_match_scene.load() && uint64_t(win_w) * 9 + 16 < uint64_t(win_h) * 16) {
    tall = std::min(float(win_h) / float(win_w) * kAspect16x9, kMaxWide);  // (up to 8:9)
  }
  uint32_t out_w = win_w, out_h = win_w * 9 / 16;
  if (out_h > win_h) out_h = win_h, out_w = win_h * 16 / 9;
  if (wide > 1.0f) out_h = win_h, out_w = std::min(win_w, uint32_t(std::lround(win_h * kAspect16x9 * wide)));
  if (tall > 1.0f) out_w = win_w, out_h = std::min(win_h, uint32_t(std::lround(win_w / kAspect16x9 * tall)));
  // (the frame images too stay within 16384 a side: a bigger window - wider
  // than any screen - gets them that size, the same shape, scaled up)
  if (float(out_w) > kMaxTargetSide || float(out_h) > kMaxTargetSide) {
    const float k = kMaxTargetSide / float(std::max(out_w, out_h));
    out_w = uint32_t(float(out_w) * k), out_h = uint32_t(float(out_h) * k);
  }
  out_w = std::max(out_w & ~1u, 64u);
  out_h = std::max(out_h & ~1u, 36u);
  // Anti-aliasing: native_aa, else the older on / off setting (on = 2x).
  auto aa_level = [] {
    const int32_t level = REXCVAR_GET(native_aa);
    return level > 0 ? uint32_t(std::clamp<int32_t>(level, 1, 4))
                     : (rex::cvar::Query<bool>("native_2x_msaa") ? 2u : 1u);
  };
  static uint32_t aa = aa_level();
  static int32_t max_scale = REXCVAR_GET(native_max_scale);
  static bool effects = REXCVAR_GET(native_scale_effects);
  static const bool shadows_once = (g_shadows = REXCVAR_GET(native_shadows));
  (void)shadows_once;
  static float res = float(REXCVAR_GET(native_render_scale));
  static int32_t scene_height = REXCVAR_GET(native_scene_height);
  static bool fsr = REXCVAR_GET(native_upscale) != "off";
  static bool fsr2 = REXCVAR_GET(native_upscale) == "fsr2" || REXCVAR_GET(native_upscale) == "dlss";
  static bool dlss = REXCVAR_GET(native_upscale) == "dlss";
  static int32_t fsr_quality = REXCVAR_GET(native_upscale_quality);
  static uint64_t aa_checked = 0;
  if (r->frames >= aa_checked + 30) {  // a cvar query isn't free: twice a second
    fsr = REXCVAR_GET(native_upscale) != "off";
    fsr2 = REXCVAR_GET(native_upscale) == "fsr2" || REXCVAR_GET(native_upscale) == "dlss";
    dlss = REXCVAR_GET(native_upscale) == "dlss";
    fsr_quality = REXCVAR_GET(native_upscale_quality);
    g_fsr_sharpness = float(std::clamp(REXCVAR_GET(native_upscale_sharpness), 0.0, 1.0));
    aa = aa_level();
    max_scale = REXCVAR_GET(native_max_scale);
    effects = REXCVAR_GET(native_scale_effects);
    g_shadows = REXCVAR_GET(native_shadows);
    res = float(REXCVAR_GET(native_render_scale));
    scene_height = REXCVAR_GET(native_scene_height);
    widescreen = REXCVAR_GET(native_widescreen);
    aa_checked = r->frames;
  }
  // Enough guest pixels for every output pixel - for `aa` per axis with
  // anti-aliasing (2x2 supersampling at 1080p: 3x; up to native_max_scale).
  // Direct3D 11 (older GPUs): AUTO anti-aliasing (native_aa 0) means the
  // Xbox 360's own 720p - supersampling would sink them. Choosing 2x-4x still
  // raises it.
  const bool d3d11_auto = SlotTables() && REXCVAR_GET(native_aa) == 0;
  if (d3d11_auto) {
    aa = 1;
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("native renderer: Direct3D 11 - render scale 1x (anti-aliasing AUTO; choose 2x-4x to raise it)");
    }
  }
  // AUTO (native_aa 0, no height chosen, no upscaler): the scene at the window's own resolution, pixel for pixel -
  // the 720p step above it with the scene's share of it (native_render_scale's way). Before, a 1368-line window
  // got 1440 lines scaled down by 5%: every pixel a blend of two, the whole picture soft, for four samples in
  // eighty. The game's own edge smoothing pass still runs; native_aa 2-4 supersamples on top.
  int32_t wanted_height = scene_height;
  if (!d3d11_auto && REXCVAR_GET(native_aa) == 0 && scene_height <= 0 && !fsr) {
    aa = 1;
    wanted_height = int32_t(out_h);
  }
  const uint32_t need = out_h * aa;
  // (4 was AUTO before 8K: the same now. Above 4 only for the window's own
  // lines - a 5K / 8K screen - never more supersampling than before.)
  const int32_t asked = max_scale == 4 ? int32_t(kMaxRenderScale) : max_scale;
  const uint32_t window_scale = (out_h + kHeight - 1) / kHeight;
  const uint32_t limit = d3d11_auto ? 1u
                                    : std::min<uint32_t>(uint32_t(std::clamp<int32_t>(asked, 1, int32_t(kMaxRenderScale))),
                                                         std::max<uint32_t>(4u, window_scale));
  uint32_t scale = std::clamp<uint32_t>((need + kHeight - 1) / kHeight, 1, limit);
  const bool wide_changed = std::fabs(wide - g_wide) > 0.002f || std::fabs(tall - g_tall) > 0.002f;
  res = std::clamp(res, 0.25f, 1.0f);
  if (scale > 1) res = 1.0f;  // (only below the Xbox 360's own resolution)
  // FSR 1: the scene at the quality's share of the window's height (the 720p step above it, scaled down to it), no
  // supersampling; Present scales it up (EASU) and sharpens it (RCAS).
  g_fsr = false;
  if (fsr) {
    constexpr float kShare[4] = {1.0f / 1.3f, 1.0f / 1.5f, 1.0f / 1.7f, 1.0f / 2.0f};
    const uint32_t h = std::clamp<uint32_t>(uint32_t(std::lround(float(out_h) * kShare[std::clamp(fsr_quality, 0, 3)])),
                                            kHeight / 4, kHeight * kMaxRenderScale);
    scale = std::min<uint32_t>((h + kHeight - 1) / kHeight, kMaxRenderScale);
    res = std::clamp(float(h) / float(kHeight * scale), 0.25f, 1.0f);
    g_fsr = h < out_h;
  } else if (wanted_height > 0) {  // A chosen height (1080P): the 720p step above it, the scene scaled down to it.
    // (anti-aliasing multiplies the chosen height - 1080P at 2X draws 2160 lines and Present averages them down -
    // up to AUTO's most, 4x or the window's own; it used to be ignored here: 900P / 1080P / 2880P had no AA at all)
    const uint64_t with_aa = uint64_t(wanted_height) * aa;
    const uint64_t cap = std::max<uint64_t>(uint64_t(kHeight) * std::max<uint32_t>(4u, window_scale), uint64_t(wanted_height));
    const uint32_t h = uint32_t(std::clamp<uint64_t>(std::min(with_aa, cap), kHeight / 4, uint64_t(kHeight) * kMaxRenderScale));
    scale = (h + kHeight - 1) / kHeight;
    res = std::clamp(float(h) / float(kHeight * scale), 0.25f, 1.0f);
  }
  // Large scales (5K / 8K): what the GPU can hold; the window is filled by
  // scaling the scene up (or FSR, when it's on).
  const char* fit_why = nullptr;
  const uint32_t wanted_scale = scale;
  scale = FitRenderScale(scale, wide, tall, fit_why);
  if (scale != wanted_scale) res = 1.0f;  // (the whole of the lower scale)
  // (FSR 2: Direct3D 12 / Vulkan - compute; not with the commands recorded on a second thread)
#if XMD_FSR2
  g_fsr2 = fsr2 && g_fsr && !SlotTables() && !r->recorder && !r->fsr2_failed;
#if XMD_DLSS
  g_dlss = g_fsr2 && dlss && !r->dlss_failed;
#endif
#endif
  const bool res_changed = std::fabs(res - g_res) > 0.001f;
  if (scale == g_scale && effects == g_scale_effects && out_w == g_out_w && out_h == g_out_h &&
      !wide_changed && !res_changed && r->outputs[0]) {
    return;
  }

  // Idle: nothing in flight may still use what is replaced.
  if (!WaitIdle(r, "output change")) return;
  r->garbage.clear();
  if (scale != g_scale || effects != g_scale_effects || wide_changed || res_changed) {
    g_res = res;
    textures::ForgetResolved();
    r->color_targets.clear();
    r->depth_targets.clear();
    r->resolved.clear();
    r->resolved_spares.clear();
    r->depth_crops.clear();
    r->main_target = nullptr;
    r->present_source = 0;
    g_scale = scale;
    g_scale_effects = effects;
    g_wide = wide;
    g_tall = tall;
    SetCameraAspect(kAspect16x9 * wide / tall);
    g_fov_scale = tall;
  }
  if (out_w != g_out_w || out_h != g_out_h || !r->outputs[0]) {
    g_out_w = out_w;
    g_out_h = out_h;
    if (!CreateOutputs(r)) OutOfVideoMemory(r, "the frame images");
  }
  r->list_state = {};
  if (fit_why)
    REXLOG_INFO("native renderer: render scale {} instead of {} for {}x{} ({}; video memory {} MB)", scale,
                wanted_scale, g_out_w, g_out_h, fit_why, VideoMemoryBytes() >> 20);
  REXLOG_INFO("native renderer: output {}x{}, render scale {}x{}{}{}{}{}", g_out_w, g_out_h, g_scale,
              g_res < 1.0f ? fmt::format(" (scene at {:.0f}%)", g_res * 100) : std::string(),
              g_fsr ? fmt::format(", FSR {} from {}p", g_fsr2 ? 2 : 1, uint32_t(std::lround(kHeight * g_scale * g_res)))
                    : std::string(),
              aa > 1 ? fmt::format(" (anti-aliasing {}x)", aa) : std::string(), g_scale_effects ? "" : ", effects unscaled",
              g_wide > 1.0f   ? fmt::format(", wide {:.3f} (aspect {:.3f})", g_wide, kAspect16x9 * g_wide)
              : g_tall > 1.0f ? fmt::format(", tall {:.3f} (aspect {:.3f})", g_tall, kAspect16x9 / g_tall)
                              : std::string());
}

// Texture quality (native_texture_quality, twice a second): a change waits
// for the GPU, then the textures are uploaded again as they're used.
void ApplyTextureQuality(Renderer* r) {
  static uint64_t checked = 0;
  static uint32_t applied = 0;
  if (r->frames && r->frames < checked + 30) return;
  checked = r->frames;
  const std::string q = REXCVAR_GET(native_texture_quality);
  const uint32_t skip = q == "low" ? 2 : q == "medium" ? 1 : 0;
  if (skip == applied) return;
  if (!WaitIdle(r, "texture quality change")) return;
  r->garbage.clear();
  textures::SetQuality(skip);
  applied = skip;
}

// False: the GPU stopped answering and the native renderer gave up.
// 30 fps with the world at 60 Hz (frame_rate.cpp): every other frame isn't
// shown and its draws and clears aren't made (the GPU work of the frames no
// one sees); resolves still run (copies of what the last shown frame drew).
std::atomic<bool> g_half_frames{false};
std::atomic<bool> g_half_suspended{false};  // (the PC can't keep 60 Hz: every frame shown - SetHalfSuspended)
std::atomic<bool> g_last_hidden{false};  // (OnPresent's frame wasn't shown)

bool BeginFrame(Renderer* r) {
  if (r->frame_open) return true;
  if (g_main) ApplyTextureQuality(r);
  if (g_main) ApplyOutputSettings(r);
  if (g_failed || !r->outputs[0]) return false;  // (no frame images: out of memory - made again at the next frame)
  r->back_index = uint32_t(r->frames % kFrames);
  r->output_index = uint32_t(r->frames % kOutputs);
  r->frame_begin = std::chrono::steady_clock::now();
  if (r->submitted[r->back_index]) {
    ScopeTimer wait(r->perf_wait_ms);
    if (!WaitForFrame(r, r->back_index, "previous frame")) return false;
  }
  // The frame kFrames back (this slot's last) has finished, and so have the
  // ones before it.
  while (!r->garbage.empty() && r->garbage.front().first + kFrames <= r->frames) {
    r->garbage.pop_front();
  }
  r->list = r->lists[r->back_index].get();
  if (r->recorder) {
    r->recorder->SetTarget(r->list);
    r->list = r->recorder.get();
  }
  r->list->begin();
  r->list_state = {};
  r->constant_uploads[0] = r->constant_uploads[1] = {};
  r->shared_upload = r->own_upload = {};
  g_stream_words_valid = false;
  r->rings[r->back_index].offset = 0;
  r->frame_open = true;
  r->hidden_frame = g_half_frames.load() && !g_half_suspended.load() && (r->frames & 1);
  r->frame_stats = {};
  r->texture_context = TextureContext(r);
  if (!r->hidden_frame) {
    textures::UploadChanging(r->texture_context);
    textures::EvictUnused(r->texture_context);
  }
  g_resolved_this_frame = false;
  g_stop_ps_hit = false;
  g_resolve_count = 0;
  g_stopped = false;
  return true;
}

uint32_t g_write_backs = 0, g_write_back_w = 0, g_write_back_h = 0;  // (native perf line)
double g_write_back_ms = 0;
std::mutex g_costs_mutex;  // (LastFrameCosts: the overlay's)
FrameCosts g_costs;
double g_resident_mb = 0;  // (the textures' host memory at the last 5 s perf log)

int BlankedSlot(Renderer* r);

std::atomic<uint32_t> g_last_draws{0};
std::atomic<uint64_t> g_frames_shown{0};

void UpdateTitle(Renderer* r) {
  g_last_draws = r->frame_stats.draws;
  ++g_frames_shown;
  ++g_shader_frame;
  ++r->stat_frames;
  ++r->perf_frames;
  r->perf_draws += r->frame_stats.draws;
  r->perf_drawn += r->frame_stats.drawn;
  r->perf_span_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               r->frame_begin).count();
  Stats& w = r->window_stats;
  const Stats& f = r->frame_stats;
  w.draws += f.draws;
  w.drawn += f.drawn;
  w.skipped_target += f.skipped_target;
  w.skipped_shader += f.skipped_shader;
  w.skipped_prim += f.skipped_prim;
  // (what became of the game's draws, logged with the performance line every 5 s)
  static Stats fate;
  fate.draws += f.draws, fate.drawn += f.drawn, fate.skipped_target += f.skipped_target;
  fate.skipped_shader += f.skipped_shader, fate.skipped_prim += f.skipped_prim, fate.resolves += f.resolves;
  const auto now = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(now - r->stat_start).count();
  if (secs < 0.5) return;
#if defined(__x86_64__) || defined(_M_X64)
  {
    static uint64_t last_ticks = 0;
    static std::chrono::steady_clock::time_point last_time;
    const uint64_t ticks = __rdtsc();
    if (last_ticks && ticks > last_ticks)
      g_ms_per_tick = std::chrono::duration<double, std::milli>(now - last_time).count() / double(ticks - last_ticks);
    last_ticks = ticks, last_time = now;
  }
#endif
  {  // (the overlay's per-frame costs: the deltas since the last half second, the 5 s totals still counting)
    static double last_draw = 0, last_wait = 0, last_submit = 0, last_draws = 0;
    static uint32_t last_frames = 0;
    if (r->perf_frames < last_frames) last_draw = last_wait = last_submit = last_draws = 0, last_frames = 0;  // (reset)
    if (const uint32_t n = r->perf_frames - last_frames) {
      FrameCosts c;
      c.draw_ms = (r->perf_draw_ms - last_draw) / n, c.gpu_wait_ms = (r->perf_wait_ms - last_wait) / n;
      c.submit_ms = (r->perf_submit_ms - last_submit) / n, c.draws = (double(r->perf_draws) - last_draws) / n;
      c.texture_mb = textures::ResidentBytes() / 1048576.0;
      c.textures = textures::ResidentCount();
      for (const auto& [k, t] : r->color_targets) c.targets_mb += double(t.host_w) * t.host_h * 4 / 1048576.0;
      for (const auto& [k, t] : r->depth_targets) c.targets_mb += double(t.host_w) * t.host_h * 4 / 1048576.0;
      std::lock_guard lock(g_costs_mutex);
      g_costs = c;
    }
    last_draw = r->perf_draw_ms, last_wait = r->perf_wait_ms, last_submit = r->perf_submit_ms;
    last_draws = double(r->perf_draws), last_frames = r->perf_frames;
  }
  const double psecs = std::chrono::duration<double>(now - r->perf_start).count();
  if (psecs >= 5.0 && r->perf_frames) {
    REXLOG_INFO("native draws per frame: {:.1f} asked, {:.1f} drawn; not drawn: {:.1f} no target, {:.1f} no shader, "
                "{:.1f} primitive type; {:.1f} resolves",
                fate.draws / double(r->perf_frames), fate.drawn / double(r->perf_frames),
                fate.skipped_target / double(r->perf_frames), fate.skipped_shader / double(r->perf_frames),
                fate.skipped_prim / double(r->perf_frames), fate.resolves / double(r->perf_frames));
    fate = Stats();
    if (g_draw_times) {
      const double n = double(r->perf_frames);
      REXLOG_INFO("native draw times per frame: shaders {:.2f} ms, layout {:.2f}, pipeline {:.2f}, vertex data {:.2f}, "
                  "indices {:.2f}, state {:.2f}, constants and textures {:.2f}, rest {:.2f}",
                  g_draw_time[0] / n, g_draw_time[1] / n, g_draw_time[2] / n, g_draw_time[3] / n, g_draw_time[4] / n,
                  g_draw_time[5] / n, g_draw_time[6] / n, g_draw_time[7] / n);
      for (double& t : g_draw_time) t = 0;
    }
    const double f = r->perf_frames;
    if (backend::ActiveApi() == backend::Api::kVulkan) {
      const uint32_t passes = plume::g_svr_render_passes.exchange(0);
      const uint64_t pixels = plume::g_svr_render_pass_pixels.exchange(0);
      REXLOG_INFO("native perf: render passes per frame {:.1f} ({:.1f} Mpixels loaded and stored)", passes / f,
                  pixels / f / 1e6);
#if defined(__SWITCH__)
      if (plume::g_svr_gpu_frame.ready.load(std::memory_order_acquire)) {
        // One frame's passes on the GPU: size, draws, ms (+ms since the pass before).
        const auto& gpu = plume::g_svr_gpu_frame;
        std::string line;
        double busy = 0;
        for (uint32_t i = 0; i < gpu.count; i++) {
          const auto& p = gpu.passes[i];
          busy += p.ms;
          line += fmt::format(" {}x{}/{}:{:.2f}", p.width, p.height, p.draws, p.ms);
          if (p.gap_ms >= 0.05) line += fmt::format("(+{:.2f})", p.gap_ms);
        }
        REXLOG_INFO("native perf: gpu passes: {:.2f} ms first to last, {:.2f} in {} passes (driver tick {} ns):{}", gpu.total_ms, busy,
                    gpu.count, gpu.period, line);
        plume::g_svr_gpu_frame.ready.store(false, std::memory_order_release);
      }
#endif
    }
#if defined(_WIN32)
    if (SlotTables()) {
      auto* d = static_cast<plume::D3D11Device*>(r->device.get());
      REXLOG_INFO("native perf: D3D11 per frame: uploads {:.2f} ms ({:.1f} MB), replay {:.2f} ms ({:.0f} draws), "
                  "context lock wait {:.2f} ms, flush {:.2f} ms; frame waits {} ({} without the context lock)",
                  d->statUploadNs.exchange(0) / f / 1e6, d->statUploadBytes.exchange(0) / f / 1e6,
                  d->statReplayNs.exchange(0) / f / 1e6, d->statDraws.exchange(0) / f,
                  d->statLockWaitNs.exchange(0) / f / 1e6, d->statFlushNs.exchange(0) / f / 1e6,
                  d->statFenceWaits.exchange(0), d->statFenceLockFree.exchange(0));
    }
#endif
    // (the processor time this thread - the game's render thread - used a frame: unlike the frame rate it says
    // the same on a busy PC, where a test game at low priority is kept waiting)
    double thread_ms = 0;
#if defined(_WIN32)
    {
      static uint64_t last_used = 0;
      FILETIME created, exited, kernel, user;
      if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        const uint64_t used = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                              (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
        if (last_used) thread_ms = double(used - last_used) / 1e4 / f;
        last_used = used;
      }
    }
#endif
    REXLOG_INFO("native perf: {:.1f} fps, per frame: draw {:.2f} ms, gpu wait {:.2f} ms, submit {:.2f} ms, "
                "span {:.2f} ms, draws {:.0f} (drawn {:.0f}), pipeline builds {:.0f} ms; thread cpu {:.2f} ms a frame",
                f / psecs, r->perf_draw_ms / f, r->perf_wait_ms / f, r->perf_submit_ms / f, r->perf_span_ms / f,
                r->perf_draws / f, r->perf_drawn / f, g_pipeline_ms, thread_ms);
    if (g_write_backs) {
      REXLOG_INFO("native perf: {} resolves written back to guest memory ({:.0f} ms waiting; last {}x{})",
                  g_write_backs, g_write_back_ms, g_write_back_w, g_write_back_h);
      g_write_backs = 0;
      g_write_back_ms = 0;
    }
    {
      const textures::Stats ts = textures::TakePerf();
      g_resident_mb = ts.resident_bytes / 1048576.0;
      REXLOG_INFO("native perf: textures: {} uploads ({:.1f} MB, {:.1f} ms, converting {:.1f} ms) in {:.1f} s; "
                  "{} resident ({:.1f} MB, {} with mip levels left out); {} seen written by the write watch",
                  ts.uploads_total, ts.upload_bytes / 1048576.0, ts.upload_ms, ts.convert_ms, psecs,
                  ts.textures, ts.resident_bytes / 1048576.0, ts.reduced, ts.watch_hits);
      REXLOG_INFO("native perf: hashed per frame: vertex buffers {:.0f} KB static + {:.0f} KB dynamic, "
                  "constants compared {:.0f} KB ({:.0f}% reused), textures {:.0f} KB; "
                  "buffers back to checks at every use {}; repeat checks {:.0f} unchanged / {:.0f} changed",
                  g_hash_vb_static / f / 1024, g_hash_vb_dynamic / f / 1024, g_hash_const / f / 1024,
                  r->perf_const_uploads + r->perf_const_reused
                      ? 100.0 * r->perf_const_reused / double(r->perf_const_uploads + r->perf_const_reused)
                      : 0.0,
                  ts.hash_bytes / f / 1024, g_vb_demotions, g_repeat_same / f, g_repeat_changed / f);
      g_repeat_same = g_repeat_changed = 0;
      g_hash_vb_static = g_hash_vb_dynamic = g_hash_const = 0;
      if (!g_vb_stats.empty()) {
        std::vector<std::pair<uint64_t, uint32_t>> top(g_vb_stats.begin(), g_vb_stats.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
          return uint64_t(a.second) * uint32_t(a.first) > uint64_t(b.second) * uint32_t(b.first);
        });
        for (size_t i = 0; i < top.size() && i < 6; ++i) {
          REXLOG_INFO("native perf: dynamic buffer {:08X} {} KB: {:.1f} hashed uses a frame",
                      uint32_t(top[i].first >> 32), uint32_t(top[i].first) / 1024, top[i].second / f);
        }
        g_vb_stats.clear();
      }
      r->perf_const_uploads = r->perf_const_reused = 0;
      if (g_const_times && g_ct_draws) {
        REXLOG_INFO("native perf: constants per frame: shared {:.3f} ms (texture lookups {:.3f}, sets {:.3f}), "
                    "gpu constants {:.3f} ms; {:.2f} textures a draw ({:.0f} draws); a slot's texture as its last "
                    "draw's {:.0f}%; kept answers {:.0f}%",
                    g_ct_shared_ns / f / 1e6, g_ct_tex_ns / f / 1e6, g_ct_sets_ns / f / 1e6, g_ct_gpu_ns / f / 1e6,
                    double(g_ct_textures) / double(g_ct_draws), g_ct_draws / f,
                    g_ct_textures ? 100.0 * g_ct_tex_same / g_ct_textures : 0.0,
                    g_ct_textures ? 100.0 * g_ct_memo_hits / g_ct_textures : 0.0);
      }
      g_ct_shared_ns = g_ct_tex_ns = g_ct_sets_ns = g_ct_gpu_ns = g_ct_textures = g_ct_draws = 0;
      g_ct_tex_same = g_ct_memo_hits = 0;
    }
    g_pipeline_ms = 0;
    r->perf_draw_ms = r->perf_wait_ms = r->perf_span_ms = r->perf_submit_ms = 0;
    r->perf_draws = r->perf_drawn = 0;
    r->perf_frames = 0;
    r->perf_start = now;
    // New pipelines, none for a few seconds (a scene loaded): keep them.
    if (g_pipeline_cache_dirty &&
        now - std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(g_last_pipeline_build.load())) >
            std::chrono::seconds(3)) {
      // Only in a light frame - a menu, a loading screen, a video (under 300 draws): serializing the library and
      // writing its file takes 200-600 ms inside the frame, and in play that was a stutter a few seconds after
      // every new effect or enemy (27 of them in a 25 minute session, 2026-11-19). Until then the new pipelines
      // are in the player's list (NotePipeline) anyway and are built ahead at the next start.
      // (not on a thread of its own: serializing while a pipeline is created on the render thread deadlocked)
      if (LastFrameDraws() < 300) {
        g_pipeline_cache_dirty = false;
        const auto save_start = std::chrono::steady_clock::now();
        backend::SavePipelineCache(r->device.get(), PipelineCacheFile());
        REXLOG_INFO("native renderer: pipeline library save took {:.0f} ms (a frame of {} draws)",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - save_start).count(),
                    LastFrameDraws());
      }
    }
  }
  r->stat_start = now;
  r->stat_frames = 0;
  r->window_stats = {};
}

// ---------------------------------------------------------------------------
// state translation

// ---------------------------------------------------------------------------
// render targets
//
// Each EDRAM colour surface the game renders to (base tile, pitch, format,
// height) gets a host render target, and each depth surface a D24S8 buffer
// (D32F S8 where the GPU has no D24S8: backend::DepthFormat);
// a Resolve copies a target into a texture the game then samples
// (textures::RegisterResolved), and Present shows the last full-screen
// colour resolve (the front buffer).

// Surface sizes from SetRenderTarget (the registers only hold the pitch):
// (pitch << 12 | EDRAM base tile) -> (width, height).
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_surface_sizes;

struct TargetFormat {
  RenderFormat resource, rtv, view, view_gamma;
  uint32_t components;
};

// Xenos colour render target format (RB_COLOR_INFO bits 16-19) -> host.
TargetFormat ColorFormat(uint32_t xenos) {
  switch (xenos) {
    case 1:  // 8_8_8_8_GAMMA: the hardware gamma-encodes on write
      return {RenderFormat::R8G8B8A8_TYPELESS, RenderFormat::R8G8B8A8_UNORM_SRGB,
              RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, 4};
    case 2:
    case 10:
      return {RenderFormat::R10G10B10A2_TYPELESS, RenderFormat::R10G10B10A2_UNORM,
              RenderFormat::R10G10B10A2_UNORM, RenderFormat::UNKNOWN, 4};
    case 3:  // 2_10_10_10_FLOAT (7e3)
    case 5:
    case 7:
    case 12:
      return {RenderFormat::R16G16B16A16_TYPELESS, RenderFormat::R16G16B16A16_FLOAT,
              RenderFormat::R16G16B16A16_FLOAT, RenderFormat::UNKNOWN, 4};
    case 4:
    case 6:
      return {RenderFormat::R16G16_TYPELESS, RenderFormat::R16G16_FLOAT, RenderFormat::R16G16_FLOAT,
              RenderFormat::UNKNOWN, 2};
    case 14:
      return {RenderFormat::R32_TYPELESS, RenderFormat::R32_FLOAT, RenderFormat::R32_FLOAT,
              RenderFormat::UNKNOWN, 1};
    case 15:
      return {RenderFormat::R32G32_TYPELESS, RenderFormat::R32G32_FLOAT, RenderFormat::R32G32_FLOAT,
              RenderFormat::UNKNOWN, 2};
    default:  // 0: 8_8_8_8
      return {RenderFormat::R8G8B8A8_TYPELESS, RenderFormat::R8G8B8A8_UNORM,
              RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, 4};
  }
}

// A barrier for a texture whose layout the renderer keeps (`layout`).
void Transition(Renderer* r, plume::RenderTexture* texture, RenderTextureLayout& layout,
                RenderTextureLayout to) {
  if (layout == to) return;
  Barrier(r, texture, to);
  layout = to;
}

ColorTarget* GetColorTarget(Renderer* r, uint32_t base, uint32_t pitch, uint32_t format,
                            uint32_t height) {
  const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(format) << 26) |
                       (uint64_t(height) << 32);
  if (auto it = r->color_targets.find(key); it != r->color_targets.end()) return &it->second;
  if (r->color_targets.size() >= kMaxColorTargets) return nullptr;
  const TargetFormat f = ColorFormat(format);
  const uint32_t scale = TargetScale(pitch, height);
  const uint32_t host_w = HostWidth(pitch, height, scale), host_h = HostHeight(pitch, height, scale);
  plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
      host_w, host_h, 1, f.resource, plume::RenderTextureFlag::RENDER_TARGET);
  d.committed = true;
  const plume::RenderClearValue cv = plume::RenderClearValue::Color(plume::RenderColor(0, 0, 0, 0), f.rtv);
  d.optimizedClearValue = &cv;
  std::shared_ptr<plume::RenderTexture> resource = TryCreateTexture(r, d);
  if (!resource) {
    if (TargetFailureLogged()) REXLOG_ERROR("native renderer: could not create a render target ({}x{})", host_w, host_h);
    OutOfVideoMemory(r, "a render target");
    return nullptr;
  }
  ColorTarget& t = r->color_targets[key];
  t.resource = std::move(resource);
  t.resource->setName(fmt::format("render target {}x{} fmt {} tile {}", pitch, height, format, base));
  t.resource_format = f.resource;
  t.format = f.rtv;
  t.view_format = f.view;
  t.view_gamma_format = f.view_gamma;
  t.components = f.components;
  t.width = pitch;
  t.height = height;
  t.scale = scale;
  t.host_w = host_w;
  t.host_h = host_h;
  t.rtv = t.resource->createTextureView(plume::RenderTextureViewDesc::Texture2D(f.rtv));
  REXLOG_INFO("native renderer: render target {}x{} format {} at EDRAM tile {}", pitch, height,
              format, base);
  return &t;
}

DepthTarget* GetDepthTarget(Renderer* r, uint32_t base, uint32_t pitch, uint32_t height) {
  const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(height) << 32);
  if (auto it = r->depth_targets.find(key); it != r->depth_targets.end()) return &it->second;
  if (r->depth_targets.size() >= kMaxDepthTargets) return nullptr;
  // D24S8 (or D32F S8: backend::DepthFormat), copyable to a sampled depth
  // texture (typeless underneath).
  const uint32_t scale = TargetScale(pitch, height);
  const uint32_t host_w = HostWidth(pitch, height, scale), host_h = HostHeight(pitch, height, scale);
  plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
      host_w, host_h, 1, backend::DepthFormat(),
      plume::RenderTextureFlag::DEPTH_TARGET);
  d.committed = true;
  const plume::RenderClearValue cv =
      plume::RenderClearValue::Depth(plume::RenderDepth(1.0f), backend::DepthFormat());
  d.optimizedClearValue = &cv;
  std::shared_ptr<plume::RenderTexture> resource = TryCreateTexture(r, d);
  if (!resource) {
    if (TargetFailureLogged()) REXLOG_ERROR("native renderer: could not create a depth target ({}x{})", host_w, host_h);
    OutOfVideoMemory(r, "a depth target");
    return nullptr;
  }
  DepthTarget& t = r->depth_targets[key];
  t.resource = std::move(resource);
  t.resource->setName(fmt::format("depth target {}x{} tile {}", pitch, height, base));
  t.width = pitch;
  t.height = height;
  t.scale = scale;
  t.host_w = host_w;
  t.host_h = host_h;
  return &t;
}

struct Targets {
  ColorTarget* color = nullptr;
  DepthTarget* depth = nullptr;
};

// The targets the EDRAM registers select now.
Targets CurrentTargets(Renderer* r) {
  const uint32_t si = Reg(RB_SURFACE_INFO), ci = Reg(RB_COLOR_INFO), di = Reg(RB_DEPTH_INFO);
  const uint32_t pitch = si & 0x3FFF;
  if (!pitch || pitch > 4096) return {};
  const uint32_t base = ci & 0xFFF;
  uint32_t height = pitch == 1280 ? 720 : pitch;
  if (auto it = g_surface_sizes.find((pitch << 12) | base); it != g_surface_sizes.end()) {
    height = it->second.second;
  }
  Targets t;
  // (2_10_10_10 and its float kind each have an "as 16_16_16_16" twin - 10, 12 - for resolving: the same
  // memory, so the same target. The game draws its scene as 3 and resolves it as 12.)
  uint32_t format = (ci >> 16) & 0xF;
  if (format == 12) format = 3;
  if (format == 10) format = 2;
  t.color = GetColorTarget(r, base, pitch, format, height);
  t.depth = GetDepthTarget(r, di & 0xFFF, pitch, height);
  if (t.color && base == 0 && pitch == 1280) r->main_target = t.color;
  return t;
}

// Binds the current targets for drawing / clearing.
bool BindTargets(Renderer* r, const Targets& t) {
  if (!t.color) return false;
  Transition(r, t.color->resource.get(), t.color->layout, RenderTextureLayout::COLOR_WRITE);
  if (t.depth) Transition(r, t.depth->resource.get(), t.depth->layout, RenderTextureLayout::DEPTH_WRITE);
  std::unique_ptr<plume::RenderFramebuffer>& fb = t.color->framebuffers[t.depth];
  if (!fb) {
    const plume::RenderTexture* color = t.color->resource.get();
    const plume::RenderTextureView* view = t.color->rtv.get();
    plume::RenderFramebufferDesc desc(&color, 1, t.depth ? t.depth->resource.get() : nullptr);
    desc.colorAttachmentViews = &view;
    fb = r->device->createFramebuffer(desc);
    if (!fb) return false;
  }
  if (r->list_state.framebuffer != fb.get()) {
    r->list->setFramebuffer(fb.get());
    r->list_state.framebuffer = fb.get();
  }
  t.color->used_frame = r->frames;
  return true;
}

plume::RenderComparisonFunction Compare(uint32_t xenos) {  // Xenos 0 never .. 7 always
  return plume::RenderComparisonFunction(int(plume::RenderComparisonFunction::NEVER) + (xenos & 7));
}

plume::RenderBlend Blend(uint32_t xenos) {
  using B = plume::RenderBlend;
  switch (xenos) {
    case 0: return B::ZERO;
    case 1: return B::ONE;
    case 4: return B::SRC_COLOR;
    case 5: return B::INV_SRC_COLOR;
    case 6: return B::SRC_ALPHA;
    case 7: return B::INV_SRC_ALPHA;
    case 8: return B::DEST_COLOR;
    case 9: return B::INV_DEST_COLOR;
    case 10: return B::DEST_ALPHA;
    case 11: return B::INV_DEST_ALPHA;
    case 12: case 14: return B::BLEND_FACTOR;
    case 13: case 15: return B::INV_BLEND_FACTOR;
    case 16: return B::SRC_ALPHA_SAT;
    default: return B::ONE;
  }
}

plume::RenderBlend BlendAlpha(uint32_t xenos) {  // colour factors are invalid for alpha
  using B = plume::RenderBlend;
  switch (Blend(xenos)) {
    case B::SRC_COLOR: return B::SRC_ALPHA;
    case B::INV_SRC_COLOR: return B::INV_SRC_ALPHA;
    case B::DEST_COLOR: return B::DEST_ALPHA;
    case B::INV_DEST_COLOR: return B::INV_DEST_ALPHA;
    default: return Blend(xenos);
  }
}

plume::RenderBlendOperation BlendOp(uint32_t xenos) {
  using O = plume::RenderBlendOperation;
  switch (xenos) {
    case 1: return O::SUBTRACT;
    case 2: return O::MIN;
    case 3: return O::MAX;
    case 4: return O::REV_SUBTRACT;
    default: return O::ADD;
  }
}

// Xenos stencil ops (xenos.h StencilOp, D3D9's numbers minus one): keep, zero, replace, incr_clamp, decr_clamp,
// invert, incr_wrap, decr_wrap. (3/4 and 6/7 were swapped until 2.2.2: the game's stencil shadow volumes - the
// ropes' and turnbuckles' (z-pass, front 6 / back 7) - clamped at 0 when a back face came first, leaving whole
// faces of the volumes as dark rectangles on the mat.)
plume::RenderStencilOp StencilOp(uint32_t xenos) {
  using S = plume::RenderStencilOp;
  static const S ops[8] = {S::KEEP, S::ZERO, S::REPLACE, S::INCREMENT_AND_CLAMP,
                           S::DECREMENT_AND_CLAMP, S::INVERT, S::INCREMENT_AND_WRAP,
                           S::DECREMENT_AND_WRAP};
  return ops[xenos & 7];
}

// Xenos vertex format (+ whether the shader reads it as uint) -> DXGI.
// A 360 D3DDECLTYPE is: bits 0-5 format, 8 signed, 9 integer (not
// normalized), 10-21 the component swizzle (3 bits per xyzw: 0-3 a
// component, 4 zero, 5 one). The XDK applies the swizzle by patching the
// shader's vertex fetches, which the converted shaders (captured unpatched)
// don't have, so it is applied here: missing components already read as
// 0/0/0/1, and D3DCOLOR's ZYXW is a BGRA format.
constexpr uint32_t kSwizzleXYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9;
constexpr uint32_t kSwizzleZYXW = 2 | 1 << 3 | 0 << 6 | 3 << 9;

bool SwizzleSupported(uint32_t type) {
  const uint32_t swizzle = (type >> 10) & 0xFFF;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t c = (swizzle >> (3 * i)) & 7;
    if (c != i && c != 4 && c != 5) return swizzle == kSwizzleZYXW && (type & 0x3F) == 6;
  }
  return true;
}

RenderFormat VertexFormat(uint32_t type, bool as_uint) {
  using F = RenderFormat;
  const bool is_signed = type & 0x100;
  switch (type & 0x3F) {
    case 6:
      if (as_uint) return is_signed ? F::R8G8B8A8_SINT : F::R8G8B8A8_UINT;
      if (((type >> 10) & 0xFFF) == kSwizzleZYXW) return F::B8G8R8A8_UNORM;  // D3DCOLOR
      return is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
    case 7:  return as_uint ? F::R10G10B10A2_UINT : F::R10G10B10A2_UNORM;
    case 16: case 17: return F::R32_UINT;  // 10_11_11 / 11_11_10, unpacked in the shader
    case 25:
      if (type & 0x40) return F::R16G16_FLOAT;
      if (as_uint) return is_signed ? F::R16G16_SINT : F::R16G16_UINT;
      return is_signed ? F::R16G16_SNORM : F::R16G16_UNORM;
    case 26:
      if (type & 0x40) return F::R16G16B16A16_FLOAT;
      if (as_uint) return is_signed ? F::R16G16B16A16_SINT : F::R16G16B16A16_UINT;
      return is_signed ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM;
    case 31: return F::R16G16_FLOAT;
    case 32: return F::R16G16B16A16_FLOAT;
    case 33: case 36: return as_uint ? F::R32_UINT : F::R32_FLOAT;
    case 34: case 37: return as_uint ? F::R32G32_UINT : F::R32G32_FLOAT;
    case 35: case 38: return as_uint ? F::R32G32B32A32_UINT : F::R32G32B32A32_FLOAT;
    case 57: return as_uint ? F::R32G32B32_UINT : F::R32G32B32_FLOAT;
    default: return F::UNKNOWN;
  }
}

const char* kUsageNames[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
                             "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT",
                             "COLOR", "FOG", "DEPTH", "SAMPLE"};

struct Element {
  uint32_t stream, offset, type, usage, index;
  uint32_t stride = 0;  // the stream's, in bytes
};

// The vertex format of the current vertex shader. The game never sets a
// vertex declaration: its vertex shaders are bound to their formats when they
// are built, which writes stream, offset, stride and format into the
// microcode's vertex fetch instructions. The container copy in the object
// lists each fetch's instruction index with its usage (POSITION0 ...).
// A shader not bound (word +0x28 is -1) gives nothing.
// A shader not bound to a format is drawn with the device's vertex declaration (the engine's own shaders:
// movies, full-screen passes): its elements, as a bound shader's fetches give them. Strides are not in a
// declaration: each stream's is where its last element ends, rounded up to 4.
std::unordered_map<uint32_t, uint32_t> g_unbound_declarations;  // vertex shader object -> declaration (OnUnboundShader)

std::vector<Element> DeviceDeclaration(uint32_t shader_object) {
  std::vector<Element> out;
  uint32_t decl = Be32(Device() + guest::kDeviceVertexDeclaration);
  if (auto it = g_unbound_declarations.find(shader_object); it != g_unbound_declarations.end()) decl = it->second;
  if (decl < 0x10000) return out;
  const uint8_t* p = Virtual(decl);
  if ((Be32(p) & 0xF) != 5) return out;
  const uint32_t n = std::min<uint32_t>(Be32(p + 0x18), 16);
  uint32_t end[16] = {};
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = p + 0x34 + 12 * i;
    const uint32_t stream = Be16(e), offset = Be16(e + 2), type = Be32(e + 4), f = type & 0x3F;
    if (stream >= 15) continue;
    const uint32_t bytes = f == 57 ? 12 : f == 38 || f == 35 || f == 26 || f == 32 ? 16 : f == 37 || f == 34 ? 8 : 4;
    end[stream] = std::max(end[stream], offset + bytes);
    out.push_back({stream, offset, type, e[9], e[10], 0});
  }
  for (Element& e : out) e.stride = (end[e.stream] + 3) & ~3u;
  return out;
}

// (for the frame dump: the fetch instructions the last Declaration() read - address, three words, each)
std::vector<std::array<uint32_t, 4>> g_declaration_fetches;

// The library, binding a shader to a vertex format, writes each fetch's stream, offset and format into the code -
// and puts the fetches in the order of their streams: the shader's own table (which instruction fetches which
// element) no longer holds for code bound to a format whose streams are in another order (the first level's
// ground: colour in stream 8, so its fetch went last and every texture coordinate was taken from the wrong
// place - the lightmap was read with the coordinates of another stream and came out black). What a fetch is
// for is known by the register it writes, which the binding keeps: `vs` (the converted shader) has each
// input's.
std::vector<Element> Declaration(const Shader* vs) {
  std::vector<Element> out;
  g_declaration_fetches.clear();
  const uint32_t object = Be32(Device() + guest::kDeviceCurrentVertexShader);
  if (object < 0x10000) return out;
  if (Be32(Virtual(object) + guest::kVertexShaderBound) == 0xFFFFFFFFu) {
    out = DeviceDeclaration(object);
    if (!out.empty()) return out;
  }
  const uint8_t* o = Virtual(object);
  const uint8_t* c = o + guest::kVertexShaderContainer;
  if ((Be32(c) & 0xFFFFFF00u) != 0x102A1100u) return out;
  const uint32_t vsize = Be32(c + 4), psize = Be32(c + 8), shader_at = Be32(c + 0x18);
  if (vsize > 0x10000 || psize > 0x10000 || shader_at + 0x24 > vsize) return out;
  const uint8_t* shader = c + shader_at;
  const uint32_t first = Be32(shader + 0x18), n = std::min<uint32_t>(Be32(shader + 0x1C), 32);
  if (shader_at + 0x24 + 4 * (first + n) > vsize) return out;
  // (the microcode the library last gave the GPU for this object, else the object's own)
  const uint32_t micro_address = g_bound_vertex_container == object + guest::kVertexShaderContainer
                                     ? g_bound_vertex_microcode
                                     : Be32(o + guest::kVertexShaderMicrocode);
  const uint8_t* micro = Virtual(micro_address);
  // (the code follows the shader's constant values in the physical part)
  const uint32_t code_at = Be32(shader), code_size = Be32(shader + 4);
  if (code_at > psize || code_size > psize - code_at) return out;
  if (g_replay_device && g_replay_vertex_code) {
    if (!GuestReadable(g_replay_vertex_code, code_size)) return out;
    micro = Virtual(g_replay_vertex_code);
  } else {
    micro += code_at;
    if (!GuestReadable(micro_address + code_at, code_size)) return out;
  }
  // The fetches in program order: a "mini" fetch reads on from the stream of
  // the full fetch before it (its own stream and stride fields are unused).
  struct Fetch {
    uint32_t at, usage, index;
  };
  Fetch fetches[32];
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t v = Be32(shader + 0x24 + 4 * (first + i));
    fetches[i] = {(v & 0xFFF) * 12, (v >> 12) & 0xF, (v >> 16) & 0xF};
  }
  std::sort(fetches, fetches + n, [](const Fetch& x, const Fetch& y) { return x.at < y.at; });
  uint32_t stream = ~0u, stride = 0;
  static const bool by_position = EnvFlag("XMD_NATIVE_FETCH_BY_POSITION");  // (as before: by the shader's table)
  for (uint32_t i = 0; i < n; ++i) {
    Fetch f = fetches[i];
    if (f.at + 12 > code_size) continue;
    const uint32_t w0 = Be32(micro + f.at), w1 = Be32(micro + f.at + 4), w2 = Be32(micro + f.at + 8);
    uint32_t converted_swizzle = w1 & 0xFFF;  // (not known: as the bound code's - nothing to make up for)
    if (vs && !by_position) {
      const int reg = int((w0 >> 12) & 0x3F);
      uint32_t mask = 0;
      for (uint32_t k = 0; k < 4; ++k)
        if (((w1 >> (3 * k)) & 7) != 7) mask |= 1u << k;
      int best = 0;
      for (const ShaderInput& in : vs->inputs) {
        if (in.reg != reg) continue;
        const int overlap = __builtin_popcount(in.mask & mask);
        if (overlap <= best) continue;
        for (uint32_t u = 0; u < std::size(kUsageNames); ++u)
          if (in.semantic == kUsageNames[u]) best = overlap, f.usage = u, f.index = in.index, converted_swizzle = in.swizzle;
      }
    }
    g_declaration_fetches.push_back({f.at | (f.usage << 16) | (f.index << 20), w0, w1, w2});
    if (!((w1 >> 30) & 1)) {  // a full fetch
      const uint32_t slot = ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3);  // fetch constant 95 = stream 0, 94 = 1 ...
      stream = slot <= 95 ? 95 - slot : ~0u;
      stride = (w2 & 0xFF) * 4;
    }
    if (stream >= 16 || !stride) continue;  // (15: a skinned model's bone rows - IndexedRows; never an input's)
    bool have = false;  // (an element fetched twice: the first)
    for (const Element& e : out) have |= e.usage == f.usage && e.index == f.index;
    if (have) continue;
    uint32_t format = (w1 >> 16) & 0x3F;
    const uint32_t is_signed = (w1 >> 12) & 1;
    // (test aid XMD_TEST_STREAM_UNORM16=1: half-float pairs in a stream of their own read as 16-bit normalized)
    static const bool test_unorm16 = std::getenv("XMD_TEST_STREAM_UNORM16") != nullptr;
    if (test_unorm16 && format == 31 && stride == 4 && stream != 0) format = 25;
    // (the type as a declaration has it: format, signed flag, identity swizzle - the fetch's own swizzle is the
    // shader's, already in the converted shader)
    // (bit 9, this renderer's own: an integer fetch - values as they are, not scaled to 0..1)
    uint32_t type = format | (is_signed ? 0x100 : 0) | (((w1 >> 13) & 1) ? 0x200 : 0) | (0x688u << 10);
    // The bound code's component order against the converted shader's: where the game's library changed it for
    // the vertex format (a colour stored as D3DCOLOR: ZYXW - the fire's sprites came out blue without it), the
    // element is handed over in that order. Colours only (8:8:8:8): the one case a vertex format has.
    if (format == 6) {
      bool red_blue = true, same = true;
      for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t bound = (w1 >> (3 * k)) & 7, conv = (converted_swizzle >> (3 * k)) & 7;
        if (bound > 3 || conv > 3) continue;  // (not written / a constant)
        same &= bound == conv;
        red_blue &= bound == (conv == 0 ? 2u : conv == 2 ? 0u : conv);
      }
      if (!same && red_blue) type = (type & ~(0xFFFu << 10)) | (kSwizzleZYXW << 10);
    }
    // (bit 6, this renderer's own: 16-bit integers fetched as they are into a float input - the HUD's vector
    // shapes have their positions so. No vertex format gives that: the data is made half floats - FixHalfWords -
    // exact up to 2048, to 1 part in 2048 beyond.)
    if ((type & 0x200) && (format == 25 || format == 26) && vs) {
      for (const ShaderInput& in : vs->inputs)
        if (f.usage < std::size(kUsageNames) && in.semantic == kUsageNames[f.usage] && in.index == f.index && !in.is_uint)
          type |= 0x40;
    }
    out.push_back({stream, ((w2 >> 8) & 0x7FFFFF) * 4, type, f.usage, f.index, stride});
  }
  return out;
}

// Vertex data is 8in32: each 32-bit word is byte-swapped, which is right for
// 32-bit and 8-bit components (Xenos takes x from the low byte of the swapped
// word, as DXGI does). 16-bit components come in memory order instead (x is
// the first half), so the words holding them get their halves swapped back:
// the stream's elements (declaration) decide which words those are.
// Returns a signature of the words it fixes (0 = none), for caching.
uint64_t HalfWords(const std::vector<Element>& decl, uint32_t stream, uint32_t* words,
                   uint32_t* count) {
  *count = 0;
  for (const Element& e : decl) {
    if (e.stream != stream) continue;
    const uint32_t f = e.type & 0x3F;
    const uint32_t bytes = (f == 25 || f == 31) ? 4 : (f == 26 || f == 32) ? 8 : 0;
    // (bit 31: integers to become half floats, bit 30: signed ones - Declaration's type bit 6)
    const uint32_t to_half = (e.type & 0x40) ? 0x80000000u | ((e.type & 0x100) ? 0x40000000u : 0) : 0;
    for (uint32_t b = 0; b < bytes && *count < 16; b += 4) words[(*count)++] = (e.offset + b) | to_half;
  }
  return *count ? XXH3_64bits(words, *count * 4) : 0;
}

uint16_t HalfFromInt(int32_t v) {
  if (!v) return 0;
  const uint32_t sign = v < 0 ? 0x8000u : 0;
  uint32_t a = uint32_t(v < 0 ? -v : v);
  uint32_t e = 31 - uint32_t(__builtin_clz(a));
  if (e > 10) {
    a += 1u << (e - 11);  // (to the nearest)
    e = 31 - uint32_t(__builtin_clz(a));
  }
  const uint32_t m = (e <= 10 ? a << (10 - e) : a >> (e - 10)) & 0x3FF;
  return uint16_t(sign | ((e + 15) << 10) | m);
}

void FixHalfWords(uint8_t* data, uint64_t size, uint32_t stride, uint32_t offset,
                  const std::vector<Element>& decl, uint32_t stream) {
  uint32_t words[16], count;
  if (!HalfWords(decl, stream, words, &count) || stride < 4) return;
  for (uint64_t v = offset % stride; v + stride <= size; v += stride) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t at = words[i] & 0xFFFFFF;
      if (at + 4 > stride) continue;
      uint32_t w;
      std::memcpy(&w, data + v + at, 4);
      w = (w >> 16) | (w << 16);
      if (words[i] & 0x80000000u) {
        const bool is_signed = words[i] & 0x40000000u;
        const uint32_t lo = w & 0xFFFF, hi = w >> 16;
        w = HalfFromInt(is_signed ? int16_t(lo) : int32_t(lo)) | uint32_t(HalfFromInt(is_signed ? int16_t(hi) : int32_t(hi))) << 16;
      }
      std::memcpy(data + v + at, &w, 4);
    }
  }
}

// Guest vertices -> little-endian into `dst` (mapped upload memory: write-
// combined, so it is only ever written, never read back): converted in a
// scratch buffer, then copied over in one go.
void ConvertVertices(uint8_t* dst, const uint8_t* src, uint64_t size, uint32_t stride,
                     uint32_t offset, const std::vector<Element>& decl, uint32_t stream) {
  uint32_t words[16], count;
  if (!HalfWords(decl, stream, words, &count) || stride < 4) {
    Swap32(dst, src, size);
    return;
  }
  thread_local std::vector<uint8_t> scratch;
  if (scratch.size() < size) scratch.resize(size);
  Swap32(scratch.data(), src, size);
  FixHalfWords(scratch.data(), size, stride, offset, decl, stream);
  std::memcpy(dst, scratch.data(), size);
}

// A guest index buffer (16-bit big-endian), converted whole and cached like a
// static vertex buffer (re-checked every 4th frame) - rather than converting
// each draw's indices into the ring (~4% of a phone's render thread in
// heavy entrances). One the game keeps rewriting is left to the ring (null).
RenderBufferReference IndexBuffer(Renderer* r, uint32_t address, uint32_t size) {
  constexpr uint32_t kRingAfterChanges = 3;
  CachedBuffer& c = r->index_buffers[(uint64_t(address) << 32) | size];
  if (c.dynamic || size < 2) return {};
  if (g_replay_device) c.sited = true;
  const bool pooled = c.pool_ref.ref && c.pool_generation == r->pool.generation;
  if ((c.resource || pooled) && r->frames - c.checked_frame < RecheckEvery(address) && !NoCache())
    return pooled ? c.pool_ref : c.resource->at(0);
  const uint8_t* src = ObjectData(address);
  const uint64_t hash = XXH3_64bits(src, size);
  g_hash_vb_static += size;
  c.checked_frame = r->frames;
  if ((c.resource || pooled) && c.hash == hash) return pooled ? c.pool_ref : c.resource->at(0);
  if (c.resource || pooled) {
    if (c.resource) Retire(r, std::move(c.resource));
    c.resource.reset();
    if (pooled) r->pool.dead += size;
    c.pool_ref = {};
    if (c.sited) ++g_site_epoch;
    c.sited = false;
    if (++c.changes >= kRingAfterChanges) {
      c.dynamic = true;
      return {};
    }
  }
  if (g_replay_device) {
    auto [cpu, ref] = PoolAllocate(r, size & ~1u);
    if (cpu) {
      Swap16(cpu, src, size & ~1u);
      c.pool_ref = ref;
      c.pool_generation = r->pool.generation;
      c.hash = hash;
      c.size = size;
      return ref;
    }
  }
  c.resource = CreateBuffer(r, size);
  if (!c.resource) return {};
  c.resource->setName(fmt::format("index buffer {:08X} size {}", address, size));
  c.hash = hash;
  c.size = size;
  Swap16(static_cast<uint8_t*>(c.resource->map()), src, size & ~1u);
  c.resource->unmap();
  return c.resource->at(0);
}

// Vertex buffer object -> converted (little-endian) GPU buffer, cached per
// guest buffer (and layout) and re-converted when its contents change. A
// buffer that keeps changing is converted into the frame's upload ring at
// every draw instead (the game refills those between draws of a frame).
// Returns the buffer (a null reference if none).
RenderBufferReference VertexBuffer(Renderer* r, uint32_t object, uint32_t stream, const Stream& s,
                                   const std::vector<Element>& decl, uint32_t* out_size) {
  constexpr uint32_t kDynamicAfterChanges = 3;
  constexpr uint32_t kMaxDynamicSize = 512u << 10;
  const uint8_t* obj = BufferObject(object);
  const uint32_t w0 = Be32(obj + 0x18), w1 = Be32(obj + 0x1C);
  const uint32_t address = w0 & ~3u;
  const uint32_t size = ((w1 >> 2) & 0xFFFFFF) * 4;
  *out_size = size;
  if (!size) return {};
  uint32_t words[16], count;
  uint64_t layout = HalfWords(decl, stream, words, &count);
  // (the offset's part that matters: where in a vertex the buffer starts)
  if (layout) layout ^= (uint64_t(s.stride) << 32) ^ (s.stride ? s.offset % s.stride : s.offset);
  CachedBuffer& c = r->vertex_buffers[((uint64_t(address) << 32) | size) ^ (layout * 0x9E3779B97F4A7C15ull)];
  // (a small buffer: into the frame's ring from the start - the game makes a new one for every dynamic draw,
  // and a buffer of its own for each came to 225 000 buffers, 19 GB, after five minutes in the first level)
  const bool pooled = c.pool_ref.ref && c.pool_generation == r->pool.generation;
  if (g_replay_device) c.sited = true;
  // (a played command buffer's: a level's own static mesh - kept, in the pool)
  static const bool pool_off = EnvFlag("XMD_NATIVE_NO_BUFFER_POOL");
  const bool to_pool = g_replay_device && !pool_off && !SlotTables();
  if (!c.resource && !pooled && !c.dynamic && size <= (16u << 10) && !to_pool) c.dynamic = true;
  c.seen_frame = r->frames;
  const uint8_t* src = ObjectData(address);
  // (a buffer object whose data isn't there - seen while a level loads: not drawn)
  if (!GuestReadable(address, size)) {
    static int logged = 0;
    if (logged++ < 8)
      REXLOG_WARN("native renderer: vertex buffer {:08X}: its data ({:08X}, {} bytes) can't be read - not drawn (stream {}, "
                  "words {:08X} {:08X})", object, address, size, stream, w0, w1);
    *out_size = 0;
    return {};
  }
  if (c.dynamic) {
    // Re-converted only when the contents changed since this frame's last
    // conversion (a hash is far cheaper than the conversion) - for buffers
    // drawn more than once a frame; the others are just converted.
    if (c.use_frame != r->frames) {
      c.last_uses = c.use_frame + 1 == r->frames ? c.uses : 0;
      // (Its last frame of use need not be the previous frame.)
      if (c.uses > 1 && !c.changed_in_frame && c.demotions < 2) {
        if (++c.stable_frames >= 30) c.per_frame = true;
      } else if (c.changed_in_frame) {
        c.stable_frames = 0;
      }
      c.changed_in_frame = false;
      c.uses = 0;
      c.use_frame = r->frames;
    }
    ++c.uses;
    const bool repeat = c.ring_frame == r->frames && c.ring_ref.ref;
    if (repeat && c.per_frame && (r->frames & 7)) return c.ring_ref;
    const bool hashing = repeat || c.last_uses > 1;
    const uint64_t hash = hashing ? XXH3_64bits(src, size) : 0;
    if (hashing) g_hash_vb_dynamic += size;
    static const bool vb_stats = EnvFlag("XMD_NATIVE_VB_STATS");
    if (vb_stats && hashing) g_vb_stats[(uint64_t(address) << 32) | size] += 1;
    if (repeat && c.ring_hashed) {
      if (c.ring_hash == hash) {
        ++g_repeat_same;
        return c.ring_ref;
      }
      ++g_repeat_changed;
      c.changed_in_frame = true;
      if (c.per_frame) {
        c.per_frame = false;
        c.stable_frames = 0;
        ++c.demotions;
        ++g_vb_demotions;
      }
    }
    auto [cpu, ref] = Allocate(r, size, 256);
    if (cpu) {
      ConvertVertices(cpu, src, size, s.stride, s.offset, decl, stream);
      c.ring_frame = r->frames;
      c.ring_hash = hash;
      c.ring_hashed = hashing;
      c.ring_ref = ref;
      return ref;
    }
    // Ring full: fall back to the cached buffer (re-checked below).
  }
  // A static buffer is re-checked every 4th frame (buffers the game rewrites
  // turn dynamic after a few changes, then are checked at every use).
  if ((c.resource || pooled) && r->frames - c.checked_frame < RecheckEvery(address) && !NoCache())
    return pooled ? c.pool_ref : c.resource->at(0);
  const uint64_t hash = XXH3_64bits(src, size);
  g_hash_vb_static += size;
  c.checked_frame = r->frames;
  if ((c.resource || pooled) && c.hash == hash) return pooled ? c.pool_ref : c.resource->at(0);
  if (c.resource || pooled) {
    if (c.resource) Retire(r, std::move(c.resource));
    c.resource.reset();
    if (pooled) r->pool.dead += size;
    c.pool_ref = {};
    if (c.sited) ++g_site_epoch;
    c.sited = false;
    if (++c.changes >= kDynamicAfterChanges && size <= kMaxDynamicSize && !c.dynamic) {
      c.dynamic = true;
      return VertexBuffer(r, object, stream, s, decl, out_size);
    }
  }
  if (to_pool) {
    auto [cpu, ref] = PoolAllocate(r, size);
    if (cpu) {
      ConvertVertices(cpu, src, size, s.stride, s.offset, decl, stream);
      c.pool_ref = ref;
      c.pool_generation = r->pool.generation;
      c.hash = hash;
      c.size = size;
      return ref;
    }
  }
  c.resource = CreateBuffer(r, size);
  if (!c.resource) return {};
  c.resource->setName(fmt::format("vertex buffer {:08X} size {}", address, size));
  c.hash = hash;
  c.size = size;
  auto* dst = static_cast<uint8_t*>(c.resource->map());
  ConvertVertices(dst, src, size, s.stride, s.offset, decl, stream);  // 8in32 (all this game's)
  c.resource->unmap();
  return c.resource->at(0);
}

struct PipelineKey {
  uint64_t vs, ps;
  uint8_t vs_variant, ps_variant, topology;
  uint8_t rt_format;  // RenderFormat of the render target
  uint32_t depth, blend, colour_mask, cull, stencil_ref_mask;
  uint32_t layout_hash;
  float bias_scale, bias_offset;  // polygon offset (0 when off)
};

// A shader's compiled form for this device (made at its first use).
plume::RenderShader* Compiled(Renderer* r, const std::vector<uint8_t>& code,
                              std::unique_ptr<plume::RenderShader>& compiled) {
  if (!compiled && !code.empty())
    compiled = r->device->createShader(code.data(), code.size(), "main", backend::ShaderFormat());
  return compiled.get();
}

plume::RenderShader* Compiled(Renderer* r, Shader* s, int variant) {
  if (s->code[variant].empty()) variant = 0;
  return Compiled(r, s->code[variant], s->compiled[variant]);
}

void NotePipeline(const PipelineKey& key, uint64_t h, uint32_t layout_base_hash,
                  const std::vector<plume::RenderInputElement>& layout, const plume::RenderInputSlot* slots,
                  uint32_t slot_count);
std::unique_ptr<plume::RenderPipeline> TakePrebuilt(uint64_t h);
std::unique_ptr<plume::RenderPipeline> CreatePipeline(Renderer* r, const PipelineKey& key, plume::RenderShader* vs,
                                                      plume::RenderShader* ps, bool has_ps,
                                                      const plume::RenderInputElement* layout,
                                                      uint32_t layout_count, const plume::RenderInputSlot* slots,
                                                      uint32_t slot_count, bool render_thread);

const plume::RenderPipeline* Pipeline(Renderer* r, Shader* vs, int vs_variant, Shader* ps,
                                      int ps_variant, const std::vector<plume::RenderInputElement>& layout,
                                      const plume::RenderInputSlot* slots, uint32_t slot_count,
                                      uint32_t layout_hash, uint32_t layout_base_hash,
                                      plume::RenderPrimitiveTopology topology, RenderFormat rt_format) {
  PipelineKey key = {};
  key.rt_format = uint8_t(rt_format);
  key.vs = vs->hash;
  key.ps = ps ? ps->hash : 0;
  key.vs_variant = uint8_t(vs_variant);
  key.ps_variant = uint8_t(ps_variant);
  key.topology = uint8_t(topology);
  key.depth = Reg(RB_DEPTHCONTROL);
  key.blend = Reg(RB_BLENDCONTROL0);
  key.colour_mask = Reg(RB_COLOR_MASK) & 0xF;
  key.cull = Reg(PA_SU_SC_MODE_CNTL) & 0x7;
  key.stencil_ref_mask = Reg(RB_STENCILREFMASK) & 0xFFFF00;  // masks (ref is dynamic)
  key.layout_hash = layout_hash;
  // Polygon offset (PA_SU_SC_MODE_CNTL bit 11 front / 12 back enable): the
  // front values (the game culls or uses the same for both).
  if (Reg(PA_SU_SC_MODE_CNTL) & (3u << 11)) {
    key.bias_scale = RegFloat(PA_SU_POLY_OFFSET_FRONT_SCALE);
    key.bias_offset = RegFloat(PA_SU_POLY_OFFSET_FRONT_SCALE + 1);
  }
  // A colour draw tested "nearer or equal" against what a depth pass left (the game's depth pass, then its
  // colour pass with other shaders): a few depth steps of slack towards the eye, for where the two shaders'
  // arithmetic doesn't agree to the last bit. XMD_NATIVE_DEPTH_SLACK=<steps> (0: none).
  static const float depth_slack = [] {
    const char* v = std::getenv("XMD_NATIVE_DEPTH_SLACK");
    // (the steps are the 32-bit depth's last bits at the triangle's depth - relative, the same share near and far.
    // 16 - one step of the console's 24-bit float depth - took the striped patches off the characters but left
    // black specks on faces, hair and eyes, flickering: the game gives its depth pass and its colour pass the
    // same transform split up differently (the depth pass a local-to-world matrix in c52-c55, the colour pass an
    // identity there), so the two never agree to the last bits. Measured on the main menu's crowd
    // (tools\holes_ab.ps1, the scene cleared to magenta): 16 leaves holes in one frame of ten, up to 10,000
    // pixels; 256 and 4096 none. 1024 is 0.006% of the distance - 0.6 mm at 10 m.)
    return v ? float(std::atof(v)) : 1024.0f;
  }();
  if (depth_slack != 0.0f && ps && (key.depth & 2) && ((key.depth >> 4) & 7) == 6)
    key.bias_offset += depth_slack / float(1 << 24);
  const uint64_t h = XXH3_64bits(&key, sizeof(key));
  auto it = r->pipelines.find(h);
  if (it != r->pipelines.end()) return it->second.get();
  // Built ahead (the known pipelines, in the menus)?
  if (std::unique_ptr<plume::RenderPipeline> built = TakePrebuilt(h)) {
    return (r->pipelines[h] = std::move(built)).get();
  }
  NotePipeline(key, h, layout_base_hash, layout, slots, slot_count);

  plume::RenderShader* vs_compiled = Compiled(r, vs, vs_variant);
  plume::RenderShader* ps_compiled = nullptr;
  // XMD_NATIVE_DEBUG_PS=<name> + XMD_NATIVE_DEBUG_PS_FOR=<hash>,...:
  // those pixel shaders are replaced by <shader dir>/<name>.ps<extension>.
  static const std::pair<std::vector<uint8_t>, std::string> debug_override = [] {
    std::pair<std::vector<uint8_t>, std::string> o;
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "XMD_NATIVE_DEBUG_PS") == 0 && e) {
      o.first = ReadShader(std::string(e) + ".ps");
      free(e);
    }
    if (_dupenv_s(&e, &n, "XMD_NATIVE_DEBUG_PS_FOR") == 0 && e) {
      o.second = e;
      free(e);
    }
    return o;
  }();
  static std::unique_ptr<plume::RenderShader> debug_solid_shader, debug_override_shader;
  char ps_name[20];
  std::snprintf(ps_name, sizeof(ps_name), "%016llX", static_cast<unsigned long long>(key.ps));
  // (=2, the draw-ID view: the scene's opaque, depth-tested draws only - not the full-screen passes or particles)
  if (g_debug_solid && !g_debug_ps.empty() &&
      (g_debug_solid_state || ((key.depth & 2) && (key.depth & 4) && key.blend == 0x00010001u && ps))) {
    ps_compiled = Compiled(r, g_debug_ps, debug_solid_shader);
  } else if (ps && !debug_override.first.empty() &&
             (debug_override.second == "*" || debug_override.second.find(ps_name) != std::string::npos)) {  // ("*": all)
    ps_compiled = Compiled(r, debug_override.first, debug_override_shader);
  } else if (ps) {
    ps_compiled = Compiled(r, ps, ps_variant);
  }
  std::unique_ptr<plume::RenderPipeline> pso =
      CreatePipeline(r, key, vs_compiled, ps_compiled, ps != nullptr, layout.data(), uint32_t(layout.size()),
                     slots, slot_count, true);
  return (r->pipelines[h] = std::move(pso)).get();
}

// A pipeline from its key (no draw state is read: the background builder
// makes the known ones with it too). `render_thread`: logged, timed.
std::unique_ptr<plume::RenderPipeline> CreatePipeline(Renderer* r, const PipelineKey& key, plume::RenderShader* vs,
                                                      plume::RenderShader* ps, bool has_ps,
                                                      const plume::RenderInputElement* layout,
                                                      uint32_t layout_count, const plume::RenderInputSlot* slots,
                                                      uint32_t slot_count, bool render_thread) {
  const auto topology = plume::RenderPrimitiveTopology(key.topology);
  const auto rt_format = RenderFormat(key.rt_format);
  plume::RenderGraphicsPipelineDesc d;
  d.pipelineLayout = r->layout.get();
  d.vertexShader = vs;
  d.pixelShader = ps;
  // Blend (render target 0).
  const uint32_t bc = key.blend;
  plume::RenderBlendDesc& bt = d.renderTargetBlend[0];
  bt.srcBlend = Blend(bc & 0x1F);
  bt.blendOp = BlendOp((bc >> 5) & 7);
  bt.dstBlend = Blend((bc >> 8) & 0x1F);
  bt.srcBlendAlpha = BlendAlpha((bc >> 16) & 0x1F);
  bt.blendOpAlpha = BlendOp((bc >> 21) & 7);
  bt.dstBlendAlpha = BlendAlpha((bc >> 24) & 0x1F);
  using B = plume::RenderBlend;
  using O = plume::RenderBlendOperation;
  bt.blendEnabled = !(bt.srcBlend == B::ONE && bt.dstBlend == B::ZERO && bt.blendOp == O::ADD &&
                      bt.srcBlendAlpha == B::ONE && bt.dstBlendAlpha == B::ZERO &&
                      bt.blendOpAlpha == O::ADD);
  bt.renderTargetWriteMask = uint8_t(key.colour_mask);
  // Rasterizer.
  const bool cull_front = key.cull & 1, cull_back = key.cull & 2, front_cw = key.cull & 4;
  d.cullMode = cull_front ? plume::RenderCullMode::FRONT
               : cull_back ? plume::RenderCullMode::BACK
                           : plume::RenderCullMode::NONE;
  d.frontFace = front_cw ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  d.depthClipEnabled = true;
  // Xenos offsets are in depth units of the 24-bit buffer and slopes in 1/16
  // (as Xenia converts them). (D32F's unit is 2^(exponent - 23): the same
  // 2^-24 at depths 0.5..1, where scenes are; nearer, a smaller offset.)
  d.depthBias = int32_t(std::lround(double(key.bias_offset) * double(1 << 24)));
  d.slopeScaledDepthBias = key.bias_scale * (1.0f / 16.0f);
  // Depth / stencil.
  const uint32_t dc = key.depth;
  d.depthEnabled = (dc >> 1) & 1;
  d.depthWriteEnabled = (dc >> 2) & 1;
  d.depthFunction = Compare((dc >> 4) & 7);
  d.stencilEnabled = dc & 1;
  d.stencilReadMask = uint8_t(key.stencil_ref_mask >> 8);
  d.stencilWriteMask = uint8_t(key.stencil_ref_mask >> 16);
  d.stencilFrontFace.failOp = StencilOp((dc >> 11) & 7);
  d.stencilFrontFace.depthFailOp = StencilOp((dc >> 17) & 7);
  d.stencilFrontFace.passOp = StencilOp((dc >> 14) & 7);
  d.stencilFrontFace.compareFunction = Compare((dc >> 8) & 7);
  if ((dc >> 7) & 1) {
    d.stencilBackFace.failOp = StencilOp((dc >> 23) & 7);
    d.stencilBackFace.depthFailOp = StencilOp((dc >> 29) & 7);
    d.stencilBackFace.passOp = StencilOp((dc >> 26) & 7);
    d.stencilBackFace.compareFunction = Compare((dc >> 20) & 7);
  } else {
    d.stencilBackFace = d.stencilFrontFace;
  }
  d.dynamicStencilReferenceEnabled = true;
  d.dynamicBlendFactorEnabled = true;
  if (g_no_depth) {
    d.depthEnabled = false;
    d.stencilEnabled = false;
  }
  if (g_no_blend) {
    bt.blendEnabled = false;
    bt.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALL);
  }
  if (g_debug_solid_state) {
    bt.blendEnabled = false;
    bt.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALL);
    d.cullMode = plume::RenderCullMode::NONE;
    d.depthEnabled = false;
    d.stencilEnabled = false;
  }
  d.inputElements = layout;
  d.inputElementsCount = layout_count;
  d.inputSlots = slots;
  d.inputSlotsCount = slot_count;
  d.primitiveTopology = topology;
  d.renderTargetCount = 1;
  d.renderTargetFormat[0] = rt_format;
  d.depthTargetFormat = backend::DepthFormat();
  // Logged before it is built: a GPU driver that crashes compiling it (seen
  // under Proton) leaves this as the log's last pipeline.
  if (render_thread) REXLOG_INFO("native renderer: pipeline {} vs {:016X}.{} ps {:016X}.{} rt {} blend {:08X}{} mask {:X} "
              "depth {:08X} cull {} bias {}/{} topology {}",
              r->pipelines.size(), key.vs, key.vs_variant, key.ps, key.ps_variant, int(rt_format),
              key.blend, bt.blendEnabled ? " on" : "", key.colour_mask, key.depth, key.cull,
              key.bias_offset, key.bias_scale, int(topology));
  std::unique_ptr<plume::RenderPipeline> pso;
  const auto build_start = std::chrono::steady_clock::now();
  if (d.vertexShader && (d.pixelShader || !has_ps)) pso = r->device->createGraphicsPipeline(d);
  if (pso && !backend::PipelineCreated(pso.get())) pso.reset();
  g_pipeline_cache_dirty = true;
  g_last_pipeline_build = std::chrono::steady_clock::now().time_since_epoch().count();
  if (render_thread) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_start).count();
    g_pipeline_ms += ms;
    if (ms >= 20.0) REXLOG_INFO("native renderer: pipeline {} took {:.0f} ms", r->pipelines.size(), ms);
    if (ms >= 2.0) {
      char what[96];
      std::snprintf(what, sizeof(what), " pipeline vs %016llX ps %016llX", static_cast<unsigned long long>(key.vs),
                    static_cast<unsigned long long>(key.ps));
      xmd::problems::FrameCause(what, ms);
    }
  }
  if (!pso && render_thread) {
    REXLOG_WARN("native renderer: pipeline creation failed (vs {:016X} ps {:016X})", key.vs,
                key.ps);
  }
  return pso;
}

// ---------------------------------------------------------------------------
// known pipelines: built ahead, in the menus
//
// A pipeline is compiled the first time the game draws with its state - in
// the middle of a scene, a hitch on slow CPUs and drivers. So every pipeline
// the renderer builds is noted (its key, vertex layout and slot strides:
// what it takes to build it again without the game's draw state) in
// UserData\cache\native_pipelines.list, beside the shipped list
// (native_shaders\pipelines.list). At start a background thread builds all
// the known ones at low priority while no match, entrance or cutscene is on
// (it waits in them); the render thread takes them from it instead of
// compiling them. They go into the pipeline cache too (D3D12 library, Vulkan
// cache), so on later starts they only load.

struct PipelineRecord {
  PipelineKey key;  // (layout_hash: the layout's own; Vulkan adds the strides)
  std::vector<std::string> names;
  std::vector<plume::RenderInputElement> layout;  // (semantic names point into names)
  uint32_t strides[16] = {};
};

struct PrebuiltPipelines {
  std::mutex mutex;
  std::condition_variable changed;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> ready;  // built, not taken yet
  std::unordered_set<uint64_t> claimed;  // the render thread builds these itself
  uint64_t busy = 0;                     // the one being built now
  std::unordered_set<uint64_t> known;    // listed (render thread)
  std::ofstream list;                    // the player's list, appended to (render thread)
  std::atomic<uint32_t> total{0}, done{0};
  std::atomic<bool> running{false};
} g_prebuilt;

// The key's hash (the pipelines' map key) for this API: Vulkan pipelines
// have the strides of the slots they use built in.
uint64_t RecordHash(PipelineRecord& rec) {
  PipelineKey key = rec.key;
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    for (const auto& e : rec.layout) key.layout_hash = key.layout_hash * 31 + rec.strides[e.slotIndex & 15] * 977;
  }
  return XXH3_64bits(&key, sizeof(key));
}

uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

float BitsFloat(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// One line: the key, the 16 strides, then the elements
// (name index location format slot offset).
std::string FormatRecord(const PipelineKey& k, uint32_t layout_base_hash,
                         const std::vector<plume::RenderInputElement>& layout, const plume::RenderInputSlot* slots,
                         uint32_t slot_count) {
  std::string s = fmt::format("{:016X} {} {:016X} {} {} {} {:08X} {:08X} {:X} {:X} {:X} {:08X} {:08X} {:08X} |", k.vs,
                              k.vs_variant, k.ps, k.ps_variant, k.topology, k.rt_format, k.depth, k.blend,
                              k.colour_mask, k.cull, k.stencil_ref_mask, layout_base_hash, FloatBits(k.bias_scale),
                              FloatBits(k.bias_offset));
  for (uint32_t i = 0; i < 16; ++i) s += fmt::format(" {}", i < slot_count ? slots[i].stride : 0);
  s += " |";
  for (const auto& e : layout) {
    s += fmt::format(" {} {} {} {} {} {}", e.semanticName, e.semanticIndex, e.location, int(e.format), e.slotIndex,
                     e.alignedByteOffset);
  }
  return s;
}

bool ParseRecord(const std::string& line, PipelineRecord& rec) {
  const size_t bar1 = line.find('|'), bar2 = bar1 == std::string::npos ? bar1 : line.find('|', bar1 + 1);
  if (bar2 == std::string::npos) return false;
  std::istringstream head(line.substr(0, bar1)), strides(line.substr(bar1 + 1, bar2 - bar1 - 1)),
      elements(line.substr(bar2 + 1));
  PipelineKey& k = rec.key;
  k = {};
  uint32_t vsv, psv, topology, rt, scale, offset;
  if (!(head >> std::hex >> k.vs >> std::dec >> vsv >> std::hex >> k.ps >> std::dec >> psv >> topology >> rt >>
        std::hex >> k.depth >> k.blend >> k.colour_mask >> k.cull >> k.stencil_ref_mask >> k.layout_hash >> scale >>
        offset)) {
    return false;
  }
  k.vs_variant = uint8_t(vsv), k.ps_variant = uint8_t(psv), k.topology = uint8_t(topology), k.rt_format = uint8_t(rt);
  k.bias_scale = BitsFloat(scale), k.bias_offset = BitsFloat(offset);
  for (uint32_t i = 0; i < 16; ++i) {
    if (!(strides >> rec.strides[i])) return false;
  }
  std::string name;
  uint32_t index, location, format, slot, element_offset;
  while (elements >> name >> index >> location >> format >> slot >> element_offset) {
    rec.names.push_back(name);
    plume::RenderInputElement e;
    e.semanticIndex = index;
    e.location = location;
    e.format = RenderFormat(format);
    e.slotIndex = slot;
    e.alignedByteOffset = element_offset;
    rec.layout.push_back(e);
  }
  for (size_t i = 0; i < rec.layout.size(); ++i) rec.layout[i].semanticName = rec.names[i].c_str();
  return true;
}

std::filesystem::path PlayerPipelineList() { return PipelineCacheFile().parent_path() / "native_pipelines.list"; }

// The render thread is about to build a pipeline itself: claimed (the
// builder skips it), and listed for next time if it's new.
void NotePipeline(const PipelineKey& key, uint64_t h, uint32_t layout_base_hash,
                  const std::vector<plume::RenderInputElement>& layout, const plume::RenderInputSlot* slots,
                  uint32_t slot_count) {
  {
    std::lock_guard lock(g_prebuilt.mutex);
    g_prebuilt.claimed.insert(h);
  }
  if (!g_prebuilt.known.insert(h).second) return;
  if (!g_prebuilt.list.is_open()) {
    std::error_code ec;
    std::filesystem::create_directories(PlayerPipelineList().parent_path(), ec);
    g_prebuilt.list.open(PlayerPipelineList(), std::ios::app);
  }
  g_prebuilt.list << FormatRecord(key, layout_base_hash, layout, slots, slot_count) << '\n';
  g_prebuilt.list.flush();
}

// A pipeline the builder made (waiting if it's building this one now).
std::unique_ptr<plume::RenderPipeline> TakePrebuilt(uint64_t h) {
  std::unique_lock lock(g_prebuilt.mutex);
  g_prebuilt.changed.wait(lock, [h] { return g_prebuilt.busy != h; });
  auto it = g_prebuilt.ready.find(h);
  if (it == g_prebuilt.ready.end()) return nullptr;
  std::unique_ptr<plume::RenderPipeline> p = std::move(it->second);
  g_prebuilt.ready.erase(it);
  return p;
}

void PrebuildPipelines(Renderer* r, std::vector<PipelineRecord> records) {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
  setpriority(PRIO_PROCESS, 0, 10);
#endif
  rex::thread::set_current_thread_name("native pipelines");
  const auto start = std::chrono::steady_clock::now();
  // The builder's own copies of the shaders (files read and compiled here,
  // not on the render thread).
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders;
  auto shader = [&](uint64_t hash, bool pixel) -> Shader* {
    std::unique_ptr<Shader>& s = shaders[hash * 2 + (pixel ? 1 : 0)];
    if (!s) {
      s = std::make_unique<Shader>();
      s->hash = hash;
      s->pixel = pixel;
      char name[64];
      std::snprintf(name, sizeof(name), "%016llX.%s", static_cast<unsigned long long>(hash), pixel ? "ps" : "vs");
      if (ShaderExists(name)) {
        LoadShader(*s);
      } else {
        s->loaded = s->missing = true;  // (a list from an older version)
      }
    }
    return s->missing ? nullptr : s.get();
  };
  uint32_t built = 0, waited_ms = 0;
  // Debug: XMD_PREBUILD_DELAY_MS=<ms> - a pause before each (a slow PC).
  const uint32_t delay_ms = [] {
    char* v = nullptr;
    size_t n = 0;
    uint32_t ms = 0;
    if (_dupenv_s(&v, &n, "XMD_PREBUILD_DELAY_MS") == 0 && v) {
      ms = uint32_t(std::strtoul(v, nullptr, 10));
      free(v);
    }
    return ms;
  }();
  for (PipelineRecord& rec : records) {
    if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    // Not while a match, entrance or cutscene is on: the game needs the CPU.
    while (g_match_scene.load() && !g_failed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      waited_ms += 250;
    }
    if (g_failed || !REXCVAR_GET(native_prepare_pipelines)) break;  // (turned off in game)
    const uint64_t h = RecordHash(rec);
    {
      std::lock_guard lock(g_prebuilt.mutex);
      if (g_prebuilt.claimed.count(h) || g_prebuilt.ready.count(h)) {
        ++g_prebuilt.done;
        continue;
      }
      g_prebuilt.busy = h;
    }
    std::unique_ptr<plume::RenderPipeline> pso;
    Shader* vs = shader(rec.key.vs, false);
    Shader* ps = rec.key.ps ? shader(rec.key.ps, true) : nullptr;
    if (vs && (ps || !rec.key.ps)) {
      plume::RenderInputSlot slots[16];
      for (uint32_t i = 0; i < 16; ++i) slots[i] = plume::RenderInputSlot(i, rec.strides[i]);
      pso = CreatePipeline(r, rec.key, Compiled(r, vs, rec.key.vs_variant),
                           ps ? Compiled(r, ps, rec.key.ps_variant) : nullptr, ps != nullptr, rec.layout.data(),
                           uint32_t(rec.layout.size()), slots, 16, false);
    }
    {
      std::lock_guard lock(g_prebuilt.mutex);
      g_prebuilt.busy = 0;
      if (pso) g_prebuilt.ready[h] = std::move(pso), ++built;
    }
    g_prebuilt.changed.notify_all();
    ++g_prebuilt.done;
  }
  g_prebuilt.running = false;
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("native renderer: {} of {} known pipelines built ahead in {:.1f} s ({:.1f} s of it waiting for a "
              "scene to end)",
              built, records.size(), secs, waited_ms / 1000.0);
}

void StartPrebuildingPipelines(Renderer* r) {
  if (!REXCVAR_GET(native_prepare_pipelines) || g_debug_solid || g_no_depth || g_no_blend) return;
  std::vector<PipelineRecord> records;
  std::unordered_set<uint64_t> seen;
  // (the shipped list beside the exe even when the shaders come from elsewhere: dev runs)
  const std::filesystem::path shipped = rex::filesystem::GetExecutableFolder() / "native_shaders" / "pipelines.list";
  for (const std::filesystem::path& file : {shipped, PlayerPipelineList()}) {
    std::ifstream in(file);
    std::string line;
    uint32_t count = 0;
    while (std::getline(in, line)) {
      PipelineRecord rec;
      if (!ParseRecord(line, rec)) continue;
      const uint64_t h = RecordHash(rec);
      if (!seen.insert(h).second) continue;
      g_prebuilt.known.insert(h);
      records.push_back(std::move(rec));
      ++count;
    }
    if (count) REXLOG_INFO("native renderer: {} known pipelines in {}", count, file.string());
  }
  if (records.empty()) return;
  g_prebuilt.total = uint32_t(records.size());
  g_prebuilt.running = true;
  std::thread(PrebuildPipelines, r, std::move(records)).detach();
}

}  // namespace

bool PreparingPipelines(uint32_t* done, uint32_t* total, bool* paused) {
  if (!g_prebuilt.running.load()) return false;
  *done = g_prebuilt.done.load();
  *total = g_prebuilt.total.load();
  *paused = g_match_scene.load();
  return true;
}

namespace {

// Copies the shader constants from the device mirror, little-endian.
RenderBufferReference Constants(Renderer* r, uint32_t device_offset, uint32_t bytes) {
  auto [cpu, gpu] = Allocate(r, bytes);
  if (!cpu) return {};
  Swap32(cpu, Device() + device_offset, bytes);
  return gpu;
}

// Uploads float4 constants [first, first + bytes/16) of the GPU constant file.
// The device mirror holds them all (the XDK also writes it inline, with no
// hookable call), big-endian: byte-swapped 16 bytes at a time straight into
// the upload; the constants loaded from memory for this draw (so far only
// the shaders' literal c252-c255) take precedence.
// `bytes`: as far as the draw's shader reads them (in whole 64-byte blocks here). Uploading all 256 at every
// draw was 28 MB a frame in the first level (4900 draws; XMD_NATIVE_WHOLE_CONSTANTS=1: as before).
uint32_t g_const_valid[2] = {0, 0};  // (how much of the stage the last upload - and the copy compared with - holds)
RenderBufferReference GpuConstants(Renderer* r, uint32_t first, uint32_t bytes) {
  ConstTimer timer(g_ct_gpu_ns);
  static const bool whole = EnvFlag("XMD_NATIVE_WHOLE_CONSTANTS");
  bytes = whole ? uint32_t(sizeof(g_const_shadow[0])) : std::clamp<uint32_t>((bytes + 63) & ~63u, 64, sizeof(g_const_shadow[0]));
  const uint32_t count = bytes / 16;
  const uint8_t* mirror = Device() + 0x780 + 16 * first;
  const int stage = first ? 1 : 0;
  // (the loaded constants changed since the last upload: not reused)
  const bool loaded_here = g_literal_version[stage] != g_literal_uploaded[stage];
  Renderer::ConstantUpload& last = r->constant_uploads[stage];
  uint8_t* shadow = g_const_shadow[stage];
  static const bool check = REXCVAR_GET(native_constant_check);
  uint64_t& dirty = g_const_dirty[stage];
  uint32_t& valid = g_const_valid[stage];
  if (!loaded_here && !g_const_force[stage] && last.ref.ref && bytes <= valid) {
    bool changed = false;
    for (uint64_t m = dirty; m && !changed;) {
      const uint32_t block = uint32_t(__builtin_clzll(m));  // (bit 63 = block 0)
      m &= ~(1ull << (63 - block));
      if (64 * block >= valid) continue;  // (not in the upload)
      changed = std::memcmp(mirror + 64 * block, shadow + 64 * block, 64) != 0;
      g_hash_const += 64;
    }
    if (!changed) {
      dirty = 0;
      if (check && std::memcmp(mirror, shadow, valid) != 0 && g_const_stale++ < 16) {
        REXLOG_WARN("native renderer: {} constants changed without a flush (upload was stale)",
                    stage ? "pixel" : "vertex");
      } else {
        ++r->perf_const_reused;
        return last.ref;
      }
    }
  }
  // The copy for later comparisons.
  std::memcpy(shadow, mirror, bytes);
  valid = bytes;
  dirty = 0;
  // (loaded constants overlay this draw's upload only: the next one re-uploads)
  g_const_force[stage] = false;
  g_literal_uploaded[stage] = g_literal_version[stage];
  auto [cpu, gpu] = Allocate(r, bytes);
  // (the shaders' view of them is 4096 bytes long whatever they read of it: all of it inside the ring)
  if (!cpu || gpu.offset + sizeof(g_const_shadow[0]) > kRingSize) {
    last = {};
    return {};
  }
  ++r->perf_const_uploads;
  last = {0, gpu};
  Swap32(cpu, mirror, bytes);
  // (the loaded flags eight at a time: most uploads have none set, and one by one this was 0.7 ms a frame)
  static_assert(sizeof(g_constants_loaded[0]) == 1);
  for (uint32_t index = first; index < first + count;) {
    if (index + 8 <= first + count) {
      uint64_t eight;
      std::memcpy(&eight, &g_constants_loaded[index], 8);
      if (!eight) {
        index += 8;
        continue;
      }
    }
    if (g_constants_loaded[index]) std::memcpy(cpu + 16 * (index - first), g_gpu_constants[index], 16);
    ++index;
  }
  // Debug: XMD_NATIVE_CONSTANT=<index 0-511>:<x>[,...] overrides a
  // constant's x (pixel constants are 256 + n).
  static const std::vector<std::pair<uint32_t, float>> overrides = [] {
    std::vector<std::pair<uint32_t, float>> v;
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "XMD_NATIVE_CONSTANT") == 0 && e) {
      for (char* p = e; *p;) {
        const uint32_t index = std::strtoul(p, &p, 10);
        if (*p != ':') break;
        const float value = std::strtof(p + 1, &p);
        if (index < 512) v.emplace_back(index, value);
        if (*p == ',') ++p;
        else break;
      }
      free(e);
    }
    return v;
  }();
  for (const auto& [index, value] : overrides)
    if (index >= first && index < first + count) std::memcpy(cpu + 16 * (index - first), &value, 4);
  return gpu;
}

textures::Context TextureContext(Renderer* r);

// Debug: XMD_NATIVE_BLANK_CYCLE=a,b,c,... binds the placeholder instead of
// the texture in fetch slot a, then b, ... for 10 s each (-1 = none); the
// window title shows the current one.
int BlankedSlot(Renderer* r) {
  static const std::vector<int> cycle = [] {
    std::vector<int> v;
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "XMD_NATIVE_BLANK_CYCLE") == 0 && e) {
      for (char* p = e; *p;) {
        v.push_back(std::strtol(p, &p, 10));
        if (*p == ',') ++p;
        else break;
      }
      free(e);
    }
    return v;
  }();
  if (cycle.empty()) return -2;
  return cycle[(r->frames / 600) % cycle.size()];
}

// Slot tables: a texture's swizzle for the shaders (3 bits a component: 0-3
// xyzw, 4 zero, 5 one), from its view's component mapping.
constexpr uint32_t kIdentitySwizzle = 0x688;
uint32_t SlotSwizzle(Renderer* r, uint32_t index) {
#if defined(_WIN32)
  const auto* table = static_cast<const TableRecord*>(r->texture_sets[0].get());
  if (index >= table->entries.size() || !table->entries[index].view) return kIdentitySwizzle;
  const plume::RenderComponentMapping& m =
      static_cast<const plume::D3D11TextureView*>(table->entries[index].view)->desc.componentMapping;
  const plume::RenderSwizzle c[4] = {m.r, m.g, m.b, m.a};
  uint32_t code = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t v = i;
    switch (c[i]) {
      case plume::RenderSwizzle::ZERO: v = 4; break;
      case plume::RenderSwizzle::ONE: v = 5; break;
      case plume::RenderSwizzle::R: v = 0; break;
      case plume::RenderSwizzle::G: v = 1; break;
      case plume::RenderSwizzle::B: v = 2; break;
      case plume::RenderSwizzle::A: v = 3; break;
      default: break;  // (IDENTITY)
    }
    code |= v << (3 * i);
  }
  return code;
#else
  return kIdentitySwizzle;
#endif
}

// Each shader stage's fetch slots' last answers (SharedConstants): most draws sample what the draw before them did
// in the same slot (80% of the lookups in a 6-man match), and while the frame and textures::Generation stay the same
// fetch constants get the same texture and sampler - the lookups (a key hash, two maps, the recheck rules) are
// skipped. Debug: XMD_NATIVE_NO_SLOT_MEMO=1 looks every one up.
struct SlotMemo {
  uint32_t fetch[6] = {};
  uint32_t dimension = UINT32_MAX;
  uint64_t frame = ~0ull, generation = 0;
  uint32_t srv = UINT32_MAX, sampler = UINT32_MAX;  // (sampler: UINT32_MAX until asked)
};
// (kept by the fetch constant's contents - 1024 places - not by slot: neighbouring draws use other textures in
// the same slot, and each miss is a walk through the texture cache's map)
SlotMemo g_slot_memo[1024];
const bool g_slot_memo_on = std::getenv("XMD_NATIVE_NO_SLOT_MEMO") == nullptr;

alignas(16) uint32_t g_shared_last[kSharedConstantsBytes / 4];  // (what Renderer::shared_upload holds)

// `ndc`: g_NdcScale (x, y) and g_HalfPixelOffset (the NDC offset, z, w).
RenderBufferReference SharedConstants(Renderer* r, bool alpha_test, const Shader* vs,
                                      const Shader* ps, const float ndc[4]) {
  ConstTimer timer(g_ct_shared_ns);
  ++g_ct_draws;
  // (made here and copied to the upload ring only when they differ from the last draw's)
  alignas(16) uint32_t made[kSharedConstantsBytes / 4];
  uint32_t* u = made;
  std::memset(made, 0, kSharedConstantsBytes);
  // Compact tables: the draw's textures and samplers get places in its own
  // sets (the indices below are those places; unused slots read place 0).
  const bool compact = backend::CompactTables();
  // Slot tables (Direct3D 11): each slot's texture and sampler at its own
  // place, and where the indices would be, its swizzle (D3D11 views have none).
  const bool slots = SlotTables();
  uint32_t places[2][kSlotTextures], used[3] = {}, samplers = 0;
  if (compact || slots) std::fill(&places[0][0], &places[0][0] + 2 * kSlotTextures, UINT32_MAX);
  if (slots) std::fill(u, u + 24 * 4, kIdentitySwizzle);
  // g_ResourceIndices[32] (uint4): 2D, 3D, cube, sampler blocks of 8 x uint4.
  // (placeholder 2D 0, 3D 1, cube 2; sampler 0 - linear wrap; the rest is zero already)
  if (!compact && !slots) {
    std::fill(u + 32, u + 64, 1u);
    std::fill(u + 64, u + 96, 2u);
  }
  // The textures the shaders sample, from the fetch constants.
  const textures::Context& ctx = r->texture_context;
  {
  ConstTimer tex_timer(g_ct_tex_ns);
  for (const Shader* s : {vs, ps}) {
    if (!s) continue;  // (a depth-only draw has no pixel shader)
    for (const auto& [slot, dimension] : s->textures) {
      ++g_ct_textures;
      const uint8_t* f = Device() + guest::RegisterOffset(0x4800) + slot * 24;
      uint32_t fetch[6];
      for (int k = 0; k < 6; ++k) fetch[k] = Be32(f + 4 * k);
      if (g_const_times) {
        uint32_t* lf = g_ct_last_fetch[s == ps ? 1 : 0][slot & 31];
        if (!std::memcmp(lf, fetch, sizeof(fetch))) ++g_ct_tex_same;
        std::memcpy(lf, fetch, sizeof(fetch));
      }
      {
        // debug: XMD_NATIVE_BLANK_CYCLE slot gets the placeholder, or with
        // XMD_NATIVE_CYCLE_ALPHA=1 its alpha forced to 1 (=2: every other
        // slot's alpha forced to 1)
        static const int alpha_mode = [] {
          char* e = nullptr;
          size_t n = 0;
          int m = 0;
          if (_dupenv_s(&e, &n, "XMD_NATIVE_CYCLE_ALPHA") == 0 && e) {
            m = std::atoi(e);
            free(e);
          }
          return m;
        }();
        const int blanked = BlankedSlot(r);
        if (blanked != -2) {
          const bool hit = int(slot) == blanked;
          if (alpha_mode == 0 && hit) continue;
          if ((alpha_mode == 1 && hit) || (alpha_mode == 2 && !hit))
            fetch[3] = (fetch[3] & ~(7u << 10)) | (5u << 10);
        }
      }
      SlotMemo& memo = g_slot_memo[(XXH3_64bits(fetch, sizeof(fetch)) + dimension * 0x9E3779B97F4A7C15ull) & 1023];
      uint32_t srv;
      if (g_slot_memo_on && memo.frame == ctx.frame && memo.dimension == dimension &&
          memo.generation == textures::Generation() && !std::memcmp(memo.fetch, fetch, sizeof(fetch))) {
        srv = memo.srv;
        ++g_ct_memo_hits;
      } else {
        srv = textures::Texture(ctx, fetch, dimension);
        // A 2D texture where the shader fetches a 3D one (this game does, the third coordinate unused): its
        // place in the 2D table, marked with the top bit (shader_common.h tfetch3D). Big tables only.
        if (srv == UINT32_MAX && dimension == 1 && !slots && !compact) {
            const uint32_t flat = textures::Texture(ctx, fetch, 0);
            if (flat != UINT32_MAX) srv = flat | 0x80000000u;
        }
        std::memcpy(memo.fetch, fetch, sizeof(fetch));
        memo.dimension = dimension;
        memo.frame = ctx.frame;
        memo.generation = textures::Generation();  // (after the lookup: its own upload counted)
        memo.srv = srv;
        memo.sampler = UINT32_MAX;
      }
      auto sampler_of = [&] {
        if (memo.sampler == UINT32_MAX) memo.sampler = textures::Sampler(ctx, fetch);
        return memo.sampler;
      };
      if (slots) {
        if (dimension > 2 || slot >= kSlotCount[dimension] || srv == UINT32_MAX) continue;
        const uint32_t index = r->texture_base[dimension] + srv;
        places[0][kSlotFirst[dimension] + slot] = index;
        u[(dimension * 8 + slot / 4) * 4 + slot % 4] = SlotSwizzle(r, index);
        places[1][SlotSampler(slot) & 15] = sampler_of();
        continue;
      }
      if (compact) {
        if (dimension > 2 || used[dimension] >= kCompactPlaces[dimension]) {
          static bool warned = false;
          if (!warned) REXLOG_WARN("native renderer: a draw has more textures than the compact tables hold");
          warned = true;
          continue;
        }
        const uint32_t place = used[dimension]++;
        places[0][kCompactFirst[dimension] + place] =
            srv == UINT32_MAX ? UINT32_MAX : r->texture_base[dimension] + srv;
        u[(dimension * 8 + slot / 4) * 4 + slot % 4] = place;
        if (srv == UINT32_MAX) continue;
        const uint32_t sampler = sampler_of();
        uint32_t s = 0;
        while (s < samplers && places[1][s] != sampler) ++s;  // (shared)
        if (s == samplers && samplers < kCompactSamplers) places[1][samplers++] = sampler;
        u[(3 * 8 + slot / 4) * 4 + slot % 4] = s < kCompactSamplers ? s : 0;
        continue;
      }
      if (srv == UINT32_MAX) continue;
      // (a 2D texture's exponent adjust - fetch word 3 bits 13-18, signed: the sample times 2^n - rides in bits 24-29
      // of its index: the game's anti-aliasing pass reads its picture at half value for some taps, and edges
      // came out twice as bright - white lines round everything)
      u[(dimension * 8 + slot / 4) * 4 + slot % 4] = srv | (dimension == 0 ? ((fetch[3] >> 13) & 0x3Fu) << 24 : 0u);
      u[(3 * 8 + slot / 4) * 4 + slot % 4] = sampler_of();
    }
  }
  }
  if (compact || slots) {
    ConstTimer sets_timer(g_ct_sets_ns);
    r->draw_sets[0] = CompactSet(r, 0, places[0]);
    r->draw_sets[1] = CompactSet(r, 1, places[1]);
  }
  // c32: g_Booleans (vertex b0-b31), g_SwappedTexcoords, g_HalfPixelOffset;
  // c33: g_AlphaThreshold, g_NdcScale, g_PsBooleans (pixel b0-b31, the
  // register file's bools 128-159).
  const uint8_t* bools = Device() + guest::RegisterOffset(0x4900);
  u[32 * 4 + 0] = Be32(bools);
  u[33 * 4 + 3] = Be32(bools + 16);
  // (vertex data is converted per element: no swapped texcoords; the upper half: InputLayout::float_uint_inputs)
  u[32 * 4 + 1] = g_draw_float_uint_inputs << 16;
  std::memcpy(&u[32 * 4 + 2], ndc + 2, 8);  // g_HalfPixelOffset (the NDC offset)
  std::memcpy(&u[33 * 4 + 1], ndc, 8);      // g_NdcScale
  const float threshold = alpha_test ? RegFloat(RB_ALPHA_REF) : 0.0f;
  std::memcpy(&u[33 * 4 + 0], &threshold, 4);
  if (r->shared_upload.ref && !std::memcmp(made, g_shared_last, kSharedConstantsBytes)) return r->shared_upload;
  auto [cpu, gpu] = Allocate(r, kSharedConstantsBytes);
  if (!cpu) return {};
  std::memcpy(cpu, made, kSharedConstantsBytes);
  std::memcpy(g_shared_last, made, kSharedConstantsBytes);
  r->shared_upload = gpu;
  return gpu;
}

// Binds the draw's constants (vertex, pixel, shared, own; null: none) - root
// CBVs on D3D12; on Vulkan their offsets in this frame's upload ring (bound
// as set 2) in the push constants. The pipeline layout must be set.
void BindConstants(Renderer* r, const RenderBufferReference constants[4]) {
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    const plume::RenderBuffer* ring = r->rings[r->back_index].buffer.get();
    uint32_t offsets[4];
    for (int i = 0; i < 4; ++i) {
      offsets[i] = constants[i].ref == ring ? uint32_t(constants[i].offset) : 0;
      if (constants[i].ref && constants[i].ref != ring) {
        static bool warned = false;
        if (!warned) REXLOG_ERROR("native renderer: constants outside the frame's upload ring (not readable on Vulkan)");
        warned = true;
      }
    }
    r->list->setGraphicsPushConstants(0, offsets, 0, sizeof(offsets));
    // (compact tables: the vertex, pixel and shared constants as uniform buffers)
    if (backend::CompactTables())
      r->list->setGraphicsDescriptorSetDynamic(r->constant_sets[r->back_index].get(), 2, offsets, 3);
    return;
  }
  for (uint32_t i = 0; i < 4; ++i)
    if (constants[i].ref) r->list->setGraphicsRootDescriptor(constants[i], i);
}

// At every draw (drawn or not): constants set through the mirror have reached
// the GPU; blocks loaded from memory for this draw take precedence.
// The XDK also writes constants into the mirror inline (no hookable call),
// so the whole mirror is taken at every draw; blocks loaded from memory for
// the draw (so far only the shaders' literal c252-c255) take precedence.
void ApplyPendingConstants() {
  // (nothing to do any more: loaded constants stay until written over - g_constants_loaded)
}

// A vertex shader's input layout for the current vertex declaration.
struct InputLayout {
  std::vector<Element> decl;
  std::vector<plume::RenderInputElement> layout;
  std::vector<std::string> names;  // (the layout's semantic names point here)
  bool packed_normals = false, byte_normals = false, packed_10_11_11 = false, uses_zero_stream = false;
  // The shader's integer inputs (in order) that this format gives floats: bit n = the nth. The particle
  // shaders declare a sprite's size BLENDINDICES - an integer input to the converted shader - and the game
  // gives it two floats: read as integers a sprite was a thousand million units across (the first level's
  // fire glows covered the screen). Their bits go through as they are and the shader takes them as floats
  // (XMD_UINT_INPUT in the converted shaders; the mask reaches them in g_SwappedTexcoords' upper half).
  uint32_t float_uint_inputs = 0;
  uint32_t hash = 0;  // (of the layout; strides are added per draw)
};
std::unordered_map<uint64_t, InputLayout> g_input_layouts;

// What a bound vertex shader's declaration is made from - the shader and its fetch instructions as they are in
// memory now - as one number (0: not a bound shader's, or not readable: ask Declaration). Reading the declaration
// out at every draw (a sort, name comparisons, a vector) was 10 ms a frame in the first level's 4900 draws.
uint64_t DeclarationKey(const Shader* vs) {
  const uint32_t object = Be32(Device() + guest::kDeviceCurrentVertexShader);
  if (object < 0x10000) return 0;
  const uint8_t* o = Virtual(object);
  if (Be32(o + guest::kVertexShaderBound) == 0xFFFFFFFFu) return 0;
  const uint8_t* c = o + guest::kVertexShaderContainer;
  if ((Be32(c) & 0xFFFFFF00u) != 0x102A1100u) return 0;
  const uint32_t vsize = Be32(c + 4), psize = Be32(c + 8), shader_at = Be32(c + 0x18);
  if (vsize > 0x10000 || psize > 0x10000 || shader_at + 0x24 > vsize) return 0;
  const uint8_t* shader = c + shader_at;
  const uint32_t first = Be32(shader + 0x18), n = std::min<uint32_t>(Be32(shader + 0x1C), 32);
  if (shader_at + 0x24 + 4 * (first + n) > vsize) return 0;
  const uint32_t micro_address = g_bound_vertex_container == object + guest::kVertexShaderContainer
                                     ? g_bound_vertex_microcode
                                     : Be32(o + guest::kVertexShaderMicrocode);
  const uint32_t code_at = Be32(shader), code_size = Be32(shader + 4);
  if (code_at > psize || code_size > psize - code_at) return 0;
  const uint8_t* micro;
  if (g_replay_device && g_replay_vertex_code) {
    if (!GuestReadable(g_replay_vertex_code, code_size)) return 0;
    micro = Virtual(g_replay_vertex_code);
  } else {
    if (!GuestReadable(micro_address + code_at, code_size)) return 0;
    micro = Virtual(micro_address) + code_at;
  }
  uint8_t words[32 * 12];
  uint32_t bytes = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t at = (Be32(shader + 0x24 + 4 * (first + i)) & 0xFFF) * 12;
    if (at + 12 > code_size) continue;
    std::memcpy(words + bytes, micro + at, 12);
    bytes += 12;
  }
  return XXH3_64bits_withSeed(words, bytes, vs->hash) | 1;
}
std::unordered_map<uint64_t, const InputLayout*> g_input_layout_of;

const InputLayout& GetInputLayout(const Shader* vs, bool exact = false) {
  // (`exact`: the frame being dumped - Declaration's own record of the fetches is wanted)
  const uint64_t quick = exact ? 0 : DeclarationKey(vs);
  if (quick) {
    auto known = g_input_layout_of.find(quick);
    if (known != g_input_layout_of.end()) return *known->second;
  }
  struct Remember {
    uint64_t quick;
    const InputLayout* il = nullptr;
    ~Remember() {
      if (quick && il) g_input_layout_of[quick] = il;
    }
  } remember{quick};
  // Keyed by the shader's contents and the declaration's elements (a
  // declaration object's address may be reused for another).
  uint64_t key = vs->hash * 0x9E3779B97F4A7C15ull;
  std::vector<Element> current = Declaration(vs);
  if (!current.empty()) key ^= XXH3_64bits(current.data(), current.size() * sizeof(Element)) + current.size();
  auto [it, inserted] = g_input_layouts.try_emplace(key);
  InputLayout& il = it->second;
  remember.il = &il;
  if (!inserted) return il;
  il.decl = std::move(current);
  const std::vector<Element>& decl = il.decl;
  il.names.reserve(vs->inputs.size());
  for (const ShaderInput& in : vs->inputs) il.names.push_back(in.semantic);
  std::vector<plume::RenderInputElement>& layout = il.layout;
  uint32_t uint_ordinal = 0;
  for (size_t k = 0; k < vs->inputs.size(); ++k) {
    const ShaderInput& in = vs->inputs[k];
    const uint32_t ordinal = in.is_uint ? uint_ordinal++ : 0;
    plume::RenderInputElement e;
    e.semanticName = il.names[k].c_str();
    e.semanticIndex = in.index;
    e.location = in.location >= 0 ? uint32_t(in.location) : uint32_t(layout.size());
    const Element* found = nullptr;
    for (const Element& el : decl) {
      if (el.usage < std::size(kUsageNames) && in.semantic == kUsageNames[el.usage] &&
          el.index == in.index) {
        found = &el;
        break;
      }
    }
    // (normals, tangents and binormals as signed bytes: fetched as unsigned integers, the shader's s32 variant
    // makes them -1..1)
    const bool frame_input = in.semantic == "NORMAL" || in.semantic == "TANGENT" || in.semantic == "BINORMAL";
    const bool bytes = found && frame_input && in.is_uint && (found->type & 0x3F) == 6;
    if (bytes) il.byte_normals = true;
    if (found && in.is_uint && ordinal < 16) {
      const uint32_t f = found->type & 0x3F;
      if (f == 36 || f == 37 || f == 38 || f == 57) il.float_uint_inputs |= 1u << ordinal;
    }
    RenderFormat fmt = found ? VertexFormat(bytes ? found->type & ~0x100u : found->type, in.is_uint) : RenderFormat::UNKNOWN;
    if (found && !SwizzleSupported(found->type)) {
      static std::unordered_map<uint32_t, bool> logged;
      if (logged.emplace(found->type, true).second)
        REXLOG_WARN("native renderer: vertex type {:08X} has an unsupported swizzle", found->type);
    }
    if (found && fmt != RenderFormat::UNKNOWN && found->stream < 15) {
      e.format = fmt;
      e.slotIndex = found->stream;
      e.alignedByteOffset = found->offset;
      // (packed normals, tangents and binormals - 10:11:11 here - are unpacked by the shader's s1 variant)
      if ((in.semantic == "NORMAL" || in.semantic == "TANGENT" || in.semantic == "BINORMAL") &&
          ((found->type & 0x3F) == 16 || (found->type & 0x3F) == 17)) {
        il.packed_normals = true;
        if ((found->type & 0x3F) == 16) il.packed_10_11_11 = true;  // (x in the low 11 bits: its own variant)
      }
    } else {
      e.format = in.is_uint ? RenderFormat::R32G32B32A32_UINT : RenderFormat::R32G32B32A32_FLOAT;
      e.slotIndex = 15;  // zero stream
      e.alignedByteOffset = 0;
      il.uses_zero_stream = true;
    }
    layout.push_back(e);
  }
  for (const auto& e : layout) {
    il.hash = il.hash * 31 + uint32_t(e.format) * 7 + e.slotIndex * 131 + e.alignedByteOffset * 17 +
              e.semanticIndex;
  }
  if (g_input_layouts.size() <= 80) {
    std::string text;
    for (const Element& el : decl)
      text += fmt::format(" {}{}:s{}+{}/{} f{}{}", el.usage < std::size(kUsageNames) ? kUsageNames[el.usage] : "?", el.index,
                          el.stream, el.offset, el.stride, el.type & 0x3F, (el.type & 0x100) ? "s" : "");
    REXLOG_INFO("native renderer: vertex layout for shader {:016X}: {} inputs, {} from the format{}{}", vs->hash,
                vs->inputs.size(), decl.size(), il.uses_zero_stream ? " (some inputs have no data)" : "", text);
    if (vs->inputs.size() != decl.size() || !vs->indexed.empty()) {  // (debug: which object, and its container as it is now)
      const uint32_t object = Be32(Device() + guest::kDeviceCurrentVertexShader);
      const uint8_t* c = Virtual(object) + guest::kVertexShaderContainer;
      const uint32_t vsize = Be32(c + 4), at = Be32(c + 0x18);
      uint64_t h = 1469598103934665603ull;
      for (uint32_t i = 0; i < vsize && vsize < 0x10000; ++i)
        h = (h ^ (i == 3 ? uint8_t(c[i] & 0xBF) : c[i])) * 1099511628211ull;
      REXLOG_INFO("native renderer:   object {:08X} flags {:08X} sizes {:X}+{:X} bound word {:08X} elements {}+{} hash now "
                  "{:016X}; last bind: container {:08X} microcode {:08X}, object's own {:08X}",
                  object, Be32(c), vsize, Be32(c + 8), Be32(Virtual(object) + guest::kVertexShaderBound),
                  Be32(c + at + 0x18), Be32(c + at + 0x1C), h, g_bound_vertex_container, g_bound_vertex_microcode,
                  Be32(Virtual(object) + guest::kVertexShaderMicrocode));
      std::string words;
      const uint8_t* micro = Virtual(Be32(Virtual(object) + guest::kVertexShaderMicrocode)) + Be32(c + at);
      const uint32_t count = std::min<uint32_t>(Be32(c + at + 0x1C), 24);
      for (uint32_t i = 0; i < count && vsize < 0x10000; ++i) {
        const uint32_t v = Be32(c + at + 0x24 + 4 * (Be32(c + at + 0x18) + i));
        const uint8_t* m = micro + (v & 0xFFF) * 12;
        words += fmt::format(" [{:08X}: {:08X} {:08X} {:08X}]", v, Be32(m), Be32(m + 4), Be32(m + 8));
      }
      REXLOG_INFO("native renderer:   fetches{}", words);
    }
  }
  return il;
}

// Whether a draw's positions are flat 2D in the target's pixels (a HUD
// sprite: x, y within the target, one depth in 0..1), from its first 64
// vertices (float positions only; a DrawPrimitiveUP's or its vertex
// buffer's). `full`: they cover the whole target (a fade).
bool FlatPixels(const std::vector<Element>& decl, const PendingUP* up, bool indexed, uint32_t start,
                uint32_t count, int32_t base_vertex, float tw, float th, bool* full) {
  for (const Element& el : decl) {
    if (el.usage != 0 || el.index != 0) continue;
    const RenderFormat f = VertexFormat(el.type, false);
    const int comps = f == RenderFormat::R32G32_FLOAT ? 2 : f == RenderFormat::R32G32B32_FLOAT ? 3
                      : f == RenderFormat::R32G32B32A32_FLOAT ? 4 : 0;
    if (!comps) return false;
    const uint8_t* data = nullptr;
    uint32_t stride = 0;
    if (up) {
      if (el.stream != 0) return false;
      data = ObjectData(up->data), stride = up->stride;
    } else {
      if (el.stream >= 15 || !g_streams[el.stream].buffer) return false;
      const Stream& st = g_streams[el.stream];
      data = ObjectData(Be32(BufferObject(st.buffer) + 0x18) & ~3u) + st.offset, stride = st.stride;
    }
    if (!data || !stride || !count) return false;
    const uint8_t* ib = nullptr;
    if (indexed && !up) {
      const uint32_t ibo = Be32(Device() + guest::kDeviceIndexBuffer);
      if (!ibo) return false;
      ib = ObjectData(Be32(Virtual(ibo) + 0x18)) + start * 2;
    }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t k = 0; k < std::min<uint32_t>(count, 64); ++k) {
      const int64_t v = ib ? int64_t(Be16(ib + 2 * k)) + base_vertex : int64_t(up ? k : start + k);
      if (v < 0) return false;
      const uint8_t* q = data + v * stride + el.offset;
      for (int c = 0; c < std::min(comps, 3); ++c) {
        const float x = BeFloat(q + 4 * c);
        mn[c] = std::min(mn[c], x), mx[c] = std::max(mx[c], x);
      }
    }
    const bool flat = comps == 2 || (mn[2] == mx[2] && mn[2] >= 0.0f && mn[2] <= 1.0f);
    const bool pixels = mn[0] >= -2 && mn[1] >= -2 && mx[0] <= tw + 2 && mx[1] <= th + 2 &&
                        (mx[0] - mn[0] > 2 || mx[1] - mn[1] > 2);
    *full = mn[0] <= 1 && mn[1] <= 1 && mx[0] >= tw - 1 && mx[1] >= th - 1;
    return flat && pixels;
  }
  return false;
}

// `up`: a DrawVerticesUP (stream 0 is the game's vertex data, not a buffer).
#if XMD_FSR2
// FSR 2's mask values (reactive) for the HUD, blended draws, skinned ones and the crowd; composition 1 for all.
// (Test aid: XMD_TEST_FSR2_MASK=<hud>,<blended>,<skinned>,<crowd>; 0,0,0,0 = no masks.)
float g_fsr2_mask_values[4] = {1.0f, 0.5f, 0.35f, 0.5f};
bool g_fsr2_masks_off = false;

// This draw's frame noted (the list started again in a new frame); false without the mask shader.
bool Fsr2MaskWanted(Renderer* r) {
  static const bool read = [] {
    if (const char* v = std::getenv("XMD_TEST_FSR2_MASK")) {
      std::sscanf(v, "%f,%f,%f,%f", &g_fsr2_mask_values[0], &g_fsr2_mask_values[1], &g_fsr2_mask_values[2],
                  &g_fsr2_mask_values[3]);
      g_fsr2_masks_off = g_fsr2_mask_values[0] <= 0 && g_fsr2_mask_values[1] <= 0 && g_fsr2_mask_values[2] <= 0 &&
                         g_fsr2_mask_values[3] <= 0;
    }
    return true;
  }();
  (void)read;
  if (g_fsr2_masks_off) return false;
  if (r->fsr2_mask_code.empty()) {
    r->fsr2_mask_code = ReadShader("fsr2_mask.ps");
    if (r->fsr2_mask_code.empty()) {
      REXLOG_WARN("native renderer: fsr2_mask.ps missing - FSR 2 without masks");
      g_fsr2_masks_off = true;
      return false;
    }
  }
  if (r->fsr2_mask_draws_frame != r->frames) {
    r->fsr2_mask_draws.clear();
    r->fsr2_mask_after_3d = 0;
    r->fsr2_mask_draws_frame = r->frames;
  }
  return true;
}

// The mask pipeline of a draw: its vertex shader and input layout, fsr2_mask.ps into the two R8 targets (the
// largest value kept), depth tested as the draw (less-equal: drawn again over its own depth) without writing.
const plume::RenderPipeline* Fsr2MaskPipeline(Renderer* r, Shader* vs, int vs_variant,
                                              const std::vector<plume::RenderInputElement>& layout,
                                              const plume::RenderInputSlot* slots, uint32_t layout_hash,
                                              plume::RenderPrimitiveTopology topology, bool depth_test) {
  const uint32_t cull = Reg(PA_SU_SC_MODE_CNTL) & 7;
  const uint64_t parts[3] = {vs->hash,
                             uint64_t(vs_variant) | uint64_t(topology) << 8 | uint64_t(cull) << 16 |
                                 uint64_t(depth_test) << 24,
                             layout_hash};
  const uint64_t h = XXH3_64bits(parts, sizeof(parts));
  auto& slot = r->fsr2_mask_pipelines[h];
  if (slot) return slot.get();
  if (!r->fsr2_mask_ps)
    r->fsr2_mask_ps = r->device->createShader(r->fsr2_mask_code.data(), r->fsr2_mask_code.size(), "main",
                                              backend::ShaderFormat());
  plume::RenderGraphicsPipelineDesc d;
  d.pipelineLayout = r->layout.get();
  d.vertexShader = Compiled(r, vs, vs_variant);
  d.pixelShader = r->fsr2_mask_ps.get();
  for (int t = 0; t < 2; ++t) {
    plume::RenderBlendDesc& b = d.renderTargetBlend[t];
    b.blendEnabled = true;
    b.srcBlend = b.dstBlend = b.srcBlendAlpha = b.dstBlendAlpha = plume::RenderBlend::ONE;
    b.blendOp = b.blendOpAlpha = plume::RenderBlendOperation::MAX;
    b.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALL);
    d.renderTargetFormat[t] = RenderFormat::R8_UNORM;
  }
  d.renderTargetCount = 2;
  d.cullMode = (cull & 1)   ? plume::RenderCullMode::FRONT
               : (cull & 2) ? plume::RenderCullMode::BACK
                            : plume::RenderCullMode::NONE;
  d.frontFace = (cull & 4) ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  d.depthClipEnabled = true;
  d.depthEnabled = depth_test;
  d.depthWriteEnabled = false;
  d.depthFunction = plume::RenderComparisonFunction::LESS_EQUAL;
  d.stencilEnabled = false;
  d.dynamicStencilReferenceEnabled = true;
  d.dynamicBlendFactorEnabled = true;
  d.inputElements = layout.data();
  d.inputElementsCount = uint32_t(layout.size());
  d.inputSlots = slots;
  d.inputSlotsCount = 16;
  d.primitiveTopology = topology;
  d.depthTargetFormat = backend::DepthFormat();
  if (d.vertexShader && d.pixelShader) slot = r->device->createGraphicsPipeline(d);
  if (slot && !backend::PipelineCreated(slot.get())) slot.reset();
  if (!slot) {
    static int logged = 0;
    if (logged++ < 3) REXLOG_WARN("native renderer: FSR 2 mask pipeline for vs {:016X} failed", vs->hash);
  }
  return slot.get();
}

// The frame's noted mask draws drawn again over the scene's depth (the scene being resolved: `depth`, `w` x `h`)
// into the reactive and composition targets.
void DrawFsr2Masks(Renderer* r, DepthTarget* depth, uint32_t w, uint32_t h) {
  if (r->fsr2_mask_draws_frame != r->frames || r->fsr2_mask_draws.empty() || !depth || !depth->resource) return;
  auto* list = r->list;
  if (!r->fsr2_reactive || r->fsr2_mask_w != w || r->fsr2_mask_h != h) {
    for (auto* t : {&r->fsr2_reactive, &r->fsr2_composition})
      if (*t) Retire(r, std::move(*t));
    r->fsr2_mask_framebuffers.clear();  // (they name the old textures)
    for (auto* t : {&r->fsr2_reactive, &r->fsr2_composition}) {
      *t = r->device->createTexture(plume::RenderTextureDesc::Texture2D(
          w, h, 1, RenderFormat::R8_UNORM, plume::RenderTextureFlag::RENDER_TARGET));
      if (!*t) return;
    }
    r->fsr2_reactive->setName("FSR 2 reactive");
    r->fsr2_composition->setName("FSR 2 composition");
    r->fsr2_reactive_layout = r->fsr2_composition_layout = RenderTextureLayout::UNKNOWN;
    r->fsr2_mask_w = w, r->fsr2_mask_h = h;
  }
  plume::RenderFramebuffer* fb = nullptr;
  for (auto& f : r->fsr2_mask_framebuffers)
    if (f.depth == depth->resource) fb = f.framebuffer.get();
  if (!fb) {
    const plume::RenderTexture* colors[2] = {r->fsr2_reactive.get(), r->fsr2_composition.get()};
    auto made = r->device->createFramebuffer(plume::RenderFramebufferDesc(colors, 2, depth->resource.get()));
    if (!made) return;
    fb = made.get();
    if (r->fsr2_mask_framebuffers.size() > 4) r->fsr2_mask_framebuffers.erase(r->fsr2_mask_framebuffers.begin());
    r->fsr2_mask_framebuffers.push_back({depth->resource, std::move(made)});
  }
  Transition(r, r->fsr2_reactive.get(), r->fsr2_reactive_layout, RenderTextureLayout::COLOR_WRITE);
  Transition(r, r->fsr2_composition.get(), r->fsr2_composition_layout, RenderTextureLayout::COLOR_WRITE);
  Transition(r, depth->resource.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
  list->setFramebuffer(fb);
  list->clearColor(0, plume::RenderColor(0, 0, 0, 0));
  list->clearColor(1, plume::RenderColor(0, 0, 0, 0));
  list->setGraphicsPipelineLayout(r->layout.get());
  r->list_state = {};
  BindTables(r, list);
  r->list_state.bound = true;
  for (size_t i = 0; i < r->fsr2_mask_draws.size(); ++i) {
    const auto& m = r->fsr2_mask_draws[i];
    if (m.hud && i < r->fsr2_mask_after_3d) continue;  // (a full-screen background, a flare in the scene)
    list->setPipeline(m.pso);
    BindConstants(r, m.constants);
    list->setVertexBuffers(0, m.views, m.view_count, m.slots);
    list->setViewports(m.viewport);
    list->setScissors(m.scissor);
    list->setStencilReference(m.stencil_ref);
    if (m.indexed) {
      list->setIndexBuffer(&m.ibv);
      list->drawIndexedInstanced(m.count, 1, 0, m.base, 0);
    } else {
      list->drawInstanced(m.count, 1, m.start, 0);
    }
  }
  Transition(r, r->fsr2_reactive.get(), r->fsr2_reactive_layout, RenderTextureLayout::SHADER_READ);
  Transition(r, r->fsr2_composition.get(), r->fsr2_composition_layout, RenderTextureLayout::SHADER_READ);
  r->list_state = {};  // (the next draw binds its own framebuffer, pipeline, buffers)
  r->fsr2_mask_frame = r->frames;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("native renderer: FSR 2 masks: {} draws", r->fsr2_mask_draws.size());
  }
}

// The skinned draws' motion pipeline: fsr2_skin.vs with the draw's input layout, fsr2_skin.ps into the motion
// vectors (RGBA16F), depth tested as the draw (less-equal, no writes); null without the shaders (test aid
// XMD_TEST_FSR2_NO_SKIN: none).
const plume::RenderPipeline* Fsr2SkinPipeline(Renderer* r, const std::vector<plume::RenderInputElement>& layout,
                                              const plume::RenderInputSlot* slots, uint32_t layout_hash,
                                              plume::RenderPrimitiveTopology topology) {
  static const bool off = EnvFlag("XMD_TEST_FSR2_NO_SKIN");
  if (off || r->fsr2_skin_off) return nullptr;
  if (!r->fsr2_skin_vs) {
    r->fsr2_skin_vs_code = ReadShader("fsr2_skin.vs");
    r->fsr2_skin_ps_code = ReadShader("fsr2_skin.ps");
    if (r->fsr2_skin_vs_code.empty() || r->fsr2_skin_ps_code.empty()) {
      REXLOG_WARN("native renderer: fsr2_skin.vs / .ps missing - the wrestlers keep the camera's motion");
      r->fsr2_skin_off = true;
      return nullptr;
    }
    r->fsr2_skin_vs = r->device->createShader(r->fsr2_skin_vs_code.data(), r->fsr2_skin_vs_code.size(), "main",
                                              backend::ShaderFormat());
    r->fsr2_skin_ps = r->device->createShader(r->fsr2_skin_ps_code.data(), r->fsr2_skin_ps_code.size(), "main",
                                              backend::ShaderFormat());
    if (!r->fsr2_skin_vs || !r->fsr2_skin_ps) {
      r->fsr2_skin_off = true;
      return nullptr;
    }
  }
  const uint32_t cull = Reg(PA_SU_SC_MODE_CNTL) & 7;
  const uint64_t parts[2] = {uint64_t(topology) << 8 | uint64_t(cull) << 16, layout_hash};
  const uint64_t h = XXH3_64bits(parts, sizeof(parts));
  auto& slot = r->fsr2_skin_pipelines[h];
  if (slot) return slot.get();
  plume::RenderGraphicsPipelineDesc d;
  d.pipelineLayout = r->layout.get();
  d.vertexShader = r->fsr2_skin_vs.get();
  d.pixelShader = r->fsr2_skin_ps.get();
  d.renderTargetBlend[0].blendEnabled = false;
  d.renderTargetBlend[0].renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALL);
  d.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
  d.renderTargetCount = 1;
  d.cullMode = (cull & 1)   ? plume::RenderCullMode::FRONT
               : (cull & 2) ? plume::RenderCullMode::BACK
                            : plume::RenderCullMode::NONE;
  d.frontFace = (cull & 4) ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  d.depthClipEnabled = true;
  d.depthEnabled = true;
  d.depthWriteEnabled = false;
  d.depthFunction = plume::RenderComparisonFunction::LESS_EQUAL;
  d.stencilEnabled = false;
  d.dynamicStencilReferenceEnabled = true;
  d.dynamicBlendFactorEnabled = true;
  d.inputElements = layout.data();
  d.inputElementsCount = uint32_t(layout.size());
  d.inputSlots = slots;
  d.inputSlotsCount = 16;
  d.primitiveTopology = topology;
  d.depthTargetFormat = backend::DepthFormat();
  slot = r->device->createGraphicsPipeline(d);
  if (slot && !backend::PipelineCreated(slot.get())) slot.reset();
  if (!slot) {
    static int logged = 0;
    if (logged++ < 3) REXLOG_WARN("native renderer: FSR 2 skinned motion pipeline failed");
  }
  return slot.get();
}

// The skinned draws noted this frame drawn again into the motion vectors with their own motion (where their last
// frame's bones are known), over the scene's depth (`depth`, `w` x `h`: the motion vectors' size); the camera's
// motion pass keeps those texels (alpha 1).
void DrawFsr2SkinMotion(Renderer* r, DepthTarget* depth, uint32_t w, uint32_t h) {
  if (r->fsr2_mask_draws_frame != r->frames || !depth || !depth->resource || !r->fsr2_motion ||
      r->fsr2_depth_w != w || r->fsr2_depth_h != h)
    return;
  uint32_t drawn = 0;
  plume::RenderFramebuffer* fb = nullptr;
  auto* list = r->list;
  for (const auto& m : r->fsr2_mask_draws) {
    if (!m.skin_key || !m.skin_pso) continue;
    const auto now = r->fsr2_skin_now.find(m.skin_key);
    const auto prev = r->fsr2_skin_prev.find(m.skin_key);
    if (now == r->fsr2_skin_now.end() || prev == r->fsr2_skin_prev.end()) continue;
    if (!fb) {
      for (auto& f : r->fsr2_skin_framebuffers)
        if (f.depth == depth->resource) fb = f.framebuffer.get();
      if (!fb) {
        const plume::RenderTexture* color = r->fsr2_motion.get();
        auto made = r->device->createFramebuffer(plume::RenderFramebufferDesc(&color, 1, depth->resource.get()));
        if (!made) return;
        fb = made.get();
        if (r->fsr2_skin_framebuffers.size() > 4) r->fsr2_skin_framebuffers.erase(r->fsr2_skin_framebuffers.begin());
        r->fsr2_skin_framebuffers.push_back({depth->resource, std::move(made)});
      }
      Transition(r, r->fsr2_motion.get(), r->fsr2_motion_layout, RenderTextureLayout::COLOR_WRITE);
      Transition(r, depth->resource.get(), depth->layout, RenderTextureLayout::DEPTH_WRITE);
      list->setFramebuffer(fb);
      list->setGraphicsPipelineLayout(r->layout.get());
      r->list_state = {};
      BindTables(r, list);
      r->list_state.bound = true;
    }
    // own constants: VP now / then, bones now / then, the bone count and NDC offset, the NDC scale, the size
    constexpr size_t kSize = (4 + 4 + 64 + 64 + 3) * 16;
    auto [cpu, gpu] = Allocate(r, kSize);
    if (!cpu) break;
    float* c = reinterpret_cast<float*>(cpu);
    std::memcpy(c, now->second.rows, 16 * 4);                 // c54-c57 now
    std::memcpy(c + 16, prev->second.rows, 16 * 4);           // then
    std::memcpy(c + 32, now->second.rows + 16, 64 * 16);      // bones now
    std::memcpy(c + 32 + 256, prev->second.rows + 16, 64 * 16);  // then
    float* params = c + 32 + 512;
    params[0] = float(m.skin_bones), params[1] = 0, params[2] = m.skin_ndc[2], params[3] = m.skin_ndc[3];
    params[4] = m.skin_ndc[0], params[5] = m.skin_ndc[1], params[6] = 0, params[7] = 0;
    params[8] = float(w), params[9] = float(h), params[10] = 0, params[11] = 0;
    RenderBufferReference constants[4] = {m.constants[0], m.constants[1], m.constants[2], gpu};
    list->setPipeline(m.skin_pso);
    BindConstants(r, constants);
    list->setVertexBuffers(0, m.views, m.view_count, m.slots);
    list->setViewports(m.viewport);
    list->setScissors(m.scissor);
    list->setStencilReference(m.stencil_ref);
    if (m.indexed) {
      list->setIndexBuffer(&m.ibv);
      list->drawIndexedInstanced(m.count, 1, 0, m.base, 0);
    } else {
      list->drawInstanced(m.count, 1, m.start, 0);
    }
    ++drawn;
  }
  if (fb) {
    Transition(r, r->fsr2_motion.get(), r->fsr2_motion_layout, RenderTextureLayout::SHADER_READ);
    r->list_state = {};
  }
  static bool logged = false;
  if (!logged && drawn) {
    logged = true;
    REXLOG_INFO("native renderer: FSR 2 / DLSS: {} skinned draws with their own motion", drawn);
  }
}
#endif

// The rows a skinned draw's vertex shader fetches by bone number (Shader::indexed), as constants: float4
// row 0 = (what the shader multiplies its index values by, 0, 0, 0), then for each entry of the stream the
// elements in the shader's order. The index values come from a vertex input: bytes fetched as integers reach
// the shader scaled to 0..1 (there is no unscaled byte format), so they are scaled back by 255.
RenderBufferReference IndexedRows(Renderer* r, const Shader* vs, const std::vector<Element>& decl) {
  constexpr uint32_t kRows = 4096;  // (a constant buffer's most: 1365 bones of three rows)
  const uint32_t per_entry = uint32_t(vs->indexed.size());
  const Element* elements[8] = {};
  if (!per_entry || per_entry > 8) return {};
  for (uint32_t k = 0; k < per_entry; ++k) {
    for (const Element& e : decl) {
      if (e.usage < std::size(kUsageNames) && vs->indexed[k].first == kUsageNames[e.usage] &&
          e.index == vs->indexed[k].second)
        elements[k] = &e;
    }
    if (!elements[k] || elements[k]->stream >= 16 || !elements[k]->stride) {
      static int logged = 0;
      if (logged++ < 8)
        REXLOG_WARN("native renderer: shader {:016X} fetches {}{} by index, but its format has no such element",
                    vs->hash, vs->indexed[k].first, vs->indexed[k].second);
      return {};
    }
  }
  const Stream& s = g_streams[elements[0]->stream];
  if (!s.buffer) return {};
  const uint8_t* vb = BufferObject(s.buffer);
  const uint32_t address = Be32(vb + 0x18) & ~3u, bytes = Be32(vb + 0x1C) & ~3u;
  const uint32_t stride = elements[0]->stride;
  if (s.offset >= bytes) return {};
  // (bone numbers are bytes: 256 entries from the stream's offset at most. The game keeps every model's bones
  // in one stream with no size - its buffer object says 16 MB - and sets the offset per draw.)
  uint32_t entries = std::min(std::min((bytes - s.offset) / stride, 256u), (kRows - 1) / per_entry);
  auto [cpu, gpu] = Allocate(r, std::max<uint64_t>(256, uint64_t(1 + entries * per_entry) * 16));
  if (!cpu) return {};
  float scale = 1.0f;
  for (const Element& e : decl)
    if ((e.type & 0x3F) == 6 && (e.type & 0x200)) scale = 255.0f;
  const uint8_t* src = ObjectData(address) + s.offset;
  // (as many as can be read: the stream's memory ends soon after the last bone in use)
  {
    const uint32_t first = address + s.offset, need = entries * stride;
    uint32_t ok = 0;
    while (ok < need) {
      const uint32_t chunk = std::min(need - ok, 0x1000 - ((first + ok) & 0xFFF));
      if (!GuestReadable(first + ok, chunk)) break;
      ok += chunk;
    }
    entries = ok / stride;
    if (!entries) return {};
  }
  // (the usual case - a bone's rows are float4s one after the other, nothing else in the entry: swapped across)
  bool plain = stride == 16 * per_entry;
  for (uint32_t k = 0; k < per_entry && plain; ++k)
    plain = (elements[k]->type & 0x3F) == 38 && elements[k]->offset == 16 * k && elements[k]->stream == elements[0]->stream;
  if (plain) {
    const float first[4] = {scale, 0.0f, 0.0f, 0.0f};
    std::memcpy(cpu, first, 16);
    Swap32(cpu + 16, src, size_t(entries) * stride);
    return gpu;
  }
  thread_local std::vector<float> rows;
  rows.assign(size_t(1 + entries * per_entry) * 4, 0.0f);
  rows[0] = scale;
  for (uint32_t k = 0; k < per_entry; ++k) {
    const Element& e = *elements[k];
    const uint32_t f = e.type & 0x3F;
    const uint32_t floats = f == 36 ? 1 : f == 37 ? 2 : f == 57 ? 3 : f == 38 ? 4 : 0;
    if (!floats || e.stream != elements[0]->stream || e.offset + 4 * floats > stride) {
      static int logged = 0;
      if (logged++ < 8)
        REXLOG_WARN("native renderer: shader {:016X}: indexed element {}{} has format {} in stream {} - not read",
                    vs->hash, vs->indexed[k].first, vs->indexed[k].second, f, e.stream);
      continue;
    }
    for (uint32_t i = 0; i < entries; ++i) {
      const uint8_t* p = src + size_t(i) * stride + e.offset;
      float* out = &rows[size_t(1 + i * per_entry + k) * 4];
      for (uint32_t c = 0; c < floats; ++c) out[c] = BeFloat(p + 4 * c);
    }
  }
  std::memcpy(cpu, rows.data(), rows.size() * 4);
  return gpu;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
  uint32_t bits;
  if (exp == 0) {
    if (!man) {
      bits = sign;
    } else {
      int e = -1;
      uint32_t m = man;
      do { ++e; m <<= 1; } while (!(m & 0x400));
      bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3FF) << 13;
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | man << 13;
  } else {
    bits = sign | (exp + 112) << 23 | man << 13;
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint16_t FloatToHalf(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
  const int32_t exp = int32_t((bits >> 23) & 0xFF) - 127 + 15;
  const uint32_t man = bits & 0x7FFFFF;
  if (exp <= 0) {
    if (exp < -10) return sign;
    const uint32_t m = (man | 0x800000) >> (1 - exp);
    return uint16_t(sign | ((m + 0x1000) >> 13));
  }
  if (exp >= 31) return uint16_t(sign | 0x7C00);
  return uint16_t(sign | (exp << 10) | ((man + 0x1000) >> 13));
}

// `n` rectangles (three guest vertices each, of one stream) as two triangles
// each, little-endian, into `dst` (6 * n vertices). The three vertices are
// three corners; the two furthest apart are the diagonal, and the fourth
// corner is diagonal end + diagonal end - the third: of the position and of
// every other float value (texture coordinates). The second triangle winds
// as the first.
void BuildRectangles(uint8_t* dst, const uint8_t* src, uint32_t n, uint32_t stride, const std::vector<Element>& decl,
                     uint32_t stream) {
  thread_local std::vector<uint8_t> in, out;
  const size_t bytes = size_t(n) * 3 * stride;
  if (in.size() < bytes) in.resize(bytes);
  if (out.size() < bytes * 2) out.resize(bytes * 2);
  Swap32(in.data(), src, bytes);
  FixHalfWords(in.data(), bytes, stride, 0, decl, stream);
  const Element* position = nullptr;
  for (const Element& e : decl) {
    const uint32_t f = e.type & 0x3F;
    if (e.stream == stream && e.usage == 0 && e.index == 0 && (f == 37 || f == 38 || f == 57) && e.offset + 8 <= stride)
      position = &e;
  }
  auto xy = [&](const uint8_t* v, float* p) { std::memcpy(p, v + (position ? position->offset : 0), 8); };
  for (uint32_t k = 0; k < n; ++k) {
    const uint8_t* v[3] = {&in[size_t(3 * k) * stride], &in[size_t(3 * k + 1) * stride], &in[size_t(3 * k + 2) * stride]};
    float p[3][2];
    for (int i = 0; i < 3; ++i) xy(v[i], p[i]);
    auto d2 = [&](int a, int b) {
      return (p[a][0] - p[b][0]) * (p[a][0] - p[b][0]) + (p[a][1] - p[b][1]) * (p[a][1] - p[b][1]);
    };
    // corner: the vertex opposite the longest side
    const float d01 = d2(0, 1), d12 = d2(1, 2), d02 = d2(0, 2);
    const int corner = d01 >= d12 && d01 >= d02 ? 2 : d12 >= d02 ? 0 : 1;
    const int a = (corner + 1) % 3, b = (corner + 2) % 3;  // (corner, a, b: the first triangle's winding)
    uint8_t* o = &out[size_t(6 * k) * stride];
    for (int i = 0; i < 3; ++i) std::memcpy(o + i * stride, v[i], stride);
    uint8_t* fourth = o + 5 * stride;
    std::memcpy(o + 3 * stride, v[b], stride);
    std::memcpy(o + 4 * stride, v[a], stride);
    std::memcpy(fourth, v[corner], stride);
    for (const Element& e : decl) {
      if (e.stream != stream) continue;
      const uint32_t f = e.type & 0x3F;
      const uint32_t floats = f == 36 ? 1 : f == 37 ? 2 : f == 57 ? 3 : f == 38 ? 4 : 0;
      const uint32_t halves = f == 31 ? 2 : f == 32 ? 4 : 0;
      for (uint32_t c = 0; c < floats && e.offset + 4 * c + 4 <= stride; ++c) {
        float fa, fb, fc;
        std::memcpy(&fa, v[a] + e.offset + 4 * c, 4);
        std::memcpy(&fb, v[b] + e.offset + 4 * c, 4);
        std::memcpy(&fc, v[corner] + e.offset + 4 * c, 4);
        const float r4 = fa + fb - fc;
        std::memcpy(fourth + e.offset + 4 * c, &r4, 4);
      }
      for (uint32_t c = 0; c < halves && e.offset + 2 * c + 2 <= stride; ++c) {
        uint16_t ha, hb, hc;
        std::memcpy(&ha, v[a] + e.offset + 2 * c, 2);
        std::memcpy(&hb, v[b] + e.offset + 2 * c, 2);
        std::memcpy(&hc, v[corner] + e.offset + 2 * c, 2);
        const uint16_t r4 = FloatToHalf(HalfToFloat(ha) + HalfToFloat(hb) - HalfToFloat(hc));
        std::memcpy(fourth + e.offset + 2 * c, &r4, 2);
      }
    }
  }
  std::memcpy(dst, out.data(), bytes * 2);
}

// The vertex streams as the GPU is given them: the vertex fetch constants (register mirror: constant 95 is
// stream 0, 94 stream 1 ...), each naming a stream from its offset to its buffer's end. A buffer is known by
// its end and converted once, from the lowest offset seen (a conversion per offset took 40 GB in the first
// level); the stream's offset is its start's distance from that. The game sets some streams by a path no hook
// sees (a skinned prop's in the HUD's pass: drawn with the streams of a draw before it, it covered the screen).
std::unordered_map<uint32_t, uint32_t> g_buffer_starts;  // a vertex buffer's end -> its lowest start seen (physical)
// (what each stream was last made from - a draw changes one or two of the sixteen; g_stream_words_valid is
// cleared wherever g_streams is put back or the frame begins)
uint32_t g_stream_words[16][2];
void StreamsFromFetch(const uint8_t* fetch) {
  if (g_buffer_starts.size() > 400000) g_buffer_starts.clear();  // (the game's short-lived buffers: forgotten in time)
  const bool valid = g_stream_words_valid;
  g_stream_words_valid = true;
  for (uint32_t s = 0; s < 16; ++s) {
    const uint32_t w0 = Be32(fetch + (95 - s) * 8), w1 = Be32(fetch + (95 - s) * 8 + 4);
    if (valid && g_stream_words[s][0] == w0 && g_stream_words[s][1] == w1) continue;
    g_stream_words[s][0] = w0, g_stream_words[s][1] = w1;
    const uint32_t start = w0 & ~3u, bytes = ((w1 >> 2) & 0xFFFFFF) * 4;
    uint32_t lowest = start;
    if ((w0 & 3) == 3 && start && bytes) {
      uint32_t& known = g_buffer_starts.try_emplace(start + bytes, start).first->second;
      known = lowest = std::min(known, start);
    }
    const uint32_t view = (w0 & 3) == 3 && bytes ? PhysicalView(lowest, 4) : 0;
    if (!view) {
      g_streams[s].buffer = 0;
      continue;
    }
    const uint32_t all = start + bytes - lowest;
    const uint32_t be[2] = {_byteswap_ulong(view | 3), _byteswap_ulong((w1 & ~0x3FFFFFCu) | all)};
    std::memcpy(g_replay_streams[s] + 0x18, be, 8);
    g_streams[s].buffer = kReplayStream | s;
    g_streams[s].offset = start - lowest;
  }
}
// (XMD_NATIVE_HOOKED_STREAMS=1: as before - the streams SetStreamSource was called with)
const bool g_fetch_streams = std::getenv("XMD_NATIVE_HOOKED_STREAMS") == nullptr;
uint8_t g_fetch_seen[128];

// A draw of a played command buffer (a "site": the n-th draw packet of a buffer with these contents) and what
// it came to the last time: its shaders, its vertex layout, its vertex buffers and indices. A level's static
// draws are the same every frame - 4200 of the first level's 4700 - and finding all of that again from the
// game's objects (each in memory nothing else has touched since the last frame) was most of a draw's cost.
// What is kept is used again while what it was made from is unchanged - the shader objects, the vertex fetch
// constants of the streams the layout reads, the index range - and for `ttl` frames at most; then the draw goes
// the long way once more (which is also when its buffers' contents are compared again).
// XMD_NATIVE_NO_SITES=1: every draw the long way.
struct ReplaySite {
  bool resolved = false;
  bool buffers = false;        // (views / index_view are good too)
  uint32_t ttl = 24;
  uint64_t stamp = 0;          // (the frame it was resolved in)
  uint32_t epoch = 0;          // (g_site_epoch then)
  uint32_t vs_object = 0, ps_object = 0, vs_code = 0;
  Shader* vs = nullptr;
  Shader* ps = nullptr;
  const InputLayout* il = nullptr;
  uint32_t used = 0;           // (the streams the layout reads: bits)
  uint32_t words[16][2] = {};  // (their vertex fetch constants)
  plume::RenderVertexBufferView views[16] = {};
  uint32_t max_slot = 0;
  uint32_t index_address = 0, index_size = 0;
  plume::RenderIndexBufferView index_view;
  // (RunCommands' own: the draw packet's index address as a readable view)
  uint32_t raw_index = 0, raw_view = 0, raw_size = 0;
  uint64_t raw_stamp = 0;
};
ReplaySite* g_site = nullptr;         // (the site of the draw RunCommands is making; null otherwise)
struct SiteStats {
  uint64_t draws = 0, fresh = 0, buffers = 0, scans = 0;
};
SiteStats g_site_stats;
bool g_replay_streams_stale = false;  // (g_streams isn't made from the played buffer's fetch constants yet)

void Draw(Renderer* r, uint32_t primitive, int32_t base_vertex, uint32_t start, uint32_t count,
          bool indexed, const PendingUP* up = nullptr) {
  ScopeTimer timer(r->perf_draw_ms);
  ApplyPendingConstants();
  ++r->frame_stats.draws;
  if (r->hidden_frame) return;  // (a frame not shown: SetHalfFrames)
  if (g_stopped) return;
  if (g_stop_draws && r->frame_stats.draws > g_stop_draws) return;
  if (g_stop_at_resolve && g_resolved_this_frame) return;
  const Targets targets = CurrentTargets(r);
  if (!targets.color) {
    ++r->frame_stats.skipped_target;
    return;
  }
  if (!g_shadows && targets.color->width == 1120 && targets.color->height == 1120 &&
      targets.color->format == RenderFormat::R32_FLOAT) {
    ++r->frame_stats.skipped_target;  // (native_shadows off: the shadow map keeps its clear)
    return;
  }
  const uint32_t vs_object = Be32(Device() + guest::kDeviceCurrentVertexShader);
  // (no pixel shader set: a depth-only draw - the game's shadow and depth passes)
  // (a played command buffer names a pixel shader for its depth pass too: RB_MODECONTROL 5 is depth only)
  const bool depth_only = g_replay_device && (Reg(0x2208) & 7) == 5;
  const uint32_t ps_object = depth_only ? 0 : Be32(Device() + guest::kDeviceCurrentPixelShader);
  ReplaySite* site = up || r->frames == g_dump_frame ? nullptr : g_site;
  const bool site_fresh = site && site->resolved && site->epoch == g_site_epoch && r->frames - site->stamp < site->ttl &&
                          site->vs_object == vs_object && site->ps_object == ps_object &&
                          site->vs_code == g_replay_vertex_code;
  if (site) ++g_site_stats.draws, g_site_stats.fresh += site_fresh;
  Shader* vs = site_fresh ? site->vs : FindShader(vs_object, false);
  Shader* ps = site_fresh ? site->ps : ps_object ? FindShader(ps_object, true) : nullptr;
  if (g_fetch_streams && !up && !g_replay_device) {
    const uint8_t* fetch = Device() + guest::RegisterOffset(0x4800);
    if (std::memcmp(g_fetch_seen, fetch + 80 * 8, sizeof(g_fetch_seen))) {
      std::memcpy(g_fetch_seen, fetch + 80 * 8, sizeof(g_fetch_seen));
      StreamsFromFetch(fetch);
    }
  }
  if (g_stop_ps) {
    if (g_stop_ps_hit && primitive == 8) {
      // (rectangle lists - the full-screen passes - go on: the cut scene still gets its post-processing)
    } else if (g_stop_ps_hit) {
      if (!g_stop_ps_left) return;
      --g_stop_ps_left;
    } else if (ps && uint32_t(ps->hash >> 32) == g_stop_ps) {
      g_stop_ps_hit = true;
      g_stop_ps_left = g_stop_ps_extra;
      if (!g_stop_ps_after) return;
    }
  }
  DrawClock clock;
  clock.Mark(0);
  if (!vs || (ps_object && !ps)) {
    ++r->frame_stats.skipped_shader;
    static std::unordered_map<uint64_t, int> logged;  // (each case once)
    const uint32_t objects[2] = {Be32(Device() + guest::kDeviceCurrentVertexShader),
                                 Be32(Device() + guest::kDeviceCurrentPixelShader)};
    if (logged.size() < 24 && logged.emplace(uint64_t(objects[0]) << 32 | objects[1], 0).second) {
      auto words = [](uint32_t object, uint32_t at) {
        if (object < 0x10000) return std::string("none");
        const uint8_t* o = Virtual(object);
        return fmt::format("{:08X}: {:08X}, container {:08X} {:X}+{:X}", object, Be32(o), Be32(o + at), Be32(o + at + 4),
                           Be32(o + at + 8));
      };
      REXLOG_INFO("native renderer: draw without a shader ({}{}): vertex {}; pixel {}", vs ? "" : "vertex ",
                  ps ? "" : "pixel", words(objects[0], guest::kVertexShaderContainer),
                  words(objects[1], guest::kPixelShaderContainer));
    }
    return;
  }

  if (static int logged = 0; indexed && !up && !g_replay_device && uint32_t(vs->hash >> 32) == 0x14A1ED41u && logged++ < 6) {
    const uint8_t* fetch = Device() + guest::RegisterOffset(0x4800) + 95 * 8;
    std::string at;
    for (uint32_t i = 0x2A60; i < 0x3400; i += 4) at += fmt::format(" {:08X}", Be32(Device() + i));
    REXLOG_INFO("native renderer: shape draw: stream 0 set = {:08X} +{}, the mirror's fetch constant {:08X} {:08X}, start {} count {}; "
                "device 2A60-3400:{}", g_streams[0].buffer, g_streams[0].offset, Be32(fetch), Be32(fetch + 4), start, count, at);
  }
  // Topology; quad lists become triangle lists.
  using T = plume::RenderPrimitiveTopology;
  T topology;
  bool quads = false, rects = false;
  switch (primitive) {
    case 1: topology = T::POINT_LIST; break;
    case 2: topology = T::LINE_LIST; break;
    case 3: topology = T::LINE_STRIP; break;
    case 4: topology = T::TRIANGLE_LIST; break;
    case 6: topology = T::TRIANGLE_STRIP; break;
    case 13: topology = T::TRIANGLE_LIST; quads = true; break;
    case 8:  // rectangle list: three corners each (the game's full-screen passes)
      if (indexed || count < 3) {
        ++r->frame_stats.skipped_prim;
        return;
      }
      topology = T::TRIANGLE_LIST;
      rects = true;
      break;
    default:
      ++r->frame_stats.skipped_prim;
      return;
  }

  // Input layout: every input the vertex shader declares, from the game's
  // vertex declaration (or a zero stream when the declaration lacks it).
  // Built once per (shader, declaration contents) - not per draw.
  const InputLayout& il = site_fresh ? *site->il : GetInputLayout(vs, r->frames == g_dump_frame);
  clock.Mark(1);
  const std::vector<Element>& decl = il.decl;
  const std::vector<plume::RenderInputElement>& layout = il.layout;
  const bool packed_normals = il.packed_normals;
  const int vs_variant = il.byte_normals ? 3 : il.packed_10_11_11 ? 4 : packed_normals ? 1 : 0;
  const bool uses_zero_stream = il.uses_zero_stream;
  g_draw_float_uint_inputs = il.float_uint_inputs;
  uint32_t layout_hash = il.hash;
  // (the game sets its streams without a stride: the bound shader has it)
  if (!up) {
    for (const Element& e : decl) {
      if (e.stream < 16 && e.stride) g_streams[e.stream].stride = e.stride;
    }
  }
  // The vertex buffer slots (0-15; strides as set now). Vulkan pipelines
  // have the strides of the slots they use built in.
  plume::RenderInputSlot slots[16];
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t stride = up && i == 0 ? up->stride : i < 15 && !up ? g_streams[i].stride : 0;
    slots[i] = plume::RenderInputSlot(i, stride);
  }
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    for (const auto& e : layout) layout_hash = layout_hash * 31 + slots[e.slotIndex].stride * 977;
  }

  // Alpha test: RB_COLORCONTROL bit 3 with a "greater" function (the only
  // kind the converted shaders implement: discard below the threshold).
  const uint32_t cc = Reg(RB_COLORCONTROL);
  const bool alpha_test = (cc & 8) && ((cc & 7) == 4 || (cc & 7) == 6);

  const plume::RenderPipeline* pso =
      Pipeline(r, vs, vs_variant, ps, alpha_test ? 2 : 0, layout, slots, 16,
               layout_hash, il.hash, topology, targets.color->format);
  if (!pso) {
    ++r->frame_stats.skipped_shader;
    return;
  }

  clock.Mark(2);
  // Vertex buffers for the streams the layout uses.
  plume::RenderVertexBufferView views[16] = {};
  uint32_t max_slot = 0;
  // (the site's buffers again: the streams its layout reads and its indices are the ones they were made from)
  bool site_buffers = site_fresh && site->buffers && !rects && !quads;
  if (site_buffers) {
    const uint8_t* fetch = Device() + guest::RegisterOffset(0x4800);
    for (uint32_t m = site->used; m && site_buffers; m &= m - 1) {
      const uint32_t stream = uint32_t(__builtin_ctz(m));
      site_buffers = Be32(fetch + (95 - stream) * 8) == site->words[stream][0] &&
                     Be32(fetch + (95 - stream) * 8 + 4) == site->words[stream][1];
    }
    if (indexed) site_buffers = site_buffers && site->index_address == g_replay_index_address && site->index_size == g_replay_index_size;
  }
  if (site_buffers) {
    ++g_site_stats.buffers;
    std::copy(site->views, site->views + 16, views);
  } else if (g_replay_device && !up && g_replay_streams_stale) {
    g_replay_streams_stale = false;
    StreamsFromFetch(Device() + guest::RegisterOffset(0x4800));
  }
  if (up) {
    // The game's vertices, converted into the ring.
    const uint64_t bytes = uint64_t(up->count) * up->stride;
    auto [cpu, ref] = Allocate(r, bytes, 16);
    if (!cpu || !bytes) return;
    ConvertVertices(cpu, ObjectData(up->data), bytes, up->stride, 0, decl, 0);
    views[0] = plume::RenderVertexBufferView(ref, uint32_t(bytes));
  }
  for (const auto& e : layout) {
    max_slot = std::max(max_slot, e.slotIndex);
    if (e.slotIndex == 15 || up || site_buffers) continue;
    const Stream& s = g_streams[e.slotIndex];
    if (!s.buffer || views[e.slotIndex].buffer.ref) continue;
    // The game's own draws without indices - its sprites, trails and beams (quad lists and strips of 4 to a few
    // hundred vertices, seven streams each): every stream is a piece of the game's per-frame vertex memory, as
    // long as the draw and no longer. Exactly those vertices are converted, now, into the frame's ring - not
    // through the buffer cache, which knows a buffer by its end and its lowest start ever seen (in this memory:
    // an unrelated earlier piece, up to 235 KB away - a fight frame had 322 such "buffers" of 16-235 KB, 37 MB,
    // for a few hundred bytes drawn from each) and compares a kept copy with the memory only every few frames:
    // stray particle and fire flicker (the user, 2026-11-19). XMD_NATIVE_NO_DIRECT_STREAMS=1: as before.
    static const bool direct_off = EnvFlag("XMD_NATIVE_NO_DIRECT_STREAMS");
    if (!direct_off && !indexed && !g_replay_device && !rects && s.stride >= 4) {
      const uint8_t* vbo = BufferObject(s.buffer);
      const uint32_t address = Be32(vbo + 0x18) & ~3u, total = ((Be32(vbo + 0x1C) >> 2) & 0xFFFFFF) * 4;
      // (from the stream's first vertex, so the draw's start vertex stays what it is)
      const uint64_t bytes = (uint64_t(start) + count) * s.stride;
      if (bytes && bytes <= (256u << 10) && s.offset + bytes <= total &&
          GuestReadable(address + s.offset, uint32_t(bytes))) {
        auto [cpu, ref] = Allocate(r, bytes, 16);
        if (cpu) {
          ConvertVertices(cpu, ObjectData(address) + s.offset, bytes, s.stride, 0, decl, e.slotIndex);
          views[e.slotIndex] = plume::RenderVertexBufferView(ref, uint32_t(bytes));
          continue;
        }
      }
    }
    uint32_t size = 0;
    const RenderBufferReference vb = VertexBuffer(r, s.buffer, e.slotIndex, s, decl, &size);
    if (!vb.ref || s.offset >= size) continue;
    views[e.slotIndex] = plume::RenderVertexBufferView(
        RenderBufferReference(vb.ref, vb.offset + s.offset), size - s.offset);
  }
  if (uses_zero_stream) {
    views[15] = plume::RenderVertexBufferView(r->zero_buffer->at(0), 256);
  }
  // Rectangle lists: each rectangle's three vertices become two triangles,
  // the fourth corner made here from the other three (as the GPU does).
  if (rects) {
    const uint32_t n = count / 3;
    bool done[16] = {};
    for (const auto& e : layout) {
      const uint32_t slot = e.slotIndex;
      if (slot >= 15 || done[slot]) continue;
      done[slot] = true;
      const uint8_t* src = nullptr;
      uint32_t stride = 0;
      if (up) {
        if (slot != 0) continue;
        src = ObjectData(up->data);
        stride = up->stride;
      } else {
        const Stream& s = g_streams[slot];
        if (!s.buffer || !s.stride) continue;
        const uint8_t* vb = BufferObject(s.buffer);
        const uint32_t address = Be32(vb + 0x18) & ~3u, bytes = Be32(vb + 0x1C) & ~3u;
        stride = s.stride;
        if (uint64_t(s.offset) + uint64_t(start + n * 3) * stride > bytes) continue;
        src = ObjectData(address) + s.offset + start * stride;  // (the object holds the address in a virtual view)
      }
      if (!src || stride < 4 || (stride & 3)) continue;
      auto [cpu, ref] = Allocate(r, uint64_t(n) * 6 * stride, 16);
      if (!cpu) return;
      BuildRectangles(cpu, src, n, stride, decl, slot);
      views[slot] = plume::RenderVertexBufferView(ref, n * 6 * stride);
    }
    count = n * 6;
    start = 0;
  }

  clock.Mark(3);
  // Indices (16-bit big-endian), converted into the ring.
  uint32_t draw_count = count;
  bool draw_indexed = indexed;
  int32_t draw_base = base_vertex;
  plume::RenderIndexBufferView ibv;
  if (site_buffers && indexed) {
    ibv = site->index_view;
  } else if (indexed) {
    const uint32_t ib = g_replay_device ? 0 : Be32(Device() + guest::kDeviceIndexBuffer);
    if (!ib && !g_replay_device) {
      ++r->frame_stats.skipped_prim;
      return;
    }
    // (a played command buffer's draw: the indices it names)
    const uint8_t* obj = ib ? Virtual(ib) : g_replay_streams[0];
    const uint32_t address = ib ? Be32(obj + 0x18) : g_replay_index_address, size = ib ? Be32(obj + 0x1C) : g_replay_index_size;
    if ((start + count) * 2 > size || !address || !GuestReadable(address, size)) {
      static int logged = 0;
      if (logged++ < 8)
        REXLOG_WARN("native renderer: index buffer {:08X} ({:08X}: data {:08X}, {} bytes) doesn't hold indices {}+{} - not "
                    "drawn", ib, Be32(obj), address, size, start, count);
      ++r->frame_stats.skipped_prim;
      return;
    }
    const uint8_t* src = ObjectData(address) + start * 2;
    const uint32_t out_count = quads ? count / 4 * 6 : count;
    // (a played command buffer names each draw's own indices: start 0, and cached by that range)
    // (not the game's own index lists - indexed BeginVertices: other indices at the same address the next time)
    const RenderBufferReference cached = quads || up ? RenderBufferReference{} : IndexBuffer(r, address, size);
    if (cached.ref) {
      ibv = plume::RenderIndexBufferView(RenderBufferReference(cached.ref, cached.offset + start * 2ull),
                                         count * 2, RenderFormat::R16_UINT);
      draw_count = count;
    } else {
      auto [cpu, gpu] = Allocate(r, out_count * 2ull, 4);
      if (!cpu) return;
      auto* dst = reinterpret_cast<uint16_t*>(cpu);
      if (quads) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
          const uint16_t a = Be16(src + 2 * q), b = Be16(src + 2 * q + 2),
                         c = Be16(src + 2 * q + 4), d = Be16(src + 2 * q + 6);
          *dst++ = a; *dst++ = b; *dst++ = c;
          *dst++ = a; *dst++ = c; *dst++ = d;
        }
      } else {
        Swap16(cpu, src, count * 2ull);
      }
      ibv = plume::RenderIndexBufferView(gpu, out_count * 2, RenderFormat::R16_UINT);
      draw_count = out_count;
    }
  } else if (quads) {
    // Non-indexed quad list: two triangles per quad, from vertex `start`.
    const uint32_t out_count = count / 4 * 6;
    auto [cpu, gpu] = Allocate(r, out_count * 2ull, 4);
    if (!cpu || !out_count) return;
    auto* dst = reinterpret_cast<uint16_t*>(cpu);
    for (uint32_t q = 0; q + 3 < count; q += 4) {
      *dst++ = uint16_t(q); *dst++ = uint16_t(q + 1); *dst++ = uint16_t(q + 2);
      *dst++ = uint16_t(q); *dst++ = uint16_t(q + 2); *dst++ = uint16_t(q + 3);
    }
    ibv = plume::RenderIndexBufferView(gpu, out_count * 2, RenderFormat::R16_UINT);
    draw_count = out_count;
    draw_indexed = true;
    draw_base = int32_t(start);
  }

  if (site && !site_buffers) {
    // (kept for the next frames: the shaders and the layout; the buffers when none of them is the game's
    // short-lived kind, converted into the frame's ring)
    const plume::RenderBuffer* ring = r->rings[r->back_index].buffer.get();
    bool keep = !rects && !quads && vs->indexed.empty() && (!indexed || (ibv.buffer.ref && ibv.buffer.ref != ring));
    uint32_t used = 0;
    for (const auto& e : layout) {
      if (e.slotIndex >= 15) continue;
      used |= 1u << e.slotIndex;
      keep = keep && views[e.slotIndex].buffer.ref && views[e.slotIndex].buffer.ref != ring;
    }
    if (!site_fresh) {
      site->resolved = true;
      site->stamp = r->frames;
      site->epoch = g_site_epoch;
      site->vs_object = vs_object, site->ps_object = ps_object, site->vs_code = g_replay_vertex_code;
      site->vs = vs, site->ps = ps, site->il = &il;
    }
    site->buffers = keep;
    if (keep) {
      const uint8_t* fetch = Device() + guest::RegisterOffset(0x4800);
      site->used = used;
      for (uint32_t m = used; m; m &= m - 1) {
        const uint32_t stream = uint32_t(__builtin_ctz(m));
        site->words[stream][0] = Be32(fetch + (95 - stream) * 8), site->words[stream][1] = Be32(fetch + (95 - stream) * 8 + 4);
      }
      std::copy(views, views + 16, site->views);
      site->index_address = g_replay_index_address, site->index_size = g_replay_index_size;
      site->index_view = ibv;
    }
  }
  clock.Mark(4);
  // Viewport from the Xenos viewport transform.
  const float xs = RegFloat(PA_CL_VPORT_XSCALE), xo = RegFloat(PA_CL_VPORT_XOFFSET);
  const float ys = RegFloat(PA_CL_VPORT_YSCALE), yo = RegFloat(PA_CL_VPORT_YOFFSET);
  const float zs = RegFloat(PA_CL_VPORT_ZSCALE), zo = RegFloat(PA_CL_VPORT_ZOFFSET);
  plume::RenderViewport vp(xo - std::fabs(xs), yo - std::fabs(ys), 2 * std::fabs(xs),
                           2 * std::fabs(ys), zo, zo + zs);
  const float tw = float(targets.color->width), th = float(targets.color->height);
  // The shaders' position goes through g_NdcScale / g_HalfPixelOffset: with
  // the viewport transform off (PA_CL_VTE_CNTL) positions are in pixels of
  // the target; with it on, the Xenos viewport's flips are kept (viewports
  // here don't flip). D3D9-style pixel centres (PA_SU_VTX_CNTL bit 0
  // clear) put pixel centres on integer coordinates: half a pixel is added.
  const uint32_t vte = Reg(PA_CL_VTE_CNTL);
  float ndc[4] = {xs < 0 ? -1.0f : 1.0f, ys > 0 ? -1.0f : 1.0f, 0.0f, 0.0f};
  if (!(vte & 1)) {  // x scale/offset off
    vp.x = 0;
    vp.width = tw;
    ndc[0] = 2.0f / tw;
    ndc[2] = -1.0f;
  }
  if (!(vte & 4)) {  // y scale/offset off
    vp.y = 0;
    vp.height = th;
    ndc[1] = -2.0f / th;
    ndc[3] = 1.0f;
  }
  if (!(vte & 0x10)) {  // z scale/offset off
    vp.minDepth = 0;
    vp.maxDepth = 1;
  }
  if (vp.width <= 0 || vp.height <= 0) vp = plume::RenderViewport(0, 0, tw, th, 0, 1);
  if (!(Reg(PA_SU_VTX_CNTL) & 1)) {
    ndc[2] += 1.0f / vp.width;
    ndc[3] -= 1.0f / vp.height;
  }
  vp.minDepth = std::clamp(vp.minDepth, 0.0f, 1.0f);
  vp.maxDepth = std::clamp(vp.maxDepth, 0.0f, 1.0f);
  // (ndc above is in guest pixels; the target has `ts` times as many - and
  // `tsx` across on a wide screen.)
  const uint32_t ts = targets.color ? targets.color->scale : targets.depth ? targets.depth->scale : g_scale;
  const float tsx = targets.color && targets.color->host_w ? float(targets.color->host_w) / tw : float(ts);
  const float tsy = targets.color && targets.color->host_h ? float(targets.color->host_h) / th : float(ts);
  // (the scene at native_render_scale: fewer host pixels per guest pixel)
  const float base = float(ts) * (targets.color && SceneTarget(tw, th) ? g_res : 1.0f);
  const bool reshaped = tsx > base * 1.001f || tsy > base * 1.001f;  // (wide or tall)
  // Wide screen: the HUD's 2D sprites (vertices in the game's 1280 x 720
  // pixels, drawn without depth) go in the 16:9 middle, unstretched; a sprite
  // covering the whole screen (a fade) and everything else fill the width.
  bool middle = false;
  const bool depth_test = (Reg(RB_DEPTHCONTROL) >> 1) & 1;
  // Free camera F7 (free_cam.h): in a match, the HUD isn't drawn - its 2D sprites (as below: flat in the target's
  // pixels, no depth, not a full-screen fade or pass) and the players' bars on the floor (in the world, no depth,
  // alpha blended: the frame's last draws; the scene's one draw without depth, the ring's, blends otherwise).
  if (xmd::UiHidden() && g_match_scene.load() && !g_frame_2d && (vte & 1) && !depth_test) {
    bool full = false;
    if (Reg(RB_BLENDCONTROL0) == 0x07060706 ||
        (FlatPixels(decl, up, indexed, start, count, base_vertex, tw, th, &full) && !full)) {
      ++r->frame_stats.skipped_target;
      return;
    }
  }
  if (reshaped && depth_test) ++r->frame_stats.wide_3d;
  if (reshaped && g_frame_2d) {
    // (screen-space passes keep the target's width; so does the scene - a 2D screen has no depth-tested draws, the
    // first frame after one does)
    middle = (vte & 1) != 0 && !depth_test;
  } else if (reshaped && (vte & 1) && !depth_test) {
    bool full = false;
    middle = FlatPixels(decl, up, indexed, start, count, base_vertex, tw, th, &full) && !full;
  }
  const float pad = middle ? (float(targets.color->host_w) - tw * base) * 0.5f : 0.0f;
  const float pad_y = middle && targets.color->host_h ? (float(targets.color->host_h) - th * base) * 0.5f : 0.0f;
  const float sx = middle ? base : tsx;
  const float sy = middle ? base : tsy;
  vp.x = vp.x * sx + pad;
  vp.y = vp.y * sy + pad_y;
  vp.width *= sx;
  vp.height *= sy;
  // native_jitter: the match scene's 3D draws (projected, depth tested, into the scene target) moved by this frame's
  // sub-pixel offset - in NDC, through g_HalfPixelOffset (the shaders add it times w: the same in every depth).
  if ((g_jitter[0] != 0.0f || g_jitter[1] != 0.0f) && g_match_scene.load() && !g_frame_2d && !middle && (vte & 1) && depth_test &&
      SceneTarget(uint32_t(tw), uint32_t(th))) {
    ndc[2] += 2.0f * g_jitter[0] / vp.width;
    ndc[3] -= 2.0f * g_jitter[1] / vp.height;
  }
#if XMD_FSR2
  // FSR 2's camera: the scene's view x projection as the game's shaders have it - vertex constants c54-c57 (its
  // columns; c50-c53 the view's), taken from the frame's first 3D scene draw. (The scene view global, 0x82E3CC00,
  // is the game logic's: a frame ahead of what the render thread draws.)
  if (g_fsr2 && g_match_scene.load() && !middle && (vte & 1) && depth_test && r->fsr2_vp_frame[0] != r->frames &&
      SceneTarget(uint32_t(tw), uint32_t(th))) {
    const uint8_t* c = Device() + guest::RegisterOffset(0x4000);
    float vp[16];
    for (int col = 0; col < 4; ++col)
      for (int row = 0; row < 4; ++row) vp[row * 4 + col] = BeFloat(c + 16 * (54 + col) + 4 * row);
    // (a perspective camera: w = the view's depth - a unit forward, the view's last column (0, 0, 0, 1))
    const float fwd = std::sqrt(vp[3] * vp[3] + vp[7] * vp[7] + vp[11] * vp[11]);
    if (std::fabs(fwd - 1.0f) < 0.01f && BeFloat(c + 16 * 53 + 12) == 1.0f && vp[15] == BeFloat(c + 16 * 52 + 12)) {
      std::memcpy(r->fsr2_vp[1], r->fsr2_vp[0], sizeof(vp));
      r->fsr2_vp_frame[1] = r->fsr2_vp_frame[0];
      std::memcpy(r->fsr2_vp[0], vp, sizeof(vp));
      r->fsr2_vp_frame[0] = r->frames;
    }
  }
#endif
  // Window scissor: D3D keeps it in the mirrored 0x2000 group (0x2011 top-left,
  // 0x2012 bottom-right; x in bits 0-14, y in bits 16-30), already clamped to
  // the scissor rect when the game enables one (device +0x2F00).
  const uint32_t sc_tl = Reg(PA_SC_WINDOW_SCISSOR_TL), sc_br = Reg(PA_SC_WINDOW_SCISSOR_BR);
  plume::RenderRect scissor(
      int32_t(std::lround(float(std::min<uint32_t>(sc_tl & 0x7FFF, targets.color->width)) * sx + pad)),
      int32_t(std::lround(float(std::min<uint32_t>((sc_tl >> 16) & 0x7FFF, targets.color->height)) * sy + pad_y)),
      int32_t(std::lround(float(std::min<uint32_t>(sc_br & 0x7FFF, targets.color->width)) * sx + pad)),
      int32_t(std::lround(float(std::min<uint32_t>((sc_br >> 16) & 0x7FFF, targets.color->height)) * sy + pad_y)));

  if (r->frames >= g_drawlog_frame && r->frames < g_drawlog_end && g_drawlog) {
    char tex[256] = "";
    for (const Shader* sh : {vs, ps}) {
      if (!sh) continue;
      for (const auto& [slot, dimension] : sh->textures) {
        const uint8_t* fc = Device() + guest::RegisterOffset(0x4800) + slot * 24;
        const uint32_t base = Be32(fc + 4) & 0xFFFFF000u;
        auto it = r->resolved.find(base);
        char one[64];
        if (it != r->resolved.end()) {
          std::snprintf(one, sizeof(one), " %s%u:R%ux%u@%X/f%u/%ux%u", sh == vs ? "v" : "p", slot,
                        it->second.width, it->second.height, base, Be32(fc + 4) & 0x3F,
                        (Be32(fc + 8) & 0x1FFF) + 1, ((Be32(fc + 8) >> 13) & 0x1FFF) + 1);
        } else {
          std::snprintf(one, sizeof(one), " %s%u:t", sh == vs ? "v" : "p", slot);
        }
        std::strncat(tex, one, sizeof(tex) - std::strlen(tex) - 1);
      }
    }
    // The positions' extent (float positions only; the first 64 vertices).
    char pos[96] = "?";
    for (const Element& el : decl) {
      if (el.usage != 0 || el.index != 0) continue;
      const RenderFormat pf = VertexFormat(el.type, false);
      const int comps = pf == RenderFormat::R32G32_FLOAT ? 2 : pf == RenderFormat::R32G32B32_FLOAT ? 3
                        : pf == RenderFormat::R32G32B32A32_FLOAT ? 4 : 0;
      if (!comps) {
        std::snprintf(pos, sizeof(pos), "fmt%d", int(pf));
        break;
      }
      const uint8_t* data = nullptr;
      uint32_t stride = 0;
      if (up) {
        data = ObjectData(up->data), stride = up->stride;
      } else if (el.stream < 15 && g_streams[el.stream].buffer) {
        const Stream& st = g_streams[el.stream];
        data = ObjectData(Be32(BufferObject(st.buffer) + 0x18) & ~3u) + st.offset, stride = st.stride;
      }
      if (!data || !stride) break;
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      const uint32_t n = std::min<uint32_t>(indexed ? count : count, 64);
      const uint8_t* ib = nullptr;
      if (indexed && !up && g_replay_device) {
        ib = ObjectData(g_replay_index_address);
      } else if (indexed && !up) {
        const uint8_t* obj = Virtual(Be32(Device() + guest::kDeviceIndexBuffer));
        ib = ObjectData(Be32(obj + 0x18)) + start * 2;
      }
      for (uint32_t k = 0; k < n; ++k) {
        const int64_t v = ib ? int64_t(Be16(ib + 2 * k)) + base_vertex : int64_t(up ? k : start + k);
        const uint8_t* q = data + v * stride + el.offset;
        for (int c = 0; c < std::min(comps, 3); ++c) {
          const float f = BeFloat(q + 4 * c);
          mn[c] = std::min(mn[c], f), mx[c] = std::max(mx[c], f);
        }
      }
      std::snprintf(pos, sizeof(pos), "x %.1f..%.1f y %.1f..%.1f z %.2f..%.2f", mn[0], mx[0], mn[1], mx[1],
                    comps > 2 ? mn[2] : 0.0f, comps > 2 ? mx[2] : 0.0f);
      break;
    }
    std::fprintf(g_drawlog, "f%llu %4u tgt %ux%u vte %02X z %u blend %08X prim %u n %u%s vs %08X ps %08X vp %.0f,%.0f %.0fx%.0f |%s | pos %s\n",
                 (unsigned long long)r->frames, r->frame_stats.drawn, targets.color->width, targets.color->height, Reg(PA_CL_VTE_CNTL) & 0x3F,
                 (Reg(RB_DEPTHCONTROL) >> 1) & 1, Reg(RB_BLENDCONTROL0), primitive, count, up ? " UP" : "",
                 uint32_t(vs->hash), uint32_t(ps ? ps->hash : 0), vp.x, vp.y, vp.width, vp.height, tex, pos);
  }
  if (r->frames == g_dump_frame && r->frame_stats.drawn >= g_dump_first &&
      r->frame_stats.drawn < g_dump_first + g_dump_count) {
    if (!g_dump) {
      // (XMD_NATIVE_DUMP_FILE: elsewhere - phones whose game folder adb can't read)
      const char* file = std::getenv("XMD_NATIVE_DUMP_FILE");
      std::string asked;
      {
        std::lock_guard dump_lock(g_dump_request_mutex);
        asked = g_dump_request_file;
      }
      g_dump = std::fopen(!asked.empty() ? asked.c_str() : file ? file : "native_draws.txt", "w");
    }
    FILE* f = g_dump;
    std::fprintf(f, "=== draw %u: prim %u base %d start %u count %u indexed %d | vs %016llX v%d ps %016llX v%d\n",
                 r->frame_stats.drawn, primitive, base_vertex, start, count, indexed,
                 (unsigned long long)vs->hash, packed_normals ? 1 : 0, (unsigned long long)(ps ? ps->hash : 0),
                 alpha_test ? 2 : 0);
    std::fprintf(f, "viewport %.1f %.1f %.1f %.1f z %.3f..%.3f  depthctl %08X cull %u\n", vp.x,
                 vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth, Reg(RB_DEPTHCONTROL),
                 Reg(PA_SU_SC_MODE_CNTL) & 7);
    std::fprintf(f, "target %ux%u fmt %d | vte %08X blend %08X colorctl %08X mask %X | surface %08X color %08X depth %08X\n",
                 targets.color->width, targets.color->height, int(targets.color->format),
                 Reg(PA_CL_VTE_CNTL), Reg(RB_BLENDCONTROL0), Reg(RB_COLORCONTROL),
                 Reg(RB_COLOR_MASK) & 0xF, Reg(RB_SURFACE_INFO), Reg(RB_COLOR_INFO),
                 Reg(RB_DEPTH_INFO));
    {
      const uint8_t* b = Device() + guest::RegisterOffset(0x4900);
      std::fprintf(f, "bools %08X %08X %08X %08X | %08X %08X %08X %08X\n", Be32(b), Be32(b + 4),
                   Be32(b + 8), Be32(b + 12), Be32(b + 16), Be32(b + 20), Be32(b + 24), Be32(b + 28));
      std::fprintf(f, "regs 2000-2012:");
      for (uint32_t reg = 0x2000; reg <= 0x2012; ++reg) std::fprintf(f, " %08X", Reg(reg));
      std::fprintf(f, "\n");
    }
    for (const Shader* sh : {vs, ps}) {
      if (!sh) continue;
      for (const auto& [slot, dimension] : sh->textures) {
        const uint8_t* fc = Device() + guest::RegisterOffset(0x4800) + slot * 24;
        std::fprintf(f, "  %s tex slot %u dim %u: %08X %08X %08X %08X %08X %08X\n",
                     sh == vs ? "vs" : "ps", slot, dimension, Be32(fc), Be32(fc + 4), Be32(fc + 8),
                     Be32(fc + 12), Be32(fc + 16), Be32(fc + 20));
      }
    }
    if (up) {
      const uint8_t* v = ObjectData(up->data);
      for (uint32_t i = 0; i < up->count && i < 4; ++i) {
        std::fprintf(f, "  up v%u:", i);
        for (uint32_t k = 0; k < up->stride / 4; ++k) std::fprintf(f, " %g", BeFloat(v + i * up->stride + 4 * k));
        std::fprintf(f, "\n");
      }
    }
    for (const Element& el : decl)
      std::fprintf(f, "  decl s%u +%u type %08X usage %u.%u\n", el.stream, el.offset, el.type, el.usage,
                   el.index);
    for (const auto& e : layout)
      std::fprintf(f, "  input %s%u fmt %d slot %u +%u\n", e.semanticName, e.semanticIndex,
                   int(e.format), e.slotIndex, e.alignedByteOffset);
    for (uint32_t sl = 0; sl < 16; ++sl) {
      const Stream& st = g_streams[sl];
      if (!st.buffer) continue;
      const uint8_t* obj = BufferObject(st.buffer);
      const uint32_t w0 = Be32(obj + 0x18), w1 = Be32(obj + 0x1C);
      std::fprintf(f, "  stream %u: obj %08X fetch %08X %08X stride %u offset %u\n", sl, st.buffer, w0,
                   w1, st.stride, st.offset);
      const uint8_t* vtx = ObjectData(w0 & ~3u);
      for (uint32_t v = 0; v < 3 && st.stride; ++v) {
        std::fprintf(f, "    v%u:", v);
        for (uint32_t k = 0; k < st.stride / 4 && k < 12; ++k) {
          const uint8_t* q = vtx + v * st.stride + 4 * k;
          std::fprintf(f, " %08X(%g)", Be32(q), BeFloat(q));
        }
        std::fprintf(f, "\n");
      }
    }
    if (indexed && g_replay_device) {
      const uint8_t* ix = ObjectData(g_replay_index_address);
      std::fprintf(f, "  ib (command buffer) %08X size %u first:", g_replay_index_address, g_replay_index_size);
      for (uint32_t k = 0; k < 12 && k < count; ++k) std::fprintf(f, " %u", Be16(ix + 2 * k));
      std::fprintf(f, "\n");
    } else if (indexed) {
      const uint8_t* obj = Virtual(Be32(Device() + guest::kDeviceIndexBuffer));
      const uint8_t* ix = ObjectData(Be32(obj + 0x18)) + start * 2;
      std::fprintf(f, "  ib %08X size %u first:", Be32(obj + 0x18), Be32(obj + 0x1C));
      for (uint32_t k = 0; k < 12 && k < count; ++k) std::fprintf(f, " %u", Be16(ix + 2 * k));
      std::fprintf(f, "\n");
    }
    for (const auto& fw : g_declaration_fetches)
      std::fprintf(f, "  vfetch usage %u.%u at +%u: %08X %08X %08X\n", (fw[0] >> 16) & 0xF, fw[0] >> 20, fw[0] & 0xFFFF, fw[1],
                   fw[2], fw[3]);
    // The vertices the draw's first indices name, element by element (raw words): what the shader is really fed.
    if (!up) {
      uint32_t used[3] = {start, start + 1, start + 2};
      if (indexed) {
        const uint8_t* ix = nullptr;
        if (g_replay_device) {
          ix = ObjectData(g_replay_index_address);
        } else if (const uint32_t ibo = Be32(Device() + guest::kDeviceIndexBuffer)) {
          ix = ObjectData(Be32(Virtual(ibo) + 0x18)) + start * 2;
        }
        for (int k = 0; k < 3 && ix; ++k) used[k] = uint32_t(int32_t(Be16(ix + 2 * k)) + base_vertex);
      }
      for (const Element& el : decl) {
        if (el.stream >= 15 || !g_streams[el.stream].buffer || !el.stride) continue;
        const Stream& st = g_streams[el.stream];
        const uint8_t* obj = BufferObject(st.buffer);
        const uint32_t address = Be32(obj + 0x18) & ~3u, bytes = ((Be32(obj + 0x1C) >> 2) & 0xFFFFFF) * 4;
        std::fprintf(f, "  used usage %u.%u s%u+%u stride %u (stream offset %u):", el.usage, el.index, el.stream, el.offset,
                     el.stride, st.offset);
        for (int k = 0; k < 3; ++k) {
          const uint64_t at = uint64_t(st.offset) + uint64_t(used[k]) * el.stride + el.offset;
          if (at + 8 > bytes || !GuestReadable(address + uint32_t(at), 8)) {
            std::fprintf(f, " v%u: out of the buffer (%u bytes)", used[k], bytes);
            continue;
          }
          const uint8_t* q = ObjectData(address) + at;
          std::fprintf(f, " v%u: %08X %08X", used[k], Be32(q), Be32(q + 4));
        }
        std::fprintf(f, "\n");
      }
    }    // The same lines the emulated GPU's dump has (XMD_GPU_DUMP_TRIGGER; tools/frame_diff.py compares the two).
    {
      auto words = [&](const char* name, uint32_t first, uint32_t n) {
        std::fprintf(f, "%s:", name);
        for (uint32_t k = 0; k < n; ++k) {
          const uint32_t offset = guest::RegisterOffset(first + k);
          std::fprintf(f, " %08X", offset || first + k == 0x4800 ? Be32(Device() + offset) : 0u);
        }
        std::fprintf(f, "\n");
      };
      words("regs 2000", 0x2000, 0x13);
      words("regs 2100", 0x2100, 0x16);
      words("regs 2180", 0x2180, 0x5);
      words("regs 2200", 0x2200, 0xD);
      words("regs 2280", 0x2280, 0x15);
      words("regs 2300", 0x2300, 0x26);
      words("regs 2380", 0x2380, 0x4);
      words("bools", 0x4900, 0x8);
      words("fetch", 0x4800, 0xC0);
      std::fprintf(f, "const:");
      for (uint32_t k = 0; k < 512; ++k) {
        uint32_t v[4];
        std::memcpy(v, Device() + 0x780 + 16 * k, 16);
        for (int c = 0; c < 4; ++c) v[c] = _byteswap_ulong(v[c]);
        if (g_constants_loaded[k]) std::memcpy(v, g_gpu_constants[k], 16);
        std::fprintf(f, " %08X %08X %08X %08X", v[0], v[1], v[2], v[3]);
      }
      std::fprintf(f, "\n");
    }
    // The constants the draw uses (GPU constant file): vertex gc0-gc255,
    // pixel pc0-pc255.
    // (as GpuConstants uploads them: the device mirror, loaded constants over it)
    for (uint32_t k = 0; k < 512; ++k) {
      float v[4];
      const uint8_t* m = Device() + 0x780 + 16 * k;
      for (int c = 0; c < 4; ++c) v[c] = BeFloat(m + 4 * c);
      if (g_constants_loaded[k]) std::memcpy(v, g_gpu_constants[k], 16);
      std::fprintf(f, "  %s%u = %g %g %g %g\n", k < 256 ? "gc" : "pc", k & 255, v[0], v[1], v[2], v[3]);
    }
    std::fflush(f);
  }

  auto* list = r->list;
  auto& ls = r->list_state;
  BindTargets(r, targets);  // (barriers first: they end a Vulkan render pass)
  if (!ls.bound) {
    list->setGraphicsPipelineLayout(r->layout.get());
    BindTables(r, list);
    ls.bound = true;
  }
  clock.Mark(5);
  RenderBufferReference draw_constants[4];
  {
    // (Superstar Threads' attire bake - the 2080x1024 target: its paint layers checked at every draw)
    textures::SetCheckEveryUse(targets.color->width == 2080 && targets.color->height == 1024);
    // (a debug pixel shader in place of the draw's own may read any of them)
    const bool any = !g_debug_ps.empty() || g_debug_solid;
    RenderBufferReference constants[4] = {GpuConstants(r, 0, vs->constants * 16),
                                           GpuConstants(r, 256, any ? kPixelConstantsBytes : ps ? ps->constants * 16 : 0),
                                           SharedConstants(r, alpha_test, vs, ps, ndc), {}};
    textures::SetCheckEveryUse(false);
    // The fourth buffer: a skinned draw's bone rows (IndexedRows); else the debug shader's colour.
    if (!vs->indexed.empty()) {
      // (a played buffer's draw that took its vertex buffers from its site has not made g_streams from the
      // buffer's fetch constants - and the bone rows' stream is no input, so the site never checked it: the
      // rows were read at the offset of whichever draw last did. A model's parts then moved with another
      // part's bones in one pass and their own in the next: black specks on faces, limbs flickering.)
      if (g_replay_device && !up && g_replay_streams_stale) {
        g_replay_streams_stale = false;
        const uint32_t before = g_streams[15].offset, before_buffer = g_streams[15].buffer;
        StreamsFromFetch(Device() + guest::RegisterOffset(0x4800));
        static uint64_t late = 0, moved = 0;
        ++late;
        if (g_streams[15].offset != before || g_streams[15].buffer != before_buffer) ++moved;
        if (late == 1 || late == 20000)
          REXLOG_INFO("native renderer: bone rows: {} skinned draws had their streams made late, {} of them would "
                      "have read another draw's bones", late, moved);
      }
      constants[3] = IndexedRows(r, vs, decl);
    }
    if (!constants[3].ref && !g_debug_solid && r->own_upload.ref) constants[3] = r->own_upload;
    auto [cpu, gpu] = constants[3].ref ? std::pair<uint8_t*, RenderBufferReference>{} : Allocate(r, 256);
    if (cpu) {
      if (!g_debug_solid) r->own_upload = gpu;
      const uint32_t n = r->frame_stats.drawn * 2654435761u;
      const float c[4] = {0.25f + ((n >> 8) & 0xFF) / 340.0f, 0.25f + ((n >> 16) & 0xFF) / 340.0f,
                          0.25f + ((n >> 24) & 0xFF) / 340.0f, 1.0f};
      std::memcpy(cpu, c, sizeof(c));
      constants[3] = gpu;
    }
    BindConstants(r, constants);
    std::copy(constants, constants + 4, draw_constants);
  }
  clock.Mark(6);
  if (backend::CompactTables() || SlotTables()) BindCompactSets(r, list);
  if (ls.pso != pso) {
    list->setPipeline(pso);
    ls.pso = pso;
  }
  {
    const uint32_t n = max_slot + 1;
    bool same = ls.view_count == n;
    for (uint32_t i = 0; same && i < n; ++i) {
      same = ls.views[i].buffer.ref == views[i].buffer.ref && ls.views[i].buffer.offset == views[i].buffer.offset &&
             ls.views[i].size == views[i].size && ls.slots[i].index == slots[i].index &&
             ls.slots[i].stride == slots[i].stride;
    }
    if (!same) {
      list->setVertexBuffers(0, views, n, slots);
      std::copy(views, views + n, ls.views);
      std::copy(slots, slots + n, ls.slots);
      ls.view_count = n;
    }
  }
  const uint32_t stencil_ref = Reg(RB_STENCILREFMASK) & 0xFF;
  if (ls.stencil_ref != stencil_ref) {
    list->setStencilReference(stencil_ref);
    ls.stencil_ref = stencil_ref;
  }
  const float blend_factor[4] = {RegFloat(RB_BLEND_RED), RegFloat(RB_BLEND_RED + 1),
                                 RegFloat(RB_BLEND_RED + 2), RegFloat(RB_BLEND_RED + 3)};
  if (std::memcmp(ls.blend, blend_factor, sizeof(blend_factor))) {
    list->setBlendFactor(blend_factor);
    std::memcpy(ls.blend, blend_factor, sizeof(blend_factor));
  }
  // (Debug, XMD_NATIVE_GPU_TEST=4: a quarter-size viewport - as many triangles, 1/16 the pixels)
  static const bool quarter_viewport = [] {
    const char* t = std::getenv("XMD_NATIVE_GPU_TEST");
    return t && t[0] == '4';
  }();
  if (quarter_viewport) {
    vp.width *= 0.25f;
    vp.height *= 0.25f;
  }
  if (std::memcmp(&ls.viewport, &vp, sizeof(vp))) {
    list->setViewports(vp);
    ls.viewport = vp;
  }
  // Debug (GPU cost): XMD_NATIVE_GPU_TEST=1 draws into a 1x1 scissor (no
  // rasterisation / pixel work, vertex work kept), =2 skips the draw calls.
  static const int gpu_test = [] {
    const char* v = std::getenv("XMD_NATIVE_GPU_TEST");
    return v ? std::atoi(v) : 0;
  }();
  // (=3 with XMD_NATIVE_GPU_TEST_RANGE=<first>,<last>: those draws of each frame only)
  static const std::pair<uint32_t, uint32_t> gpu_range = [] {
    std::pair<uint32_t, uint32_t> v{0, ~0u};
    if (const char* e = std::getenv("XMD_NATIVE_GPU_TEST_RANGE")) std::sscanf(e, "%u,%u", &v.first, &v.second);
    return v;
  }();
  if (gpu_test == 1 || (gpu_test == 3 && r->frame_stats.drawn >= gpu_range.first &&
                        r->frame_stats.drawn <= gpu_range.second))
    scissor = plume::RenderRect(0, 0, 1, 1);
  if (std::memcmp(&ls.scissor, &scissor, sizeof(scissor))) {
    list->setScissors(scissor);
    ls.scissor = scissor;
  }
  if (gpu_test == 2) {
    ++r->frame_stats.drawn;
    return;
  }
  clock.Mark(7);
  if (draw_indexed) {
    if (ls.ibv.buffer.ref != ibv.buffer.ref || ls.ibv.buffer.offset != ibv.buffer.offset ||
        ls.ibv.size != ibv.size) {
      list->setIndexBuffer(&ibv);
      ls.ibv = ibv;
    }
    list->drawIndexedInstanced(draw_count, 1, 0, draw_base, 0);
  } else {
    list->drawInstanced(draw_count, 1, start, 0);
  }
  ++r->frame_stats.drawn;
#if XMD_FSR2
  // FSR 2's masks: this draw noted for them (drawn again at the scene's resolve) when it is the HUD (no depth
  // test - the ring's no-depth quad, blend 01860186, aside), blended over the scene, or skinned (the wrestlers,
  // the referee: they move without motion vectors).
  if (g_fsr2 && g_match_scene.load() && !middle && targets.depth && SceneTarget(uint32_t(tw), uint32_t(th))) {
    const uint32_t blend = Reg(RB_BLENDCONTROL0);
    // (blended: drawn over the scene without writing depth - the game's models blend too, alpha 1, writing it)
    const bool blended =
        ((blend & 0x1F) != 1 || ((blend >> 8) & 0x1F) != 0) && !((Reg(RB_DEPTHCONTROL) >> 2) & 1);
    // (skinned: weights and indices - the wrestlers, the referee, the 3D front-row fans; ropes and rigid props
    // have indices only)
    bool weights = false, indices = false;
    for (const Element& el : decl) weights |= el.usage == 1, indices |= el.usage == 2;
    const bool skinned = weights && indices;
    float mask[4] = {0, 0, 0, 0};  // (reactive, composition)
    if (!depth_test && blend != 0x01860186u) {
      mask[0] = g_fsr2_mask_values[0], mask[1] = 1.0f;
    } else if (depth_test && blended) {
      mask[0] = g_fsr2_mask_values[1], mask[1] = 1.0f;
    } else if (depth_test && skinned) {
      mask[0] = g_fsr2_mask_values[2], mask[1] = 1.0f;
    }
    // (the crowd: alpha-tested, blended, indices without weights - animated cut-outs with no motion vectors)
    else if (depth_test && alpha_test && indices && ((blend & 0x1F) != 1 || ((blend >> 8) & 0x1F) != 0)) {
      mask[0] = g_fsr2_mask_values[3], mask[1] = 1.0f;
    }
    // (test aid XMD_TEST_FSR2_IDS: every scene draw, its number in the masks - logged at match frame 900)
    static const bool ids = EnvFlag("XMD_TEST_FSR2_IDS");
    if (ids) {
      const uint32_t n = r->frame_stats.drawn;
      mask[0] = float(n % 255 + 1) / 255.0f, mask[1] = float(n / 255) / 255.0f;
      if (uint32_t(g_frames_shown.load()) == 900) {
        std::string u;
        for (const Element& el : decl) u += fmt::format(" {}.{}", el.usage, el.index);
        REXLOG_INFO("[fsr2ids] {} blend {:08X} depth {:08X} cc {:08X} vs {:016X} ps {:016X} n {} decl{}", n, blend,
                    Reg(RB_DEPTHCONTROL), Reg(RB_COLORCONTROL), vs->hash, ps->hash, draw_count, u);
      }
    }
    if ((mask[0] > 0.0f || mask[1] > 0.0f) && Fsr2MaskWanted(r)) {
      const plume::RenderPipeline* mask_pso =
          Fsr2MaskPipeline(r, vs, packed_normals ? 1 : 0, layout, slots, layout_hash, topology, depth_test);
      auto [cpu, gpu] = Allocate(r, 256);
      if (mask_pso && cpu) {
        std::memcpy(cpu, mask, sizeof(mask));
        Renderer::Fsr2MaskDraw m;
        m.pso = mask_pso;
        std::copy(draw_constants, draw_constants + 4, m.constants);
        m.constants[3] = gpu;
        m.view_count = max_slot + 1;
        std::copy(views, views + m.view_count, m.views);
        std::copy(slots, slots + m.view_count, m.slots);
        m.ibv = ibv;
        m.indexed = draw_indexed;
        m.count = draw_count;
        m.start = start;
        m.base = draw_base;
        m.viewport = vp;
        m.scissor = scissor;
        m.stencil_ref = stencil_ref;
        m.hud = !depth_test;
        // (a skinned draw: its view x projection and bones kept by its key - the vertex data's, its range, its
        // shader - for the next frame's motion, and its own motion pipeline)
        if (depth_test && skinned) {
          if (r->fsr2_skin_frame != r->frames) {
            std::swap(r->fsr2_skin_prev, r->fsr2_skin_now);
            r->fsr2_skin_now.clear();
            if (r->fsr2_skin_frame + 1 != r->frames) r->fsr2_skin_prev.clear();  // (frames skipped: none to match)
            r->fsr2_skin_frame = r->frames;
          }
          uint64_t key_parts[6] = {0, 0, uint64_t(start), uint64_t(draw_count), uint64_t(uint32_t(draw_base)),
                                   vs->hash};
          for (const Element& el : decl)
            if (el.usage == 0 && el.index == 0 && el.stream < 15)
              key_parts[0] = g_streams[el.stream].buffer, key_parts[1] = g_streams[el.stream].offset;
          const uint64_t key = XXH3_64bits(key_parts, sizeof(key_parts));
          auto& rows = r->fsr2_skin_now[key];
          const uint8_t* c = Device() + guest::RegisterOffset(0x4000);
          for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k) rows.rows[i * 4 + k] = BeFloat(c + 16 * (54 + i) + 4 * k);
          for (int i = 0; i < 64; ++i)
            for (int k = 0; k < 4; ++k) rows.rows[16 + i * 4 + k] = BeFloat(c + 16 * (74 + i) + 4 * k);
          const uint32_t bools = Be32(Device() + guest::RegisterOffset(0x4900));
          m.skin_bones = (bools & (1u << 3)) ? 4 : (bools & (1u << 2)) ? 3 : (bools & (1u << 1)) ? 2 : 1;
          std::copy(ndc, ndc + 4, m.skin_ndc);
          m.skin_pso = Fsr2SkinPipeline(r, layout, slots, layout_hash, topology);
          m.skin_key = m.skin_pso ? key : 0;
        }
        r->fsr2_mask_draws.push_back(m);
      }
    }
    if (depth_test && Fsr2MaskWanted(r)) r->fsr2_mask_after_3d = r->fsr2_mask_draws.size();
  }
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// entry points

uint32_t LastFrameDraws() { return g_last_draws.load(); }
uint64_t FramesShown() { return g_frames_shown.load(); }

FrameCosts LastFrameCosts() {
  std::lock_guard lock(g_costs_mutex);
  return g_costs;
}

std::string RendererLabel() {
  if (g_failed) return "Native - stopped";
  if (!g_main) return "Native - starting";
  return fmt::format("Native {}x", g_scale);
}

// (always: the game draws only with the native renderer)
bool Enabled() { return g_main; }

// No switching to the emulated renderer any more.
bool CanSwitch() { return false; }

bool UpscalerAvailable(const std::string& kind, std::string* why) {
  std::string reason;
  if (kind == "fsr2") {
#if XMD_FSR2
    const backend::Api api = backend::ActiveApi();
    if (api == backend::Api::kD3D11)
      reason = "FSR 2 needs compute shaders: Direct3D 12 or Vulkan (the game draws with Direct3D 11)";
    else if (api == backend::Api::kNone)
      reason = "FSR 2: the renderer hasn't started";
#else
    reason = "FSR 2 isn't in this build (Windows only)";
#endif
  } else if (kind == "dlss") {
#if XMD_DLSS
    const backend::Api api = backend::ActiveApi();
    if (api == backend::Api::kD3D11)
      reason = "DLSS needs Direct3D 12 (the game draws with Direct3D 11)";
    else if (api == backend::Api::kNone || !g_r)
      reason = "DLSS: the renderer hasn't started";
    else if (!xmd::dlss::Probe(DlssHost(g_r), &reason) && reason.empty())
      reason = "DLSS can't run here";
#else
    reason = "DLSS isn't in this build (built without the DLSS SDK)";
#endif
  } else {
    reason = "unknown upscaler " + kind;
  }
  static std::mutex mutex;
  static std::set<std::string> logged;
  if (!reason.empty()) {
    std::lock_guard lock(mutex);
    if (logged.insert(reason).second) REXLOG_INFO("native renderer: upscaling choice hidden - {}", reason);
  }
  if (why) *why = reason;
  return reason.empty();
}

void SetNativeActive(bool) {}

std::string FailureReason() {
  std::lock_guard lock(g_fail_mutex);
  return g_fail_reason;
}

bool NativeActive() { return g_main && !g_failed && !g_suspended; }

void SetMatchScene(bool in_match) { g_match_scene = in_match; }

void RequestDrawDump(const std::string& file) {
  {
    std::lock_guard lock(g_dump_request_mutex);
    g_dump_request_file = file;
  }
  g_dump_requested = true;
}

void RequestFrameCapture(bool previous) {
  {
    std::lock_guard lock(g_capture_mutex);
    g_capture.clear();
  }
  g_capture_output = previous ? g_last_shown_output.load() : -1;
  g_capture_wanted = true;
}
bool TakeFrameCapture(std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height) {
  std::lock_guard lock(g_capture_mutex);
  if (g_capture.empty()) return false;
  rgb = std::move(g_capture), width = g_capture_w, height = g_capture_h;
  g_capture.clear();
  return true;
}

float CameraFovScale() { return g_fov_scale.load(std::memory_order_relaxed); }

// a^-1 (4 x 4, Gauss-Jordan); false if singular.
bool Invert4(const float a[16], float out[16]) {
  double m[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? a[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int piv = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
    if (std::fabs(m[piv][c]) < 1e-12) return false;
    std::swap(m[piv], m[c]);
    const double inv = 1.0 / m[c][c];
    for (int k = 0; k < 8; ++k) m[c][k] *= inv;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = m[r][c];
        for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(m[r][c + 4]);
  return true;
}

void SetWindowSizeSource(std::function<std::pair<uint32_t, uint32_t>()> source) {
  g_window_size = std::move(source);
}

void Attach(rex::memory::Memory* memory) {
  g_memory = memory;
  // Debug: XMD_HASH_MEM=<hex physical>:<hex size>[,...] - the regions'
  // hashes every 2 s in the log (in either renderer mode).
  if (const char* spec = std::getenv("XMD_HASH_MEM")) {
    std::vector<std::pair<uint32_t, uint32_t>> regions;
    for (const char* s = spec; *s;) {
      char* e = nullptr;
      const uint32_t a = uint32_t(std::strtoul(s, &e, 16));
      if (!e || *e != ':') break;
      const uint32_t n = uint32_t(std::strtoul(e + 1, &e, 16));
      regions.emplace_back(a, n);
      s = *e == ',' ? e + 1 : e;
    }
    std::thread([memory, regions] {
      rex::thread::set_current_thread_name("memory regions log");
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string line;
        for (const auto& [a, n] : regions) {
          line += fmt::format(" {:08X}={:016X}", a, XXH3_64bits(memory->TranslatePhysical(a), n));
        }
        REXLOG_INFO("[xmd] memory hashes:{}", line);
      }
    }).detach();
  }
  g_virtual_base = memory->virtual_membase();
  for (uint32_t i = 0; i < 4096; ++i) {
    const auto* heap = memory->LookupHeap(i << 20);
    g_virtual_offset[i] = heap ? heap->host_address_offset() : 0;
  }
  // On a crash (usually the emulator aborting on a lost GPU device - the
  // same device as this renderer's), log the device's fault details.
  xmd::SetCrashHook([] {
    Renderer* r = g_r;
    if (!r || !r->device) return;
    if (backend::DeviceLost(r->device.get())) backend::ReportDeviceLost(r->device.get());
  });
  {
    g_main = true;
    g_scale = 1;  // set with the frame images' size at the first frame (ApplyOutputSettings)
    // The emulated GPU keeps running the command stream but no longer draws;
    // its presentation shows this renderer's frames (only: no fallback).
    rex::external_frame::SetHostDrawingDisabled(true);
    rex::external_frame::SetProvider([](rex::external_frame::Frame& f) {
      if (g_failed || g_suspended) return false;
      return backend::GetFrame(f);
    });
    REXLOG_INFO("native renderer: main renderer at {}x resolution (the only one: no emulated fallback)", g_scale);
  }
}

// Present: the front buffer (the last full-screen colour resolve of the
// frame; with XMD_NATIVE_STOP_AT_RESOLVE the main target before
// post-processing) is copied to the window.
// The pipeline showing a texture in the frame image (shaders/present.*.hlsl).
// (the full-screen triangle with pixel shader `ps_name`: present.ps, or FSR 1's fsr_easu.ps / fsr_rcas.ps)
const plume::RenderPipeline* PresentPipeline(Renderer* r, const char* ps_name = "present.ps",
                                             RenderFormat target = RenderFormat::R8G8B8A8_UNORM) {
  struct Built {
    std::unique_ptr<plume::RenderShader> vs, ps;
    std::unique_ptr<plume::RenderPipeline> pso;
  };
  static std::map<std::string, Built> built;
  const std::string key = std::string(ps_name) + "/" + std::to_string(int(target));
  const bool tried = built.count(key) != 0;
  Built& b = built[key];
  if (tried) return b.pso.get();
  auto& vs = b.vs;
  auto& ps = b.ps;
  auto& pso = b.pso;
  const std::vector<uint8_t> vs_code = ReadShader("present.vs");
  const std::vector<uint8_t> ps_code = ReadShader(ps_name);
  if (vs_code.empty() || ps_code.empty()) {
    REXLOG_WARN("native renderer: {} shaders missing", ps_name);
    return nullptr;
  }
  plume::RenderGraphicsPipelineDesc d;
  d.pipelineLayout = r->layout.get();
  d.vertexShader = Compiled(r, vs_code, vs);
  d.pixelShader = Compiled(r, ps_code, ps);
  d.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  d.cullMode = plume::RenderCullMode::NONE;
  d.depthClipEnabled = true;
  d.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  d.renderTargetCount = 1;
  d.renderTargetFormat[0] = target;
  if (d.vertexShader && d.pixelShader) pso = r->device->createGraphicsPipeline(d);
  if (pso && !backend::PipelineCreated(pso.get())) pso.reset();
  if (!pso) REXLOG_ERROR("native renderer: could not create the {} pipeline", ps_name);
  return pso.get();
}

plume::RenderTexture* FrameImage(Renderer* r) { return r->outputs[r->output_index].get(); }


#if XMD_FSR2
// FSR 2 (experimental): the front buffer (the resolve copy at physical `base`; `region` the part shown) gathered with
// this frame's scene depth (caught at the scene's resolve) into FSR 1's intermediate, the window's size; false if it
// couldn't (Present uses FSR 1 then). `swap`: the copy's red and blue swapped.
bool PresentFsr2(Renderer* r, uint32_t base, const float region[2], bool* swap) {
  auto it = r->resolved.find(base);
  if (it == r->resolved.end() || !it->second.resource || !r->fsr_image) return false;
  ResolvedTexture& color = it->second;
  const uint32_t rw = std::min(uint32_t(float(color.host_w) * region[0] + 0.5f), r->fsr2_depth_w);
  const uint32_t rh = std::min(uint32_t(float(color.host_h) * region[1] + 0.5f), r->fsr2_depth_h);
  if (rw < 64 || rh < 64) return false;
  auto* list = r->list;
  if (!r->fsr2 || r->fsr2->MaxRenderWidth() != r->fsr2_depth_w || r->fsr2->MaxRenderHeight() != r->fsr2_depth_h ||
      r->fsr2->DisplayWidth() != g_out_w || r->fsr2->DisplayHeight() != g_out_h) {
    if (r->fsr2 && !WaitIdle(r, "FSR 2 sizes")) return false;
    r->fsr2.reset();
    xmd::fsr2::Host host;
    host.device = r->device.get();
    host.format = backend::ShaderFormat();
    host.vulkan = backend::ActiveApi() == backend::Api::kVulkan;
    host.read_shader = [](const std::string& name) { return ReadShader(name); };
    r->fsr2 = xmd::fsr2::Create(host, r->fsr2_depth_w, r->fsr2_depth_h, g_out_w, g_out_h, kFrames);
    if (!r->fsr2) {
      r->fsr2_failed = true;  // (FSR 1 from now on)
      g_fsr2 = false;
      return false;
    }
    // The motion vectors: none yet (a still camera's) - cleared once.
    if (r->fsr2_motion) Retire(r, std::move(r->fsr2_motion));
    r->fsr2_motion = r->device->createTexture(plume::RenderTextureDesc::Texture2D(
        r->fsr2_depth_w, r->fsr2_depth_h, 1, RenderFormat::R16G16B16A16_FLOAT,
        plume::RenderTextureFlag::RENDER_TARGET | plume::RenderTextureFlag::STORAGE |
            plume::RenderTextureFlag::UNORDERED_ACCESS));
    if (!r->fsr2_motion) return false;
    if (r->fsr2_depth32) Retire(r, std::move(r->fsr2_depth32));
    r->fsr2_depth32 = r->device->createTexture(plume::RenderTextureDesc::Texture2D(
        r->fsr2_depth_w, r->fsr2_depth_h, 1, RenderFormat::R32_FLOAT,
        plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS));
    if (!r->fsr2_depth32) return false;
    r->fsr2_depth32->setName("FSR 2 / DLSS depth");
    const plume::RenderTexture* mv = r->fsr2_motion.get();
    auto fb = r->device->createFramebuffer(plume::RenderFramebufferDesc(&mv, 1));
    Barrier(r, r->fsr2_motion.get(), RenderTextureLayout::COLOR_WRITE);
    list->setFramebuffer(fb.get());
    list->clearColor(0, plume::RenderColor(0, 0, 0, 0));
    Barrier(r, r->fsr2_motion.get(), RenderTextureLayout::SHADER_READ);
    Retire(r, std::shared_ptr<plume::RenderFramebuffer>(std::move(fb)));
    r->fsr2_motion_layout = RenderTextureLayout::SHADER_READ;
    r->list_state = {};
  }
  if (r->fsr_image_fresh && r->fsr_framebuffer) {  // (a render target: cleared before any other use - Direct3D 12)
    Barrier(r, r->fsr_image.get(), RenderTextureLayout::COLOR_WRITE);
    list->setFramebuffer(r->fsr_framebuffer.get());
    list->clearColor(0, plume::RenderColor(0, 0, 0, 1));
    r->fsr_image_fresh = false;
    r->list_state = {};
  }
  using xmd::fsr2::Texture;
  xmd::fsr2::Frame f;
  // (UNKNOWN: plume knows where each is - the backend's first barrier always goes)
  // (read as the target's view format: the copy's own is typeless - an invalid view on Direct3D 12)
  const RenderFormat color_format = color.view_format != RenderFormat::UNKNOWN ? color.view_format : color.format;
  f.color = Texture{color.resource.get(), color_format, color.host_w, color.host_h, RenderTextureLayout::UNKNOWN};
  f.depth = Texture{r->fsr2_depth.get(), backend::DepthFormat(), r->fsr2_depth_w, r->fsr2_depth_h,
                    RenderTextureLayout::UNKNOWN};
  f.motion = Texture{r->fsr2_motion.get(), RenderFormat::R16G16B16A16_FLOAT, r->fsr2_depth_w, r->fsr2_depth_h,
                     RenderTextureLayout::UNKNOWN};
  f.output = Texture{r->fsr_image.get(), RenderFormat::R8G8B8A8_UNORM, g_out_w, g_out_h, RenderTextureLayout::UNKNOWN};
  if (r->fsr2_mask_frame == r->frames && r->fsr2_reactive && r->fsr2_mask_w == r->fsr2_depth_w &&
      r->fsr2_mask_h == r->fsr2_depth_h) {
    f.reactive = Texture{r->fsr2_reactive.get(), RenderFormat::R8_UNORM, r->fsr2_mask_w, r->fsr2_mask_h,
                         RenderTextureLayout::UNKNOWN};
    f.composition = Texture{r->fsr2_composition.get(), RenderFormat::R8_UNORM, r->fsr2_mask_w, r->fsr2_mask_h,
                            RenderTextureLayout::UNKNOWN};
  }
  f.render_w = rw;
  f.render_h = rh;
  f.jitter_x = g_jitter[0];
  f.jitter_y = g_jitter[1];
  f.frame_ms = 1000.0f / 60.0f;
  f.fov_y = 0.2967f;  // (a match camera's; the scene camera's below when its projection was caught)
  if (r->fsr2_vp_frame[0] == r->frames) {
    const float* m = r->fsr2_vp[0];
    const float sy = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    if (sy > 0.01f) f.fov_y = 2.0f * std::atan(1.0f / sy);
  }
  f.sharpness = g_fsr_sharpness;
  f.reset = r->fsr2_last_frame == ~0ull || r->fsr2_last_frame + 1 != r->frames;
  // The camera's motion vectors: this frame's clip space (unjittered) to the last frame's - none (identity) without
  // both cameras.
  {
    float reproject[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float inv[16];
    // (test aid: XMD_TEST_FSR2_NO_MOTION - none, to compare)
    static const bool no_motion = EnvFlag("XMD_TEST_FSR2_NO_MOTION");
    if (!no_motion && !f.reset && r->fsr2_vp_frame[0] == r->frames && r->fsr2_vp_frame[1] + 1 == r->frames &&
        Invert4(r->fsr2_vp[0], inv)) {
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
          float sum = 0;
          for (int k = 0; k < 4; ++k) sum += inv[i * 4 + k] * r->fsr2_vp[1][k * 4 + j];
          reproject[i * 4 + j] = sum;
        }
    }
    const float jitter[2] = {2.0f * g_jitter[0] / float(rw), -2.0f * g_jitter[1] / float(rh)};
    Texture depth32{r->fsr2_depth32.get(), RenderFormat::R32_FLOAT, r->fsr2_depth_w, r->fsr2_depth_h,
                    RenderTextureLayout::UNKNOWN};
    r->fsr2->Motion(list, r->back_index, f.depth, f.motion, depth32, reproject, jitter, rw, rh);
    f.motion.layout = RenderTextureLayout::UNKNOWN;
    // Test aid: XMD_TEST_FSR2_DUMP=<prefix> - FSR 2's colour and motion (raw texels: the motion RGBA16F, the rest 4 bytes) and the
    // reprojection (<prefix>.txt) in the frames XMD_TEST_FRAME_SHOT(S/_AT) take: the motion vectors checked
    // offline against the next frame.
    static const std::string dump = [] {
      char* v = nullptr;
      size_t n = 0;
      std::string s;
      if (_dupenv_s(&v, &n, "XMD_TEST_FSR2_DUMP") == 0 && v) s = v, free(v);
      return s;
    }();
    static int dumps_taken = 0;
    if (!dump.empty() && !r->hidden_frame && dumps_taken < std::max(1, EnvInt("XMD_TEST_FRAME_SHOTS", 1)) &&
        uint32_t(g_frames_shown.load()) > uint32_t(std::max(1, EnvInt("XMD_TEST_FRAME_SHOT_AT", 600)))) {
      const int i = dumps_taken++;
      auto take = [&](plume::RenderTexture* t, RenderFormat format, uint32_t w, uint32_t h, const char* what,
                      uint32_t bytes = 4) {
        Renderer::Fsr2Dump d;
        d.w = w, d.h = h, d.bytes = bytes, d.row = ((w * bytes + 255) & ~255u) / bytes;
        d.file = dump + "_" + std::to_string(i) + "_" + what + ".raw";
        d.buffer = r->device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(d.row) * h * bytes));
        if (!d.buffer) return;
        Barrier(r, t, RenderTextureLayout::COPY_SOURCE);
        list->copyTextureRegion(
            plume::RenderTextureCopyLocation::PlacedFootprint(d.buffer.get(), format, w, h, 1, d.row),
            plume::RenderTextureCopyLocation::Subresource(t));
        Barrier(r, t, RenderTextureLayout::SHADER_READ);
        r->fsr2_dumps.push_back(std::move(d));
      };
      take(color.resource.get(), color_format, color.host_w, color.host_h, "color");
      take(r->fsr2_motion.get(), RenderFormat::R16G16B16A16_FLOAT, r->fsr2_depth_w, r->fsr2_depth_h, "motion", 8);
      if (f.reactive.texture) {
        take(r->fsr2_reactive.get(), RenderFormat::R8_UNORM, r->fsr2_mask_w, r->fsr2_mask_h, "reactive", 1);
        take(r->fsr2_composition.get(), RenderFormat::R8_UNORM, r->fsr2_mask_w, r->fsr2_mask_h, "composition", 1);
      }
      if (FILE* t = std::fopen((dump + ".txt").c_str(), i ? "a" : "w")) {
        std::fprintf(t, "%d render %u %u color %u %u depth %u %u jitter %.6f %.6f swap %d reset %d reproject", i, rw, rh,
                     color.host_w, color.host_h, r->fsr2_depth_w, r->fsr2_depth_h, g_jitter[0], g_jitter[1],
                     color.swap_rb ? 1 : 0, f.reset ? 1 : 0);
        for (float v : reproject) std::fprintf(t, " %.9g", v);
        std::fprintf(t, "\n");
        std::fclose(t);
      }
      r->list_state = {};
    }
  }
  bool upscaled = false;
#if XMD_DLSS
  if (g_dlss) {
    static int32_t quality = 1;
    if ((r->frames & 31) == 0 || !r->dlss) quality = REXCVAR_GET(native_upscale_quality);
    if (!r->dlss || r->dlss->MaxRenderWidth() != r->fsr2_depth_w || r->dlss->MaxRenderHeight() != r->fsr2_depth_h ||
        r->dlss->DisplayWidth() != g_out_w || r->dlss->DisplayHeight() != g_out_h || r->dlss->Quality() != quality) {
      if (r->dlss && !WaitIdle(r, "DLSS sizes")) return false;
      r->dlss = xmd::dlss::Create(DlssHost(r), r->fsr2_depth_w, r->fsr2_depth_h, g_out_w, g_out_h, quality);
      if (!r->dlss) {
        r->dlss_failed = true;  // (FSR 2 from now on)
        g_dlss = false;
      }
    }
    if (r->dlss) {
      xmd::fsr2::Frame d = f;
      d.depth = Texture{r->fsr2_depth32.get(), RenderFormat::R32_FLOAT, r->fsr2_depth_w, r->fsr2_depth_h,
                        RenderTextureLayout::UNKNOWN};
      upscaled = r->dlss->Dispatch(list, d);
      if (!upscaled && !r->dlss_failed) {
        static int fails = 0;
        if (++fails > 30) r->dlss_failed = true, g_dlss = false;  // (a lasting failure: FSR 2)
      }
      // (DLSS sets its own state: the renderer binds everything again)
      r->list_state = {};
      static bool logged = false;
      if (upscaled && !logged) {
        logged = true;
        REXLOG_INFO("native renderer: DLSS {}x{} -> {}x{}", rw, rh, g_out_w, g_out_h);
      }
    }
  }
#endif
  if (!upscaled && !r->fsr2->Dispatch(list, r->back_index, f)) return false;
  // (back to what the renderer tracks: the colour, depth and motion read; the result read by the present pass)
  color.layout = RenderTextureLayout::SHADER_READ;
  r->fsr2_depth_layout = RenderTextureLayout::SHADER_READ;
  r->fsr2_motion_layout = RenderTextureLayout::SHADER_READ;
  Barrier(r, color.resource.get(), RenderTextureLayout::SHADER_READ);
  Barrier(r, r->fsr2_depth.get(), RenderTextureLayout::SHADER_READ);
  Barrier(r, r->fsr_image.get(), RenderTextureLayout::SHADER_READ);
  if (f.reactive.texture) {
    r->fsr2_reactive_layout = r->fsr2_composition_layout = RenderTextureLayout::SHADER_READ;
    Barrier(r, r->fsr2_reactive.get(), RenderTextureLayout::SHADER_READ);
    Barrier(r, r->fsr2_composition.get(), RenderTextureLayout::SHADER_READ);
  }
  Barrier(r, r->fsr2_depth32.get(), RenderTextureLayout::SHADER_READ);
  r->list_state = {};
  r->fsr2_last_frame = r->frames;
  *swap = color.swap_rb;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("native renderer: FSR 2 {}x{} -> {}x{}", rw, rh, g_out_w, g_out_h);
  }
  return true;
}
#endif

// Draws the front buffer texture Swap named into the frame image; false if
// it can't be shown.
bool PresentFrontBuffer(Renderer* r, uint32_t front_buffer) {
  if (!front_buffer || g_stop_at_resolve) return false;
  const uint8_t* obj = Virtual(front_buffer);
  if ((Be32(obj) & 0xF) != 3) return false;  // not a texture
  uint32_t fetch[6];
  for (int k = 0; k < 6; ++k) fetch[k] = Be32(obj + 0x1C + 4 * k);
  // The object's fetch constant has a virtual address; textures:: expects
  // the physical one the GPU sees.
  const uint32_t physical = g_memory->GetPhysicalAddress(fetch[1] & 0xFFFFF000u);
  fetch[1] = (fetch[1] & 0xFFFu) | physical;
  const plume::RenderPipeline* pso = PresentPipeline(r);
  if (!pso) return false;
  // (a pass's texture shown for debugging: red, green, blue as they are and alpha one, whatever swizzle the game
  // samples it with)
  if (g_stop_after && g_stop_texture == front_buffer) fetch[3] = (fetch[3] & ~(0x3FFFFu << 1)) | (0xA88u << 1);
  const textures::Context ctx = TextureContext(r);
  const uint32_t srv = textures::Texture(ctx, fetch, 0);
  {
    static uint32_t logged = 0;
    if (logged != front_buffer) {
      logged = front_buffer;
      REXLOG_INFO("native renderer: front buffer texture {:08X}: fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {}",
                  front_buffer, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5],
                  srv == UINT32_MAX ? "no texture" : "shown");
    }
  }
  if (srv == UINT32_MAX) return false;
  // (bilinear whatever the game's fetch says: present.ps averages a bigger front buffer down with bilinear taps
  // - with the game's point filter each tap was one texel, so ANTI-ALIASING 2X, a 2:1 scene, averaged nothing)
  uint32_t linear_fetch[6];
  std::memcpy(linear_fetch, fetch, sizeof(linear_fetch));
  linear_fetch[3] = (linear_fetch[3] & ~(0xFu << 19)) | (1u << 19) | (1u << 21);  // (mag / min filter: linear)
  const uint32_t sampler = textures::Sampler(ctx, linear_fetch);
  const uint32_t w = (fetch[2] & 0x1FFF) + 1, h = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const float scale[2] = {std::min(1.0f, float(kWidth) / float(w)),
                          std::min(1.0f, float(kHeight) / float(h))};
  const float out_size[2] = {float(g_out_w), float(g_out_h)};
  auto* list = r->list;
  // A full-screen pass: texture table index `tex` (the sampler's), constants {tex, sampler, c, the frame's size}.
  auto pass = [&](const plume::RenderPipeline* p, uint32_t tex, const float c[2]) {
    auto [cpu, gpu] = Allocate(r, 256);
    if (!cpu) return false;
    const uint32_t indices[2] = {tex, sampler};
    std::memcpy(cpu, indices, 8);
    std::memcpy(cpu + 8, c, 8);
    std::memcpy(cpu + 16, out_size, 8);
    list->setGraphicsPipelineLayout(r->layout.get());
    BindTables(r, list);
    if (backend::CompactTables() || SlotTables()) {
      // (compact / slot tables: the texture and sampler in place 0 of the draw's sets)
      uint32_t places[2][kSlotTextures];
      std::fill(&places[0][0], &places[0][0] + 2 * kSlotTextures, UINT32_MAX);
      places[0][0] = r->texture_base[0] + tex;
      places[1][0] = sampler;
      r->draw_sets[0] = CompactSet(r, 0, places[0]);
      r->draw_sets[1] = CompactSet(r, 1, places[1]);
      BindCompactSets(r, list);
      const uint32_t zero[2] = {0, 0};
      std::memcpy(cpu, zero, 8);
    }
    const RenderBufferReference constants[4] = {{}, {}, {}, gpu};
    BindConstants(r, constants);
    list->setPipeline(p);
    list->setViewports(plume::RenderViewport(0, 0, out_size[0], out_size[1], 0, 1));
    return true;
  };
  // FSR 2 (experimental; this frame's scene depth caught): into the intermediate, shown through RCAS unsharpened.
  bool fsr2 = false, fsr2_swap = false;
#if XMD_FSR2
  if (g_fsr2 && r->fsr2_depth && r->fsr2_depth_frame == r->frames)
    fsr2 = PresentFsr2(r, fetch[1] & 0xFFFFF000u, scale, &fsr2_swap);  // (fetch[1]: physical by now)
  // (the front buffer's fetch swizzle - ZYXW: red and blue swapped - on top of the copy's own swap)
  if (fsr2 && ((fetch[3] >> 1) & 7) == 2) fsr2_swap = !fsr2_swap;
#endif
  // FSR 1 (the scene smaller than the frame): EASU into the intermediate, then RCAS into the frame image below.
  const plume::RenderPipeline* easu =
      !fsr2 && g_fsr && r->fsr_framebuffer ? PresentPipeline(r, "fsr_easu.ps") : nullptr;
  const plume::RenderPipeline* rcas = easu || fsr2 ? PresentPipeline(r, "fsr_rcas.ps") : nullptr;
  if (easu) {
    Barrier(r, r->fsr_image.get(), RenderTextureLayout::COLOR_WRITE);
    list->setFramebuffer(r->fsr_framebuffer.get());
    if (r->fsr_image_fresh) {  // (a render target is cleared before its first draw - Direct3D 12)
      list->clearColor(0, plume::RenderColor(0, 0, 0, 1));
      r->fsr_image_fresh = false;
    }
    if (!pass(easu, srv, scale)) return false;
    list->setScissors(plume::RenderRect(0, 0, int32_t(out_size[0]), int32_t(out_size[1])));
    list->drawInstanced(3, 1, 0, 0);
    Barrier(r, r->fsr_image.get(), RenderTextureLayout::SHADER_READ);
    r->list_state = {};
  }
  Barrier(r, FrameImage(r), RenderTextureLayout::COLOR_WRITE);
  list->setFramebuffer(r->output_framebuffers[r->output_index].get());
  // (RCAS's strength: 2 stops of sharpening over the 0-1 setting, 1 the most)
  // (FSR 2 sharpened already; y: its result red / blue swapped back)
  const float sharp[2] = {
      !fsr2 && g_fsr_sharpness > 0.0f ? std::exp2(-2.0f * (1.0f - g_fsr_sharpness)) : 0.0f, fsr2_swap ? 1.0f : 0.0f};
  if (!(rcas ? pass(rcas, kFsrSrv, sharp) : pass(pso, srv, scale))) return false;
  if (g_frame_2d && g_wide > 1.0f) {
    // A 2D screen on a wide one: its 16:9 middle, black sides.
    list->clearColor(0, plume::RenderColor(0, 0, 0, 1));
    const int32_t mid = int32_t(std::lround(out_size[1] * kAspect16x9)), x0 = (int32_t(out_size[0]) - mid) / 2;
    list->setScissors(plume::RenderRect(x0, 0, x0 + mid, int32_t(out_size[1])));
  } else if (g_frame_2d && g_tall > 1.0f) {
    // On a tall one: its 16:9 middle, black above and below.
    list->clearColor(0, plume::RenderColor(0, 0, 0, 1));
    const int32_t mid = int32_t(std::lround(out_size[0] / kAspect16x9)), y0 = (int32_t(out_size[1]) - mid) / 2;
    list->setScissors(plume::RenderRect(0, y0, int32_t(out_size[0]), y0 + mid));
  } else {
    list->setScissors(plume::RenderRect(0, 0, int32_t(out_size[0]), int32_t(out_size[1])));
  }
  list->drawInstanced(3, 1, 0, 0);
  // Shader-readable between frames: the emulator's presentation reads it.
  Barrier(r, FrameImage(r), RenderTextureLayout::SHADER_READ);
  r->list_state = {};
  return true;
}

double g_present_publish_ms = 0;
void DoPresent(uint32_t front_buffer) {
  const auto present_start = std::chrono::steady_clock::now();
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  if (!BeginFrame(r)) return;
  // Vertex buffers not used for 20 s of frames are let go (every 512th frame).
  if ((r->frames & 127) == 0) {
    size_t gone = 0;
    for (auto it = r->vertex_buffers.begin(); it != r->vertex_buffers.end();) {
      // (an entry without a buffer of its own - the game's one-draw buffers, 40 new ones a frame - after 2 s: with
      // 50 000 of them in the map every lookup was a cache miss, 1 ms a frame)
      const bool kept = it->second.resource || (it->second.pool_ref.ref && it->second.pool_generation == r->pool.generation);
      if (r->frames - it->second.seen_frame > (kept ? 1200u : 120u)) {
        if (kept && it->second.sited) ++g_site_epoch;
        if (it->second.resource) Retire(r, std::move(it->second.resource));
        if (it->second.pool_ref.ref && it->second.pool_generation == r->pool.generation) r->pool.dead += it->second.size;
        it = r->vertex_buffers.erase(it);
        ++gone;
      } else {
        ++it;
      }
    }
    if (gone > 30000) REXLOG_INFO("native renderer: {} unused vertex buffers let go, {} kept", gone, r->vertex_buffers.size());
    if (r->pool.dead > (48ull << 20)) {
      REXLOG_INFO("native renderer: the small-buffer pool starts again ({} MB in {} buffers, {} MB of it unused)",
                  r->pool.chunks.size() * (kPoolChunk >> 20), r->pool.chunks.size(), r->pool.dead >> 20);
      for (auto& chunk : r->pool.chunks) Retire(r, std::move(chunk.buffer));
      r->pool.chunks.clear();
      r->pool.dead = 0;
      ++r->pool.generation;
      ++g_site_epoch;
    }
  }
  auto* list = r->list;
  plume::RenderTexture* back = FrameImage(r);
  // Debug: XMD_NATIVE_SHOW=<hex guest texture object> - that texture (a pass's
  // resolve target: see the log's "resolve ... -> texture" lines) on screen
  // instead of the front buffer.
  static const uint32_t show = [] {
    const char* v = std::getenv("XMD_NATIVE_SHOW");
    return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0u;
  }();
  if (show) front_buffer = show;
  {
    // (XMD_NATIVE_STOP_FILE=<file>: the number read from it twice a second, to step through a frame while it runs)
    static const std::string stop_file = [] {
      const char* v = std::getenv("XMD_NATIVE_STOP_FILE");
      return std::string(v ? v : "");
    }();
    static uint32_t tick = 0;
    if (!stop_file.empty() && ++tick % 15 == 0) {
      uint32_t n = 0;
      if (std::ifstream(stop_file) >> n; n != g_stop_after) {
        g_stop_after = n;
        g_stop_texture = 0;
      }
      n = 0;
      std::ifstream(stop_file + ".draws") >> n;
      g_stop_draws = n;
      n = 0;
      std::ifstream(stop_file + ".base") >> std::hex >> n;
      g_stop_base = n;
      n = 0;
      uint32_t after = 0;
      uint32_t extra = 0;
      std::ifstream(stop_file + ".ps") >> std::hex >> n >> after >> std::dec >> extra;
      g_stop_ps = n;
      g_stop_ps_extra = extra;
      g_stop_ps_after = after != 0;
    }
  }
  if (g_stop_after && g_stop_texture) front_buffer = g_stop_texture;
  const bool shown = PresentFrontBuffer(r, front_buffer);
  plume::RenderTexture* src = nullptr;
  RenderTextureLayout* src_layout = nullptr;
  uint32_t w = 0, h = 0, src_host_w = 0, src_host_h = 0;
  RenderFormat src_format = RenderFormat::UNKNOWN;
  // XMD_NATIVE_PRESENT=<hex physical address>: show that resolve instead.
  static const uint32_t forced = [] {
    char* v = nullptr;
    size_t n = 0;
    uint32_t a = 0;
    if (_dupenv_s(&v, &n, "XMD_NATIVE_PRESENT") == 0 && v) {
      a = uint32_t(std::strtoul(v, nullptr, 16)) & 0xFFFFF000u;
      free(v);
    }
    return a;
  }();
  if (shown) {
    // done
  } else if (!g_stop_at_resolve && (forced || r->present_source)) {
    auto it = r->resolved.find(forced ? forced : r->present_source);
    if (it != r->resolved.end()) {
      src = it->second.resource.get();
      src_layout = &it->second.layout;
      w = it->second.width;
      h = it->second.height;
      src_host_w = it->second.host_w;
      src_host_h = it->second.host_h;
      src_format = it->second.format;
    }
  }
  if (!shown && !src && r->main_target) {
    src = r->main_target->resource.get();
    src_layout = &r->main_target->layout;
    w = r->main_target->width;
    h = r->main_target->height;
    src_host_w = r->main_target->host_w;
    src_host_h = r->main_target->host_h;
    src_format = r->main_target->resource_format;
  }
  // The frame image is RGBA8: copy only what is in the same format family.
  if (src && src_format != RenderFormat::R8G8B8A8_TYPELESS) {
    static bool logged = false;
    if (!logged) REXLOG_WARN("native renderer: front buffer format {} is not RGBA8", int(src_format));
    logged = true;
    src = nullptr;
  }
  if (src) {
    Barrier(r, back, RenderTextureLayout::COPY_DEST);
    const RenderTextureLayout before = *src_layout;
    Transition(r, src, *src_layout, RenderTextureLayout::COPY_SOURCE);
    // (a fallback: unscaled, cut to the frame image)
    const plume::RenderBox box(0, 0, int32_t(std::min(src_host_w ? src_host_w : std::min(w, kWidth) * g_scale, g_out_w)),
                               int32_t(std::min(src_host_h ? src_host_h : std::min(h, kHeight) * g_scale, g_out_h)));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(back),
                            plume::RenderTextureCopyLocation::Subresource(src), 0, 0, 0, &box);
    if (before != RenderTextureLayout::UNKNOWN) Transition(r, src, *src_layout, before);
    Barrier(r, back, RenderTextureLayout::SHADER_READ);
  } else if (!shown) {
    Barrier(r, back, RenderTextureLayout::COLOR_WRITE);
    list->setFramebuffer(r->output_framebuffers[r->output_index].get());
    list->clearColor(0, plume::RenderColor(0.05f, 0.05f, 0.08f, 1.0f));
    Barrier(r, back, RenderTextureLayout::SHADER_READ);
  }
  // Test aid: XMD_TEST_FRAME_SHOT=<file.ppm> - the frame image as it is (the window's pixels, not a desktop
  // capture), once, 10 s into a match (upscaling comparisons); XMD_TEST_FRAME_SHOTS=<n>: n frames in a row
  // (<file>_<i>.ppm - motion bursts), XMD_TEST_FRAME_SHOT_AT=<match frames> instead of 600;
  // XMD_TEST_FRAME_SHOT_ON_2D=1: from the frame after the match's first frame without the scene after that.
  // XMD_TEST_FRAME_SHOT_EVERY=<k>: the n shots k match frames apart (contact sheets across a move).
  static const std::string frame_shot = [] {
    char* v = nullptr;
    size_t n = 0;
    std::string s;
    if (_dupenv_s(&v, &n, "XMD_TEST_FRAME_SHOT") == 0 && v) s = v, free(v);
    return s;
  }();
  static const int frame_shots = std::max(1, EnvInt("XMD_TEST_FRAME_SHOTS", 1));
  static const uint32_t frame_shot_at = uint32_t(std::max(1, EnvInt("XMD_TEST_FRAME_SHOT_AT", 600)));
  static const bool frame_shot_on_2d = EnvInt("XMD_TEST_FRAME_SHOT_ON_2D", 0) != 0;
  static const uint32_t frame_shot_every = uint32_t(std::max(0, EnvInt("XMD_TEST_FRAME_SHOT_EVERY", 0)));
  static int frame_shots_taken = 0;
  bool test_shot = !frame_shot.empty() && frame_shots_taken < frame_shots && !r->hidden_frame &&
                   g_match_scene.load() &&
                   uint32_t(g_frames_shown.load()) > frame_shot_at + frame_shot_every * uint32_t(frame_shots_taken) &&
                   (!frame_shot_on_2d || g_flat_frame_seen);
  if (frame_shot_on_2d && g_match_scene.load() && uint32_t(g_frames_shown.load()) <= frame_shot_at) g_flat_frame_seen = false;
  // Test aid: XMD_TEST_SHOT_REQUEST=<file> - whenever that file appears (anywhere: menus, story walks), the next
  // frame is shot to the .ppm named in it and the file is removed (a hidden test game has no window to capture).
  static const std::string shot_request = [] {
    char* v = nullptr;
    size_t n = 0;
    std::string s;
    if (_dupenv_s(&v, &n, "XMD_TEST_SHOT_REQUEST") == 0 && v) s = v, free(v);
    return s;
  }();
  static uint32_t shot_request_poll = 0;
  std::string requested_file;
  const bool capture = !r->hidden_frame && g_capture_wanted.exchange(false);  // (RequestFrameCapture)
  if (capture) test_shot = true;
  // (the frame shown before this one: its image is still in the ring - kOutputs - for a few frames)
  const int capture_output = capture ? g_capture_output.exchange(-1) : -1;
  plume::RenderTexture* shot_src = capture_output >= 0 && uint32_t(capture_output) != r->output_index
                                       ? r->outputs[uint32_t(capture_output) % kOutputs].get()
                                       : back;
  if (!r->hidden_frame) g_last_shown_output = int(r->output_index);
  if (!test_shot && !shot_request.empty() && !r->hidden_frame && ++shot_request_poll % 15 == 0)
    if (FILE* f = std::fopen(shot_request.c_str(), "rb")) {
      char name[512] = {};
      const size_t got = std::fread(name, 1, sizeof name - 1, f);
      std::fclose(f);
      std::remove(shot_request.c_str());
      requested_file.assign(name, got);
      while (!requested_file.empty() && std::isspace(uint8_t(requested_file.back()))) requested_file.pop_back();
    }
  std::shared_ptr<plume::RenderBuffer> shot;
  const uint32_t shot_row = (g_out_w + 63) & ~63u;  // (256-byte rows)
  std::string shot_file = requested_file.empty() ? frame_shot : requested_file;
  if (!requested_file.empty()) test_shot = true;
  if (test_shot) {
    if (frame_shots > 1 && requested_file.empty()) {
      const size_t dot = shot_file.rfind('.');
      shot_file.insert(dot == std::string::npos ? shot_file.size() : dot, "_" + std::to_string(frame_shots_taken));
    }
    if (requested_file.empty() && !capture) ++frame_shots_taken;
    shot = r->device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(shot_row) * g_out_h * 4));
    if (shot) {
      Barrier(r, shot_src, RenderTextureLayout::COPY_SOURCE);
      list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(
                                  shot.get(), RenderFormat::R8G8B8A8_UNORM, g_out_w, g_out_h, 1, shot_row),
                              plume::RenderTextureCopyLocation::Subresource(shot_src));
      Barrier(r, shot_src, RenderTextureLayout::SHADER_READ);
    }
  }
#if XMD_DEBUG_SUITE
  // (a dbg_shot due: the frame image copied to a readback buffer, written once the frame is done - below)
  if (!shot && !r->hidden_frame && xmd::dbg::ShotWanted()) {
    shot = r->device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(shot_row) * g_out_h * 4));
    if (shot) {
      Barrier(r, back, RenderTextureLayout::COPY_SOURCE);
      list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(
                                  shot.get(), RenderFormat::R8G8B8A8_UNORM, g_out_w, g_out_h, 1, shot_row),
                              plume::RenderTextureCopyLocation::Subresource(back));
      Barrier(r, back, RenderTextureLayout::SHADER_READ);
    }
  }
#endif
  r->list->end();
  // Debug: XMD_NATIVE_TEST_STALL=<frame> stalls the GPU queue for good at
  // that frame (as a stuck GPU job would), to test WaitForFrame's way out.
  static const uint64_t stall_frame = [] {
    char* v = nullptr;
    size_t n = 0;
    uint64_t f = ~0ull;
    if (_dupenv_s(&v, &n, "XMD_NATIVE_TEST_STALL") == 0 && v) {
      f = std::strtoull(v, nullptr, 10);
      free(v);
    }
    return f;
  }();
  if (r->frames == stall_frame) {
    backend::StallQueue(r->queue.get());
    REXLOG_WARN("native renderer: test stall at frame {}", r->frames);
  }
  plume::RenderCommandList* submit = r->list;
  if (r->recorder) {
    r->recorder->Finish();  // (the worker has recorded the whole frame)
    submit = r->recorder->target();
  }
  {
    ScopeTimer t(r->perf_submit_ms);  // (with the wait for the queue, shared with the emulator)
    r->queue->executeCommandLists(submit, r->fences[r->back_index].get());
  }
  r->submitted[r->back_index] = true;
  // (test: XMD_TEST_DEVICE_LOST_AT=<frame> - the device taken as lost there)
  static const uint64_t test_lost = uint64_t(EnvInt("XMD_TEST_DEVICE_LOST_AT", 0));
  if (backend::DeviceLost(r->device.get()) || (test_lost && r->frames == test_lost)) {
    ReportDeviceRemoved(r);
    return;
  }
  if (shot && capture && WaitForFrame(r, r->back_index, "frame capture")) {
    if (const auto* px = static_cast<const uint8_t*>(shot->map())) {
      std::vector<uint8_t> rgb(size_t(g_out_w) * g_out_h * 3);
      for (uint32_t y = 0; y < g_out_h; ++y) {
        const uint8_t* s = px + size_t(y) * shot_row * 4;
        uint8_t* d = rgb.data() + size_t(y) * g_out_w * 3;
        for (uint32_t x = 0; x < g_out_w; ++x) std::memcpy(d + size_t(x) * 3, s + size_t(x) * 4, 3);
      }
      shot->unmap();
      std::lock_guard lock(g_capture_mutex);
      g_capture = std::move(rgb), g_capture_w = g_out_w, g_capture_h = g_out_h;
    }
  } else if (shot && test_shot && WaitForFrame(r, r->back_index, "test frame shot")) {
    if (const auto* px = static_cast<const uint8_t*>(shot->map())) {
      if (FILE* f = std::fopen(shot_file.c_str(), "wb")) {  // (binary PPM, RGB)
        std::fprintf(f, "P6\n%u %u\n255\n", g_out_w, g_out_h);
        std::vector<uint8_t> row(size_t(g_out_w) * 3);
        for (uint32_t y = 0; y < g_out_h; ++y) {
          const uint8_t* s = px + size_t(y) * shot_row * 4;
          for (uint32_t x = 0; x < g_out_w; ++x) std::memcpy(&row[size_t(x) * 3], s + size_t(x) * 4, 3);
          std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        REXLOG_INFO("native renderer: test frame shot {}x{} -> {} (jitter {} {})", g_out_w, g_out_h, shot_file,
                    g_jitter[0], g_jitter[1]);
      }
      shot->unmap();
    }
  }
#if XMD_FSR2
  if (!r->fsr2_dumps.empty() && WaitForFrame(r, r->back_index, "FSR 2 dump")) {
    for (auto& d : r->fsr2_dumps)
      if (const auto* px = static_cast<const uint8_t*>(d.buffer->map())) {
        if (FILE* f = std::fopen(d.file.c_str(), "wb")) {
          for (uint32_t y = 0; y < d.h; ++y) std::fwrite(px + size_t(y) * d.row * d.bytes, d.bytes, d.w, f);
          std::fclose(f);
        }
        d.buffer->unmap();
      }
    r->fsr2_dumps.clear();
  }
#endif
#if XMD_DEBUG_SUITE
  if (shot && !test_shot && WaitForFrame(r, r->back_index, "dbg shot")) {
    if (const auto* px = static_cast<const uint8_t*>(shot->map())) {
      xmd::dbg::WriteShot(px, g_out_w, g_out_h, shot_row * 4);
      shot->unmap();
    }
  }
#endif
  // This frame, for the emulator's next swap (rex/external_frame.h).
  g_last_hidden = r->hidden_frame;
  const auto publish_start = std::chrono::steady_clock::now();
  if (!r->hidden_frame)  // (a frame not shown keeps the last one on screen)
    backend::PublishFrame(r->outputs[r->output_index], g_out_w, g_out_h, r->fences[r->back_index].get());
  g_present_publish_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - publish_start).count();
  r->frame_open = false;
  ++r->frames;
  {
    // native_jitter: the next frame's offset - Halton (2, 3), 8 frames, centred on the pixel.
    static bool jitter = REXCVAR_GET(native_jitter);
    if ((r->frames & 31) == 0) jitter = REXCVAR_GET(native_jitter);
    // (FSR 2 gathers the jittered frames: on with it, in the frames it runs)
#if XMD_FSR2
    const bool fsr2_jitter = g_fsr2 && r->fsr2_last_frame + 2 >= r->frames;
#else
    const bool fsr2_jitter = false;
#endif
    auto halton = [](uint32_t i, uint32_t b) {
      float f = 1.0f, v = 0.0f;
      for (; i; i /= b) v += (f /= float(b)) * float(i % b);
      return v;
    };
    const uint32_t i = uint32_t(r->frames % 8) + 1;
    g_jitter[0] = jitter || fsr2_jitter ? halton(i, 2) - 0.5f : 0.0f;
    g_jitter[1] = jitter || fsr2_jitter ? halton(i, 3) - 0.5f : 0.0f;
  }
  {
    // A 2D screen (no depth-tested draws into the wide / tall targets): the next frame's 2D in the 16:9 middle,
    // black bars around it. In a match only once it lasts (kFrames2d): a big move's camera cut has a lone frame
    // or two without the scene (2 draws), which got the bars and the next frame's scene squeezed into the
    // 16:9 band - a "cut in half" flash with a black line (16:10 report, 23 Oct).
    constexpr int kFrames2d = 4;
    static int run_2d = 0;
    const bool flat =
        (g_wide > 1.0f || g_tall > 1.0f) && r->frame_stats.drawn > 0 && r->frame_stats.wide_3d == 0;
    run_2d = flat ? run_2d + 1 : 0;
    g_frame_2d = flat && (run_2d >= kFrames2d || !g_match_scene.load());
    if (flat && run_2d == 1 && g_match_scene.load()) {
      g_flat_frame_seen = true;  // (test aid: XMD_TEST_FRAME_SHOT_ON_2D)
      static int logged = 0;
      if (logged++ < 20)
        REXLOG_INFO("native renderer: frame {} without the scene in a match ({} draws) - kept full size", r->frames,
                    r->frame_stats.drawn);
    }
  }
  {
    static const std::string drawlog = [] {
      char* e = nullptr;
      size_t n = 0;
      std::string s;
      if (_dupenv_s(&e, &n, "XMD_DRAWLOG") == 0 && e) {
        s = e;
        free(e);
      }
      return s;
    }();
    if (g_drawlog && r->frames >= g_drawlog_end) {
      std::fclose(g_drawlog);
      g_drawlog = nullptr;
    }
    std::error_code ec;
    if (!drawlog.empty() && r->frames % 15 == 0 && std::filesystem::exists(drawlog + ".go", ec)) {
      std::filesystem::remove(drawlog + ".go", ec);
      g_drawlog = std::fopen(drawlog.c_str(), "w");
      g_drawlog_frame = r->frames;
      static const int frames = [] {
        char* e = nullptr;
        size_t n = 0;
        int f = 1;
        if (_dupenv_s(&e, &n, "XMD_DRAWLOG_FRAMES") == 0 && e) {
          f = std::max(1, std::atoi(e));
          free(e);
        }
        return f;
      }();
      g_drawlog_end = r->frames + frames;
    }
  }
  // Debug: XMD_NATIVE_DUMP_TRIGGER=<file>: when the file appears, the next
  // frame is dumped (as XMD_NATIVE_DUMP_FRAME) and the file removed.
  static const std::filesystem::path trigger = [] {
    char* e = nullptr;
    size_t n = 0;
    std::filesystem::path t;
    if (_dupenv_s(&e, &n, "XMD_NATIVE_DUMP_TRIGGER") == 0 && e) {
      t = e;
      free(e);
    }
    return t;
  }();
  // (the dumped frame is over: its file whole on disk now, not when the next dump is asked for)
  if (g_dump && r->frames > g_dump_frame) {
    std::fclose(g_dump);
    g_dump = nullptr;
  }
  if (g_dump_requested.exchange(false)) {  // (RequestDrawDump: an F4 capture's draws.txt)
    g_dump_frame = r->frames + 1;
    g_dump_first = 0;
    g_dump_count = 100000;
    if (g_dump) {
      std::fclose(g_dump);
      g_dump = nullptr;
    }
  }
  if (!trigger.empty() && r->frames % 15 == 0) {
    std::error_code ec;
    if (std::filesystem::exists(trigger, ec)) {
      std::filesystem::remove(trigger, ec);
      g_dump_frame = r->frames + 1;
      g_dump_first = 0;
      g_dump_count = 100000;
      if (g_dump) {
        std::fclose(g_dump);
        g_dump = nullptr;
      }
    }
  }
  const auto title_start = std::chrono::steady_clock::now();
  UpdateTitle(r);
  backend::DrainDebugMessages(r->device.get());
  // A slow present, and which part of it (the problems log's hitch lines name "the present" only).
  const auto present_end = std::chrono::steady_clock::now();
  const double total = std::chrono::duration<double, std::milli>(present_end - present_start).count();
  if (total > 40.0) {
    char text[160];
    std::snprintf(text, sizeof(text), " present %.0f ms (handing the frame to the window %.0f, statistics %.0f)", total,
                  g_present_publish_ms, std::chrono::duration<double, std::milli>(present_end - title_start).count());
    xmd::problems::FrameCause(text, total);
  }
}

// DrawIndexedVertices / DrawVertices(device, primitive, start, count): from
// the current index buffer and streams (indexed) or the streams alone.
void DoDraw(uint32_t primitive, uint32_t start, uint32_t count, bool indexed) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  if (!BeginFrame(r)) return;
  Draw(r, primitive, 0, start, count, indexed);
}

// The microcode the GPU gets for a shader (the D3D library's bind at a draw:
// the object's container copy and the microcode's address). A vertex shader's
// vertex fetches are read from it (Declaration).
void DoShaderBound(uint32_t container, uint32_t microcode) {
  if (!g_memory || container < 0x10000) return;
  const uint32_t flags = Be32(Virtual(container));
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || !(flags & 1)) return;
  g_bound_vertex_container = container;
  g_bound_vertex_microcode = microcode;
}

// DrawVerticesUP(device, primitive, count, stride) returns the space (r3) the
// game then fills with the vertices; the draw is made at EndVertices.
constexpr uint32_t kUpDeviceBytes = 0x3400;
std::vector<uint8_t> g_up_device;

void DoDrawUP(uint32_t primitive, uint32_t count, uint32_t stride, uint32_t data) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  r->pending_up = {data != 0, primitive, count, stride, data};
  // The device as it is now. The draw itself is made at the next D3D call (the vertices are filled in after
  // this one returns) - by then the game may have set the next draw's state: the HUD's shapes were drawn
  // with their neighbours' colour mask and stencil state and never showed (found with tools/frame_diff.py).
  if (data) {
    g_up_device.resize(kUpDeviceBytes);
    std::memcpy(g_up_device.data(), Device(), kUpDeviceBytes);
  }
  static const bool log_up = EnvFlag("XMD_NATIVE_LOG_UP");
  if (log_up) {  // debug: VS c1 at DrawVerticesUP
    const uint8_t* c = Device() + 0x780 + 16;
    REXLOG_INFO("native renderer: UP begin vs {:08X} c1 {} {} {} {} (loaded {})",
                Be32(Device() + guest::kDeviceCurrentVertexShader), BeFloat(c), BeFloat(c + 4),
                BeFloat(c + 8), BeFloat(c + 12), g_constants_loaded[1]);
  }
}

// D3DDevice_BeginIndexedVertices(device, base vertex, vertex count, index count, index format (1: 16-bit),
// stride, void** indices, void** vertices): as DrawVerticesUP with indices of its own - the HUD's vector shapes (Scaleform's
// triangle lists) are all drawn this way.
void DoDrawIndexedUP(int32_t base_vertex, uint32_t vertices, uint32_t indices, uint32_t format, uint32_t stride,
                      uint32_t index_data, uint32_t data) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  if (format & 4) {
    static bool warned = false;
    if (!warned) REXLOG_WARN("native renderer: BeginIndexedVertices with 32-bit indices - not drawn");
    warned = true;
    return;
  }
  const bool ok = data != 0 && index_data != 0;
  static int logged = 0;
  if (logged++ < 6)
    REXLOG_WARN("native renderer: indexed UP draw prim {} vertices {} indices {} format {} stride {} -> {:08X} {:08X}",
                uint32_t(base_vertex), vertices, indices, format, stride, index_data, data);
  // (always a triangle list; r4 is what is added to every index)
  r->pending_up = {ok, 4, vertices, stride, data, index_data, indices, base_vertex};
  if (ok) {
    g_up_device.resize(kUpDeviceBytes);
    std::memcpy(g_up_device.data(), Device(), kUpDeviceBytes);
  }
}

void DoEndVertices() {
  std::lock_guard lock(g_mutex);
  Renderer* r = g_failed ? nullptr : g_r;
  if (!r || !r->pending_up.active) return;
  const PendingUP up = r->pending_up;
  r->pending_up.active = false;
  static const bool log_up = EnvFlag("XMD_NATIVE_LOG_UP");
  if (log_up) {  // debug: VS c1 at EndVertices
    const uint8_t* c = Device() + 0x780 + 16;
    REXLOG_INFO("native renderer: UP end c1 {} {} {} {} gpu {} {} {} {}", BeFloat(c), BeFloat(c + 4),
                BeFloat(c + 8), BeFloat(c + 12), g_gpu_constants[1][0], g_gpu_constants[1][1],
                g_gpu_constants[1][2], g_gpu_constants[1][3]);
  }
  if (!BeginFrame(r)) return;
  static const bool late_state = EnvFlag("XMD_NATIVE_UP_LATE_STATE");  // (as before: the state at the next call)
  const bool snapshot = !late_state && g_up_device.size() == kUpDeviceBytes && !g_replay_device;
  if (snapshot) {
    g_replay_device = g_up_device.data();
    g_const_dirty[0] = g_const_dirty[1] = ~0ull;
  }
  if (up.index_count) {
    static int logged = 0;
    if (logged++ < 6) REXLOG_WARN("native renderer: indexed UP end, snapshot {}", snapshot);
    if (snapshot) {
      g_replay_index_address = up.index_data, g_replay_index_size = up.index_count * 2;
      Draw(r, up.primitive, up.base_vertex, 0, up.index_count, true, &up);
    }
  } else {
    Draw(r, up.primitive, 0, 0, up.count, false, &up);
  }
  if (snapshot) {
    g_replay_device = nullptr;
    g_const_dirty[0] = g_const_dirty[1] = ~0ull;
  }
}

// D3DDevice_Clear(device, flags, target index (-1 all), rect left/top/right/
// bottom (r6-r9), colour* (r10, float4, NULL = black), z (f1), stencil (stack
// argument at caller r1 + 0x5C)): flags 0x0F colour targets, 0x10 depth,
// 0x20 stencil.
void DoClear(const HookArgs& a) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  if (!BeginFrame(r)) return;
  if (r->hidden_frame) return;  // (a frame not shown: SetHalfFrames)
  if (g_stopped) return;
  if (g_stop_at_resolve && g_resolved_this_frame) return;
  const Targets t = CurrentTargets(r);
  if (!BindTargets(r, t)) return;
  const uint32_t flags = a.r4;
  if (flags & 0xF) {
    float colour[4] = {0, 0, 0, 0};
    if (a.has_colour) {
      const uint8_t* c = a.colour;
      for (int i = 0; i < 4; ++i) colour[i] = BeFloat(c + 4 * i);
    }
    // (debug: XMD_NATIVE_DEBUG_CLEAR=1 - the scene is cleared to magenta, so a pixel no colour draw reached -
    // a depth pass's triangle its colour pass lost, a hole - shows and can be counted: toolsurst.py)
    if (static const bool magenta = EnvFlag("XMD_NATIVE_DEBUG_CLEAR"); magenta && t.color && t.color->width >= 1152)
      colour[0] = 1.0f, colour[1] = 0.0f, colour[2] = 1.0f, colour[3] = 1.0f;
    r->list->clearColor(0, plume::RenderColor(colour[0], colour[1], colour[2], colour[3]));
  }
  const bool clear_depth = flags & 0x10, clear_stencil = flags & 0x20;
  if ((clear_depth || clear_stencil) && t.depth) {
    const float z = std::clamp(a.z, 0.0f, 1.0f);
    const uint8_t stencil = uint8_t(a.stencil);
    r->list->clearDepthStencil(clear_depth, clear_stencil, z, stencil);
  }
}


// ---------------------------------------------------------------------------
// resolve write-back

// Ends the frame's command list, runs it and waits for the GPU, then opens a
// new list for the rest of the frame (for reading GPU results on the CPU).
bool FlushFrameAndWait(Renderer* r) {
  if (!r->frame_open) return true;
  r->list->end();
  plume::RenderCommandList* submit = r->list;
  if (r->recorder) {
    r->recorder->Finish();
    submit = r->recorder->target();
  }
  r->queue->executeCommandLists(submit, r->fences[r->back_index].get());
  r->submitted[r->back_index] = true;
  if (!WaitForFrame(r, r->back_index, "resolve write-back")) return false;
  r->frame_open = false;
  return BeginFrame(r);
}

// Some resolves are read by the game's own code, not only sampled: Superstar
// Threads bakes an attire's textures on the GPU (2048x1024 and its mips)
// and then builds the attire texture from them on the CPU. The native
// renderer's resolves live only on the host GPU, so the game read whatever
// was in guest memory. A resolve that isn't one of the recurring per-frame
// ones (its destination not resolved in the last 30 frames) is copied back
// to guest memory at once: the frame so far runs, the image is read back and
// written at the guest's size, tiled and byte-swapped as the destination
// texture says (32-bit colour, k_8_8_8_8 only).
// The resolves the game's own code reads (the rest are only sampled, and each
// write-back waits for the GPU): Superstar Threads' attire bake (2048x1024
// and its mips, from a 2080-wide target), a Created Superstar's portrait
// (736x1280, compressed into the save and its Community Creations preview)
// and a highlight reel's cover (128x128 from a 160x128 target). A wider rule
// (any power-of-two resolve not repeated every frame) also took entrances'
// 512x512 and 64x64 effect targets, resolved every other frame: hundreds of
// GPU waits a second, the frame rate in single digits as entrances started.
bool CpuReadsResolve(uint32_t dst_w, uint32_t dst_h, uint32_t src_w, uint32_t src_h) {
  if (src_w == 2080 && src_h == 1024) return true;                    // Threads bake
  if (dst_h > dst_w && src_h == 1280) return true;                    // portrait
  if (dst_w == 128 && dst_h == 128 && src_w == 160 && src_h == 128) return true;  // reel cover
  return false;
}

bool WriteBackResolve(Renderer* r, uint32_t base, const ResolvedTexture& dst, const uint32_t fetch[6],
                      bool swap_rb, uint32_t src_w, uint32_t src_h) {
  if (dst.format != RenderFormat::R8G8B8A8_TYPELESS || (fetch[1] & 0x3F) != 6) return false;
  if (!CpuReadsResolve(dst.width, dst.height, src_w, src_h)) return false;
  const uint32_t guest_w = dst.width, guest_h = dst.height;
  const uint32_t host_w = dst.host_w ? dst.host_w : guest_w * dst.scale;
  const uint32_t host_h = dst.host_h ? dst.host_h : guest_h * dst.scale;
  const uint32_t row_texels = (host_w + 63) & ~63u;  // (256-byte rows)
  std::shared_ptr<plume::RenderBuffer> buffer =
      r->device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(row_texels) * host_h * 4));
  if (!buffer) return false;
  auto& layout = const_cast<ResolvedTexture&>(dst).layout;
  Transition(r, dst.resource.get(), layout, RenderTextureLayout::COPY_SOURCE);
  r->list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(
                                 buffer.get(), RenderFormat::R8G8B8A8_UNORM, host_w, host_h, 1, row_texels),
                             plume::RenderTextureCopyLocation::Subresource(dst.resource.get()));
  Transition(r, dst.resource.get(), layout, RenderTextureLayout::SHADER_READ);
  const auto wait_start = std::chrono::steady_clock::now();
  if (!FlushFrameAndWait(r)) return false;
  g_write_back_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
  ++g_write_backs;
  g_write_back_w = guest_w;
  g_write_back_h = guest_h;
  const auto* src = static_cast<const uint8_t*>(buffer->map());
  if (!src) return false;
  const bool tiled = fetch[0] >> 31;
  const uint32_t pitch = std::max(((fetch[0] >> 22) & 0x1FFu) * 32, guest_w);
  const uint32_t endian = (fetch[1] >> 6) & 3;
  const uint32_t mask = endian == 1 ? 1 : endian == 2 ? 3 : endian == 3 ? 2 : 0;
  auto* guest = const_cast<uint8_t*>(Physical(base));
  const uint64_t limit = 0x20000000ull - base;
  const uint32_t sx = std::max(1u, host_w / guest_w), sy = std::max(1u, host_h / guest_h);
  for (uint32_t y = 0; y < guest_h; ++y) {
    for (uint32_t x = 0; x < guest_w; ++x) {
      uint32_t sum[4] = {};  // (the host pixels of this guest pixel, averaged)
      for (uint32_t j = 0; j < sy; ++j) {
        const uint8_t* row = src + (size_t(std::min(y * sy + j, host_h - 1)) * row_texels) * 4;
        for (uint32_t i = 0; i < sx; ++i) {
          const uint8_t* q = row + size_t(std::min(x * sx + i, host_w - 1)) * 4;
          for (int c = 0; c < 4; ++c) sum[c] += q[c];
        }
      }
      const uint32_t n = sx * sy;
      uint8_t px[4];
      for (int c = 0; c < 4; ++c) px[c] = uint8_t((sum[c] + n / 2) / n);
      if (swap_rb) std::swap(px[0], px[2]);
      const int64_t off = tiled ? texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, 2)
                                : (int64_t(y) * pitch + x) * 4;
      if (off < 0 || uint64_t(off) + 4 > limit) continue;
      for (uint32_t c = 0; c < 4; ++c) guest[off + (c ^ mask)] = px[c];
    }
  }
  buffer->unmap();
  // (the span written: tiled textures are laid out in 32 x 32 tiles)
  const uint64_t rows = tiled ? (uint64_t(guest_h) + 31) & ~31ull : guest_h;
  textures::GuestWritten(base, uint32_t(std::min<uint64_t>(uint64_t(pitch) * rows * 4, limit)));
  static uint32_t logged = 0;
  if (logged++ < 32) {
    REXLOG_INFO("native renderer: resolve {:08X} ({}x{}) written back to guest memory", base, guest_w, guest_h);
  }
  return true;
}

// D3DDevice_Resolve(device, flags, ..., destination texture in r8): flags 0-3
// colour target n, 4 depth. Copies the current target into the texture the
// game will sample (always the whole surface from (0, 0) in this game).
void GuestWritten(uint32_t address, uint32_t size) {
  std::lock_guard lock(g_mutex);
  if (!g_memory || !size) return;
  textures::GuestWritten(g_memory->GetPhysicalAddress(address), size);
}

// A resolve a plain copy can't do, drawn from the plain copy (shaders/resolve_copy.ps) into a texture of its
// own that takes the copy's place as the texture at `base`:
//  - part of a depth target into a texture of that part's size (the game's shadow map: 2048 x 1024 of
//    2080 x 2080 - its shaders read it with coordinates of the destination's size; depth copies are whole);
//  - a colour resolve with an exponent bias (every value times 2^bias: the HDR scene at 1/32 for the bloom).
// `whole`, the plain copy, has just been registered at `base`. Big texture tables only (Direct3D 12, Vulkan).
bool ResolveThrough(Renderer* r, uint32_t base, const uint32_t object_fetch[6], uint32_t width, uint32_t height,
                    uint32_t left, uint32_t top, float host_scale_x, float host_scale_y, RenderFormat format,
                    const float multiply[4], const float add[4], RenderFormat view_format, RenderFormat view_gamma,
                    uint32_t components, bool swap_rb) {
  if (backend::CompactTables() || SlotTables()) return false;
  const plume::RenderPipeline* pso = PresentPipeline(r, "resolve_copy.ps", format);
  if (!pso) return false;
  uint32_t fetch[6];
  std::memcpy(fetch, object_fetch, sizeof(fetch));
  fetch[1] = (fetch[1] & 0xFFFu) | base;
  fetch[3] = (fetch[3] & ~(0xFFFu << 1)) | (0x688u << 1);  // (its components as they are)
  const textures::Context ctx = TextureContext(r);
  const uint32_t srv = textures::Texture(ctx, fetch, 0);
  if (srv == UINT32_MAX) return false;
  const uint32_t host_w = std::max(1u, uint32_t(width * host_scale_x)), host_h = std::max(1u, uint32_t(height * host_scale_y));
  Renderer::DepthCrop& crop =
      r->depth_crops[(uint64_t(base) << 32) ^ (uint64_t(host_w) << 16) ^ host_h ^ (uint64_t(format) << 56)];
  if (!crop.texture) {
    plume::RenderTextureDesc d =
        plume::RenderTextureDesc::Texture2D(host_w, host_h, 1, format, plume::RenderTextureFlag::RENDER_TARGET);
    d.committed = true;
    crop.texture = r->device->createTexture(d);
    if (!crop.texture) return false;
    crop.texture->setName(fmt::format("resolve through a shader {:08X} {}x{}", base, width, height));
    const plume::RenderTexture* color = crop.texture.get();
    crop.framebuffer = r->device->createFramebuffer(plume::RenderFramebufferDesc(&color, 1));
    crop.layout = RenderTextureLayout::UNKNOWN;
  }
  auto [cpu, gpu] = Allocate(r, 256);
  if (!cpu) return false;
  const uint32_t indices[2] = {srv, 0};
  const float values[4] = {left * host_scale_x, top * host_scale_y, 1.0f, 1.0f};
  std::memset(cpu, 0, 64);
  std::memcpy(cpu, indices, 8);
  std::memcpy(cpu + 8, values, 16);
  std::memcpy(cpu + 32, multiply, 16);
  std::memcpy(cpu + 48, add, 16);
  auto* list = r->list;
  Transition(r, crop.texture.get(), crop.layout, RenderTextureLayout::COLOR_WRITE);
  list->setFramebuffer(crop.framebuffer.get());
  list->setGraphicsPipelineLayout(r->layout.get());
  BindTables(r, list);
  const RenderBufferReference constants[4] = {{}, {}, {}, gpu};
  BindConstants(r, constants);
  list->setPipeline(pso);
  list->setViewports(plume::RenderViewport(0, 0, float(host_w), float(host_h), 0, 1));
  list->setScissors(plume::RenderRect(0, 0, int32_t(host_w), int32_t(host_h)));
  list->drawInstanced(3, 1, 0, 0);
  Transition(r, crop.texture.get(), crop.layout, RenderTextureLayout::SHADER_READ);
  r->list_state = {};
  textures::RegisterResolved(base, crop.texture.get(), view_format, view_gamma, components, swap_rb, width * height * 4);
  return true;
}

void DoResolve(const HookArgs& a) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r || !a.r8) return;
  if (!BeginFrame(r)) return;
  if (g_stopped) return;
  if (g_stop_after && ++g_resolve_count == g_stop_after) {
    g_stopped = true;  // (after this one)
    g_stop_texture = a.r8;
  }
  const Targets t = CurrentTargets(r);
  const bool depth = (a.r4 & 7) == 4;
  if (t.color && t.color == r->main_target && !depth) {
    if (g_stop_at_resolve && g_resolved_this_frame) return;
    g_resolved_this_frame = true;
  }
  const uint8_t* obj = Virtual(a.r8);
  if ((Be32(obj) & 0xF) != 3) return;  // not a texture
  uint32_t fetch[6];
  for (int k = 0; k < 6; ++k) fetch[k] = Be32(obj + 0x1C + 4 * k);
  // The object's fetch constant holds a guest virtual address (0xE0000000+
  // view); the fetch constants draws use hold the physical address.
  const uint32_t base = g_memory->GetPhysicalAddress(fetch[1] & 0xFFFFF000u);
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  plume::RenderTexture* src = nullptr;
  RenderTextureLayout* src_layout = nullptr;
  RenderFormat family = RenderFormat::UNKNOWN;
  uint32_t src_w = 0, src_h = 0, scale = g_scale, src_host_w = 0, src_host_h = 0;
  if (depth) {
    if (!t.depth) return;
    src = t.depth->resource.get();
    src_layout = &t.depth->layout;
    family = backend::DepthFormat();
    src_w = t.depth->width;
    src_h = t.depth->height;
    scale = t.depth->scale;
    src_host_w = t.depth->host_w;
    src_host_h = t.depth->host_h;
  } else {
    if (!t.color) return;
    src = t.color->resource.get();
    src_layout = &t.color->layout;
    family = t.color->resource_format;
    src_w = t.color->width;
    src_h = t.color->height;
    scale = t.color->scale;
    src_host_w = t.color->host_w;
    src_host_h = t.color->host_h;
  }
  {
    static std::unordered_map<uint64_t, bool> logged;
    static const bool log_all = EnvFlag("XMD_LOG_RESOLVES");  // (debug: every resolve)
    if (log_all || logged.size() < 256 &&
        logged.emplace((uint64_t(base) << 20) ^ (uint64_t(width) << 8) ^ height ^ (uint64_t(family) << 50), true).second) {
      REXLOG_INFO("native renderer: resolve flags {} -> texture {:08X} base {:08X} {}x{} fetch {:08X} {:08X} {:08X} from {}x{} format {}",
                  a.r4, a.r8, base, width, height, fetch[0], fetch[1], fetch[2], src_w,
                  src_h, int(family));
    }
  }
  // (1: the first resolve that also clears its target - in gameplay the scene, saved before the post passes)
  if (g_stop_base && (g_stop_base == 1 ? (a.r4 & 0x100) != 0 : base == g_stop_base) && !g_stopped) {
    g_stopped = true;  // (after this one)
    g_stop_texture = a.r8;
  }
  if (g_dump && r->frames == g_dump_frame) {
    std::fprintf(g_dump, "=== resolve flags %X -> texture %08X base %08X %ux%u fetch %08X %08X %08X %08X from %ux%u (%s) | surface %08X color %08X depth %08X\n",
                 a.r4, a.r8, base, width, height, fetch[0], fetch[1], fetch[2], fetch[3], src_w, src_h,
                 depth ? "depth" : "colour", Reg(RB_SURFACE_INFO), Reg(RB_COLOR_INFO), Reg(RB_DEPTH_INFO));
    if (a.has_rect) {
      const uint8_t* rect = a.rect;
      std::fprintf(g_dump, "  source rect %u %u %u %u\n", Be32(rect), Be32(rect + 4), Be32(rect + 8), Be32(rect + 12));
    }
  }
  // Debug: XMD_NATIVE_DEBUG_FILL=1 - at the resolve the frame is stopped at (XMD_NATIVE_STOP_AFTER_RESOLVE), the
  // colour target is filled with magenta first: magenta on screen means the resolve and its showing work, and
  // what was there before was what the draws left.
  if (!depth && g_stopped && g_stop_texture == a.r8 && EnvFlag("XMD_NATIVE_DEBUG_FILL") && BindTargets(r, t)) {
    r->list->clearColor(0, plume::RenderColor(1.0f, 0.0f, 1.0f, 1.0f));
    r->list_state = {};
  }
  // Depth can only be copied whole, so its copy has the target's size.
  const uint32_t dst_w = depth ? src_w : std::min(width, src_w);
  const uint32_t dst_h = depth ? src_h : std::min(height, src_h);
  // (the host width: the target's share of it)
  const uint32_t dst_host_w =
      depth || dst_w == src_w ? src_host_w : std::max(1u, uint32_t(uint64_t(src_host_w) * dst_w / src_w));
  const uint32_t dst_host_h =
      depth || dst_h == src_h ? src_host_h : std::max(1u, uint32_t(uint64_t(src_host_h) * dst_h / src_h));
  ResolvedTexture& dst = r->resolved[base];
  if (!dst.resource || dst.width != dst_w || dst.height != dst_h || dst.format != family ||
      dst.scale != scale || dst.host_w != dst_host_w || dst.host_h != dst_host_h) {
    auto shape = [base](uint32_t w, uint32_t h, RenderFormat format, uint32_t sc, uint32_t hw, uint32_t hh) {
      const uint32_t parts[7] = {base, w, h, uint32_t(format), sc, hw, hh};
      return XXH3_64bits(parts, sizeof(parts));
    };
    if (dst.resource) {  // (kept for when this shape comes back)
      const uint64_t old = shape(dst.width, dst.height, dst.format, dst.scale, dst.host_w, dst.host_h);
      r->resolved_spares[old].push_back(std::move(dst));
    }
    dst = {};
    auto& spares = r->resolved_spares[shape(dst_w, dst_h, family, scale, dst_host_w, dst_host_h)];
    if (!spares.empty()) {
      dst = std::move(spares.back());
      spares.pop_back();
    } else {
      plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
          dst_host_w, dst_host_h, 1, family,
          depth && (backend::ActiveApi() == backend::Api::kVulkan || SlotTables()) ? plume::RenderTextureFlag::DEPTH_TARGET
                                                                  : plume::RenderTextureFlag::NONE);
      d.committed = true;
      dst.resource = r->device->createTexture(d);
      if (!dst.resource) {
        REXLOG_ERROR("native renderer: could not create a resolve texture");
        r->resolved.erase(base);
        return;
      }
      dst.resource->setName(fmt::format("resolve {:08X} {}x{}", base, dst_w, dst_h));
      dst.width = dst_w;
      dst.height = dst_h;
      dst.format = family;
      dst.scale = scale;
      dst.host_w = dst_host_w;
      dst.host_h = dst_host_h;
    }
  }
  auto* list = r->list;
#if XMD_FSR2
  if (g_fsr2 && !depth && t.depth && g_match_scene.load() && SceneTarget(src_w, src_h) &&
      t.depth->host_w == src_host_w && t.depth->host_h == src_host_h) {
    // FSR 2's depth: the scene's, copied as the scene is resolved (its colour - the front buffer - comes later).
    if (!r->fsr2_depth || r->fsr2_depth_w != src_host_w || r->fsr2_depth_h != src_host_h) {
      if (r->fsr2_depth) Retire(r, std::move(r->fsr2_depth));
      plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
          src_host_w, src_host_h, 1, backend::DepthFormat(),
          backend::ActiveApi() == backend::Api::kVulkan ? plume::RenderTextureFlag::DEPTH_TARGET
                                                        : plume::RenderTextureFlag::NONE);
      d.committed = true;
      r->fsr2_depth = r->device->createTexture(d);
      r->fsr2_depth_layout = RenderTextureLayout::UNKNOWN;
      r->fsr2_depth_w = src_host_w, r->fsr2_depth_h = src_host_h;
      if (r->fsr2_depth) r->fsr2_depth->setName("FSR 2 depth");
    }
    DrawFsr2Masks(r, t.depth, src_host_w, src_host_h);
    DrawFsr2SkinMotion(r, t.depth, src_host_w, src_host_h);
    if (r->fsr2_depth) {
      const RenderTextureLayout before = t.depth->layout;
      Transition(r, t.depth->resource.get(), t.depth->layout, RenderTextureLayout::COPY_SOURCE);
      Transition(r, r->fsr2_depth.get(), r->fsr2_depth_layout, RenderTextureLayout::COPY_DEST);
      list->copyTexture(r->fsr2_depth.get(), t.depth->resource.get());
      Transition(r, r->fsr2_depth.get(), r->fsr2_depth_layout, RenderTextureLayout::SHADER_READ);
      if (before != RenderTextureLayout::UNKNOWN) Transition(r, t.depth->resource.get(), t.depth->layout, before);
      r->fsr2_depth_frame = r->frames;
    }
  }
#endif
  const RenderTextureLayout src_before = *src_layout;
  Transition(r, src, *src_layout, RenderTextureLayout::COPY_SOURCE);
  Transition(r, dst.resource.get(), dst.layout, RenderTextureLayout::COPY_DEST);
  if (depth) {
    list->copyTexture(dst.resource.get(), src);
  } else {
    const plume::RenderBox box(0, 0, int32_t(dst_host_w), int32_t(dst_host_h));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dst.resource.get()),
                            plume::RenderTextureCopyLocation::Subresource(src), 0, 0, 0, &box);
  }
  Transition(r, dst.resource.get(), dst.layout, RenderTextureLayout::SHADER_READ);
  if (src_before != RenderTextureLayout::UNKNOWN) Transition(r, src, *src_layout, src_before);
  if (depth) {
    textures::RegisterResolved(base, dst.resource.get(), backend::DepthFormat(),
                               RenderFormat::UNKNOWN, 1, false, dst_w * dst_h * 4);
    // A part of the target into a smaller texture: cropped (the source rectangle is r5: left, top, right, bottom).
    if ((width < src_w || height < src_h) && src_w && src_h) {
      uint32_t left = 0, top = 0;
      if (a.has_rect) {
        const uint8_t* rect = a.rect;
        left = std::min(Be32(rect), src_w - 1), top = std::min(Be32(rect + 4), src_h - 1);
      }
      static int logged = 0;
      const float one_channel[4] = {1, 0, 0, 0}, opaque[4] = {0, 0, 0, 1};
      const bool cropped =
          ResolveThrough(r, base, fetch, std::min(width, src_w - left), std::min(height, src_h - top), left, top,
                         float(src_host_w) / float(src_w), float(src_host_h) / float(src_h), RenderFormat::R32_FLOAT,
                         one_channel, opaque, RenderFormat::R32_FLOAT, RenderFormat::UNKNOWN, 1, false);
      if (logged++ < 4)
        REXLOG_INFO("native renderer: depth resolve of {}x{} at {},{} of a {}x{} target: {}", width, height, left, top,
                    src_w, src_h, cropped ? "cropped" : "NOT cropped (the whole copy is used)");
    }
  } else {
    // Resolves to ARGB textures store red and blue swapped (copy_dest_swap),
    // undone by those textures' ZYXW fetch swizzle; the copy here is RGBA,
    // so the view swaps them back. (The mirror doesn't hold the resolve's
    // RB_COPY_DEST_INFO: the destination's swizzle tells.)
    const bool swap_rb = t.color->components == 4 && ((fetch[3] >> 1) & 7) == 2;
    dst.swap_rb = swap_rb;
    dst.view_format = t.color->view_format;
    textures::RegisterResolved(base, dst.resource.get(), t.color->view_format,
                               t.color->view_gamma_format, t.color->components, swap_rb,
                               dst_w * dst_h * 4);
    // A float target copied into an 8-bit texture (k_8: the engine's shadow mask; k_8_8_8_8: the finished scene for
    // its anti-aliasing pass): the console stores 0..1 there. Left as floats, mask values over 1 made the shadow
    // pass brighten every silhouette (white outlines, 2026-11-17).
    if (const uint32_t dest_format = fetch[1] & 0x3F;
        (dest_format == 2 || dest_format == 6) && t.color->view_format == RenderFormat::R16G16B16A16_FLOAT &&
        !(int32_t(a.r4) >> 26)) {
      const float one[4] = {1, 1, 1, 1}, zero[4] = {0, 0, 0, 0};
      const bool held = ResolveThrough(r, base, fetch, dst_w, dst_h, 0, 0, float(dst_host_w) / float(dst_w),
                                       float(dst_host_h) / float(dst_h), RenderFormat::R8G8B8A8_UNORM, one, zero,
                                       RenderFormat::R8G8B8A8_UNORM, RenderFormat::UNKNOWN, 4, false);
      static int logged = 0;
      if (logged++ < 3)
        REXLOG_INFO("native renderer: float target copied to an 8-bit texture (format {}, {}x{}): {}", dest_format, dst_w,
                    dst_h, held ? "held to 0..1" : "NOT held");
    }
    // An exponent bias (the flags' top six bits, signed): the copy's values times 2^bias. Float targets only (the
    // game's HDR scene, resolved at 1/32 for its bloom: left alone, the bloom was 32 times too bright - a white
    // screen in the levels).
    if (const int bias = int32_t(a.r4) >> 26; bias && t.color->view_format == RenderFormat::R16G16B16A16_FLOAT) {
      const float f = std::ldexp(1.0f, bias);
      const float multiply[4] = {f, f, f, f}, add[4] = {0, 0, 0, 0};
      const bool scaled = ResolveThrough(r, base, fetch, dst_w, dst_h, 0, 0, float(dst_host_w) / float(dst_w),
                                         float(dst_host_h) / float(dst_h), RenderFormat::R16G16B16A16_FLOAT, multiply,
                                         add, t.color->view_format, t.color->view_gamma_format, t.color->components,
                                         swap_rb);
      static int logged = 0;
      if (logged++ < 3)
        REXLOG_INFO("native renderer: resolve with exponent bias {} ({}x{}): {}", bias, dst_w, dst_h,
                    scaled ? "scaled" : "NOT scaled");
    }
    if (dst_w >= kWidth && dst_h >= kHeight) r->present_source = base;
    static const bool write_back = REXCVAR_GET(native_resolve_write_back);
    // Per-frame resolves (the scene, post-processing chains: resolved in
    // every frame) are left alone; bursts such as the attire bake - several
    // resolves into the same texture, one per attire part - are all copied.
    if (dst.last_frame != r->frames) {
      dst.streak = dst.last_frame != ~0ull && dst.last_frame + 1 == r->frames ? dst.streak + 1 : 0;
      dst.last_frame = r->frames;
    }
    const bool recurring = dst.streak >= 30 || t.color == r->main_target;
    if (write_back && !recurring && WriteBackResolve(r, base, dst, fetch, swap_rb, src_w, src_h)) {
      textures::ForgetResolved(base);  // (sampled from memory from now on)
    }
  }
  // D3DRESOLVE_CLEARRENDERTARGET (0x100) / CLEARDEPTHSTENCIL (0x200): the target is cleared once copied (colour*
  // r10, z f1, stencil on the stack - as Clear's). The engine (Unreal 3) saves its scene with one and reads the
  // cleared target back as "no shadow here": without the clear the scene's own red channel was the shadow mask,
  // and the first level's ground came out black.
  if (static const bool off = EnvFlag("XMD_NATIVE_NO_RESOLVE_CLEAR"); !off && (a.r4 & 0x300) && BindTargets(r, t)) {
    if (a.r4 & 0x100) {
      float colour[4] = {0, 0, 0, 0};
      if (a.has_colour) {
        const uint8_t* c = a.colour;
        for (int i = 0; i < 4; ++i) colour[i] = BeFloat(c + 4 * i);
      }
      r->list->clearColor(0, plume::RenderColor(colour[0], colour[1], colour[2], colour[3]));
      static int logged = 0;
      if (logged++ < 6)
        REXLOG_INFO("native renderer: resolve flags {:X} clears the {}x{} target to {} {} {} {}", a.r4, t.color->width,
                    t.color->height, colour[0], colour[1], colour[2], colour[3]);
    }
    if ((a.r4 & 0x200) && t.depth) {
      const float z = std::clamp(a.z, 0.0f, 1.0f);
      const uint8_t stencil = uint8_t(a.stencil);
      r->list->clearDepthStencil(true, true, z, stencil);
    }
  }
  ++r->frame_stats.resolves;
}

// D3DDevice_SetRenderTarget(device, index, surface): the surface object has
// the EDRAM registers (+0x18 RB_SURFACE_INFO, +0x1C RB_COLOR_INFO) and the
// size (+0x24: width - 1 in bits 18-30, height - 1 in bits 3-15).
void DoSetRenderTarget(uint32_t index, uint32_t surface) {
  if (!g_memory || index != 0 || !surface) return;
  std::lock_guard lock(g_mutex);
  const uint8_t* obj = Virtual(surface);
  if ((Be32(obj) & 0xF) != 4) return;  // not a surface
  const uint32_t si = Be32(obj + 0x18), ci = Be32(obj + 0x1C), dims = Be32(obj + 0x24);
  const uint32_t w = ((dims >> 18) & 0x1FFF) + 1, h = ((dims >> 3) & 0x1FFF) + 1;
  if (w > 4096 || h > 4096) return;
  const uint32_t key = ((si & 0x3FFF) << 12) | (ci & 0xFFF);
  auto [it, added] = g_surface_sizes.try_emplace(key, w, h);
  if (added) {
    REXLOG_INFO("native renderer: surface {:08X}: {}x{} pitch {} tile {} format {}", surface, w,
                h, si & 0x3FFF, ci & 0xFFF, (ci >> 16) & 0xF);
  }
  it->second = {w, h};
}

// (debug: where in the device a stream's buffer object is kept - after SetStreamSource)
void DebugFindStream(uint32_t device, uint32_t stream, uint32_t buffer, uint32_t offset) {
  static std::atomic<int> logged{0};
  if (!g_memory || buffer < 0x10000 || logged >= 16) return;
  ++logged;
  const uint8_t* d = Virtual(device);
  std::string at;
  for (uint32_t i = 0; i < 0x4000; i += 4)
    if (Be32(d + i) == buffer) at += fmt::format(" +{:X}", i);
  const uint8_t* fetch = d + guest::RegisterOffset(0x4800) + (95 - (stream & 15)) * 8;
  const uint8_t* o = Virtual(buffer);
  REXLOG_INFO("native renderer: stream {} = buffer {:08X} (words {:08X} {:08X}) offset {}: in the device at{}; its fetch "
              "constant in the mirror {:08X} {:08X}", stream, buffer, Be32(o + 0x18), Be32(o + 0x1C), offset, at, Be32(fetch),
              Be32(fetch + 4));
}

void DoSetStreamSource(uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride) {
  if (stream >= 16) return;  // (15 is kept for IndexedRows only: input slot 15 is the renderer's zero stream)
  std::lock_guard lock(g_mutex);
  if (g_fetch_streams) return;  // (the fetch constants tell: StreamsFromFetch)
  g_streams[stream] = {buffer, offset, stride};
}

void DoSetShaderConstants(bool pixel, uint32_t start, uint32_t count) {
  // (each draw takes all constants from the mirror; this marks the blocks)
  if (!count) return;
  const uint32_t a = std::min(start, 255u) / 4, b = std::min(start + count - 1, 255u) / 4;
  for (uint32_t block = a; block <= b; ++block) g_const_dirty[pixel ? 1 : 0] |= 1ull << (63 - block);
}

// D3D writes the dirty mirror ranges to the GPU: take them from the mirror.
void DoFlushShaderConstants(uint64_t mask, uint32_t reg) {
  // Each draw takes the constants from the mirror (GpuConstants), which D3D
  // has just updated; this tells it which stage changed.
  if (!mask) return;
  g_const_dirty[reg >= 0x4400 ? 1 : 0] |= mask;
  // (the mirror's blocks go over whatever was loaded there: bit 63 = constants 0-3 of the stage)
  if (reg == 0x4000 || reg == 0x4400) {
    const uint32_t stage_first = reg == 0x4400 ? 256 : 0;
    for (uint64_t m = mask; m;) {
      const uint32_t block = uint32_t(__builtin_clzll(m));
      m &= ~(1ull << (63 - block));
      for (uint32_t k = 0; k < 4; ++k) {
        bool& loaded = g_constants_loaded[stage_first + block * 4 + k];
        if (loaded) loaded = false, ++g_literal_version[stage_first ? 1 : 0];
      }
    }
  }
  static int logged = 0;
  if (logged < 4 && reg != 0x4000 && reg != 0x4400) {
    ++logged;
    REXLOG_WARN("native renderer: constant flush at register {:04X} (mask {:016X})", reg, mask);
  }
}

// 82925D78(device, shader, base): the shader object's table (offset at +20)
// lists {u16 first float4, u16 dword count, u32 offset}; each block is loaded
// from base + offset into the constant file (LOAD_ALU_CONSTANT).
void DoLoadShaderConstants(uint32_t shader_object, uint32_t base) {
  if (!g_memory || !shader_object) return;
  std::lock_guard lock(g_mutex);
  const uint8_t* obj = Virtual(shader_object);
  const uint32_t table_offset = Be32(obj + 20);
  if (!table_offset) return;
  const uint8_t* table = obj + table_offset + 20;
  const uint32_t table_size = Be32(obj + table_offset + 16);
  if (table_size > 0x10000) {  // (not a shader object's table: 8 bytes an entry, a few dozen at most)
    static std::atomic<int> bad{0};
    if (bad++ < 4)
      REXLOG_WARN("native renderer: shader object {:08X} has a constant table of {} bytes - not loaded", shader_object,
                  table_size);
    return;
  }
  const uint8_t* end = table + table_size;
  {
    // Log each distinct table once (which constant ranges come from memory). Never destroyed: the runtime's disc
    // error path left by exit() while this thread drew on - a destroyed map's read of 0 (2.1.9 report edcc87).
    static auto& seen = *new std::unordered_map<uint64_t, int>;
    const uint64_t key = end > table ? XXH3_64bits(table, size_t(end - table)) : 0;
    if (seen.size() < 64 && seen.emplace(key, 0).second) {
      std::string entries;
      for (const uint8_t* e = table; e < end && entries.size() < 200; e += 8)
        entries += fmt::format(" [{}+{}]", Be16(e), Be16(e + 2));
      REXLOG_INFO("native renderer: constant table{} (object {:08X}, base {:08X})", entries,
                  shader_object, base);
    }
  }
  for (const uint8_t* e = table; e < end;) {
    const uint32_t first = Be16(e), dwords = Be16(e + 2);
    if (!dwords) break;
    const uint32_t offset = Be32(e + 4);
    e += 8;
    const uint8_t* src = ObjectData(base + offset);
    for (uint32_t d = 0; d < dwords; ++d) {
      const uint32_t index = first + d / 4;
      if (index >= 512) break;
      const float value = BeFloat(src + 4 * d);
      float& held = g_gpu_constants[index][d % 4];
      if (!g_constants_loaded[index] || std::memcmp(&held, &value, 4)) ++g_literal_version[index >= 256 ? 1 : 0];
      held = value;
      g_constants_loaded[index] = true;
    }
  }
}

// The D3D library loads a shader that isn't bound to a vertex format: it writes the microcode into the command
// stream with the vertex fetches filled in from a declaration. (device, 1, shader object r5, ..., r7): which of
// r5 / r7 / r8 is the declaration is checked by its type.
void DoUnboundShader(uint32_t a, uint32_t b, uint32_t c) {
  if (!g_memory) return;
  std::lock_guard lock(g_mutex);
  uint32_t shader = 0, decl = 0;
  for (uint32_t object : {a, b, c}) {
    if (object < 0x10000) continue;
    const uint32_t type = Be32(Virtual(object)) & 0xF;
    if (type == 6 && !shader) shader = object;
    if (type == 5 && !decl) decl = object;
  }
  static int logged = 0;
  if (logged < 8) {
    ++logged;
    std::string text;
    if (decl) {
      const uint8_t* p = Virtual(decl);
      for (uint32_t i = 0; i < 0x70; i += 4) text += fmt::format(" {:08X}", Be32(p + i));
    }
    REXLOG_INFO("native renderer: unbound shader load: arguments {:08X} {:08X} {:08X} -> shader {:08X} declaration {:08X}:{}",
                a, b, c, shader, decl, text);
  }
  if (shader && decl) g_unbound_declarations[shader] = decl;
}

// ---------------------------------------------------------------------------
// Recorded command buffers. The game records a level's static draws (its depth pass with the visibility
// surveys, its colour pass) into D3D command buffers when the level loads - on a second device object - and
// plays them every frame (D3DDevice_RunCommandBuffer): the GPU runs the recorded packets, no draw call is made.
// Here the packets are read instead: register writes go into a copy of the device's register mirror, shader
// loads are matched to the shader objects by their microcode's address (OnShaderMicrocode: every bind the D3D
// library makes, on either device), draws go through Draw with that copy as the device.
//
// The object: +0x98 the packets (a view of physical memory), +0x88 where they end (the fix-up table follows).
// Packets (Xenos PM4): type 0 = register writes, type 3 = opcodes - IM_LOAD 0x27 (shader microcode),
// LOAD_ALU_CONSTANT 0x2F (constants from memory), SET_CONSTANT 0x2D, DRAW_INDX 0x22; the bin masks and
// visibility surveys (0x60-0x63, 0x5A, 0x46) are passed over: every draw is made.

struct MicrocodeOwner {
  uint32_t container = 0;  // the shader object's container copy
  uint32_t microcode = 0;  // as the library names it (a view of physical memory)
};
std::map<uint32_t, MicrocodeOwner> g_microcode_owners;  // by physical address
std::mutex g_microcode_mutex;
std::atomic<uint32_t> g_microcode_epoch{1};  // (counted when g_microcode_owners changes)



void OnShaderMicrocode(uint32_t container, uint32_t microcode) {
  if (container < 0x10000 || microcode < 0x10000) return;
  if (!g_memory || !GuestReadable(container, 0x24)) return;
  // (the GPU is given the code alone - it follows the shader's constant values in the microcode's memory)
  const uint8_t* c = Virtual(container);
  uint32_t code_at = 0;
  if ((Be32(c) & 0xFFFFFF00u) != 0x102A1100u) return;
  if (const uint32_t vsize = Be32(c + 4), shader_at = Be32(c + 0x18); vsize <= 0x10000 && shader_at + 4 <= vsize)
    code_at = Be32(c + shader_at);
  std::lock_guard lock(g_microcode_mutex);
  if (static int logged[2] = {}; logged[Be32(c) & 1]++ < 4)
    REXLOG_INFO("native renderer: microcode {:08X} (physical {:08X}, code at +{:X}) is container {:08X}'s ({:08X})", microcode,
                PhysicalOf(microcode), code_at, container, Be32(c));
  auto set = [&](uint32_t at) {
    MicrocodeOwner& owner = g_microcode_owners[at];
    if (owner.container == container && owner.microcode == microcode) return;
    owner = {container, microcode};
    ++g_microcode_epoch;
  };
  set(PhysicalOf(microcode));
  if (code_at && code_at < 0x10000) set(PhysicalOf(microcode) + code_at);
}

// SetPixelShader(device, object), either device.
void OnPixelShaderSet(uint32_t object) {
  if (!g_memory || object < 0x10000 || !GuestReadable(object, 0x60)) return;
  const uint8_t* o = Virtual(object);
  if (static std::atomic<int> logged{0}; logged++ < 6)
    REXLOG_INFO("native renderer: pixel shader set: {:08X} type {:08X} microcode {:08X} container {:08X} {:X}+{:X}", object,
                Be32(o), Be32(o + guest::kPixelShaderMicrocode), Be32(o + 0x28), Be32(o + 0x2C), Be32(o + 0x30));
  if ((Be32(o) & 0xF) != 7) return;
  OnShaderMicrocode(object + guest::kPixelShaderContainer, Be32(o + guest::kPixelShaderMicrocode));
}

uint64_t g_replay_logged_frame = 0;
struct ReplayStats {
  uint32_t buffers = 0, draws = 0, no_shader = 0, no_indices = 0, packets = 0;
};
ReplayStats g_replay_stats;

// A register's place in the device (guest::RegisterOffset, 0: none) from a table: a played command buffer
// writes 150 000 registers a frame.
const uint16_t* RegisterOffsets() {
  static const std::vector<uint16_t> table = [] {
    std::vector<uint16_t> t(0x5000);
    for (uint32_t reg = 0; reg < t.size(); ++reg) t[reg] = uint16_t(guest::RegisterOffset(reg));
    return t;
  }();
  return table.data();
}

// (not inlined: the profiler names its lines - inlined into OnRunCommandBuffer its 4 ms a frame were one line)
__attribute__((noinline)) void RunCommands(Renderer* r, uint32_t buffer) {
  const uint8_t* o = Virtual(buffer);
  const uint32_t data = Be32(o + 0x98), end = Be32(o + 0x88);
  if (end <= data || end - data > (8u << 20) || ((end - data) & 3) || !GuestReadable(data, end - data)) return;
  const uint8_t* words = Virtual(data);
  const uint32_t count = (end - data) / 4;
  constexpr uint32_t kDeviceBytes = 0x3400;
  static std::vector<uint8_t> device(kDeviceBytes);
  std::memcpy(device.data(), Device(), kDeviceBytes);
  auto put = [&](uint32_t offset, uint32_t value) {
    const uint32_t be = _byteswap_ulong(value);
    std::memcpy(device.data() + offset, &be, 4);
  };
  put(guest::kDeviceIndexBuffer, 0);
  // (what the draw code keeps of the device's own state: put back after)
  Stream streams[16];
  std::copy(g_streams, g_streams + 16, streams);
  uint8_t stream_objects[16][0x20];
  std::memcpy(stream_objects, g_replay_streams, sizeof(stream_objects));
  const uint32_t bound_container = g_bound_vertex_container, bound_microcode = g_bound_vertex_microcode;
  const uint32_t loaded_pending = g_loaded_pending_count;
  g_loaded_pending_count = 0;
  g_replay_device = device.data();
  g_stream_words_valid = false;
  bool fetch_changed = true, vs_known = true, ps_known = true;
  ++g_replay_stats.buffers;
  // (a buffer not played before - by its contents - is read through once first, for its vertex buffers' starts)
  static std::unordered_set<uint64_t> scanned;
  const uint64_t contents = XXH3_64bits(words, size_t(count) * 4);
  bool scan = scanned.insert(contents).second;
  // (what the buffer's draws and shader loads came to before: ReplaySite)
  struct LoadSite {
    bool resolved = false;
    uint32_t word = 0, epoch = 0, object = 0, vertex_code = 0;
    uint64_t stamp = 0;
    MicrocodeOwner owner;
  };
  struct ReplayCache {
    std::vector<ReplaySite> sites;
    std::vector<LoadSite> loads;
  };
  static std::unordered_map<uint64_t, ReplayCache> caches;
  static const bool no_sites = EnvFlag("XMD_NATIVE_NO_SITES");
  if (caches.size() > 400) caches.clear();
  // (by the buffer object and its length, not its contents: the game patches values into a buffer between
  // frames - what a site keeps is compared with what the draw is given anyway)
  ReplayCache* cache = no_sites ? nullptr : &caches[(uint64_t(buffer) << 32) | count];
  if (scan) ++g_site_stats.scans;
  uint32_t draw_ordinal = 0, load_ordinal = 0;
  g_replay_streams_stale = true;

  const uint16_t* register_offsets = RegisterOffsets();
  auto write = [&](uint32_t reg, uint32_t value) {
    if (reg == 0x2081 || reg == 0x2082) reg = reg - 0x2081 + PA_SC_WINDOW_SCISSOR_TL;  // (the mirror's slots for them)
    const uint32_t offset = reg < 0x5000 ? register_offsets[reg] : 0;
    if (!offset) return;
    put(offset, value);
    if (reg >= 0x4000 && reg < 0x4800) {
      g_const_dirty[reg >= 0x4400 ? 1 : 0] |= 1ull << (63 - ((reg & 0x3FF) >> 4));
      bool& loaded = g_constants_loaded[(reg - 0x4000) >> 2];
      if (loaded && !scan) loaded = false, ++g_literal_version[reg >= 0x4400 ? 1 : 0];
    }
    else if (reg >= 0x4800 && reg < 0x48C0) fetch_changed = true;
  };
  auto constant_base = [](uint32_t type) -> uint32_t {
    switch (type) {
      case 0: return 0x4000;
      case 1: return 0x4800;
      case 2: return 0x4900;
      case 3: return 0x4908;
      case 4: return 0x2000;
      default: return 0;
    }
  };
  auto word = [&](uint32_t i) { return Be32(words + 4 * i); };

  for (int pass = scan ? 0 : 1; pass < 2; ++pass, scan = false) {
  if (pass == 1 && fetch_changed == false) fetch_changed = true;
  if (pass == 1) {  // (the scan's register writes undone)
    std::memcpy(device.data(), LiveDevice(), kDeviceBytes);
    put(guest::kDeviceIndexBuffer, 0);
    fetch_changed = true;
  }
  for (uint32_t i = 0; i < count;) {
    const uint32_t h = word(i);
    ++g_replay_stats.packets;
    const uint32_t type = h >> 30;
    if (type == 2) {
      ++i;
      continue;
    }
    if (type == 1) {
      if (i + 3 > count) break;
      write(h & 0x7FF, word(i + 1));
      write((h >> 11) & 0x7FF, word(i + 2));
      i += 3;
      continue;
    }
    const uint32_t n = ((h >> 16) & 0x3FFF) + 1;
    if (i + 1 + n > count) break;
    const uint32_t a = i + 1;
    i += 1 + n;
    if (type == 0) {
      const uint32_t reg = h & 0x7FFF, same = (h >> 15) & 1;
      // A run of registers whose places in the mirror are consecutive too (the constants, mostly): the packet's
      // words are big-endian as the mirror's are - copied as they stand, then what write() notes for each done
      // for the range. (Word by word - swapped out of the packet and back into the mirror - this loop was 2.6 ms
      // a frame of a 6,000-draw fight.)
      if (!same && n >= 4 && reg >= 0x2100 && reg + n <= 0x5000) {
        const uint32_t first = register_offsets[reg];
        bool run = first != 0;
        for (uint32_t k = 1; k < n && run; ++k) run = register_offsets[reg + k] == first + 4 * k;
        if (run) {
          std::memcpy(device.data() + first, words + 4 * a, size_t(n) * 4);
          const uint32_t last = reg + n - 1;
          if (last >= 0x4000 && reg < 0x4800) {
            const uint32_t lo = std::max(reg, 0x4000u), hi = std::min(last, 0x47FFu);
            for (uint32_t b = lo >> 4; b <= hi >> 4; ++b)
              g_const_dirty[b >= 0x440 ? 1 : 0] |= 1ull << (63 - (b & 0x3F));
            for (uint32_t c = (lo - 0x4000) >> 2; c <= (hi - 0x4000) >> 2; ++c) {
              bool& loaded = g_constants_loaded[c];
              if (loaded && !scan) loaded = false, ++g_literal_version[c >= 256 ? 1 : 0];
            }
          }
          if (last >= 0x4800 && reg < 0x48C0) fetch_changed = true;
          continue;
        }
      }
      for (uint32_t k = 0; k < n; ++k) write(same ? reg : reg + k, word(a + k));
      continue;
    }
    switch ((h >> 8) & 0x7F) {
      case 0x2D: {  // SET_CONSTANT: (type, index), values
        const uint32_t base = constant_base((word(a) >> 16) & 0xFF), index = word(a) & 0x7FF;
        if (base)
          for (uint32_t k = 1; k < n; ++k) write(base + index + k - 1, word(a + k));
        break;
      }
      case 0x2E:    // SET_CONSTANT2 / SET_SHADER_CONSTANTS: register, values
      case 0x32:
        for (uint32_t k = 1; k < n; ++k) write((word(a) & 0xFFFF) + k - 1, word(a + k));
        break;
      case 0x2F: {  // LOAD_ALU_CONSTANT: address, (type, index), dwords
        if (n < 3) break;
        const uint32_t base = constant_base((word(a + 1) >> 16) & 0xFF), index = word(a + 1) & 0x7FF;
        const uint32_t dwords = word(a + 2) & 0xFFF;
        const uint32_t from = PhysicalView(word(a) & 0x3FFFFFFF, dwords * 4);
        if (!base || !from) break;
        const uint8_t* src = Virtual(from);
        if (base == 0x4000 && !scan) {
          // (float constants: they stay loaded after the buffer too, as in the GPU's constant file)
          for (uint32_t k = 0; k < dwords && index + k < 0x800; ++k) {
            const uint32_t c = (index + k) >> 2;
            const float value = BeFloat(src + 4 * k);
            float& held = g_gpu_constants[c][(index + k) & 3];
            // (the same values again - the next draw's shader has the same literals: nothing to upload anew)
            if (!g_constants_loaded[c] || std::memcmp(&held, &value, 4)) ++g_literal_version[c >= 256 ? 1 : 0];
            held = value;
            g_constants_loaded[c] = true;
          }
          break;
        }
        for (uint32_t k = 0; k < dwords; ++k) write(base + index + k, Be32(src + 4 * k));
        break;
      }
      case 0x27: {  // IM_LOAD: (microcode address | 0 vertex, 1 pixel), size
        if (scan) break;
        const bool pixel = (word(a) & 3) == 1;
        MicrocodeOwner owner;
        LoadSite* load = nullptr;
        if (cache) {
          if (load_ordinal >= cache->loads.size()) cache->loads.resize(load_ordinal + 1);
          load = &cache->loads[load_ordinal++];
        }
        const bool known_load = load && load->resolved && load->word == word(a) && load->epoch == g_microcode_epoch.load() &&
                                r->frames - load->stamp < 40 + (load_ordinal & 31);
        if (known_load) {
          owner = load->owner;
        } else {
          std::lock_guard lock(g_microcode_mutex);
          // (the code is inside the owner's microcode: after its constant values)
          const uint32_t at = word(a) & 0x1FFFFFFCu;
          auto it = g_microcode_owners.upper_bound(at);
          if (it != g_microcode_owners.begin()) {
            --it;
            if (GuestReadable(it->second.container, 12) && at - it->first < std::max<uint32_t>(Be32(Virtual(it->second.container) + 8), 4))
              owner = it->second;
            else if (static int logged = 0; logged++ < 12)
              REXLOG_INFO("native renderer:   microcode {:08X}: the nearest known is {:08X} (container {:08X}: {:08X} {:X}+{:X})", at,
                          it->first, it->second.container, Be32(Virtual(it->second.container)),
                          Be32(Virtual(it->second.container) + 4), Be32(Virtual(it->second.container) + 8));
          }
        }
        uint32_t object = known_load ? load->object : 0;
        if (owner.container && !known_load) {
          const bool vertex_container = Be32(Virtual(owner.container)) & 1;
          if (vertex_container == !pixel)
            object = owner.container - (pixel ? guest::kPixelShaderContainer : guest::kVertexShaderContainer);
        }
        if (!object) {
          static int logged = 0;
          if (logged++ < 12)
            REXLOG_WARN("native renderer: command buffer {:08X}: no shader object known for the {} microcode at {:08X}",
                        buffer, pixel ? "pixel" : "vertex", word(a));
        }
        if (pixel) {
          ps_known = object != 0;
          put(guest::kDeviceCurrentPixelShader, object);
        } else {
          vs_known = object != 0;
          put(guest::kDeviceCurrentVertexShader, object);
          g_bound_vertex_container = owner.container;
          g_bound_vertex_microcode = owner.microcode;
          g_replay_vertex_code = known_load ? load->vertex_code : PhysicalView(word(a) & 0x1FFFFFFCu, 12);
          if (!known_load && owner.container && GuestReadable(owner.container, 0x24)) {
            const uint8_t* c = Virtual(owner.container);
            const uint32_t shader_at = Be32(c + 0x18);
            const uint32_t own = PhysicalOf(owner.microcode) + (shader_at + 4 <= Be32(c + 4) ? Be32(c + shader_at) : 0);
            static int logged = 0;
            if (own != (word(a) & 0x1FFFFFFCu) && logged++ < 6)
              REXLOG_INFO("native renderer: command buffer {:08X} loads vertex code at {:08X}, not the shader object's own "
                          "({:08X}): its fetches are read from there", buffer, word(a) & 0x1FFFFFFCu, own);
          }
        }
        if (load && !known_load && object) {
          load->resolved = true;
          load->word = word(a), load->epoch = g_microcode_epoch.load(), load->object = object;
          load->vertex_code = pixel ? 0 : g_replay_vertex_code;
          load->stamp = r->frames;
          load->owner = owner;
        }
        break;
      }
      case 0x22: {  // DRAW_INDX: visibility, initiator, [indices, their size]
        if (n < 2) break;
        const uint32_t initiator = word(a + 1), primitive = initiator & 0x3F, source = (initiator >> 6) & 3;
        const uint32_t indices = initiator >> 16;
        if (scan) {
          if (fetch_changed) {
            fetch_changed = false;
            const uint8_t* fetch = device.data() + guest::RegisterOffset(0x4800);
            for (uint32_t s = 0; s < 16; ++s) {
              const uint32_t w0 = Be32(fetch + (95 - s) * 8), w1 = Be32(fetch + (95 - s) * 8 + 4);
              const uint32_t start = w0 & ~3u, bytes = ((w1 >> 2) & 0xFFFFFF) * 4;
              if ((w0 & 3) != 3 || !start || !bytes) continue;
              uint32_t& known = g_buffer_starts.try_emplace(start + bytes, start).first->second;
              known = std::min(known, start);
            }
          }
          break;
        }
        ++g_replay_stats.draws;
        ReplaySite* site = nullptr;
        if (cache) {
          if (draw_ordinal >= cache->sites.size()) cache->sites.resize(draw_ordinal + 1);
          site = &cache->sites[draw_ordinal];
          site->ttl = 24 + (draw_ordinal & 15);
          ++draw_ordinal;
        }
        if (!vs_known || !ps_known) {
          ++g_replay_stats.no_shader;
          break;
        }
        const bool indexed = source == 0;
        if (indexed) {
          if (n < 4 || ((initiator >> 11) & 1)) {  // (32-bit indices: not seen)
            ++g_replay_stats.no_indices;
            break;
          }
          g_replay_index_size = indices * 2;
          if (site && site->raw_view && site->raw_index == word(a + 2) && site->raw_size == g_replay_index_size &&
              r->frames - site->raw_stamp < site->ttl) {
            g_replay_index_address = site->raw_view;
          } else {
            g_replay_index_address = PhysicalView(word(a + 2), g_replay_index_size);
            if (site) {
              site->raw_index = word(a + 2), site->raw_view = g_replay_index_address, site->raw_size = g_replay_index_size;
              site->raw_stamp = r->frames;
            }
          }
          if (!g_replay_index_address) {
            ++g_replay_stats.no_indices;
            break;
          }
        } else if (source != 2) {
          ++g_replay_stats.no_indices;
          break;
        }
        // (the streams are made from the fetch constants when the draw needs them: Draw)
        if (fetch_changed) fetch_changed = false, g_replay_streams_stale = true;
        g_site = site;
        Draw(r, primitive, int32_t(Reg(0x2102)), 0, indices, indexed);
        g_site = nullptr;
        break;
      }
      default:
        break;
    }
  }
  }

  g_replay_device = nullptr;
  g_replay_streams_stale = false;
  g_replay_vertex_code = 0;
  std::copy(streams, streams + 16, g_streams);
  std::memcpy(g_replay_streams, stream_objects, sizeof(stream_objects));
  g_stream_words_valid = false;
  std::memset(g_fetch_seen, 0xFF, sizeof(g_fetch_seen));  // (the device's own streams: made again at its next draw)
  g_bound_vertex_container = bound_container, g_bound_vertex_microcode = bound_microcode;
  g_loaded_pending_count = loaded_pending;
  g_const_dirty[0] = g_const_dirty[1] = ~0ull;  // (the mirror compared with from here on is the device's again)
}

void DoRunCommandBuffer(uint32_t buffer, uint32_t predication) {
  if (!g_memory || buffer < 0x10000) return;
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  static const bool off = EnvFlag("XMD_NATIVE_NO_COMMAND_BUFFERS");
  if (off || !BeginFrame(r)) return;
  RunCommands(r, buffer);
  if (r->frames - g_replay_logged_frame >= 600) {
    g_replay_logged_frame = r->frames;
    uint64_t vb_bytes = 0, vb_live = 0;
    for (const auto& [key, c] : r->vertex_buffers)
      if (c.resource) vb_bytes += c.size, ++vb_live;
    REXLOG_INFO("native renderer: draw sites in 600 frames: {} draws, {} with their shaders kept, {} with their buffers "
                "kept; {} buffers read through first; epoch {}",
                g_site_stats.draws, g_site_stats.fresh, g_site_stats.buffers, g_site_stats.scans, g_site_epoch);
    g_site_stats = {};
    REXLOG_INFO("native renderer: small-buffer pool: {} buffers of {} MB, {} KB used in the last, {} KB unused",
                r->pool.chunks.size(), kPoolChunk >> 20, r->pool.chunks.empty() ? 0 : r->pool.chunks.back().used >> 10,
                r->pool.dead >> 10);
    REXLOG_INFO("native renderer: command buffers played in 600 frames: {} with {} draws ({} without a shader, {} without "
                "indices), {} packets; {} vertex buffer ends known; vertex buffers cached: {} of {} entries, {} MB",
                g_replay_stats.buffers, g_replay_stats.draws, g_replay_stats.no_shader, g_replay_stats.no_indices,
                g_replay_stats.packets, g_buffer_starts.size(), vb_live, r->vertex_buffers.size(), vb_bytes >> 20);
    g_replay_stats = {};
  }
}

// ---------------------------------------------------------------------------
// The render thread. The renderer's work for a graphics call used to be done inside the call, on the game's
// render thread - which also runs the game's own rendering code: the two added up (a 6,000-draw fight: about
// 10 ms of the game's and 12 ms of the renderer's a frame). Now a call only notes what it is, with a copy of the
// D3D device as it stands (13 KB: the register mirror, the constants, the current shaders - the one thing the
// game changes at its very next call), and a thread of the renderer's own makes the calls in order from those
// notes. Everything else a call reads - vertex and index data, textures, constant blocks, command buffers -
// the game keeps as it is until the frame is over: the console's GPU reads it later than the call too.
// Present waits until the thread has done the whole frame, so the game never runs ahead into the next one.
// native_render_thread=false: inside the calls, as before.

namespace {

enum class JobType : uint8_t {
  kDraw, kRunBuffer, kDrawUP, kDrawIndexedUP, kEndVertices, kClear, kResolve, kPresent, kSetRenderTarget,
  kSetStream, kSetConstants, kFlushConstants, kLoadConstants, kUnboundShader, kShaderBound,
};

struct Job {
  JobType type;
  bool flag = false;
  uint32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0;
  uint64_t m = 0;
  const uint8_t* device = nullptr;  // (the copy: null for calls that don't read it)
  HookArgs args;
};

constexpr uint32_t kJobRing = 1u << 15;
constexpr uint32_t kDeviceCopy = 0x3400;
constexpr size_t kCopyChunk = size_t(kDeviceCopy) * 512;  // (6.6 MB)

struct RenderThread {
  std::vector<Job> ring = std::vector<Job>(kJobRing);
  std::atomic<uint64_t> pushed{0}, done{0};
  std::atomic<bool> sleeping{false}, waiting{false};
  std::mutex mutex, push_mutex;
  std::condition_variable work, idle;
  std::thread thread;
  // The device copies of the frame (the producer's; started again once the thread has caught up).
  std::vector<std::unique_ptr<uint8_t[]>> chunks;
  size_t chunk = 0, used = 0;
  uint64_t copies = 0, jobs_frame = 0;
  // The frame's work on the thread (its own; read at Present, when it has caught up): all of it, and its
  // slowest single call of the kinds that can be slow (a command buffer, a resolve, the present).
  double busy_ms = 0, slow_ms = 0;
  JobType slow_type = JobType::kDraw;
} g_rt;

// (history.h: the calls' names in an F4 capture's render_calls.txt, in JobType's order)
const char* const kJobNames[] = {"draw", "command_buffer", "draw_up", "draw_indexed_up", "end_vertices", "clear",
                                 "resolve", "present", "set_render_target", "set_stream", "set_constants",
                                 "flush_constants", "load_constants", "unbound_shader", "shader_bound"};

const char* JobName(JobType t) {
  switch (t) {
    case JobType::kRunBuffer: return "a command buffer";
    case JobType::kResolve: return "a resolve";
    case JobType::kPresent: return "the present";
    case JobType::kClear: return "a clear";
    default: return "draws";
  }
}

bool Threaded() {
  static const bool on = REXCVAR_GET(native_render_thread) && !EnvFlag("XMD_NATIVE_NO_RENDER_THREAD") && g_main;
  return on;
}

void RunJob(const Job& j) {
  g_job_device = j.device;
  switch (j.type) {
    case JobType::kDraw: DoDraw(j.a, j.b, j.c, j.flag); break;
    case JobType::kRunBuffer: DoRunCommandBuffer(j.a, j.b); break;
    case JobType::kDrawUP: DoDrawUP(j.a, j.b, j.c, j.d); break;
    case JobType::kDrawIndexedUP: DoDrawIndexedUP(int32_t(j.a), j.b, j.c, j.d, j.e, j.f, j.g); break;
    case JobType::kEndVertices: DoEndVertices(); break;
    case JobType::kClear: DoClear(j.args); break;
    case JobType::kResolve: DoResolve(j.args); break;
    case JobType::kPresent: DoPresent(j.a); break;
    case JobType::kSetRenderTarget: DoSetRenderTarget(j.a, j.b); break;
    case JobType::kSetStream: DoSetStreamSource(j.a, j.b, j.c, j.d); break;
    case JobType::kSetConstants: DoSetShaderConstants(j.flag, j.a, j.b); break;
    case JobType::kFlushConstants: DoFlushShaderConstants(j.m, j.a); break;
    case JobType::kLoadConstants: DoLoadShaderConstants(j.a, j.b); break;
    case JobType::kUnboundShader: DoUnboundShader(j.a, j.b, j.c); break;
    case JobType::kShaderBound: DoShaderBound(j.a, j.b); break;
  }
  g_job_device = nullptr;
}

void RenderThreadMain() {
  rex::thread::set_current_thread_name("native renderer");
  uint64_t at = 0;
  for (;;) {
    uint64_t end = g_rt.pushed.load(std::memory_order_acquire);
    if (end == at) {
      for (int i = 0; i < 2000 && end == at; ++i) {
        std::this_thread::yield();
        end = g_rt.pushed.load(std::memory_order_acquire);
      }
      if (end == at) {
        std::unique_lock lock(g_rt.mutex);
        g_rt.sleeping.store(true, std::memory_order_seq_cst);
        g_rt.work.wait(lock, [&] { return g_rt.pushed.load(std::memory_order_seq_cst) != at; });
        g_rt.sleeping.store(false, std::memory_order_relaxed);
        continue;
      }
    }
    const auto batch = std::chrono::steady_clock::now();
    for (; at != end; ++at) {
      const Job& job = g_rt.ring[at & (kJobRing - 1)];
      const bool timed = job.type == JobType::kRunBuffer || job.type == JobType::kResolve ||
                         job.type == JobType::kPresent || job.type == JobType::kClear;
      const auto t0 = timed ? std::chrono::steady_clock::now() : batch;
      RunJob(job);
      if (timed) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > g_rt.slow_ms) g_rt.slow_ms = ms, g_rt.slow_type = job.type;
      }
      // (the batch's time before the last call is marked done: Present reads it once everything is)
      if (at + 1 == end)
        g_rt.busy_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - batch).count();
      g_rt.done.store(at + 1, std::memory_order_release);
    }
    if (g_rt.waiting.load(std::memory_order_seq_cst)) {
      { std::lock_guard lock(g_rt.mutex); }
      g_rt.idle.notify_all();
    }
  }
}

// Until the thread has made every call noted so far.
void WaitForRenderThread() {
  const uint64_t end = g_rt.pushed.load(std::memory_order_relaxed);
  // (">=", not "==": a second thread of the game - the loading screen's - may note calls after `end` was read, and
  // `done` then passes it without ever being equal: both threads slept for good, the game frozen on "Loading")
  for (int i = 0; i < 4000 && g_rt.done.load(std::memory_order_acquire) < end; ++i) std::this_thread::yield();
  if (g_rt.done.load(std::memory_order_acquire) >= end) return;
  g_rt.waiting.store(true, std::memory_order_seq_cst);
  std::unique_lock lock(g_rt.mutex);
  g_rt.idle.wait(lock, [&] { return g_rt.done.load(std::memory_order_seq_cst) >= end; });
  g_rt.waiting.store(false, std::memory_order_relaxed);
}

// The device as it is now, kept for the call (null: no device yet).
const uint8_t* CopyDevice() {
  const uint32_t address = g_device_address.load(std::memory_order_relaxed);
  if (!g_memory || address < 0x10000) return nullptr;
  if (g_rt.chunks.empty() || g_rt.used + kDeviceCopy > kCopyChunk) {
    if (!g_rt.chunks.empty()) ++g_rt.chunk;
    // (a frame of more than 400 MB of copies - 30,000 calls: caught up with first, and started again)
    if (g_rt.chunk >= 64) {
      WaitForRenderThread();
      g_rt.chunk = 0;
    }
    if (g_rt.chunk == g_rt.chunks.size()) g_rt.chunks.push_back(std::make_unique<uint8_t[]>(kCopyChunk));
    g_rt.used = 0;
  }
  uint8_t* copy = g_rt.chunks[g_rt.chunk].get() + g_rt.used;
  g_rt.used += kDeviceCopy;
  std::memcpy(copy, Virtual(address), kDeviceCopy);
  ++g_rt.copies;
  return copy;
}

void Push(Job job, bool with_device) {
  std::lock_guard push_lock(g_rt.push_mutex);  // (the game's render thread, nearly always: uncontended)
  if (static bool started = false; !started) {
    started = true;
    g_rt.thread = std::thread(RenderThreadMain);
    g_rt.thread.detach();
    xmd::history::SetRenderCallNames(kJobNames, uint32_t(std::size(kJobNames)));
    REXLOG_INFO("native renderer: the graphics calls are made on the renderer's own thread");
  }
  if (with_device) job.device = CopyDevice();
  xmd::history::RenderCall(uint8_t(job.type), job.flag, job.a, job.b, job.c, job.d);
  const uint64_t at = g_rt.pushed.load(std::memory_order_relaxed);
  // (the ring full - 32,768 calls waiting: the thread is far behind)
  while (at - g_rt.done.load(std::memory_order_acquire) >= kJobRing) std::this_thread::yield();
  g_rt.ring[at & (kJobRing - 1)] = job;
  g_rt.pushed.store(at + 1, std::memory_order_seq_cst);
  ++g_rt.jobs_frame;
  if (g_rt.sleeping.load(std::memory_order_seq_cst)) {
    { std::lock_guard lock(g_rt.mutex); }
    g_rt.work.notify_one();
  }
}

HookArgs CaptureArgs(const PPCContext& ctx) {
  HookArgs a;
  a.r4 = ctx.r4.u32, a.r8 = ctx.r8.u32;
  a.z = static_cast<float>(ctx.f1.f64);
  if (GuestReadable(ctx.r1.u32 + 0x5C, 4)) a.stencil = Be32(Virtual(ctx.r1.u32 + 0x5C));
  if (ctx.r5.u32 >= 0x10000 && GuestReadable(ctx.r5.u32, 16)) {
    a.has_rect = true;
    std::memcpy(a.rect, Virtual(ctx.r5.u32), 16);
  }
  if (ctx.r10.u32 >= 0x10000 && GuestReadable(ctx.r10.u32, 16)) {
    a.has_colour = true;
    std::memcpy(a.colour, Virtual(ctx.r10.u32), 16);
  }
  return a;
}

}  // namespace

void OnPresent(uint32_t front_buffer) {
  xmd::profile::FrameThread();  // (profile_sampler.h: test aid)
  if (!Threaded()) {
    xmd::problems::FrameEnd();    // (problem_log.h: a frame that took far too long, and what it built)
    xmd::history::Frame h;
    h.draws = LastFrameDraws();
    xmd::history::FrameEnd(h);
    xmd::PerfFrameShown();        // (perf_overlay.h: the frame rate, CPU time a frame)
    xmd::LatencyOnSwap();  // (frame_rate.h: test aid)
    xmd::PerfHintFrame(xmd::FrameRateNow());  // (android_perf.h: the phone's clocks)
    return DoPresent(front_buffer);
  }
  Job j{JobType::kPresent};
  j.a = front_buffer;
  Push(j, true);
  // The frame ends here for the game too: what it handed over is drawn before it goes on to the next.
  const auto wait_start = std::chrono::steady_clock::now();
  WaitForRenderThread();
  const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
  // (the thread is idle now: its frame's numbers, and what it noted for the problems log, are whole)
  char split[200];
  std::snprintf(split, sizeof(split),
                "the renderer's thread worked %.0f ms (slowest: %s, %.0f ms); the game waited %.0f ms for it at the "
                "frame's end - the rest is the game's own side",
                g_rt.busy_ms, JobName(g_rt.slow_type), g_rt.slow_ms, waited);
  {
    xmd::history::Frame h;
    h.renderer_ms = float(g_rt.busy_ms), h.waited_ms = float(waited), h.slow_ms = float(g_rt.slow_ms);
    h.slow = JobName(g_rt.slow_type);
    h.draws = LastFrameDraws(), h.device_copies = uint32_t(g_rt.copies);
    xmd::history::FrameEnd(h);
  }
  g_rt.busy_ms = g_rt.slow_ms = 0;
  g_rt.slow_type = JobType::kDraw;
  xmd::problems::FrameSplit(split);
  xmd::problems::FrameEnd();
  xmd::PerfFrameShown();
  xmd::LatencyOnSwap();
  xmd::PerfHintFrame(xmd::FrameRateNow());
  std::lock_guard push_lock(g_rt.push_mutex);
  g_rt.chunk = g_rt.used = 0;
  static uint64_t frames = 0, jobs = 0, copies = 0;
  jobs += g_rt.jobs_frame, copies += g_rt.copies;
  g_rt.jobs_frame = g_rt.copies = 0;
  if (++frames == 600) {
    REXLOG_INFO("native renderer: render thread: {:.0f} calls a frame handed over, {:.0f} with a copy of the device "
                "({:.1f} MB a frame)", jobs / 600.0, copies / 600.0, copies / 600.0 * kDeviceCopy / 1048576.0);
    frames = jobs = copies = 0;
  }
}

void OnDraw(uint32_t primitive, uint32_t start, uint32_t count, bool indexed) {
  if (!Threaded()) return DoDraw(primitive, start, count, indexed);
  Job j{JobType::kDraw, indexed};
  j.a = primitive, j.b = start, j.c = count;
  Push(j, true);
}

void OnRunCommandBuffer(uint32_t buffer, uint32_t predication) {
  if (!Threaded()) return DoRunCommandBuffer(buffer, predication);
  Job j{JobType::kRunBuffer};
  j.a = buffer, j.b = predication;
  Push(j, true);
}

void OnDrawUP(const PPCContext& args, uint32_t data) {
  g_up_waiting.store(data != 0, std::memory_order_relaxed);  // (drawn at the next D3D call: BeforeD3DCall)
  if (!Threaded()) return DoDrawUP(args.r4.u32, args.r5.u32, args.r6.u32, data);
  Job j{JobType::kDrawUP};
  j.a = args.r4.u32, j.b = args.r5.u32, j.c = args.r6.u32, j.d = data;
  Push(j, true);
}

void OnDrawIndexedUP(const PPCContext& args) {
  if (!g_memory) return;
  const uint32_t index_data = args.r9.u32 >= 0x10000 ? Be32(Virtual(args.r9.u32)) : 0;
  const uint32_t data = args.r10.u32 >= 0x10000 ? Be32(Virtual(args.r10.u32)) : 0;
  g_up_waiting.store(data != 0 && index_data != 0 && !(args.r7.u32 & 4), std::memory_order_relaxed);
  if (!Threaded()) return DoDrawIndexedUP(args.r4.s32, args.r5.u32, args.r6.u32, args.r7.u32, args.r8.u32, index_data, data);
  Job j{JobType::kDrawIndexedUP};
  j.a = args.r4.u32, j.b = args.r5.u32, j.c = args.r6.u32, j.d = args.r7.u32, j.e = args.r8.u32, j.f = index_data, j.g = data;
  Push(j, true);
}

void OnEndVertices() {
  g_up_waiting.store(false, std::memory_order_relaxed);
  if (!Threaded()) return DoEndVertices();
  Push(Job{JobType::kEndVertices}, false);
}

void OnClear(const PPCContext& ctx) {
  if (!g_memory) return;
  Job j{JobType::kClear};
  j.args = CaptureArgs(ctx);
  if (!Threaded()) return DoClear(j.args);
  Push(j, true);
}

void OnResolve(const PPCContext& ctx) {
  if (!g_memory) return;
  Job j{JobType::kResolve};
  j.args = CaptureArgs(ctx);
  if (!Threaded()) return DoResolve(j.args);
  Push(j, true);
}

void OnSetRenderTarget(uint32_t index, uint32_t surface) {
  if (!Threaded()) return DoSetRenderTarget(index, surface);
  Job j{JobType::kSetRenderTarget};
  j.a = index, j.b = surface;
  Push(j, false);
}

void OnSetStreamSource(uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride) {
  if (!Threaded()) return DoSetStreamSource(stream, buffer, offset, stride);
  Job j{JobType::kSetStream};
  j.a = stream, j.b = buffer, j.c = offset, j.d = stride;
  Push(j, false);
}

void OnSetShaderConstants(bool pixel, uint32_t start, uint32_t count) {
  if (!Threaded()) return DoSetShaderConstants(pixel, start, count);
  Job j{JobType::kSetConstants, pixel};
  j.a = start, j.b = count;
  Push(j, false);
}

void OnFlushShaderConstants(uint64_t mask, uint32_t reg) {
  if (!Threaded()) return DoFlushShaderConstants(mask, reg);
  Job j{JobType::kFlushConstants};
  j.m = mask, j.a = reg;
  Push(j, false);
}

void OnLoadShaderConstants(uint32_t shader_object, uint32_t base) {
  if (!Threaded()) return DoLoadShaderConstants(shader_object, base);
  Job j{JobType::kLoadConstants};
  j.a = shader_object, j.b = base;
  Push(j, false);
}

void OnUnboundShader(uint32_t a, uint32_t b, uint32_t c) {
  if (!Threaded()) return DoUnboundShader(a, b, c);
  Job j{JobType::kUnboundShader};
  j.a = a, j.b = b, j.c = c;
  Push(j, false);
}

void OnShaderBound(uint32_t container, uint32_t microcode) {
  if (!Threaded()) return DoShaderBound(container, microcode);
  Job j{JobType::kShaderBound};
  j.a = container, j.b = microcode;
  Push(j, false);
}

void SetDevice(uint32_t device) { g_device_address.store(device, std::memory_order_relaxed); }

// (shaders are found from their objects when drawn: FindShader)
void OnShaderCreated(uint32_t, uint32_t, bool) {}

}  // namespace xmd::native

namespace xmd::native {
void SetHalfFrames(bool on) { g_half_frames = on; }
void SetHalfSuspended(bool on) { g_half_suspended = on; }
bool HalfFrames() { return g_half_frames.load() && Enabled(); }
bool LastFrameHidden() { return g_last_hidden.load(); }
}  // namespace xmd::native
