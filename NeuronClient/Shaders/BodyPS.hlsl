#include "Body.hlsli"

float4 main(VsOut i) : SV_Target
{
  // The normal arrives per triangle from the mesh builder and is *not* faced towards the eye the way
  // ScenePS faces its derivative normal. ScenePS has to, because the OBJ import reverses winding;
  // here the builder guarantees the normal points outward, so the far side of a sphere comes out
  // correctly dark rather than lit as if it were the near side (Design/Archive/PlanetRenderer.md 7.2).
  float3 n = normalize(i.normal);
  float lambert = saturate(dot(n, normalize(lightDirAmbient.xyz)));

  // ambient + (1 - ambient) * lambert, which is ScenePS's line: a body and a hull under one light
  // have to agree, and they only do if the ambient term is a floor rather than an addition.
  float3 lit = i.col.rgb * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);
  return float4(lit, 1.0);
}
