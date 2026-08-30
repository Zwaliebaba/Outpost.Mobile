#include "Fx.hlsli"

// Thruster glows and trails, batched into the effect ring.
//
// This is DecalPS's glow branch, arithmetic for arithmetic, which is what makes the change that
// brought it here a batching change and not a look change. Two things moved: the quad-local
// coordinate comes from the vertex rather than from DecalVS's unit quad, because there is no
// per-glow matrix any more; and the colour comes from the vertex rather than a root constant,
// because that is the whole point -- a thousand glows of different colours in one draw.
//
// The falloff stays a constant, because it is one tuning number for the game rather than something
// a glow decides for itself. It rides in cameraPos.w, which the other effect passes leave at zero
// and never read.
float4 main(VsOut i) : SV_Target
{
  float d = length(i.uv);
  float glow = pow(saturate(1.0 - d), max(1.0, cameraPos.w));
  clip(glow - 0.002);
  return float4(i.col.rgb, glow * i.col.a);
}
