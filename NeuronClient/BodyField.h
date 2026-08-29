#pragma once

#include "BodyDesc.h"
#include "BodyParams.h"
#include "Noise3.h"

#include <DirectXMath.h>

#include <cstdint>

namespace Neuron
{
// The height of a planet or an asteroid at a direction: the source's amplitude law sampled in three
// dimensions, continents as spherical caps that max-merge, craters and pads as flatten areas, and
// polar caps (Design/PlanetRenderer.md 5).
//
// The constructor draws every random number the body needs -- once, in a fixed order -- and flattens
// the description into a BodyParams. After it returns, Height and Climate are pure functions of a
// direction: they can be evaluated in any order, on any thread, as often as a caller likes, and two
// fields built from the same BodyDesc answer identically. That is the property the whole feature
// rests on, and it is the reason the drawing and the evaluating are separated at all.
//
// Device-free by construction: Pcg32, Noise3 and DirectXMath, and nothing else. The day the
// simulation needs a planet's surface -- a colony pad that must be flat, a ray against terrain --
// this file moves to NeuronCore as a move rather than a rewrite (Design/PlanetRenderer.md 4).
class BodyField
{
public:
  // N = 2^gridPower + 1 per face, and one noise octave per grid level. Eight is 257 samples a side,
  // 396 000 over the six faces; a description asking for more is clamped and traced, because the
  // cost of the next one up is four times this and no body in the game wants it.
  static constexpr std::uint32_t MIN_GRID_POWER = 1;
  static constexpr std::uint32_t MAX_GRID_POWER = 8;

  explicit BodyField(const BodyDesc& _desc);

  // The height above the ellipsoid surface at unit direction _d, in metres. The mesh builder places
  // the vertex at P = _d * ellipsoid * (radiusMetres + Height(_d)).
  [[nodiscard]] float Height(const DirectX::XMFLOAT3& _d) const noexcept;

  // The height the colour ramp is indexed by: the real height plus the polar lift, in metres, so
  // that a pole reads as cold as a summit does and the equator reads as its own altitude
  // (Design/PlanetRenderer.md 5.6). Equal to _height exactly when polarStrength is zero.
  [[nodiscard]] float Climate(const DirectX::XMFLOAT3& _d, float _height) const noexcept;

  // The field's maximum over the grid, in metres, measured while constructing -- or the
  // description's own maxHeight when it set one. It is what the ramp's v axis divides by, and what
  // the polar term is a fraction of.
  [[nodiscard]] float MaxHeight() const noexcept;

  [[nodiscard]] const BodyParams& Params() const noexcept;

private:
  [[nodiscard]] static BodyParams FlattenDesc(const BodyDesc& _desc);

  [[nodiscard]] float Octaves(const DirectX::XMFLOAT3& _d, std::uint32_t _tile) const noexcept;
  [[nodiscard]] float Lumpiness(const DirectX::XMFLOAT3& _d) const noexcept;
  [[nodiscard]] float Field(const DirectX::XMFLOAT3& _d) const noexcept;
  [[nodiscard]] float Flattened(const DirectX::XMFLOAT3& _d) const noexcept;
  [[nodiscard]] float PolarCap(const DirectX::XMFLOAT3& _d) const noexcept;
  [[nodiscard]] float CapFade(const DirectX::XMFLOAT3& _d, const BodyParams::Tile& _tile) const noexcept;

  void MeasureTiles() noexcept;
  void MeasureMaxHeight() noexcept;

  BodyParams m_params;
  Noise3 m_noise;
  float m_maxHeight = 0.0f;

  // desiredHeight divided by the tile's own measured maximum, so that desiredHeight means what it
  // says. Measured rather than predicted: no closed form gives the amplitude law's maximum back.
  float m_tileScale[BodyParams::MAX_TILES] = {};

  // Pow(len * 10, fractalDimension) * the compensated height scale, per tile per octave. It varies
  // with neither the direction nor the sample, and evaluating it inside the sample loop would put
  // a million calls to pow between the boot screen and the first frame.
  float m_octaveAmplitude[BodyParams::MAX_TILES][MAX_GRID_POWER] = {};
};
} // namespace Neuron
