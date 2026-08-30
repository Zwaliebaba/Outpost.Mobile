#pragma once

#include "SimTuning.h"

#include <cmath>
#include <cstdint>

namespace Game
{
// Where something is, in the one representation the simulation uses for it.
//
// A sector index and an offset within it. The sector pair carries the range -- int64 sectors of
// 8,192 m each is +/-7.6x10^22 m, about eight million light years, past any number of star systems
// -- and the local offset carries the precision, a
// uniform 0.49 mm everywhere rather than the precision decay a single global float suffers with
// distance from its origin. Every line of float maths in the simulation runs in one sector's frame
// and never sees the difference (Design/Collision.md 3).
//
// The sector fields come FIRST, and that is deliberate rather than aesthetic. Brace-initialising a
// position as WorldPos{x, z} is ill-formed with an integer field in front of a float argument --
// [dcl.init.list] makes floating-point to integer narrowing in a braced list an error with no
// constant-expression exception, so even WorldPos{0.0f, 0.0f} is rejected. That property is what
// turned this slice from a hunt into a compile: the 94 two-argument constructions in the tree all
// failed at once and none could be missed. Reorder these fields and the next person to widen this
// struct loses that (Design/Archive/Collision-slice-8.md 5.1).
//
// There is no Y. The simulation is a plane; the height a hull is drawn hovering at is the view's.
struct WorldPos
{
  std::int64_t sectorX = 0;
  std::int64_t sectorZ = 0;
  float localX = 0.0f; // invariant: [0, SECTOR_SIZE_METRES), maintained by Translate
  float localZ = 0.0f;
};

// How many whole sectors a displacement carries: floor(_metres / SECTOR_SIZE_METRES), spelled out
// rather than handed to std::floor, which is not constexpr before C++23 and this tree is C++20.
//
// A cast truncates toward zero, which is right for a positive value and one short for a negative
// one that is not an exact multiple -- the case that decides whether a step west of a sector's
// origin belongs to the sector before it. The division is exact because SECTOR_SIZE_METRES is a
// power of two, so the multiplication back is the same quantity and the comparison is not a
// near-miss. One helper for both callers below, because a carry that disagreed with itself between
// construction and displacement would put the same point in two sectors.
[[nodiscard]] constexpr std::int64_t SectorCarry(float _metres) noexcept
{
  std::int64_t whole = static_cast<std::int64_t>(_metres / SECTOR_SIZE_METRES);
  if (static_cast<float>(whole) * SECTOR_SIZE_METRES > _metres)
    --whole;
  return whole;
}

// A position given as metres from the universe origin -- which is where all content sits, and how
// every test and every picked ground point spells one.
//
// It normalises, and that is not a nicety: LocalPos(0, -600) without it is sector zero at localZ
// -600, which denotes the right point while breaking the invariant the type promises, so the same
// place has two spellings and IsSamePosition says they are different. Every WorldPos in existence
// satisfies the invariant because these are the only two functions that ever set the fields.
[[nodiscard]] constexpr WorldPos LocalPos(float _x, float _z) noexcept
{
  const std::int64_t carryX = SectorCarry(_x);
  const std::int64_t carryZ = SectorCarry(_z);
  return WorldPos{carryX, carryZ, _x - static_cast<float>(carryX) * SECTOR_SIZE_METRES,
                  _z - static_cast<float>(carryZ) * SECTOR_SIZE_METRES};
}

// The relative vector between two positions, always through these and never by subtracting the
// fields directly. That is the whole discipline the type exists to impose, and it is what let the
// sector pair land here rather than in every caller.
//
// Exact while the two positions are within 2^24 sectors of each other -- 1.4 * 10^11 m -- and
// approximate beyond that. Nothing in this design goes near the limit: the query radius caps at
// 655 m (Design/Collision.md 10), an order of magnitude inside a single sector, so no interaction
// spans even two.
[[nodiscard]] inline float OffsetX(const WorldPos& _from, const WorldPos& _to) noexcept
{
  return static_cast<float>(_to.sectorX - _from.sectorX) * SECTOR_SIZE_METRES + (_to.localX - _from.localX);
}

[[nodiscard]] inline float OffsetZ(const WorldPos& _from, const WorldPos& _to) noexcept
{
  return static_cast<float>(_to.sectorZ - _from.sectorZ) * SECTOR_SIZE_METRES + (_to.localZ - _from.localZ);
}

[[nodiscard]] inline float DistanceSquared(const WorldPos& _a, const WorldPos& _b) noexcept
{
  const float dx = OffsetX(_a, _b);
  const float dz = OffsetZ(_a, _b);
  return dx * dx + dz * dz;
}

[[nodiscard]] inline float Distance(const WorldPos& _a, const WorldPos& _b) noexcept
{
  return std::sqrt(DistanceSquared(_a, _b));
}

// Moves a position by a local displacement, then carries whole sectors out of the offset so that
// the invariant on localX and localZ holds on every position the simulation ever stores.
//
// Eagerly, not lazily, and the reason is determinism rather than tidiness: leave the carry until
// something needs it and one point has many representations, which hash to different cells, compare
// unequal and sort differently. That is a replay divergence a long way from its cause.
//
// Through SectorCarry rather than a bare truncating cast, because the case that matters is
// negative: displacing a position at localX = 0 by -5 m must give the previous sector at
// SECTOR_SIZE_METRES - 5, and truncation would leave it in this sector at -5.
inline void Translate(WorldPos& _pos, float _dx, float _dz) noexcept
{
  _pos.localX += _dx;
  _pos.localZ += _dz;

  const std::int64_t carryX = SectorCarry(_pos.localX);
  const std::int64_t carryZ = SectorCarry(_pos.localZ);
  _pos.sectorX += carryX;
  _pos.sectorZ += carryZ;
  _pos.localX -= static_cast<float>(carryX) * SECTOR_SIZE_METRES;
  _pos.localZ -= static_cast<float>(carryZ) * SECTOR_SIZE_METRES;
}

// _a displaced by _t of the way towards _b. Computed through the offset rather than by
// interpolating the fields, so it stays right with a sector boundary between the two.
[[nodiscard]] inline WorldPos Lerp(const WorldPos& _a, const WorldPos& _b, float _t) noexcept
{
  WorldPos result = _a;
  Translate(result, OffsetX(_a, _b) * _t, OffsetZ(_a, _b) * _t);
  return result;
}
} // namespace Game
