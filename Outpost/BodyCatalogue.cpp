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

// Every body this describes is a rock. The planet branches are gone with the generated planet
// (Design/Decisions/0026), and with them the polar caps: BodyDesc still declares polarStrength,
// capStart and polarGeometry, and this leaves all three at their defaults, which is zero strength --
// the term BodyField and BodyBake.hlsli compute from it then multiplies out to nothing. Cutting the
// machinery itself is a slice of its own, because those two are mirrored implementations pinned to
// each other byte for byte and have to change together.
BodyDesc RandomBody(std::uint64_t _seed, float _radiusMetres)
{
  Pcg32 rng(_seed);

  BodyDesc desc;
  desc.seed = _seed;
  desc.radiusMetres = _radiusMetres;
  desc.gridPower = BODY_ASTEROID_GRID_POWER;
  desc.fractalDimension = BODY_FRACTAL_DIMENSION;
  desc.lowlandSmoothing = BODY_LOWLAND_SMOOTHING;
  desc.outsideHeight = BODY_OUTSIDE_HEIGHT;

  // 1. The ellipsoid: squashed on two axes, which is most of what makes it read as a rock rather
  //    than as a small planet.
  const float squashY = Between(rng, BODY_ASTEROID_ELLIPSOID_MIN, 1.0f);
  const float squashZ = Between(rng, BODY_ASTEROID_ELLIPSOID_MIN, 1.0f);
  desc.ellipsoid = XMFLOAT3(1.0f, squashY, squashZ);

  // 2. Lumpiness: a whole-body deformation under the terrain, so the silhouette is not a squashed
  //    sphere with bumps on it.
  desc.lumpiness = BODY_ASTEROID_LUMPINESS;

  // 3. The relief. heightScale is what the roughness law measures a height against and desiredHeight
  //    is what the tile is rescaled to, and they are drawn as one number: the rescale divides the
  //    first back out, so two different values would make the second the only one that showed
  //    (Design/PlanetRenderer.md 5.2, 5.3).
  const float relief = Between(rng, BODY_ASTEROID_HEIGHT_SCALE_MIN, BODY_ASTEROID_HEIGHT_SCALE_MAX);
  desc.heightScale = relief;

  // 4. One tile over the whole rock.
  BodyTile whole;
  whole.centre = XMFLOAT3(0.0f, 1.0f, 0.0f);
  whole.halfWidthRad = BODY_ASTEROID_TILE_HALF_WIDTH_RAD;
  whole.edgeFraction = BODY_ASTEROID_TILE_EDGE_FRACTION;
  whole.desiredHeight = relief;
  whole.heightScale = relief;
  whole.fractalDimension = BODY_FRACTAL_DIMENSION;
  whole.lowlandSmoothing = BODY_LOWLAND_SMOOTHING;
  desc.tiles.push_back(whole);

  // 5. Craters.
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

  // 6. No tilt: a tumble has no axis to tilt.
  desc.spinAxis = XMFLOAT3(0.0f, 1.0f, 0.0f);
  return desc;
}
} // namespace Outpost
