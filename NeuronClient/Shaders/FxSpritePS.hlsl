#include "Fx.hlsli"

// One line, and both blends share it: the fade over a particle's life and the zero alpha the
// darkening pass needs were baked into the vertex on the CPU (SpriteParticles::Build), so there is
// nothing per-pixel left to decide.
float4 main(VsOut i) : SV_Target
{
  return FxTex.Sample(ClampPoint, i.uv) * i.col;
}
