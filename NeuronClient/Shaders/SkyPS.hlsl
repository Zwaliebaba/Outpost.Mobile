#include "Sky.hlsli"

float4 main(VsOut i) : SV_Target
{
  // The line mask is the texture's **alpha**, not its red. Glow, Starburst and CloudyGlow are all
  // white everywhere in rgb and carry their shape in alpha, exactly as TriangleOutline.dds does;
  // sample rgb and every star in the sky is a square.
  float shape = SkyTex.Sample(ClampLinear, i.uv).a;

  // The blend is ONE, ONE on color and ZERO, ONE on alpha, so what is returned here is added to the
  // frame and the destination's alpha is left alone. The alpha written is therefore never read.
  return float4(i.col * shape, 0.0);
}
