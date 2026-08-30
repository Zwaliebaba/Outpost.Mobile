#include "Planet.hlsli"

static const float INV_TWO_PI = 0.15915494309189535;
static const float INV_PI = 0.31830988618379067;

float4 main(VsOut i) : SV_Target
{
  float3 d = normalize(i.objectPos);

  // The equirectangular lookup, derived here rather than carried on the vertex. A uv on the vertex
  // has to wrap from 1 back to 0 somewhere, and the ring of triangles that straddles that seam
  // interpolates the whole map backwards across itself -- a bright scar from pole to pole. Derived
  // per pixel there is no seam to straddle, because atan2 is continuous over the surface even where
  // its *value* is not.
  //
  // The one thing that discontinuity would normally cost is mip selection, which reads the screen
  // space derivatives of the uv and sees a whole texture's worth of change across the seam pixel.
  // The file carries a single mip, so there is no chain to select from and nothing goes wrong.
  float2 uv = float2(atan2(d.z, d.x) * INV_TWO_PI + 0.5, acos(clamp(d.y, -1.0, 1.0)) * INV_PI);
  float3 albedo = PlanetTex.Sample(WrapLinear, uv).rgb;

  // ambient + (1 - ambient) * lambert, which is ScenePS's and BodyPS's line: a globe, a body and a
  // hull under one light have to agree, and they only do if the ambient term is a floor rather than
  // an addition. The normal is interpolated, so the terminator is a smooth curve rather than the
  // staircase a per-triangle normal would give across a sphere this size.
  float3 n = normalize(i.normal);
  float lambert = saturate(dot(n, normalize(lightDirAmbient.xyz)));
  return float4(albedo * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert), 1.0);
}
