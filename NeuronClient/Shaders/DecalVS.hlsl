#include "Decal.hlsli"

VsOut main(VsIn i)
{
  VsOut o;
  float4 wp = mul(float4(i.pos, 1.0), world);
  o.clip = mul(wp, viewProj);
  o.local = i.pos.xz * 2.0; // the unit quad spans +-0.5, so this lands on +-1
  return o;
}
