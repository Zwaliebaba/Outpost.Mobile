#include "Text.hlsli"

VsOut main(VsIn i)
{
  VsOut o;
  o.pos = float4(i.posPx.x * invViewportPx.x * 2.0 - 1.0, 1.0 - i.posPx.y * invViewportPx.y * 2.0, 0.0, 1.0);
  o.uv = i.uv;
  o.col = i.col;
  return o;
}
