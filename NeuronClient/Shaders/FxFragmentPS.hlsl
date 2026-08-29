#include "Fx.hlsli"

float4 main(VsOut i) : SV_Target
{
  // GL_DECAL: where the wireframe texture has alpha, its colour replaces the panel's; where it does
  // not, the panel's own colour is what shows. That is what makes a shard read as a piece of hull
  // with a glowing edge rather than a grey sliver.
  float4 tex = FxTex.Sample(WrapLinear, i.uv);
  float3 rgb = lerp(i.col.rgb, tex.rgb, tex.a);

  // Faced towards the eye, as ScenePS does, so triangle winding cannot matter -- a fragment is
  // double-sided and is seen from whichever side its tumble happens to present.
  float3 n = normalize(i.normal);
  if (dot(n, cameraPos.xyz - i.worldPos) < 0.0)
  {
    n = -n;
  }

  // The scene pass's lighting line, so a shard is lit exactly as the hull it came off was.
  float lambert = saturate(dot(n, normalize(lightDirAmbient.xyz)));
  float3 lit = rgb * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);
  return float4(lit, i.col.a);
}
