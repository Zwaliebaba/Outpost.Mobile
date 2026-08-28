// Shared by SceneVS and ScenePS. The vertex stage's output is the pixel stage's input, so the two
// cannot be allowed to drift: they are declared once, here, and both files include this.
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
  float4 baseColour;       // rgb base colour, w material mix
  float4 lightDirAmbient;  // xyz towards the light, w ambient level
  float4 gridColour;       // rgb line colour, a strength
  float4 gridParams;       // x spacing, y line width px, z fade distance, w 1 for the ground
  float4 cameraPos;        // xyz eye
};

struct VsIn
{
  float3 pos : POSITION;
  float3 col : COLOR0;
};

struct VsOut
{
  float4 clip : SV_Position;
  float3 worldPos : TEXCOORD0;
  float3 col : COLOR0;
};
