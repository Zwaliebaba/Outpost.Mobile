#pragma once

#include "Pcg32.h"

#include <cmath>
#include <cstdint>
#include <span>

namespace Neuron
{
// Seeded Perlin-style gradient noise in three dimensions. Header-only, device-free, and the whole
// of what a body's shape is made of (Design/Archive/PlanetRenderer.md 5.2).
//
// The permutation is shuffled out of a Pcg32 rather than being the reference implementation's
// published static table. A fixed table is an unseeded constant hiding in a header: every body in
// the game would sample one function and differ only by where it started reading. Shuffled, two
// Noise3 built from generators seeded alike are the same function and two seeded differently are
// different functions -- on every machine and every build, which is what lets a world be described
// by a seed (Design/Archive/PlanetRenderer.md 10).
//
// Sample is pure float arithmetic -- multiply, add, floor, compare -- with no library call in it, so
// it carries that guarantee on its own rather than inheriting whichever vendor's libm was linked.
class Noise3
{
public:
  static constexpr std::uint32_t PERMUTATION_SIZE = 256;

  // Shuffles the permutation from _rng, drawing PERMUTATION_SIZE - 1 numbers out of it. A caller
  // that cares about the order of its draws should know this is the shuffle's cost.
  explicit Noise3(Pcg32& _rng) noexcept
  {
    for (std::uint32_t i = 0; i < PERMUTATION_SIZE; ++i)
      m_permutation[i] = i;

    // Fisher-Yates, descending, one Below(i + 1) per step. Descending because that is the form the
    // draw count is stated against above; ascending would be a different shuffle from the same
    // stream and every pinned height in the tests would move.
    for (std::uint32_t i = PERMUTATION_SIZE - 1; i > 0; --i)
    {
      const std::uint32_t j = _rng.Below(i + 1);
      const std::uint32_t swap = m_permutation[i];
      m_permutation[i] = m_permutation[j];
      m_permutation[j] = swap;
    }

    Mirror();
  }

  // Adopts a permutation shuffled elsewhere. BodyParams carries one because a compute kernel would
  // read it from there (Design/Archive/PlanetRenderer.md 17.3); adopting means the block and the noise the
  // CPU evaluates are one table by construction, rather than two copies that have to be kept equal.
  explicit Noise3(std::span<const std::uint32_t, PERMUTATION_SIZE> _permutation) noexcept
  {
    for (std::uint32_t i = 0; i < PERMUTATION_SIZE; ++i)
      m_permutation[i] = _permutation[i] & (PERMUTATION_SIZE - 1u);

    Mirror();
  }

  // In [-0.5, 0.5]. Continuous everywhere, exactly zero at every integer lattice point, and periodic
  // with period PERMUTATION_SIZE on each axis.
  [[nodiscard]] float Sample(float _x, float _y, float _z) const noexcept
  {
    const float floorX = std::floor(_x);
    const float floorY = std::floor(_y);
    const float floorZ = std::floor(_z);

    // The cast goes through int rather than straight to unsigned so a negative coordinate keeps its
    // sign through the truncation; the mask after it is what wraps it into the table.
    const std::uint32_t cellX = static_cast<std::uint32_t>(static_cast<std::int32_t>(floorX)) & (PERMUTATION_SIZE - 1u);
    const std::uint32_t cellY = static_cast<std::uint32_t>(static_cast<std::int32_t>(floorY)) & (PERMUTATION_SIZE - 1u);
    const std::uint32_t cellZ = static_cast<std::uint32_t>(static_cast<std::int32_t>(floorZ)) & (PERMUTATION_SIZE - 1u);

    const float x = _x - floorX;
    const float y = _y - floorY;
    const float z = _z - floorZ;

    const float u = Fade(x);
    const float v = Fade(y);
    const float w = Fade(z);

    const std::uint32_t a = m_permutation[cellX] + cellY;
    const std::uint32_t aa = m_permutation[a] + cellZ;
    const std::uint32_t ab = m_permutation[a + 1u] + cellZ;
    const std::uint32_t b = m_permutation[cellX + 1u] + cellY;
    const std::uint32_t ba = m_permutation[b] + cellZ;
    const std::uint32_t bb = m_permutation[b + 1u] + cellZ;

    const float x00 = Lerp(Grad(m_permutation[aa], x, y, z), Grad(m_permutation[ba], x - 1.0f, y, z), u);
    const float x10 = Lerp(Grad(m_permutation[ab], x, y - 1.0f, z), Grad(m_permutation[bb], x - 1.0f, y - 1.0f, z), u);
    const float x01 = Lerp(Grad(m_permutation[aa + 1u], x, y, z - 1.0f), Grad(m_permutation[ba + 1u], x - 1.0f, y, z - 1.0f), u);
    const float x11 =
      Lerp(Grad(m_permutation[ab + 1u], x, y - 1.0f, z - 1.0f), Grad(m_permutation[bb + 1u], x - 1.0f, y - 1.0f, z - 1.0f), u);

    // The reference's output spans [-1, 1]; the amplitude law in Design/Archive/PlanetRenderer.md 5.2 was
    // written against a diamond-square displacement of half that, so halve it here and nowhere else.
    return Lerp(Lerp(x00, x10, v), Lerp(x01, x11, v), w) * 0.5f;
  }

  // For a consumer that has to carry the same function elsewhere -- BodyParams, and through it the
  // compute kernel of Design/Archive/PlanetRenderer.md 17.
  [[nodiscard]] std::span<const std::uint32_t, PERMUTATION_SIZE> Permutation() const noexcept
  {
    return std::span<const std::uint32_t, PERMUTATION_SIZE>(m_permutation, PERMUTATION_SIZE);
  }

private:
  // Quintic 6t^5 - 15t^4 + 10t^3: zero first and second derivative at both ends, which is what keeps
  // the second derivative continuous across a lattice plane and the terrain free of creases.
  [[nodiscard]] static constexpr float Fade(float _t) noexcept
  {
    return _t * _t * _t * (_t * (_t * 6.0f - 15.0f) + 10.0f);
  }

  [[nodiscard]] static constexpr float Lerp(float _from, float _to, float _t) noexcept
  {
    return _from + (_to - _from) * _t;
  }

  // The twelve edge midpoints of the cube -- (+-1, +-1, 0) and its two rotations -- picked by the
  // low four bits of the hash. Sixteen codes over twelve directions, so four repeat; the reference
  // folds those four onto the diagonals that keep the set unbiased, and this is that fold.
  [[nodiscard]] static constexpr float Grad(std::uint32_t _hash, float _x, float _y, float _z) noexcept
  {
    const std::uint32_t h = _hash & 15u;
    const float u = (h < 8u) ? _x : _y;
    const float v = (h < 4u) ? _y : ((h == 12u || h == 14u) ? _x : _z);
    return ((h & 1u) == 0u ? u : -u) + ((h & 2u) == 0u ? v : -v);
  }

  // The second copy of the table, so the +1 lookups in Sample need no masking of their own.
  constexpr void Mirror() noexcept
  {
    for (std::uint32_t i = 0; i < PERMUTATION_SIZE; ++i)
      m_permutation[i + PERMUTATION_SIZE] = m_permutation[i];
  }

  std::uint32_t m_permutation[PERMUTATION_SIZE * 2] = {};
};
} // namespace Neuron
