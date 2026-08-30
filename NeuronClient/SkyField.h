#pragma once

#include "SkyVertex.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace Neuron
{
// Which texture a billboard is drawn with, and -- because the three are contiguous in the buffer in
// this order -- the order they are drawn in. Nebula first because it is the faintest thing in the
// sky and everything else sits in front of it.
enum class SkyLayer : std::uint8_t
{
  Nebula, // CloudyGlow: the Milky Way's diffuse glow and the dust clouds off it
  Star,   // Glow: every star
  Burst,  // Starburst: the flare over the handful of brightest stars
};

inline constexpr std::uint32_t SKY_LAYER_COUNT = 3;

// One generated sky: the vertices of every billboard, and how many of them belong to each layer.
// The layers are contiguous and in SkyLayer order, so the renderer draws three ranges out of one
// buffer and the only thing it changes between them is which texture is bound.
struct SkyMesh
{
  std::vector<SkyVertex> verts;
  std::uint32_t layerVertexCount[SKY_LAYER_COUNT] = {};

  // Where layer _layer starts. Its length is layerVertexCount[_layer].
  [[nodiscard]] std::uint32_t LayerFirstVertex(SkyLayer _layer) const noexcept
  {
    std::uint32_t first = 0;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(_layer); ++i)
      first += layerVertexCount[i];
    return first;
  }
};

// The generator: a seed in, a sky out, and no device anywhere in it -- the same arrangement
// BodyField and SpriteParticles have, and for the same reason. What a star looks like is decided
// here and is decided by tests; what it costs to draw is decided in SkyRenderer.
//
// **It is generated once and never again.** Nothing about a star changes when the camera moves, so
// the whole sky is a static vertex buffer and the per-frame cost of it is three draw calls. The
// twinkle is the one thing that animates, and it animates in the vertex shader out of numbers baked
// into the vertex, so it costs no CPU work either (Design/Archive/Skybox.md 6).
//
// **What makes it look like a sky rather than scattered dots** is four things, and they are the
// whole of section 5 of the design:
//
//   - Magnitudes follow the real count law, N(<m) proportional to 10^(0.6m), so faint stars vastly
//     outnumber bright ones and a first-magnitude star is an event.
//   - Color is a blackbody hue over the naked-eye spectral mix, correlated with brightness because
//     the bright stars a person can see are luminosity-biased towards the hot end, and desaturated
//     towards white as the star dims because a faint star has no color to the eye.
//   - Stars cluster towards a galactic plane, thickest towards a galactic center, with dust lanes
//     cut out of the band by the same noise the planets are made of.
//   - The band's own unresolved glow, and the clouds off it, are CloudyGlow patches placed by the
//     same distribution and rolled so no two read as the same picture.
class SkyField
{
public:
  struct Desc
  {
    std::uint64_t seed = 0;
    std::uint32_t starCount = 14000;
    // The stars that also get a flare. They are the brightest the draw produced, not stars generated
    // separately: a sky whose bright stars were a different population has them in the wrong places.
    std::uint32_t brightStarCount = 24;
    std::uint32_t nebulaCount = 240;

    // The share of stars that belong to the band rather than to the field. The rest are spread
    // uniformly over the whole sphere, which is what keeps the sky away from the band populated.
    float bandFraction = 0.55f;
    // How fast the band thins out with galactic latitude: the density falls by 1/e over this angle.
    float bandScaleHeightRad = 0.13f;

    // The galactic pole, in render space (east, up, north), normalized on use. Tilted off vertical
    // on purpose: a band that ran level with the ground plane would read as a wall rather than as a
    // galaxy the outpost happens to be inside.
    DirectX::XMFLOAT3 galacticPole{0.74f, 0.38f, -0.55f};
    // Where the band is brightest, projected onto the galactic plane. Any direction not parallel to
    // the pole will do; it is made perpendicular on use.
    DirectX::XMFLOAT3 galacticCentre{-0.62f, 0.0f, 0.79f};
  };

  // Fills _out with a whole sky. Deterministic in _desc.seed and in nothing else: the same
  // description gives the same bytes on every machine and every run, which is what lets a
  // screenshot of a sky be reproduced from one number.
  static void Build(const Desc& _desc, SkyMesh& _out);

  // The blackbody hue of a star at _kelvin, normalized so its largest channel is 1 -- brightness is
  // the vertex's own and is not folded in here. Public because it is the one piece of this file with
  // an answer somebody can look up, and a test pins it against the Planckian locus.
  [[nodiscard]] static DirectX::XMFLOAT3 TemperatureColour(float _kelvin) noexcept;
};
} // namespace Neuron
