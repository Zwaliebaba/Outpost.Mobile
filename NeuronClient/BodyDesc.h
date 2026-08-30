#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace Neuron
{
// The source's five flatten modes, over a spherical cap instead of a square
// (Design/Archive/PlanetRenderer.md 5.3). Subtract is a crater, Absolute a landing pad, Smooth erosion.
enum class FlattenMode : std::uint8_t
{
  Absolute,
  Add,
  Subtract,
  Subtract2,
  Smooth
};

// A continent: a spherical cap that generates its own terrain and is max-merged into the body, which
// is LandscapeTile and MergeTileIntoLandscape with a cap where the source had a square.
struct BodyTile
{
  DirectX::XMFLOAT3 centre{0.0f, 1.0f, 0.0f}; // unit direction
  float halfWidthRad = 1.0f;                  // angular radius of the cap
  float edgeFraction = 0.25f;                 // share of halfWidthRad over which the cap fades out
  float desiredHeight = 0.05f;                // the tile's maximum is rescaled to this
  float posY = 0.0f;                          // whole-tile lift
  float fractalDimension = 0.8f;              // source: m_fractalDimension
  float heightScale = 0.05f;                  // source: m_heightScale
  float lowlandSmoothing = 1.2f;              // source: m_lowlandSmoothingFactor
  bool ridged = false;                        // source: m_generationMethod != 0
};

// A crater, a basin or a landing pad: one spherical cap and what it does to the height inside it.
struct BodyFlatten
{
  DirectX::XMFLOAT3 centre{0.0f, 1.0f, 0.0f};
  float halfWidthRad = 0.1f;
  FlattenMode mode = FlattenMode::Subtract;
  float value = 0.02f;    // the pad height, the bowl depth, the add
  float threshold = 0.0f; // Subtract2 only: subtract where the height is above this
};

// Every number a planet or an asteroid is generated from (Design/Archive/PlanetRenderer.md 8.1). One
// description, one body, forever: the same BodyDesc produces the same height at the same direction on
// every machine and every run, which is what will let a server describe a world with sixteen bytes.
//
// **Every length here is a fraction of radiusMetres**, except radiusMetres itself. That is the rule
// that lets one catalogue row describe a 400 m world and a 1 200 m one, and it is why none of these
// fields carries a unit in its name (AGENTS.md R6): there is no unit to carry.
struct BodyDesc
{
  std::uint64_t seed = 0;
  float radiusMetres = 500.0f;
  DirectX::XMFLOAT3 ellipsoid{1.0f, 1.0f, 1.0f}; // per-axis radius ratios; (1, 1, 1) is a planet
  std::uint32_t gridPower = 6;                   // N = 2^gridPower + 1 samples along a face
  float heightScale = 0.05f;                     // the body's own, for tiles that do not override it
  float fractalDimension = 0.8f;
  float lowlandSmoothing = 1.2f;
  bool ridged = false;
  float outsideHeight = -0.02f; // the height outside every tile; below zero is an ocean world
  float maxHeight = 0.0f;       // colour scaling only; zero means "use the field's own maximum"
  float lumpiness = 0.0f;       // asteroids: a whole-body deformation, applied before the tiles
  std::vector<BodyTile> tiles;
  std::vector<BodyFlatten> flatten;
  float polarStrength = 0.0f; // how much of maxHeight a pole adds to the climate (5.6)
  float capStart = 0.75f;     // |sin latitude| at which the cap begins
  float capNoise = 0.1f;      // how ragged the cap's edge is
  float polarGeometry = 0.0f; // above zero, the caps have thickness as well as colour
  DirectX::XMFLOAT3 spinAxis{0.0f, 1.0f, 0.0f};
};
} // namespace Neuron
