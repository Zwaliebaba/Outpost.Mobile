#pragma once

#include "WorldPos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// Routes around architecture. Local avoidance is not pathfinding, and large static structures are
// what makes the difference matter: a ship steering locally around a 503 m Structure will hug it,
// and can be trapped in a concave pocket or orbit it indefinitely. No amount of tuning in the
// steering fixes that, because the information needed -- that the way around is left, not right --
// is not available locally (Design/Collision.md 12).
//
// The input is the static store the index already keeps. Nothing mobile is ever an obstacle: ships
// route around architecture and *avoid* each other, and keeping those two problems apart is what
// keeps both of them small.
//
// Why this and not the alternatives: a tangent-visibility graph over inflated obstacle circles
// gives prettier paths for sparse convex obstacles and structurally cannot handle a station with an
// interior, because concave geometry breaks it. Flow fields are the right tool when hundreds of
// ships share one goal and are far too heavy as a first step; they can land later behind the same
// waypoint seam.
class PathGrid
{
public:
  struct Obstacle
  {
    WorldPos pos;
    float radiusMetres = 0.0f;
  };

  // Rebuilt on obstacle spawn and despawn only -- the same cadence the static index rebuilds on,
  // and never per tick.
  void Rebuild(std::span<const Obstacle> _obstacles);

  [[nodiscard]] bool HasObstacles() const noexcept
  {
    return !m_clearance.empty();
  }

  // Distance from this point to the nearest obstacle surface. Outside the grid the answer is "far",
  // which is true: the grid is built with a margin wider than anything worth routing around.
  //
  // Computed exactly, as the minimum over obstacles, rather than as a chamfer transform over a
  // rasterised occupancy. It is the same field and it is simpler; the approximation only starts
  // paying when obstacle counts are large, and it can replace this behind the same accessor.
  [[nodiscard]] float ClearanceAt(const WorldPos& _pos) const noexcept;

  // Whether a straight run between two points keeps the required clearance the whole way. This is
  // both the fast path -- most orders need no plan at all -- and what the string-pull is built on.
  [[nodiscard]] bool IsClearBetween(const WorldPos& _from, const WorldPos& _to, float _requiredClearanceMetres) const;

  // A route as a short waypoint list. True when the last waypoint is _to itself; false when the
  // route ran out of list before it got there, in which case the last waypoint is the furthest safe
  // point along it and the follower re-plans from there.
  //
  // Deterministic with nothing added, because it is a pure function of the static set and the two
  // endpoints -- provided the A* tie-break is total, which is why it is (f, g, cellIndex) for the
  // same reason every other ordering in this simulation is total.
  [[nodiscard]] bool FindPath(const WorldPos& _from, const WorldPos& _to, float _requiredClearanceMetres,
                              std::vector<WorldPos>& _outWaypoints);

  // Bumped on every rebuild. A follower that planned against an older one re-plans.
  [[nodiscard]] std::uint32_t Version() const noexcept
  {
    return m_version;
  }

private:
  // Both take an offset from m_origin, not a coordinate. Positions reach them through
  // OffsetX/OffsetZ so that a grid spanning a sector boundary indexes correctly.
  [[nodiscard]] std::int32_t ClampedCellX(float _offsetMetres) const noexcept;
  [[nodiscard]] std::int32_t ClampedCellZ(float _offsetMetres) const noexcept;
  [[nodiscard]] WorldPos CentreOf(std::uint32_t _cell) const noexcept;

  std::vector<float> m_clearance; // metres to the nearest obstacle surface, per cell
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  WorldPos m_origin; // the centre of cell (0, 0)
  std::uint32_t m_version = 0;

  // A* scratch, reused so a plan allocates nothing after the first one.
  struct Open
  {
    float estimate = 0.0f;  // f
    float travelled = 0.0f; // g
    std::uint32_t cell = 0;
  };

  // Total, and that is the whole point. Two cells with equal f and equal g must still order the
  // same way on every run, or the same static set and the same endpoints would produce different
  // paths and recorded outcomes would stop reproducing (Design/Collision.md 12).
  [[nodiscard]] static bool Worse(const Open& _a, const Open& _b) noexcept;
  std::vector<float> m_travelled;
  std::vector<std::uint32_t> m_cameFrom;
  std::vector<std::uint8_t> m_visited;
  std::vector<Open> m_open;
  std::vector<std::uint32_t> m_cellPath;
};
} // namespace Game
