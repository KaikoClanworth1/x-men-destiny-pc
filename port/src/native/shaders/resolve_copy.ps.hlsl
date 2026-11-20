// X-Men: Destiny native renderer: a resolve that a plain copy can't do, drawn
// from the plain copy (native_renderer.cpp ResolveThrough):
//  - part of a depth buffer as a one-channel float texture: the game resolves
//    a rectangle of its shadow depth target (2048 x 1024 of 2080 x 2080) into
//    a texture of the rectangle's size, and depth can only be copied whole;
//  - a colour resolve with an exponent bias (the game's HDR scene is resolved
//    at 1/32 for its bloom): every value times 2^bias.
#include "own_constants.hlsli"
OWN_TEXTURE_TABLES
#ifdef __spirv__
#define g_Texture OWN_CONSTANT(uint, 0)
#define g_Offset OWN_CONSTANT(float2, 8)
#define g_Scale OWN_CONSTANT(float2, 16)
#define g_Multiply OWN_CONSTANT(float4, 32)
#define g_Add OWN_CONSTANT(float4, 48)
#elif defined(SVR_D3D11)
cbuffer CopyConstants : register(b3) {
  uint2 g_Unused;
  float2 g_Offset;
  float2 g_Scale;
  float4 g_Multiply;
  float4 g_Add;
};
#define g_Texture 0
#else
cbuffer CopyConstants : register(b3, space4) {
  uint g_Texture;   // SRV heap index of the plain copy
  uint g_Unused;
  float2 g_Offset;  // the rectangle's corner in the copy, texels
  float2 g_Scale;   // copy texels per output pixel
  float4 g_Multiply;
  float4 g_Add;
};
#endif

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
  Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_Texture];
  const int2 at = int2(g_Offset + (pos.xy - 0.5) * g_Scale + 0.5);
  return tex.Load(int3(at, 0)) * g_Multiply + g_Add;
}
