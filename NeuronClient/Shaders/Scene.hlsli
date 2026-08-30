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
  float4 baseColour;      // rgb base colour, w material mix
  float4 lightDirAmbient; // xyz towards the light, w ambient level
  float4 cameraPos;       // xyz eye
};

struct VsIn
{
  float3 pos : POSITION;
  float3 col : COLOR0;
};

// The per-instance stream, at input slot 1, read by SceneInstancedVS alone. A matrix cannot be one
// input element, so it arrives as its four rows; `row_major` above is what makes assembling them in
// that order mean what XMFLOAT4X4 means by it.
struct VsInstance
{
  float4 worldRow0 : INSTANCEWORLD0;
  float4 worldRow1 : INSTANCEWORLD1;
  float4 worldRow2 : INSTANCEWORLD2;
  float4 worldRow3 : INSTANCEWORLD3;
  float4 tint : INSTANCETINT; // rgb base colour, w material mix -- baseColour's two halves, per instance
};

struct VsOut
{
  float4 clip : SV_Position;
  float3 worldPos : TEXCOORD0;
  float3 col : COLOR0;
  // What baseColour used to be, carried from the vertex stage rather than read from a root constant.
  // An instanced draw has one root constant and many tints, so the tint has to travel with the
  // vertex; the non-instanced path writes the constant into it so one pixel shader serves both.
  float4 tint : COLOR1;
};
