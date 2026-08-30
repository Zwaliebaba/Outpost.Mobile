// Shared by PlanetVS and PlanetPS: a world whose surface is an authored equirectangular map rather
// than a generated height field. It is a separate pair from Body*, not a third pixel stage on
// Body.hlsli, because the two want opposite things from the same vertex -- Body.hlsli's normal is
// `nointerpolation` on purpose, since the mesh builder writes one per triangle and flat facets are
// what a generated body is meant to look like, and a textured globe wants that normal interpolated.
//
// It shares the *root signature*, so the constant blocks below have to be laid out exactly as
// Body.hlsli lays them out. Both pipelines are bound between one Begin and the next.
//
// `row_major` matches XMFLOAT4X4's storage, so mul(rowVector, matrix) means what DirectXMath means
// by it. Change that and every transform in the tree silently transposes.

cbuffer VsConstants : register(b0)
{
  row_major float4x4 world;
  row_major float4x4 viewProj;
};

cbuffer PsConstants : register(b1)
{
  float4 lightDirAmbient; // xyz towards the light, w ambient level -- Scene.hlsli's meaning exactly
  float4 cameraPos;       // xyz eye
  float4 overlayParams;   // the outline's, unread here; the block is shared and must not shift
};

Texture2D PlanetTex : register(t0);
SamplerState WrapLinear : register(s0);

// Only what this pair reads. The input layout still carries the colour and the uv, because it is
// FxVertex and one layout serves every body pipeline; an element the shader does not name is simply
// not fetched.
struct VsIn
{
  float3 pos : POSITION;
  float3 normal : NORMAL;
};

struct VsOut
{
  float4 clip : SV_Position;
  float3 objectPos : TEXCOORD0; // object space, so the map is fixed to the globe and turns with it
  float3 normal : TEXCOORD1;    // world space, interpolated -- this is what makes the shading smooth
};
