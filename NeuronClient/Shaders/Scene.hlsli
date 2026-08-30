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
  float4 baseColour;      // rgb livery, w highlight lift -- the non-instanced path's VsOut.tint
  float4 lightDirAmbient; // xyz towards the light, w ambient level
  float4 cameraPos;       // xyz eye
};

struct VsIn
{
  float3 pos : POSITION;
  float3 col : COLOR0;
  // 0 where the model paints this surface, 1 where the faction does (Design/Archive/NmoFormat.md 5.5). It
  // is per vertex because both kinds of surface are on one hull and a hull is one draw. Carried and
  // not yet read: ScenePS starts using it in the livery slice.
  float race : RACE;
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
  float4 tint : INSTANCETINT; // rgb livery, w highlight lift -- baseColour's two halves, per instance
};

struct VsOut
{
  float4 clip : SV_Position;
  float3 worldPos : TEXCOORD0;
  float3 col : COLOR0;
  // What baseColour used to be, carried from the vertex stage rather than read from a root constant.
  // An instanced draw has one root constant and many tints, so the tint has to travel with the
  // vertex; the non-instanced path writes the constant into it so one pixel shader serves both.
  //
  // rgb is the flying faction's livery and w is a highlight lift in 0..1. The same four floats said
  // something else before liveries -- a base colour and a material mix -- so read this rather than
  // assuming (Design/Archive/NmoFormat.md 5.5).
  float4 tint : COLOR1;
  float race : RACE;
};
