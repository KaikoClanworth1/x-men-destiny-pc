// X-Men: Destiny - the game's Direct3D (Xbox 360 XDK) as a native renderer
// reads it from guest memory.
//
// The library is linked into the game without symbols, at about
// 0x82725000-0x8274F000. Its functions were found by matching them against
// SmackDown vs. Raw 2011's, where they are known (tools/d3d_match.py ->
// runs/d3d_match.csv), and checked in the running game (src/d3d_probe.cpp,
// XMD_D3D_PROBE=1). All values are big-endian in guest memory.

#pragma once

#include <cstdint>

namespace xmd::guest {

// The game's D3D device is r3 of every device call. Its address is fixed for a
// session but not across builds (0x40069900 and 0x40069A00 were seen): take it
// from Present's r3, never from a constant.

// First word of every D3D object: low 4 bits = resource type.
enum class ResourceType : uint32_t {
  kVertexBuffer = 1,
  kIndexBuffer = 2,
  kTexture = 3,
  kSurface = 4,
  kVertexDeclaration = 5,
  kVertexShader = 6,
  kPixelShader = 7,
};

// The device's mirror of the GPU registers it programs, in groups (seen in the
// (register, mirror) pairs the library's own flush functions get - the same
// layout as SvR 2011's library). Register `reg` of a group is at
// device + device_offset + (reg - first_register) * 4.
struct RegisterGroup {
  uint32_t first_register;
  uint32_t device_offset;
  uint32_t count;
};

constexpr RegisterGroup kRegisterGroups[] = {
    {0x4800, 0x0480, 0x00C0},  // fetch constants: 32 slots x 6 dwords (not seen flushed yet: SvR's offset)
    {0x4000, 0x0780, 0x0800},  // float constants: vertex c0-c255 (+0x0780), pixel (0x4400: +0x1780)
    {0x4900, 0x2780, 0x0008},  // boolean constants
    {0x2000, 0x2880, 0x0013},  // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, ...
    {0x2100, 0x28CC, 0x0016},  // index limits, colour mask, blend colour, stencil, alpha ref, viewport
    {0x2180, 0x2920, 0x0005},  // SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, interpolators
    {0x2200, 0x2934, 0x000D},  // RB_DEPTHCONTROL, RB_BLENDCONTROL0-3, RB_COLORCONTROL, cull, ...
    {0x2280, 0x2964, 0x0015},  // point/line size, tessellation, ...
    {0x2300, 0x29B8, 0x0026},  // AA config, SQ_VS/PS_CONST, RB_COPY_* (resolve)
    {0x2380, 0x2A50, 0x0004},  // polygon offset
};

// Other device fields (the Set calls' objects, found by value after each call).
constexpr uint32_t kDeviceCurrentPixelShader = 0x328C;   // guest pointer (type 7)
constexpr uint32_t kDeviceCurrentVertexShader = 0x3290;  // guest pointer (type 6)
constexpr uint32_t kDeviceIndexBuffer = 0x318C;          // set by SetIndices
// The vertex declaration, for shaders not bound to a format (SvR's offset + 0x48 like the others; no call
// setting it was found - the renderer checks the object's type before trusting it).
constexpr uint32_t kDeviceVertexDeclaration = 0x2F20;

// Shader objects (see the note below).
constexpr uint32_t kPixelShaderMicrocode = 0x18;    // physical address
constexpr uint32_t kPixelShaderContainer = 0x28;    // copy of the container's virtual part
constexpr uint32_t kVertexShaderMicrocode = 0x20;   // physical address
constexpr uint32_t kVertexShaderBound = 0x28;       // -1: not bound to a vertex format
constexpr uint32_t kVertexShaderContainer = 0x368;  // copy of the container's virtual part (flag 0x40: bound)

constexpr uint32_t RegisterOffset(uint32_t reg) {
  for (const auto& g : kRegisterGroups) {
    if (reg >= g.first_register && reg < g.first_register + g.count) {
      return g.device_offset + (reg - g.first_register) * 4;
    }
  }
  return 0;
}

// Shader objects. A pixel shader object (type 7) has its microcode's physical
// address at +0x18 and a copy of its container's header (0x102A1100 ...) from
// +0x28; a vertex shader object (type 6) has the microcode address at +0x20.
// Most are built in place by the engine from its own archives - only a few go
// through CreateVertexShader / CreatePixelShader - and no vertex declaration
// is ever set: the vertex formats are already in the shaders' fetch
// instructions.

namespace fn {
// certain: the only caller of VdSwap, once a frame
constexpr uint32_t kPresent = 0x82738208;
// checked in the running game (arguments and call counts as expected)
constexpr uint32_t kCreateVertexShader = 0x8272FE08;
constexpr uint32_t kCreatePixelShader = 0x8272FA60;
constexpr uint32_t kSetVertexShader = 0x8272FEF8;
constexpr uint32_t kSetPixelShader = 0x8272FB50;
constexpr uint32_t kDrawIndexedVertices = 0x8273C300;  // r4 primitive, r7 index count
constexpr uint32_t kDrawVerticesUP = 0x8273B4A8;       // r4 primitive (8: rect list), r5 count, r6 stride
constexpr uint32_t kSetTexture = 0x8272F0A8;
constexpr uint32_t kSetStreamSource = 0x82728188;
constexpr uint32_t kSetIndices = 0x82728330;
constexpr uint32_t kClear = 0x8273A690;                // r4 flags (0x3F: colour + depth + stencil)
constexpr uint32_t kSetVertexShaderConstantF = 0x8272F790;  // r4 start, r5 data, r6 count
constexpr uint32_t kSetPixelShaderConstantF = 0x8272F6B8;
constexpr uint32_t kFlushShaderConstants = 0x8273E198;  // r4 dirty mask, r5 0x4000/0x4400, r6 mirror
constexpr uint32_t kWriteRegisterGroup = 0x8273DE00;    // r5 first register, r6 mirror
constexpr uint32_t kWriteRegisterRange = 0x8273E040;
constexpr uint32_t kBindShaderAtDraw = 0x8273ED38;
// From a census frame of the first level (tools/d3d_frame.py; 241 indexed draws,
// 13 quad draws, 19 resolves - a long post-processing chain):
constexpr uint32_t kResolve = 0x827390F8;          // r4 flags (0 colour, 4 depth, 0x14, 0x100), r6 destination texture
constexpr uint32_t kSetRenderTarget = 0x827283C0;  // r4 index, r5 surface or 0 (set again before most passes: ~65 a frame)
// likely, from their arguments only:
constexpr uint32_t kSetDepthStencilSurface = 0x82728750;  // r4 surface or 0; ~11 a frame, beside the render target sets
constexpr uint32_t kBeginVerticesSetup = 0x82735988;      // before every quad draw (r4 0x15 / 0x40)
}  // namespace fn

}  // namespace xmd::guest
