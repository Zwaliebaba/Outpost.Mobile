#include "Text.hlsli"

float4 main(VsOut i) : SV_Target
{
  // The atlas holds coverage, not colour: the font is a bitmap mask and the tint is the vertex
  // colour, so one atlas serves every colour the HUD draws in.
  float coverage = Atlas.Sample(Samp, i.uv);
  return float4(i.col.rgb, i.col.a * coverage);
}
