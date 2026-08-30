#include "Decal.hlsli"

// Rings and order markers. The thruster glow used to branch out of here on decalParams.z, one draw
// per billboard; it is batched into the effect ring now and its arithmetic moved, unchanged, to
// FxGlowPS.
float4 main(VsOut i) : SV_Target
{
  float d = length(i.local);

  // Ring, antialiased against its own screen-space width so it stays crisp at any zoom and never
  // thins away to nothing.
  float aa = fwidth(d) + 1e-5;
  float halfWidth = max(decalParams.x, aa) * 0.5;
  float centre = 1.0 - halfWidth;
  float ring = 1.0 - smoothstep(halfWidth - aa, halfWidth + aa, abs(d - centre));
  float fill = decalParams.y * (1.0 - smoothstep(1.0 - aa, 1.0 + aa, d));
  float alpha = saturate(ring + fill) * decalColour.a;
  clip(alpha - 0.002);
  return float4(decalColour.rgb, alpha);
}
