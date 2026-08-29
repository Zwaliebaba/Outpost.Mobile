// Shared by SkyVS and SkyPS. The vertex stage's output is the pixel stage's input, so the two are
// declared once, here, and both files include this.
//
// `row_major` matches XMFLOAT4X4's storage, so mul(rowVector, matrix) means what DirectXMath means
// by it -- the same reason Scene.hlsli gives.
//
// There is no world matrix and there are no world positions. A vertex carries a *direction* on the
// celestial sphere and a corner index, and the quad is built here, against the camera's own right
// and up: the sky is a static buffer that the CPU touches once ever, and turning the camera changes
// nothing in it (SkyVertex.h, Design/Decisions/0021).

cbuffer VsConstants : register(b0)
{
  row_major float4x4 viewProj;
  float4 cameraRight; // xyz camera right in world space
  float4 cameraUp;    // xyz camera up in world space
  float4 cameraPos;   // xyz eye
  float4 skyParams;   // x sky radius in meters, y time in seconds, z master intensity, w maximum twinkle rate rad/s
};

Texture2D SkyTex : register(t0);
SamplerState ClampLinear : register(s0);

struct VsIn
{
  float3 dir : POSITION;     // unit direction to the billboard's center
  float2 corner : TEXCOORD0; // the quad corner before the roll, +-1
  float4 col : COLOR0;       // rgb hue times intensity, a unused
  float4 twinkle : COLOR1;   // x amount, y rate as a fraction of skyParams.w, z phase / 2pi
  float2 size : TEXCOORD1;   // x angular half-size in radians, y roll in radians
};

struct VsOut
{
  float4 clip : SV_Position;
  float2 uv : TEXCOORD0;
  float3 col : COLOR0;
};
