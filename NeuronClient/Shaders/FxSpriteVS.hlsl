#include "Fx.hlsli"

// The same ten lines as FxFragmentVS today. It is its own file because a shader here is named for
// the stage it is and the pass it serves, and because the two part company the day the billboard
// corners are built on the GPU.
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
