#pragma once

#include "WorldPos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// The path lattice: where the cells are, as a property of the world rather than of any grid.
//
// A cell's index is a pure function of the position and of nothing else -- not of the obstacle set,
// not of which grid is asking. That is what stops the same architecture producing a different route
// because something was built a kilometre away, and it is what will let two grids overlap and agree
// about every cell they share (Design/RegionalPathfinding.md 3.1, 5).
//
// Exact, because PATH_CELLS_PER_SECTOR is exact and the local offset is inside one sector by the
// type's invariant. The index is a sector multiplied by 256, so it covers a factor of 256 less of
// the universe than a WorldPos does -- 2^55 sectors, about 3x10^20 m, thirty thousand light years.
// Past that it overflows, which is why this says so rather than claiming the whole range.
[[nodiscard]] std::int64_t PathCellX(const WorldPos& _pos) noexcept;
[[nodiscard]] std::int64_t PathCellZ(const WorldPos& _pos) noexcept;

// The centre of a lattice cell: the point a clearance is measured at, and the point A* searches.
// The inverse of the pair above over that range: PathCellX(PathCellCentre(x, z)) == x.
[[nodiscard]] WorldPos PathCellCentre(std::int64_t _cellX, std::int64_t _cellZ) noexcept;

// Routes around architecture. Local avoidance is not pathfinding, and large static structures are
// what makes the difference matter: a ship steering locally around a 503 m Structure will hug it,
// and can be trapped in a concave pocket or orbit it indefinitely. No amount of tuning in the
// steering fixes that, because the information needed -- that the way around is left, not right --
// is not available locally (Design/Archive/Collision.md 12).
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

  // How far along that run the clearance first fails, as a fraction of it, and greater than one when
  // it never does. IsClearBetween is this compared with one; the fraction itself is what tells a
  // caller holding several grids which of them the run meets *first*, which is the question a route
  // across more than one island turns on (Design/RegionalPathfinding.md 3.4).
  [[nodiscard]] float FirstBlockedFraction(const WorldPos& _from, const WorldPos& _to, float _requiredClearanceMetres) const;

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
  // The cell a position falls in, pulled into the window and flattened. Clamped rather than
  // rejected because a route may start or end outside the grid entirely, and the nearest edge cell
  // is the honest answer for both.
  [[nodiscard]] std::uint32_t ClampedCell(const WorldPos& _pos) const noexcept;
  [[nodiscard]] WorldPos CentreOf(std::uint32_t _cell) const noexcept;

  std::vector<float> m_clearance; // metres to the nearest obstacle surface, per cell
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  // Which cells of the world lattice this grid holds, as the index of its south-west one. A grid
  // chooses its window, never where the cells are: store a WorldPos origin instead and the lattice
  // becomes a function of the obstacles, which is the defect this shape exists to remove.
  std::int64_t m_originCellX = 0;
  std::int64_t m_originCellZ = 0;
  std::uint32_t m_version = 0;

  // What the last build was built from, so a rebuild with an unchanged obstacle set can leave the
  // version alone. The version is what makes every routed ship re-plan (World::AdvanceRoute), so
  // bumping it for a rebuild that changed nothing is a universe-wide replan for no reason.
  std::vector<Obstacle> m_built;

  // A* scratch, reused so a plan allocates nothing after the first one.
  struct Open
  {
    float estimate = 0.0f;  // f
    float travelled = 0.0f; // g
    std::uint32_t cell = 0;
  };

  // Total, and that is the whole point. Two cells with equal f and equal g must still order the
  // same way on every run, or the same static set and the same endpoints would produce different
  // paths and recorded outcomes would stop reproducing (Design/Archive/Collision.md 12).
  [[nodiscard]] static bool Worse(const Open& _a, const Open& _b) noexcept;
  std::vector<float> m_travelled;
  std::vector<std::uint32_t> m_cameFrom;
  std::vector<std::uint8_t> m_visited;
  std::vector<Open> m_open;
  std::vector<std::uint32_t> m_cellPath;
};

// Whether two obstacle sets are the same obstacles in the same order. Order counts because a set
// comes out of the static store in array order, and a set that has been permuted is a set whose ids
// have moved -- which is a change a router has to hear about. All five fields, compared exactly: two
// positions a whole sector apart share their local offsets, so comparing those alone would call them
// equal, and the question here is "did this change" rather than "is this close".
//
// Public because both PathGrid and PathIslands gate a rebuild on it, at their own level: the outer
// one decides whether the world changed at all, the inner whether this island did.
[[nodiscard]] bool SameObstacles(std::span<const PathGrid::Obstacle> _a, std::span<const PathGrid::Obstacle> _b) noexcept;
} // namespace Game
