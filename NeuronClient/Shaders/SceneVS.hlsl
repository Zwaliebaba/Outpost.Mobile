#include "Scene.hlsli"

VsOut main(VsIn i)
{
  VsOut o;
  float4 wp = mul(float4(i.pos, 1.0), world);
  o.worldPos = wp.xyz;
  o.clip = mul(wp, viewProj);
  o.col = i.col;
  return o;
}
