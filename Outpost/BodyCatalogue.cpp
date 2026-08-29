#include "pch.h"
#include "BodyCatalogue.h"

#include "ViewTuning.h"

using namespace DirectX;
using namespace Neuron;

namespace Outpost
{
namespace
{
[[nodiscard]] float Between(Pcg32& _rng, float _low, float _high) noexcept
{
  return _low + _rng.Float01() * (_high - _low);
}

[[nodiscard]] int Between(Pcg32& _rng, int _low, int _high) noexcept
{
  return _low + static_cast<int>(_rng.Below(static_cast<std::uint32_t>(_high - _low + 1)));
}

// A direction uniform over the sphere: the height of a band is what has to be uniform, not the
// latitude, or every continent bunches at the poles.
//
// The two draws are named locals and not two arguments, because the order a compiler evaluates
// arguments in is unspecified and this is a seeded draw (BodyField.cpp says the same at more length;
// gcc and clang built two different worlds out of one seed before that was fixed).
[[nodiscard]] XMFLOAT3 RandomDirection(Pcg32& _rng) noexcept
{
  const float height = _rng.Float01() * 2.0f - 1.0f;
  const float around = _rng.Float01() * XM_2PI;
  const float ring = std::sqrt(std::max(0.0f, 1.0f - height * height));
  return XMFLOAT3(ring * std::cos(around), height, ring * std::sin(around));
}
} // namespace

BodyDesc RandomBody(std::uint64_t _seed, BodyClass _class, float _radiusMetres)
{
  const BodyClassSpec& spec = BodyClassOf(_class);
  Pcg32 rng(_seed);

  BodyDesc desc;
  desc.seed = _seed;
  desc.radiusMetres = _radiusMetres;
  desc.gridPower = spec.asteroid ? BODY_ASTEROID_GRID_POWER : BODY_PLANET_GRID_POWER;
  desc.fractalDimension = BODY_FRACTAL_DIMENSION;
  desc.lowlandSmoothing = BODY_LOWLAND_SMOOTHING;
  desc.outsideHeight = spec.wet ? BODY_PLANET_OUTSIDE_HEIGHT_WET : BODY_PLANET_OUTSIDE_HEIGHT_DRY;
  desc.polarStrength = spec.polarStrength;
  desc.capStart = spec.capStart;
  desc.capNoise = BODY_CAP_NOISE;
  // Caps get thickness only where they have colour: a body with no cap would otherwise grow a bulge
  // at each pole out of a term that multiplies by zero everywhere it is visible.
  desc.polarGeometry = (spec.polarStrength > 0.0f) ? BODY_POLAR_GEOMETRY : 0.0f;

  // 1. The ellipsoid. A planet is a sphere; a rock is squashed on two axes, which is most of what
  //    makes it read as a rock rather than as a small planet.
  if (spec.asteroid)
  {
    const float squashY = Between(rng, BODY_ASTEROID_ELLIPSOID_MIN, 1.0f);
    const float squashZ = Between(rng, BODY_ASTEROID_ELLIPSOID_MIN, 1.0f);
    desc.ellipsoid = XMFLOAT3(1.0f, squashY, squashZ);
  }

  // 2. Lumpiness: a whole-body deformation under the terrain, so the silhouette is not a squashed
  //    sphere with bumps on it.
  desc.lumpiness = spec.asteroid ? BODY_ASTEROID_LUMPINESS : 0.0f;

  // 3. The relief. heightScale is what the roughness law measures a height against and desiredHeight
  //    is what the tile is rescaled to, and they are drawn as one number: the rescale divides the
  //    first back out, so two different values would make the second the only one that showed
  //    (Design/PlanetRenderer.md 5.2, 5.3).
  const float relief = spec.asteroid ? Between(rng, BODY_ASTEROID_HEIGHT_SCALE_MIN, BODY_ASTEROID_HEIGHT_SCALE_MAX)
                                     : Between(rng, BODY_PLANET_HEIGHT_SCALE_MIN, BODY_PLANET_HEIGHT_SCALE_MAX);
  desc.heightScale = relief;

  // 4. Continents. One tile over the whole rock for an asteroid; one to four caps for a planet.
  if (spec.asteroid)
  {
    BodyTile whole;
    whole.centre = XMFLOAT3(0.0f, 1.0f, 0.0f);
    whole.halfWidthRad = BODY_ASTEROID_TILE_HALF_WIDTH_RAD;
    whole.edgeFraction = BODY_ASTEROID_TILE_EDGE_FRACTION;
    whole.desiredHeight = relief;
    whole.heightScale = relief;
    whole.fractalDimension = BODY_FRACTAL_DIMENSION;
    whole.lowlandSmoothing = BODY_LOWLAND_SMOOTHING;
    desc.tiles.push_back(whole);
  }
  else
  {
    const int tileCount = Between(rng, BODY_PLANET_TILES_MIN, BODY_PLANET_TILES_MAX);
    for (int i = 0; i < tileCount; ++i)
    {
      BodyTile tile;
      tile.centre = RandomDirection(rng);
      tile.halfWidthRad = Between(rng, BODY_PLANET_TILE_HALF_WIDTH_MIN_RAD, BODY_PLANET_TILE_HALF_WIDTH_MAX_RAD);
      tile.edgeFraction = BODY_PLANET_TILE_EDGE_FRACTION;
      tile.desiredHeight = relief;
      tile.heightScale = relief;
      tile.fractalDimension = BODY_FRACTAL_DIMENSION;
      tile.lowlandSmoothing = BODY_LOWLAND_SMOOTHING;
      desc.tiles.push_back(tile);
    }
  }

  // 5. Craters, on asteroids only. A planet's 0-3 landing pads are in the design (5.4) and are not
  //    here: this slice's tuning block names no range for them and nothing places a colony yet. The
  //    Absolute mode they would use is built and tested; it is a catalogue row away.
  if (spec.asteroid)
  {
    const int craters = Between(rng, BODY_ASTEROID_CRATERS_MIN, BODY_ASTEROID_CRATERS_MAX);
    for (int i = 0; i < craters; ++i)
    {
      BodyFlatten bowl;
      bowl.centre = RandomDirection(rng);
      bowl.halfWidthRad = Between(rng, BODY_ASTEROID_CRATER_HALF_WIDTH_MIN_RAD, BODY_ASTEROID_CRATER_HALF_WIDTH_MAX_RAD);
      bowl.mode = FlattenMode::Subtract;
      bowl.value = Between(rng, BODY_ASTEROID_CRATER_DEPTH_MIN, BODY_ASTEROID_CRATER_DEPTH_MAX);
      desc.flatten.push_back(bowl);
    }
  }

  // 6. The tilt. The spin axis is also the axis the polar caps are measured from, so it is drawn
  //    once, here, and both the colour and the rotation read the same field.
  if (spec.asteroid)
  {
    desc.spinAxis = XMFLOAT3(0.0f, 1.0f, 0.0f); // a tumble has no axis to tilt
  }
  else
  {
    const float tiltX = XMConvertToRadians(rng.Signed(BODY_PLANET_TILT_MAX_DEG));
    const float tiltZ = XMConvertToRadians(rng.Signed(BODY_PLANET_TILT_MAX_DEG));
    XMFLOAT3 axis;
    XMStoreFloat3(&axis,
                  XMVector3Normalize(XMVector3TransformNormal(g_XMIdentityR1.v, XMMatrixRotationX(tiltX) * XMMatrixRotationZ(tiltZ))));
    desc.spinAxis = axis;
  }

  return desc;
}
} // namespace Outpost
