#pragma once

#include "BodyField.h"
#include "ColourRamp.h"
#include "FxVertex.h"
#include "MeshData.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace Neuron
{
// What a build produced, so the composition root can trace it and a test can pin it.
struct BodyBuildStats
{
  std::uint32_t trianglesEmitted = 0;
  std::uint32_t trianglesCulled = 0; // sea level, slice 5; always zero until then
  float maxHeightMetres = 0.0f;
};

// A BodyField turned into triangles: three unshared FxVertex each, one normal and one colour per
// triangle, uv the grid cell so that one outline tile covers one cell (Design/PlanetRenderer.md 8.2).
//
// Unshared vertices because there is no other way to get one colour and one normal per triangle
// without a provoking-vertex buffer, and because nothing in this tree carries an index buffer. The
// cost is stated in the design: a 65-grid planet is 7.1 MB.
//
// Device-free like everything else in the feature. It writes into a caller's vector; who uploads it,
// and which ramp it was given, are somebody else's decisions.
class BodyMeshBuilder
{
public:
  // What the engine draws when the game handed it no ramp. It is named here rather than in the
  // game's tuning header because it is not content -- it is what this code does with nothing.
  static constexpr DirectX::XMFLOAT3 BODY_FALLBACK_GREY{0.5f, 0.5f, 0.5f};

  // The three numbers of Design/PlanetRenderer.md 5.5, as fractions of the radius. They are the
  // engine's port of the source's 0.3 and -10 on a 2 000-unit map, not a game's tuning: what they
  // encode is the ratio the source's coastline was built at, and a class that wanted a different
  // shoreline would change its heights, not these.
  static constexpr float BODY_SHORE_THRESHOLD = 0.0003f; // a corner below this is a coast
  static constexpr float BODY_SHORE_DIP = -0.01f;        // and is pushed to here, under the water

  // The width of the map the source's colour constants were tuned on. The dither strength below is
  // an expression in height, and it bends at the heights a 2 000-unit map had, so a height that is a
  // fraction of a radius here is multiplied up into those units before it goes in. This is the one
  // place the conversion survives: slice 1's amplitude law dropped it when the term it fed turned
  // out to have no fixed point (Design/PlanetRenderer.md 5.2).
  static constexpr float SOURCE_MAP_SIZE = 2000.0f;

  // Appends _field's terrain to _outTerrain and, for a body whose outsideHeight is below zero, its
  // ocean sphere to _outOcean. A null _ramp bakes BODY_FALLBACK_GREY and traces once.
  //
  // The ocean is MeshVertex and not FxVertex because it goes through SceneRenderer::DrawMesh: it
  // needs no texture, no normal and no pipeline of its own, and the scene pass's derivative shading
  // gives it the same facets as everything else (Design/PlanetRenderer.md 5.5). A dry body leaves
  // _outOcean untouched and its terrain is bitwise what it was before the ocean existed, which is a
  // test rather than a claim.
  static void Build(const BodyField& _field, const ColourRamp* _ramp, std::vector<FxVertex>& _outTerrain,
                    std::vector<MeshVertex>& _outOcean, const DirectX::XMFLOAT3& _oceanColour, BodyBuildStats& _outStats);

  // Seeds one triangle's colour dither. Integer throughout -- no float anywhere in it -- so that the
  // grain is reproducible in HLSL, where integer arithmetic is exact on every GPU and float
  // arithmetic is not (Design/PlanetRenderer.md 17.3). Public because both a test and, one day, a
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
