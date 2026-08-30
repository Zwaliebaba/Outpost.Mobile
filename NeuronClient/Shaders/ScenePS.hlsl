#include "Scene.hlsli"

float4 main(VsOut i) : SV_Target
{
  // Flat shading from screen-space derivatives, then faced towards the eye so triangle winding
  // cannot matter. NMO specifies clockwise front faces (Design/NmoFormat.md 5.2), but nothing
  // here relies on it and the pass culls nothing, so a mesh wound either way shades the same.
  float3 crossed = cross(ddx(i.worldPos), ddy(i.worldPos));
  float3 faceNormal = (dot(crossed, crossed) > 1e-12) ? normalize(crossed) : float3(0.0, 1.0, 0.0);
  if (dot(faceNormal, cameraPos.xyz - i.worldPos) < 0.0)
  {
    faceNormal = -faceNormal;
  }

  // From the vertex rather than from baseColour directly: an instanced draw has one root constant
  // and a tint per ship, so SceneVS and SceneInstancedVS each put theirs here and this reads one
  // thing (Scene.hlsli).
  float3 albedo = lerp(i.tint.rgb, i.col, i.tint.w);

  float lambert = saturate(dot(faceNormal, normalize(lightDirAmbient.xyz)));
  float3 lit = albedo * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);
  return float4(lit, 1.0);
}
