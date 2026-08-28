#include "Scene.hlsli"

float4 main(VsOut i) : SV_Target
{
  // Flat shading from screen-space derivatives, then faced towards the eye so triangle winding
  // cannot matter -- the OBJ import flips Z, which reverses it.
  float3 crossed = cross(ddx(i.worldPos), ddy(i.worldPos));
  float3 faceNormal = (dot(crossed, crossed) > 1e-12) ? normalize(crossed) : float3(0.0, 1.0, 0.0);
  if (dot(faceNormal, cameraPos.xyz - i.worldPos) < 0.0)
  {
    faceNormal = -faceNormal;
  }

  float3 albedo = lerp(baseColour.rgb, i.col, baseColour.w);
  float3 normal = faceNormal;

  if (gridParams.w > 0.5)
  {
    // The ground. Its grid is procedural, so grid spacing is a constant with no vertex rebuild and
    // the plane can follow the camera without the lines sliding across it.
    normal = float3(0.0, 1.0, 0.0);
    float2 cell = i.worldPos.xz / max(gridParams.x, 0.001);
    float2 toLine = abs(frac(cell - 0.5) - 0.5) / max(fwidth(cell), 1e-6);
    float onLine = 1.0 - saturate(min(toLine.x, toLine.y) - gridParams.y * 0.5);
    float fade = saturate(1.0 - length(i.worldPos.xz - cameraPos.xz) / max(gridParams.z, 0.001));
    albedo = lerp(baseColour.rgb, gridColour.rgb, onLine * fade * gridColour.a);
  }

  float lambert = saturate(dot(normal, normalize(lightDirAmbient.xyz)));
  float3 lit = albedo * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);
  return float4(lit, 1.0);
}
