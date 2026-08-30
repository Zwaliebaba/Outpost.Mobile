#pragma once

#include "BodyField.h"
#include "ColourRamp.h"
#include "FxVertex.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace Neuron
{
// What a build produced, so the composition root can trace it and a test can pin it.
struct BodyBuildStats
{
  std::uint32_t trianglesEmitted = 0;
  float maxHeightMetres = 0.0f;
};

// A BodyField turned into triangles: three unshared FxVertex each, one normal and one colour per
// triangle, uv the grid cell so that one outline tile covers one cell (Design/Archive/PlanetRenderer.md 8.2).
//
// Unshared vertices because there is no other way to get one colour and one normal per triangle
// without a provoking-vertex buffer, and because nothing in this tree carries an index buffer. The
// cost is stated in the design: a 65-grid planet is 4.1 MB.
//
// Device-free like everything else in the feature. It writes into a caller's vector; who uploads it,
// and which ramp it was given, are somebody else's decisions.
class BodyMeshBuilder
{
public:
  // What the engine draws when the game handed it no ramp. It is named here rather than in the
  // game's tuning header because it is not content -- it is what this code does with nothing.
  static constexpr DirectX::XMFLOAT3 BODY_FALLBACK_GREY{0.5f, 0.5f, 0.5f};

  // The width of the map the source's colour constants were tuned on. The dither strength below is
  // an expression in height, and it bends at the heights a 2 000-unit map had, so a height that is a
  // fraction of a radius here is multiplied up into those units before it goes in. This is the one
  // place the conversion survives: slice 1's amplitude law dropped it when the term it fed turned
  // out to have no fixed point (Design/Archive/PlanetRenderer.md 5.2).
  static constexpr float SOURCE_MAP_SIZE = 2000.0f;

  // Appends _field's terrain to _outTerrain. A null _ramp bakes BODY_FALLBACK_GREY and traces once.
  //
  // Every body this builds is a rock. There was a second kind -- an ocean world, whose sea floor was
  // culled, whose coast was dipped under a sphere of water and which got that sphere as a second
  // mesh -- and it went when the one world that used it started wearing a picture instead
  // (Design/Decisions/0026). What is left is the path a dry body always took, unchanged: the pinned
  // vertex hash in BodyMeshTests is the same literal it was before an ocean existed anywhere here.
  static void Build(const BodyField& _field, const ColourRamp* _ramp, std::vector<FxVertex>& _outTerrain, BodyBuildStats& _outStats);

  // A smooth sphere in the body vertex format, for a world whose surface is a picture rather than a
  // generated height field. Two things differ from Build's output and both are the point:
  //
  //   - the normal is per *vertex*, not per triangle, so the globe shades smoothly. On a sphere the
  //     direction to a point is exactly its normal, so there is nothing to compute for it;
  //   - there is no uv. An equirectangular map cannot be carried on the vertex without a seam --
  //     the triangle that straddles longitude pi interpolates u from 1 back to 0 and smears the
  //     whole map backwards across itself -- so PlanetPS derives it per pixel from the direction.
  //
  // No field, no ramp, no ocean and no height: a sphere of this radius is the whole of it.
  static void BuildSphere(float _radiusMetres, std::uint32_t _gridPower, std::vector<FxVertex>& _outSphere);

  // Seeds one triangle's colour dither. Integer throughout -- no float anywhere in it -- so that the
  // grain is reproducible in HLSL, where integer arithmetic is exact on every GPU and float
  // arithmetic is not (Design/Archive/PlanetRenderer.md 17.3). Public because both a test and, one day, a
  // shader have to be able to pin it.
  [[nodiscard]] static constexpr std::uint32_t CellHash(std::uint64_t _seed, std::uint32_t _face, std::uint32_t _x,
                                                        std::uint32_t _z) noexcept
  {
    std::uint32_t hash = static_cast<std::uint32_t>(_seed) ^ static_cast<std::uint32_t>(_seed >> 32);
    hash ^= _face * 0x9E3779B9u;
    hash = (hash ^ (hash >> 16)) * 0x85EBCA6Bu;
    hash ^= _x * 0xC2B2AE35u;
    hash = (hash ^ (hash >> 13)) * 0x27D4EB2Fu;
    hash ^= _z;
    return hash ^ (hash >> 16);
  }
};
} // namespace Neuron
