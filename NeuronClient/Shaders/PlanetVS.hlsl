#include "Planet.hlsli"

VsOut main(VsIn i)
{
  VsOut o;
  float4 worldPos = mul(float4(i.pos, 1.0), world);
  o.clip = mul(worldPos, viewProj);

  // Object space, not world: the map has to turn with the globe, and the world matrix carries the
  // spin. Normalised in the pixel stage rather than here, because interpolating two unit vectors
  // does not give a unit vector and the lookup needs one.
  o.objectPos = i.pos;

  // The upper 3x3 is enough: a body's world matrix is a rotation and a translation, the ellipsoid
  // having been baked into the vertices, so there is no inverse transpose to take (Body.hlsli).
  o.normal = mul(i.normal, (float3x3)world);
  return o;
}
