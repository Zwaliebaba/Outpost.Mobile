#pragma once

#include <cmath>

namespace Game
{
// Where something is, in the one representation the simulation uses for it.
//
// Today it is a pair of plain floats and nothing more, because day-one content sits within a few
// kilometres of the origin, where a float still carries millimetre precision. The name arrives
// ahead of the machinery so that position has one definition instead of being spelled XMFLOAT3 in
// ShipState, in orders, in formations and in the index: the int64 sector pair that makes a universe
// of thousands of star systems representable is then a mechanical change behind this name rather
// than a rewrite of every caller (Design/Collision.md 3).
//
// There is no Y. The simulation is a plane; the height a hull is drawn hovering at is the view's.
struct WorldPos
{
  float localX = 0.0f;
  float localZ = 0.0f;
};

// The relative vector between two positions, always through these and never by subtracting the
// fields directly. That is the whole discipline the type exists to impose: the day localX grows a
// sector index beside it, the difference becomes sectorDelta * SECTOR_SIZE + localDelta in these
// four functions, and every caller in the tree is already correct.
[[nodiscard]] inline float OffsetX(const WorldPos& _from, const WorldPos& _to) noexcept
{
  return _to.localX - _from.localX;
}

[[nodiscard]] inline float OffsetZ(const WorldPos& _from, const WorldPos& _to) noexcept
{
  return _to.localZ - _from.localZ;
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

// Moves a position by a local displacement. Separate from operator+= because this is where the
// carry into the sector index will live, and a caller writing pos.localX += dx would not get it.
inline void Translate(WorldPos& _pos, float _dx, float _dz) noexcept
{
  _pos.localX += _dx;
  _pos.localZ += _dz;
}

// _a displaced by _t of the way towards _b. Computed through the offset rather than by
// interpolating the fields, so it stays right once a sector boundary can sit between the two.
[[nodiscard]] inline WorldPos Lerp(const WorldPos& _a, const WorldPos& _b, float _t) noexcept
{
  WorldPos result = _a;
  Translate(result, OffsetX(_a, _b) * _t, OffsetZ(_a, _b) * _t);
  return result;
}
} // namespace Game
