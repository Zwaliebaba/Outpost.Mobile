#include "Sky.hlsli"

static const float TWO_PI = 6.28318530718;

VsOut main(VsIn i)
{
  float3 dir = normalize(i.dir);

  // The quad's half-extent in meters at the sky radius. tan, not the small-angle approximation: a
  // star would not notice the difference, and a nebula reaches sixteen degrees, where it is four
  // percent.
  float halfSize = tan(i.size.x) * skyParams.x;

  // The roll turns the quad in its own plane. A star passes zero and this collapses to the identity;
  // a cloud and a flare pass an angle, which is what stops one 128-pixel picture repeated a hundred
  // times reading as one 128-pixel picture repeated a hundred times.
  float s, c;
  sincos(i.size.y, s, c);
  float2 rolled = float2(i.corner.x * c - i.corner.y * s, i.corner.x * s + i.corner.y * c);

  // Centered on the eye, so the sky never gets nearer however far the camera travels. The pass draws
  // first and with the depth test off, so the radius has nothing to clear but the far plane.
  float3 centre = cameraPos.xyz + dir * skyParams.x;
  float3 world = centre + cameraRight.xyz * (rolled.x * halfSize) + cameraUp.xyz * (rolled.y * halfSize);

  // Scintillation, out of the three numbers the vertex carries and one the frame does. A cloud's
  // amount is zero, which makes this the identity rather than a branch.
  float scint = 1.0 + i.twinkle.x * sin(skyParams.y * (i.twinkle.y * skyParams.w) + i.twinkle.z * TWO_PI);

  VsOut o;
  o.clip = mul(float4(world, 1.0), viewProj);
  // The *unrolled* corner, so the texture turns with the quad instead of staying upright inside it.
  o.uv = i.corner * 0.5 + 0.5;
  o.col = i.col.rgb * scint * skyParams.z;
  return o;
}
