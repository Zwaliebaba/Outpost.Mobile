#include "Scene.hlsli"

VsOut main(VsIn i)
{
  VsOut o;
  float4 wp = mul(float4(i.pos, 1.0), world);
  o.worldPos = wp.xyz;
  o.clip = mul(wp, viewProj);
  o.col = i.col;
  o.race = i.race;
  // Straight from the root constant. The instanced path takes it from the vertex instead, and this
  // is the line that lets both share one pixel shader.
  o.tint = baseColour;
  return o;
}
