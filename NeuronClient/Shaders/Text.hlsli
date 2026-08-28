// The screen-space overlay: HUD text, and the untextured quads that share its pipeline by sampling
// the one atlas cell with no glyph in it.

cbuffer Root : register(b0)
{
  float2 invViewportPx;
};

Texture2D<float> Atlas : register(t0);
SamplerState Samp : register(s0);

struct VsIn
{
  float2 posPx : POSITION;
  float2 uv : TEXCOORD0;
  float4 col : COLOR0;
};

struct VsOut
{
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float4 col : COLOR0;
};
