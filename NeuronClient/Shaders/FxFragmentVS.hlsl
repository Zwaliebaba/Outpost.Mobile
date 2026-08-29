#include "Fx.hlsli"

VsOut main(VsIn i)
{
  VsOut o;
  o.clip = mul(float4(i.pos, 1.0), viewProj);
  o.worldPos = i.pos;
  o.normal = i.normal;
  o.col = i.col;
  o.uv = i.uv;
  return o;
}
