#include "Body.hlsli"

float4 main(VsOut i) : SV_Target
{
  // The line mask is the texture's **alpha**, not its red: TriangleOutline.dds is white everywhere
  // in rgb and carries its ink in alpha. Multiply by tex.r and the whole body glows.
  float4 tex = OutlineTex.Sample(WrapLinear, i.uv);

  float3 n = normalize(i.normal);
  float3 l = normalize(lightDirAmbient.xyz);
  float3 v = normalize(cameraPos.xyz - i.worldPos);
  float lambert = saturate(dot(n, l));
  float spec = pow(saturate(dot(n, normalize(l + v))), overlayParams.w) * overlayParams.z;

  // The file has one mip and the tree generates none, so at distance a 128-texel line pattern under
  // one pixel sparkles. Rather than build a mip chain, the outline fades out as the cell shrinks --
  // the same fwidth device the ground grid anti-aliases itself with (Design/Archive/PlanetRenderer.md 6.3).
  float fade = saturate(1.0 - length(fwidth(i.uv)) * overlayParams.y);

  // Both channels carry tex.a, so the additive blend adds exactly nothing where there is no line.
  float3 rgb = (i.col.rgb * overlayParams.x * (lightDirAmbient.w + lambert) + spec) * tex.a;
  return float4(rgb * fade, tex.a * fade);
}
