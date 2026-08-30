// Rings and order markers. Shares the scene pass's root signature -- 32 vertex
// DWORDs of matrices, 12 pixel DWORDs -- so the two pipelines can be swapped without rebinding. The
// two blocks therefore have to agree on where cameraPos sits, and this is the other half of that
// agreement: change one and Scene.hlsli changes in the same commit.
//
// Every decal reads the same unit quad and is shaped entirely in the pixel shader, so radius and
// thickness stay plain constants with no geometry to rebuild.

cbuffer VsConstants : register(b0)
{
  row_major float4x4 world;
  row_major float4x4 viewProj;
};

cbuffer PsConstants : register(b1)
{
  float4 decalColour; // rgb, a
  float4 decalParams; // x thickness as a fraction of the radius, y fill; z and w unused
  float4 cameraPos;   // xyz eye -- the same slot Scene.hlsli puts it in
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
