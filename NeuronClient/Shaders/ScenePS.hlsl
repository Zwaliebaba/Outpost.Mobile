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

  float lambert = saturate(dot(faceNormal, normalize(lightDirAmbient.xyz)));
  float3 lit = albedo * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);
  return float4(lit, 1.0);
}
