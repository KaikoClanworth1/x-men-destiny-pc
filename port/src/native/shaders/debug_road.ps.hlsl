// Native renderer debug pixel shader: shows the texture in fetch slot 0 at
// TEXCOORD0 (SVR2011_NATIVE_DEBUG_PS=debug_tex, for the pixel shaders listed in
// SVR2011_NATIVE_DEBUG_PS_FOR). Same inputs as the converted shaders.
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
cbuffer SharedConstants : register(b2, space4) { uint4 g_ResourceIndices[32]; };

void main(
	in float4 iPos : SV_Position,
	in float4 iTexCoord0 : TEXCOORD0,
	in float4 iTexCoord1 : TEXCOORD1,
	in float4 iTexCoord2 : TEXCOORD2,
	in float4 iTexCoord3 : TEXCOORD3,
	in float4 iTexCoord4 : TEXCOORD4,
	in float4 iTexCoord5 : TEXCOORD5,
	in float4 iTexCoord6 : TEXCOORD6,
	in float4 iTexCoord7 : TEXCOORD7,
	in float4 iTexCoord8 : TEXCOORD8,
	in float4 iTexCoord9 : TEXCOORD9,
	in float4 iTexCoord10 : TEXCOORD10,
	in float4 iTexCoord11 : TEXCOORD11,
	in float4 iTexCoord12 : TEXCOORD12,
	in float4 iTexCoord13 : TEXCOORD13,
	in float4 iTexCoord14 : TEXCOORD14,
	in float4 iTexCoord15 : TEXCOORD15,
	in float4 iColor0 : COLOR0,
	in float4 iColor1 : COLOR1,
	in float4 iColor2 : COLOR2,
	in float4 iColor3 : COLOR3,
	in float4 iColor4 : COLOR4,
	in float4 iColor5 : COLOR5,
	in float4 iColor6 : COLOR6,
	in float4 iColor7 : COLOR7,
in uint iFace : SV_IsFrontFace,
	out float4 oC0 : SV_Target0)
{
	// (probe for the road material A6E85E82: red = texture 1's green, green = texture 1's blue - the layer
	// mask, blue = texture 2's red at five times the coordinates)
	const uint t1 = g_ResourceIndices[0].y & 0xFFFFFF, t2 = g_ResourceIndices[0].z & 0xFFFFFF;
	const uint s1 = g_ResourceIndices[24].y, s2 = g_ResourceIndices[24].z;
	float4 a = g_Texture2DDescriptorHeap[t1].Sample(g_SamplerDescriptorHeap[s1], iTexCoord1.xy);
	float4 b = g_Texture2DDescriptorHeap[t2].Sample(g_SamplerDescriptorHeap[s2], iTexCoord1.xy * 5.0);
	oC0 = float4(a.g, a.b, b.r, 1.0);
}