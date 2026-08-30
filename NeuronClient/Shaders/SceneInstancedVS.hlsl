#include "Scene.hlsli"

// The same ten lines as SceneVS, with the world matrix and the tint coming from the instance stream
// rather than from root constants. That is the whole of instancing: five hundred ships sharing five
// hulls were five hundred one-instance draws, because the only place a per-object matrix could live
// was a root constant and a root constant is set per draw
// (Design/MmoScalabilityReview.md G2).
VsOut main(VsIn i, VsInstance n)
{
  // Built from its rows, which is what float4x4(a, b, c, d) does and what the four input elements
  // read: element k is bytes 16k..16k+15 of MeshInstance::world, and XMFLOAT4X4 stores rows.
  // No row_major here -- that qualifier describes a buffer's memory layout, and this is a local.
  float4x4 instanceWorld = float4x4(n.worldRow0, n.worldRow1, n.worldRow2, n.worldRow3);

  VsOut o;
  float4 wp = mul(float4(i.pos, 1.0), instanceWorld);
  o.worldPos = wp.xyz;
  o.clip = mul(wp, viewProj);
  o.col = i.col;
  o.tint = n.tint;
  return o;
}
