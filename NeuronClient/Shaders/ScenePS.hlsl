#include "Scene.hlsli"

float4 main(VsOut i) : SV_Target
{
  // Flat shading from screen-space derivatives, then faced towards the eye so triangle winding
  // cannot matter. NMO specifies clockwise front faces (Design/Archive/NmoFormat.md 5.2), but nothing
  // here relies on it and the pass culls nothing, so a mesh wound either way shades the same.
  float3 crossed = cross(ddx(i.worldPos), ddy(i.worldPos));
  float3 faceNormal = (dot(crossed, crossed) > 1e-12) ? normalize(crossed) : float3(0.0, 1.0, 0.0);
  if (dot(faceNormal, cameraPos.xyz - i.worldPos) < 0.0)
  {
    faceNormal = -faceNormal;
  }

  // Two authorities paint a hull. i.col is the model's own -- plating, glass -- and stands as
  // authored. Where the material was RaceTinted (Design/Archive/NmoFormat.md 5.5) i.col is a *shade*
  // instead, and the livery in i.tint supplies the hue: one multiply, so the faction's colour
  // survives exactly and the shade ladder (plate < accent < thruster) survives with it.
  //
  // A multiply and not the lerp it replaces. The old rule mixed a tint against whatever the model
  // was painted, so the two hues argued -- a red tint over a green panel arrived at olive, which is
  // not a red ship. A multiply cannot argue, because the shade it multiplies has no hue to
  // contribute (Design/Decisions/0036).
  float3 albedo = lerp(i.col, i.tint.rgb * i.col, i.race);

  float lambert = saturate(dot(faceNormal, normalize(lightDirAmbient.xyz)));
  float3 lit = albedo * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);

  // What was the material mix is the selection highlight now, and it is a lift towards white rather
  // than a hue: under liveries a mint-green selected hull reads as a different faction, and the
  // player's own livery might be mint. Selection is a brightness and a ring; identity is a hue.
  lit = lerp(lit, float3(1.0, 1.0, 1.0), saturate(i.tint.w));
  return float4(lit, 1.0);
}
