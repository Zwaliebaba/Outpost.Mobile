#pragma once

#include "SimTuning.h"
#include "UniversePos.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>

namespace Game
{
// The geometry of a patrol ring, as two functions and no state.
//
// The composition root spawns ships onto these points and Universe::StepPatrols steers between them,
// so one definition owns the ring: a root that laid its ships out with its own arithmetic would put
// them somewhere the pass then walks away from on the first leg.

// Waypoint _index of a ring of PATROL_RING_WAYPOINTS around _anchor. Index 0 is due north (+Z) and
// increasing it walks north -> east -> south, which is clockwise on a north-up map and matches the
// headingRad convention ShipState uses. The index wraps, so a caller never has to.
//
// Built through Translate rather than by writing localX, because localX is an offset inside a sector
// and a ring around a station near a boundary would otherwise leave the invariant behind.
[[nodiscard]] inline UniversePos PatrolRingPoint(const UniversePos& _anchor, std::uint32_t _index, float _ringRadiusMetres) noexcept
{
  const float theta = static_cast<float>(_index % PATROL_RING_WAYPOINTS) * DirectX::XM_2PI / static_cast<float>(PATROL_RING_WAYPOINTS);
  UniversePos point = _anchor;
  Translate(point, std::sin(theta) * _ringRadiusMetres, std::cos(theta) * _ringRadiusMetres);
  return point;
}

// The direction of travel at that waypoint: the tangent, theta + pi/2. What a ship spawned onto the
// ring should be facing, so that its first leg does not begin with a turn it never needed.
[[nodiscard]] inline float PatrolRingHeadingRad(std::uint32_t _index) noexcept
{
  const float theta = static_cast<float>(_index % PATROL_RING_WAYPOINTS) * DirectX::XM_2PI / static_cast<float>(PATROL_RING_WAYPOINTS);
  return theta + DirectX::XM_PIDIV2;
}

// The ring point nearest a position's bearing from the anchor. What AssignPatrol enters the ring at,
// so an assignment never teleports intent -- a ship already flying near waypoint 7 is not sent all
// the way round to 0.
[[nodiscard]] inline std::uint32_t NearestPatrolRingIndex(const UniversePos& _anchor, const UniversePos& _from) noexcept
{
  const float bearingRad = std::atan2(OffsetX(_anchor, _from), OffsetZ(_anchor, _from));
  float turns = bearingRad / DirectX::XM_2PI;
  if (turns < 0.0f)
    turns += 1.0f;
  const long nearest = std::lround(turns * static_cast<float>(PATROL_RING_WAYPOINTS));
  return static_cast<std::uint32_t>(nearest) % PATROL_RING_WAYPOINTS;
}
} // namespace Game
