// Rings, order markers and thruster glows. Shares the scene pass's root signature -- 32 vertex
// DWORDs of matrices, 20 pixel DWORDs -- so the two pipelines can be swapped without rebinding,
// which is why the unused slots below are declared rather than removed.
//
// Every decal reads the same unit quad and is shaped entirely in the pixel shader, so radius,
// thickness and falloff stay plain constants with no geometry to rebuild.

cbuffer VsConstants : register(b0)
{
  row_major float4x4 world;
  row_major float4x4 viewProj;
};

cbuffer PsConstants : register(b1)
{
  float4 decalColour; // rgb, a
  float4 decalParams; // x thickness as a fraction of the radius, y fill or glow falloff, z 1 for a glow
  float4 unusedA;
  float4 unusedB;
  float4 cameraPos;
};

struct VsIn
{
  float3 pos : POSITION;
  float3 col : COLOR0;
};

struct VsOut
{
  float4 clip : SV_Position;
  float2 local : TEXCOORD0;
};
