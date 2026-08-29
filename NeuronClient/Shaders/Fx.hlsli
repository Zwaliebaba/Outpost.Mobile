// Shared by the two effect passes -- tumbling hull fragments and camera-facing sprites. Both stages
// of both pairs include this, so the vertex output and the pixel input cannot drift apart.
//
// `row_major` matches XMFLOAT4X4's storage, so mul(rowVector, matrix) means what DirectXMath means
// by it -- the same reason Scene.hlsli gives.
//
// There is no world matrix. Every vertex arrives in world space already, because the CPU built it
// there (MeshShatter, SpriteParticles); that is what lets one buffer hold a thousand fragments from
// three different shatters and one draw call empty it.

cbuffer VsConstants : register(b0)
{
  row_major float4x4 viewProj;
};

cbuffer PsConstants : register(b1)
{
  float4 lightDirAmbient; // xyz towards the light, w ambient level -- the same meaning as Scene.hlsli
  float4 cameraPos;       // xyz eye
};

Texture2D FxTex : register(t0);
SamplerState WrapLinear : register(s0);
SamplerState ClampPoint : register(s1);

struct VsIn
{
  float3 pos : POSITION;
  float3 normal : NORMAL;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

struct VsOut
{
  float4 clip : SV_Position;
  float3 worldPos : TEXCOORD0;
  float3 normal : TEXCOORD1;
  float4 col : COLOR0;
  float2 uv : TEXCOORD2;
};
