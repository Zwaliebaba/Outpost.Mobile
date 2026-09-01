#pragma once

#include "PathGrid.h"
#include "UniversePos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// The universe's architecture, partitioned into islands, with one PathGrid over each.
//
// One grid over everything was the right first shape and does not survive a universe. It sweeps a
// single bounding box over every obstacle there is, so two stations 20 km apart ask for a grid of
// hundreds of millions of cells, it declines to build, and A* goes off for *every ship in the
// universe* rather than for the space between them. HullSpec.h is explicit that this is not a graceful
// degradation: a capital's look-ahead is deliberately shorter than its own turning circle, so a
// single distant outpost turns every Carrier in the game into a ship that flies into things
// (Design/Archive/RegionalPathfinding.md 1.1).
//
// Two obstacles belong to the same island when the gap between their surfaces is narrower than
// IslandGapMetres() -- narrower, that is, than the widest corridor a hull could need. A wider gap is
// one the straight-line test flies through on its own and needs no plan; a narrower one is a wall.
// The islands are the connected components of that relation, so the partition is a function of the
// obstacle set and of nothing else.
//
// The seam is PathGrid's, unchanged: Universe holds this instead of a grid and calls the same three
// things. What it gives up is optimality *between* islands -- a ship cannot see that going round
// island two is cheaper than through it until it gets there. That is the right trade at this scale;
// the alternative is a portal graph answering a question no content in this tree asks yet
// (Design/Archive/RegionalPathfinding.md 3.4, 6).

// What a route was planned against, carried by the route so that a later change can be asked
// whether it invalidated this one.
//
// The island is named by its lowest occupied path cell, NOT by its index. An index is not a handle:
// the partition is recomputed on every change and the islands are ordered by where they sit, so
// building anything renumbers every island after it and an index silently comes to mean a different
// island -- the ShipId failure of ADR 0005 with a worse symptom, which is a ship flying confidently
// into a station. A cell index is a universe coordinate and moves for nobody, and no two islands can
// share a lowest cell (Partition says why). ADR 0034 named this key as the design that eventually
// wins and declined to build it without a measured cost; ADR 0059 is that cost arriving.
//
// scopedToIsland is false when no single island owns the plan, and then `version` is the whole
// partition's. Two cases fall there and both must: a run that met NO island depends on the absence
// of architecture along it, so a new island anywhere can invalidate it; and a run that met SEVERAL
// is aimed at the open water between two of them, so it depends on where both sit. Only a run that
// met exactly one island can be scoped to it -- which is the common case, and the whole gain.
struct PlanStamp
{
  std::int64_t islandCellX = 0;
  std::int64_t islandCellZ = 0;
  std::uint32_t version = 0;
  bool scopedToIsland = false;
};

class PathIslands
{
public:
  // Rebuilt on obstacle spawn and despawn only -- the same cadence the static index rebuilds on,
  // and never per tick. Partitions first, then builds one grid per island.
  void Rebuild(std::span<const PathGrid::Obstacle> _obstacles);

  // Bumped when the architecture changes anywhere, and only then. It is what an unscoped plan is
  // checked against -- one that met no island, or several -- and it is also the number a rebuilt
  // island is stamped with, so island versions are drawn from one monotone sequence and two islands
  // can never be confused by having the same one.
  [[nodiscard]] std::uint32_t Version() const noexcept
  {
    return m_version;
  }

  // Whether a plan made against this partition is still a plan against this partition.
  //
  // Unscoped: the whole partition must be unchanged. Scoped: the island it names must still be here,
  // AND still be the island it was. Both halves are load-bearing and ADR 0034 named both --
  // an island that vanished or merged upward loses its key and fails the first; one that grew keeps
  // its key, so it must fail the second, which it does because growing means its obstacle set
  // changed, which means it was rebuilt, which means it carries the version of that rebuild.
  //
  // Fails closed: an unknown key re-plans. That is the direction that costs a route rather than the
  // one that flies a ship into a station.
  [[nodiscard]] bool IsStampCurrent(const PlanStamp& _stamp) const noexcept;

  // The version the island whose lowest cell is (_cellX, _cellZ) carries right now, or NO_ISLAND
  // when nothing has that key.
  //
  // For the state codec, which stores a route's currency as a RELATION rather than as a number --
  // a version is an epoch counter with no meaning outside the run that produced it -- and so has to
  // turn "it was current" back into this run's numbers on the way in. The key it stores IS
  // meaningful across runs, which is the second thing keying islands by cell buys.
  static constexpr std::uint32_t NO_ISLAND = 0xFFFFFFFFu;
  [[nodiscard]] std::uint32_t IslandVersion(std::int64_t _cellX, std::int64_t _cellZ) const noexcept;

  // A route as a short waypoint list, with PathGrid::FindPath's meaning exactly: true when the last
  // waypoint is _to itself, false when the last waypoint is the furthest safe point along the way
  // and the follower re-plans from there.
  //
  // Three cases (Design/Archive/RegionalPathfinding.md 3.4). The run is clear of every island, and the
  // answer is the destination. It meets exactly one, and that island plans it. It meets more than
  // one, and the first island plans as far as it can while the answer says the route is unfinished
  // -- because the second island is still in the way however well the first one planned, and an
  // unfinished route is what makes the follower come back for the rest.
  //
  // _outStamp says what the answer was planned against, for IsStampCurrent to check later. It is
  // written on every path, including the ones that return early, because a caller holding a stale
  // stamp from a previous plan is a caller that will not re-plan when it must.
  [[nodiscard]] bool FindPath(const UniversePos& _from, const UniversePos& _to, float _requiredClearanceMetres,
                              std::vector<UniversePos>& _outWaypoints, PlanStamp& _outStamp);

  // How many islands the architecture came to. For the F1 readout and for the tests that have to
  // say "these two stations are two islands and not one".
  [[nodiscard]] std::size_t IslandCount() const noexcept
  {
    return m_islands.size();
  }

  // How many islands the last rebuild actually built a clearance field for. The rest were handed the
  // obstacles they already held and kept the grid they had, which is the whole of slice 4: one
  // station moving in a universe of thirty pays for its own island and not for the universe
  // (Design/Archive/RegionalPathfinding.md 4). Read off a benchmark rather than inferred from a timing.
  [[nodiscard]] std::uint32_t RebuiltIslandCount() const noexcept
  {
    return m_rebuiltIslands;
  }

  // How many of them refused to build, because a single island is genuinely wider than
  // PATH_GRID_MAX_CELLS_PER_AXIS allows. Its neighbours keep routing -- that is the whole gain over
  // one grid -- but ships crossing *it* fall back to straight-line steering, and a declining island
  // is otherwise indistinguishable from open space. Counted so it can be read off the screen rather
  // than inferred from ships that stopped avoiding things (Design/Archive/RegionalPathfinding.md 3.3).
  [[nodiscard]] std::size_t DeclinedCount() const noexcept
  {
    std::size_t declined = 0;
    for (const PathGrid& island : m_islands)
    {
      if (island.Declined())
        ++declined;
    }
    return declined;
  }

  // The grid over one island, in the universe-fixed order Rebuild put them in. For diagnostics and
  // tests; routing goes through FindPath, which is the whole point of the class.
  [[nodiscard]] const PathGrid& Island(std::size_t _at) const noexcept
  {
    return m_islands[_at];
  }

private:
  // One island while it is being found: which union-find root it is, and the lowest path cell any
  // of its members sits in, which is what puts the islands in a universe-fixed order.
  struct Found
  {
    std::uint32_t root = 0;
    std::int64_t cellX = 0;
    std::int64_t cellZ = 0;
  };

  // Union-find over the obstacle array, and then the grouping. Separate from Rebuild because the
  // partition is the part with a determinism rule on it and is worth reading alone.
  void Partition(std::span<const PathGrid::Obstacle> _obstacles);
  [[nodiscard]] std::uint32_t RootOf(std::uint32_t _at) noexcept;

  // Where the island with that key sits in m_islandKey, or m_islandKey.size(). One lookup for the
  // two callers above, because two binary searches ordered differently is a bug that reports itself
  // as "everything re-plans" and never as an error.
  [[nodiscard]] std::size_t FindIsland(std::int64_t _cellX, std::int64_t _cellZ) const noexcept;

  std::vector<PathGrid> m_islands;

  // One key and one version per entry of m_islands, in the same order.
  //
  // The keys are the sort key Partition already orders the islands by, so this vector is sorted by
  // (cellZ, cellX) and IsStampCurrent can binary-search it. Kept as its own array rather than read
  // back out of m_found, because m_found is partition scratch and is clobbered by the next rebuild.
  std::vector<PlanStamp> m_islandKey;
  std::vector<std::uint32_t> m_keptVersion;
  std::uint32_t m_version = 0;

  // What the last build was built from, so a rebuild with an unchanged obstacle set can leave the
  // version alone and every route with it -- the same gate PathGrid keeps, one level up, because
  // this is now the version a Route is stamped with (Design/Archive/MmoScalabilityReview.md U4).
  std::vector<PathGrid::Obstacle> m_built;

  // What each island's grid was built from, one list per entry of m_islands and in the same order.
  // A rebuild that hands an island the same obstacles it already holds keeps that grid rather than
  // computing the same clearance field again, which is the difference between paying for the universe
  // and paying for what moved (Design/Archive/RegionalPathfinding.md 4).
  //
  // Matched by content and not by position, because the islands are ordered by where they sit in the
  // universe and building anything renumbers every island after it. An island index is not a handle,
  // for the same reason a ShipId is not (ADR 0005, ADR 0034) -- so this asks "which of the old lists
  // is this one" rather than "what was at this slot".
  std::vector<std::vector<PathGrid::Obstacle>> m_islandBuilt;
  std::vector<PathGrid> m_keptIslands;
  std::vector<std::vector<PathGrid::Obstacle>> m_keptBuilt;
  std::vector<std::uint8_t> m_claimed;
  std::uint32_t m_rebuiltIslands = 0;

  // Partition scratch, reused so a rebuild allocates nothing after the first one. m_parent is the
  // union-find forest over m_built's indices; m_members is those indices grouped by island and
  // m_memberStart indexes into it, which is one allocation rather than one vector per island.
  std::vector<std::uint32_t> m_parent;
  std::vector<Found> m_found;
  std::vector<std::uint32_t> m_islandOf; // which entry of m_found each obstacle landed in
  std::vector<std::uint32_t> m_order;    // m_found's entries, in the universe-fixed order
  std::vector<std::uint32_t> m_members;
  std::vector<std::uint32_t> m_memberStart;
  std::vector<PathGrid::Obstacle> m_islandScratch;
};
} // namespace Game
