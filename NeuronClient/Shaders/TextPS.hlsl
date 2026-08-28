#include "Text.hlsli"

float4 main(VsOut i) : SV_Target
{
  // The atlas holds coverage, not colour: GDI drew white on black and the bake took the luminance.
  float coverage = Atlas.Sample(Samp, i.uv);
  return float4(i.col.rgb, i.col.a * coverage);
}
