#include "Body.hlsli"

VsOut main(VsIn i)
{
  VsOut o;
  float4 worldPos = mul(float4(i.pos, 1.0), world);
  o.clip = mul(worldPos, viewProj);
  o.worldPos = worldPos.xyz;
  o.normal = mul(i.normal, (float3x3)world);
  o.col = i.col;
  o.uv = i.uv;
  return o;
}
