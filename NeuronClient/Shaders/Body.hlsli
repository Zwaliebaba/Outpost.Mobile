// Shared by BodyVS and the two body pixel stages, so the vertex output and the pixel input cannot
// drift apart -- the same arrangement Scene.hlsli and Fx.hlsli have, for the same reason.
//
// `row_major` matches XMFLOAT4X4's storage, so mul(rowVector, matrix) means what DirectXMath means
// by it. Change that and every transform in the tree silently transposes.
//
// There *is* a world matrix here, unlike Fx.hlsli: a body spins, so its vertices stay in object
// space and the matrix carries the spin and the placement. It is a rotation and a translation only
// -- the ellipsoid is baked into the vertices (Design/Archive/PlanetRenderer.md 5.1) -- which is why the
// normal goes through the plain upper 3x3 and needs no inverse transpose.

cbuffer VsConstants : register(b0)
{
  row_major float4x4 world;
  row_major float4x4 viewProj;
};

cbuffer PsConstants : register(b1)
{
  float4 lightDirAmbient; // xyz towards the light, w ambient level -- Scene.hlsli's meaning exactly
  float4 cameraPos;       // xyz eye
  float4 overlayParams;   // x gain, y fade rate, z specular strength, w shininess
};

Texture2D OutlineTex : register(t0);
SamplerState WrapLinear : register(s0);

struct VsIn
{
  float3 pos : POSITION;
  float3 normal : NORMAL;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

// The normal and the colour are nointerpolation because they are already constant across a triangle
// -- the builder writes one of each to all three vertices -- so interpolating them would be work
// that cannot change an answer.
struct VsOut
{
  float4 clip : SV_Position;
  float3 worldPos : TEXCOORD0;
  nointerpolation float3 normal : TEXCOORD1;
  nointerpolation float4 col : COLOR0;
  float2 uv : TEXCOORD2;
};
