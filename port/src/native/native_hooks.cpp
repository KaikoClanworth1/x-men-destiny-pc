// X-Men: Destiny - hooks feeding the native renderer.
//
// Each hook runs the game's own D3D function first (the emulated GPU still
// gets its command stream: fences, interrupts) and then hands the call to the
// native renderer. Function addresses: native/guest_d3d.h. Not built into the
// census or probe builds (XMD_D3D_TRACE / XMD_D3D_PROBE), which hook the same
// functions themselves.
//
// The game's EndVertices is inlined, so nothing says when the vertices of a
// BeginVertices draw are in. They are written straight after it returns, so
// the draw is made at the next call into the D3D library, whichever it is:
// native_flush_hooks.cpp (tools/gen_native_flush.py) covers the functions not
// hooked here.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/xmd_init.h"
#include "native/native_renderer.h"

namespace nr = xmd::native;

namespace {

// The device that presents is the one drawn from. The game also makes D3D
// calls on a second device object from another thread (while loading, and at
// the title): those are counted here (the "fps:" line) and not drawn.
std::atomic<uint32_t> g_primary{0};
std::atomic<uint32_t> g_other_device{0};
std::atomic<uint64_t> g_primary_draws{0}, g_other_draws{0};

inline bool Primary(uint32_t device) {
  if (device == g_primary.load(std::memory_order_relaxed)) return true;
  g_other_device.store(device, std::memory_order_relaxed);
  return false;
}
inline bool PrimaryDraw(uint32_t device) {
  const bool primary = Primary(device);
  (primary ? g_primary_draws : g_other_draws).fetch_add(1, std::memory_order_relaxed);
  return primary;
}

// XMD_FPS_LOG=1: every 5 s, the frames shown a second and how much of the
// game thread's time went by inside Present.
void FpsLog(double inside_seconds) {
  using Clock = std::chrono::steady_clock;
  static const bool on = std::getenv("XMD_FPS_LOG") != nullptr;
  if (!on) return;
  static Clock::time_point since = Clock::now(), last = since;
  static double inside = 0, longest = 0;
  static uint32_t frames = 0;
  const auto now = Clock::now();
  inside += inside_seconds;
  longest = std::max(longest, std::chrono::duration<double>(now - last).count());
  last = now;
  ++frames;
  const double span = std::chrono::duration<double>(now - since).count();
  if (span < 5.0) return;
  REXLOG_INFO("fps: {:.1f} frames/s, {:.0f}% of the time in Present, longest frame {:.0f} ms ({}); draws a frame: {:.1f} "
              "on the device, {:.1f} on another ({:08X})",
              frames / span, 100.0 * inside / span, longest * 1000.0, nr::RendererLabel(),
              g_primary_draws.exchange(0) / double(frames), g_other_draws.exchange(0) / double(frames),
              g_other_device.load());
  since = now;
  inside = longest = 0;
  frames = 0;
}

}  // namespace

// D3DDevice_Present / Swap(device, front buffer texture)
REX_EXTERN(__imp__sub_82738208);
REX_HOOK_RAW(sub_82738208) {
  nr::BeforeD3DCall();
  g_primary.store(ctx.r3.u32, std::memory_order_relaxed);
  nr::SetDevice(ctx.r3.u32);
  const uint32_t front_buffer = ctx.r4.u32;
  const auto in = std::chrono::steady_clock::now();
  __imp__sub_82738208(ctx, base);
  FpsLog(std::chrono::duration<double>(std::chrono::steady_clock::now() - in).count());
  if (nr::Enabled()) nr::OnPresent(front_buffer);
}

// D3DDevice_DrawIndexedVertices(device, primitive, start index, index count)
REX_EXTERN(__imp__sub_8273C300);
REX_HOOK_RAW(sub_8273C300) {
  nr::BeforeD3DCall();
  const bool primary = PrimaryDraw(ctx.r3.u32);
  const uint32_t primitive = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  __imp__sub_8273C300(ctx, base);
  if (primary && nr::Enabled()) nr::OnDraw(primitive, start, count, true);
}

// D3DDevice_DrawVertices(device, primitive, start vertex, vertex count)
REX_EXTERN(__imp__sub_8273BEE8);
REX_HOOK_RAW(sub_8273BEE8) {
  nr::BeforeD3DCall();
  const bool primary = PrimaryDraw(ctx.r3.u32);
  const uint32_t primitive = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  __imp__sub_8273BEE8(ctx, base);
  if (primary && nr::Enabled()) nr::OnDraw(primitive, start, count, false);
}

// D3DDevice_BeginVertices(device, primitive, count, stride): returns (r3) the
// space the game then writes the vertices to.
REX_EXTERN(__imp__sub_8273B4A8);
REX_HOOK_RAW(sub_8273B4A8) {
  nr::BeforeD3DCall();
  const bool primary = PrimaryDraw(ctx.r3.u32);
  const PPCContext args = ctx;  // the call clobbers argument registers
  __imp__sub_8273B4A8(ctx, base);
  if (primary && nr::Enabled()) nr::OnDrawUP(args, ctx.r3.u32);
}

// D3DDevice_BeginIndexedVertices(device, base vertex, vertex count, index count, index format, stride,
// void** indices, void** vertices): the game then writes both; drawn at the next D3D call.
REX_EXTERN(__imp__sub_8273B9B0);
REX_HOOK_RAW(sub_8273B9B0) {
  nr::BeforeD3DCall();
  const bool primary = PrimaryDraw(ctx.r3.u32);
  const PPCContext args = ctx;  // the call clobbers argument registers
  __imp__sub_8273B9B0(ctx, base);
  if (primary && nr::Enabled()) nr::OnDrawIndexedUP(args);
}

// D3DDevice_SetRenderTarget(device, index, surface)
REX_EXTERN(__imp__sub_827283C0);
REX_HOOK_RAW(sub_827283C0) {
  nr::BeforeD3DCall();
  if (Primary(ctx.r3.u32) && nr::Enabled()) nr::OnSetRenderTarget(ctx.r4.u32, ctx.r5.u32);
  __imp__sub_827283C0(ctx, base);
}

// D3DDevice_Clear(device, flags, ...)
REX_EXTERN(__imp__sub_8273A690);
REX_HOOK_RAW(sub_8273A690) {
  nr::BeforeD3DCall();
  const bool primary = Primary(ctx.r3.u32);
  const PPCContext args = ctx;
  __imp__sub_8273A690(ctx, base);
  if (primary && nr::Enabled()) nr::OnClear(args);
}

// D3DDevice_Resolve(device, flags, source rectangle, destination texture, ...)
REX_EXTERN(__imp__sub_827390F8);
REX_HOOK_RAW(sub_827390F8) {
  nr::BeforeD3DCall();
  const bool primary = Primary(ctx.r3.u32);
  PPCContext args = ctx;
  args.r8.u64 = ctx.r6.u64;  // (the renderer reads the destination texture from r8)
  __imp__sub_827390F8(ctx, base);
  if (primary && nr::Enabled() && args.r8.u32) nr::OnResolve(args);
}

// D3DDevice_SetStreamSource(device, stream, buffer, offset, stride - always 0 here)
REX_EXTERN(__imp__sub_82728188);
REX_HOOK_RAW(sub_82728188) {
  nr::BeforeD3DCall();
  const uint32_t device = ctx.r3.u32, stream = ctx.r4.u32, buffer = ctx.r5.u32, offset = ctx.r6.u32;
  if (Primary(ctx.r3.u32) && nr::Enabled()) nr::OnSetStreamSource(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  __imp__sub_82728188(ctx, base);
  if (Primary(device) && nr::Enabled()) nr::DebugFindStream(device, stream, buffer, offset);
}

// D3DDevice_SetVertexShaderConstantF / SetPixelShaderConstantF(device, start, data, count)
REX_EXTERN(__imp__sub_8272F790);
REX_HOOK_RAW(sub_8272F790) {
  nr::BeforeD3DCall();
  const bool primary = Primary(ctx.r3.u32);
  const uint32_t start = ctx.r4.u32, count = ctx.r6.u32;
  __imp__sub_8272F790(ctx, base);
  if (primary && nr::Enabled()) nr::OnSetShaderConstants(false, start, count);
}

REX_EXTERN(__imp__sub_8272F6B8);
REX_HOOK_RAW(sub_8272F6B8) {
  nr::BeforeD3DCall();
  const bool primary = Primary(ctx.r3.u32);
  const uint32_t start = ctx.r4.u32, count = ctx.r6.u32;
  __imp__sub_8272F6B8(ctx, base);
  if (primary && nr::Enabled()) nr::OnSetShaderConstants(true, start, count);
}

// D3D internal: write dirty shader constants from the device mirror to the GPU.
REX_EXTERN(__imp__sub_8273E198);
REX_HOOK_RAW(sub_8273E198) {
  if (Primary(ctx.r3.u32) && nr::Enabled()) nr::OnFlushShaderConstants(ctx.r4.u64, ctx.r5.u32);
  __imp__sub_8273E198(ctx, base);
}

// D3D internal, at a draw whose shaders changed: (device, the shader object's
// container copy, its microcode) - the microcode the GPU gets, and the
// shader's constant blocks loaded from memory (LOAD_ALU_CONSTANT).
REX_EXTERN(__imp__sub_8273ED38);
REX_HOOK_RAW(sub_8273ED38) {
  if (nr::Enabled()) nr::OnShaderMicrocode(ctx.r4.u32, ctx.r5.u32);
  if (Primary(ctx.r3.u32) && nr::Enabled()) {
    nr::OnShaderBound(ctx.r4.u32, ctx.r5.u32);
    nr::OnLoadShaderConstants(ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_8273ED38(ctx, base);
}

// D3D internal, at a draw with a vertex shader not bound to a vertex format: the microcode goes into the
// command stream with its vertex fetches filled in from a declaration.
REX_EXTERN(__imp__sub_8273EBD8);
REX_HOOK_RAW(sub_8273EBD8) {
  if (Primary(ctx.r3.u32) && nr::Enabled()) nr::OnUnboundShader(ctx.r5.u32, ctx.r7.u32, ctx.r8.u32);
  __imp__sub_8273EBD8(ctx, base);
}

// D3DDevice_RunCommandBuffer(device, command buffer, predication): the game records its levels' static
// draws (depth pass, shadow passes) into command buffers once, on a second device object, and plays them
// every frame - no draw call reaches the hooks above for them.
REX_EXTERN(__imp__sub_82741028);
REX_HOOK_RAW(sub_82741028) {
  nr::BeforeD3DCall();
  const bool primary = Primary(ctx.r3.u32);
  const uint32_t buffer = ctx.r4.u32, predication = ctx.r5.u32;
  __imp__sub_82741028(ctx, base);
  if (primary && nr::Enabled()) nr::OnRunCommandBuffer(buffer, predication);
}
// D3DDevice_SetPixelShader(device, shader): whose microcode is whose, for the command buffers' shader loads.
REX_EXTERN(__imp__sub_8272FB50);
REX_HOOK_RAW(sub_8272FB50) {
  if (nr::Enabled()) nr::OnPixelShaderSet(ctx.r4.u32);
  __imp__sub_8272FB50(ctx, base);
}