#pragma once

#include "Noise3.h"

#include <DirectXMath.h>

#include <cstdint>

namespace Neuron
{
// A BodyDesc with every random number already drawn and every vector flattened into fixed capacity:
// the block BodyField evaluates from, and the one a compute kernel would take as a constant buffer
// (Design/PlanetRenderer.md 8.1, 17.3).
//
// Every field is a float4 or a group of four uints because that is what HLSL constant-buffer packing
// wants, and obeying it now is what makes the bake of design 17.4 a memcpy rather than a rewrite. It
// costs nothing here: the block is written once per body and read a hundred thousand times.
//
// The split it draws is the one that matters for determinism. The CPU owns the Pcg32 and produces
// this block; everything downstream of it -- Height, Climate, and one day a shader -- is pure
// mathematics on it. Moving the evaluation to the GPU therefore cannot move the random draws.
struct BodyParams
{
  static constexpr std::uint32_t MAX_TILES = 8;
  static constexpr std::uint32_t MAX_FLATTEN = 32;
  static constexpr std::uint32_t MAX_OCTAVES = 8; // one per grid level, and BodyField::MAX_GRID_POWER is eight

  struct Tile
  {
    DirectX::XMFLOAT4 centreHalfWidth{0.0f, 1.0f, 0.0f, 1.0f};         // xyz centre direction, w halfWidthRad
    DirectX::XMFLOAT4 edgeDesiredPosYRidged{0.25f, 0.05f, 0.0f, 0.0f}; // x edgeFraction, y desiredHeight, z posY, w ridged
    DirectX::XMFLOAT4 fractal{0.8f, 0.05f, 1.2f, 0.0f};                // x fractalDimension, y heightScale, z lowlandSmoothing, w amplitude
  };

  struct Flatten
  {
    DirectX::XMFLOAT4 centreHalfWidth{0.0f, 1.0f, 0.0f, 0.1f};    // xyz centre direction, w halfWidthRad
    DirectX::XMFLOAT4 modeValueThreshold{0.0f, 0.0f, 0.0f, 0.0f}; // x FlattenMode, y value, z threshold, w unused
  };

  DirectX::XMFLOAT4 radiusEllipsoid{500.0f, 1.0f, 1.0f, 1.0f};    // x radiusMetres, yzw ellipsoid
  DirectX::XMFLOAT4 seedOffset{0.0f, 0.0f, 0.0f, 0.0f};           // xyz where in the noise this body reads, w lumpiness
  DirectX::XMFLOAT4 outsideMaxHeightGrid{0.0f, 0.0f, 6.0f, 6.0f}; // x outsideHeight, y maxHeight, z gridPower, w octaves
  DirectX::XMFLOAT4 polar{0.0f, 0.75f, 0.1f, 0.0f};               // x polarStrength, y capStart, z capNoise, w polarGeometry
  DirectX::XMFLOAT4 spinAxis{0.0f, 1.0f, 0.0f, 0.0f};             // xyz, w unused

  Tile tiles[MAX_TILES]{};
  Flatten flatten[MAX_FLATTEN]{};

  std::uint32_t tileCount = 0;
  std::uint32_t flattenCount = 0;

  // The description's seed, split in two because shader model 5.1 has no 64-bit integer. It seeds the
  // cap-edge dither, which is a pure function of direction and has to be the same draw on the CPU and
  // in the kernel; the two words are what the work order's pad0 and pad1 were reserving space for.
  std::uint32_t seedLow = 0;
  std::uint32_t seedHigh = 0;

  // Pow(len * 10, fractalDimension) * the compensated height scale, per tile per octave. It varies
  // with neither the direction nor the sample, so it is drawn once here rather than evaluated inside
  // the sample loop -- and putting it in the block rather than beside it is what lets a kernel read
  // it too. HLSL's pow is a twenty-one-bit exp2/log2 pair; six of them per sample would be the
  // largest error in a baked body, and there is no reason for the GPU to compute a constant.
  //
  // A float4 group per four octaves, so the array packs the same way in a constant buffer.
  DirectX::XMFLOAT4 octaveAmplitude[MAX_TILES][MAX_OCTAVES / 4]{};

  std::uint32_t permutation[Noise3::PERMUTATION_SIZE]{};

  // The two accessors the float4 packing costs. A constant buffer packs an array element to sixteen
  // bytes whatever its type, so eight floats per tile have to be written as two float4s; these are
  // what stop every call site from spelling that out, and they add no state to the block.
  [[nodiscard]] float OctaveAmplitude(std::uint32_t _tile, std::uint32_t _octave) const noexcept
  {
    const DirectX::XMFLOAT4& group = octaveAmplitude[_tile][_octave / 4u];
    const std::uint32_t within = _octave % 4u;
    return (within == 0u) ? group.x : ((within == 1u) ? group.y : ((within == 2u) ? group.z : group.w));
  }

  void SetOctaveAmplitude(std::uint32_t _tile, std::uint32_t _octave, float _value) noexcept
  {
    DirectX::XMFLOAT4& group = octaveAmplitude[_tile][_octave / 4u];
    const std::uint32_t within = _octave % 4u;
    if (within == 0u)
      group.x = _value;
    else if (within == 1u)
      group.y = _value;
    else if (within == 2u)
      group.z = _value;
    else
      group.w = _value;
  }
};

static_assert(sizeof(BodyParams) % 16 == 0, "BodyParams is laid out for a constant buffer");
} // namespace Neuron
