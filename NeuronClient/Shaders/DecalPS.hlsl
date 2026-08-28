#include "Decal.hlsli"

float4 main(VsOut i) : SV_Target
{
  float d = length(i.local);

  if (decalParams.z > 0.5) // thruster glow: soft radial falloff, no edge
  {
    float glow = pow(saturate(1.0 - d), max(1.0, decalParams.y));
    clip(glow - 0.002);
    return float4(decalColour.rgb, glow * decalColour.a);
  }

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
