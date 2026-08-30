#pragma once

#include "PathGrid.h"
#include "WorldPos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// The world's architecture, partitioned into islands, with one PathGrid over each.
//
// One grid over everything was the right first shape and does not survive a universe. It sweeps a
// single bounding box over every obstacle there is, so two stations 20 km apart ask for a grid of
// hundreds of millions of cells, it declines to build, and A* goes off for *every ship in the
// world* rather than for the space between them. HullSpec.h is explicit that this is not a graceful
// degradation: a capital's look-ahead is deliberately shorter than its own turning circle, so a
// single distant outpost turns every Carrier in the game into a ship that flies into things
// (Design/RegionalPathfinding.md 1.1).
//
// Two obstacles belong to the same island when the gap between their surfaces is narrower than
// IslandGapMetres() -- narrower, that is, than the widest corridor a hull could need. A wider gap is
// one the straight-line test flies through on its own and needs no plan; a narrower one is a wall.
// The islands are the connected components of that relation, so the partition is a function of the
// obstacle set and of nothing else.
//
// The seam is PathGrid's, unchanged: World holds this instead of a grid and calls the same three
// things. What it gives up is optimality *between* islands -- a ship cannot see that going round
// island two is cheaper than through it until it gets there. That is the right trade at this scale;
// the alternative is a portal graph answering a question no content in this tree asks yet
// (Design/RegionalPathfinding.md 3.4, 6).
class PathIslands
{
public:
  // Rebuilt on obstacle spawn and despawn only -- the same cadence the static index rebuilds on,
  // and never per tick. Partitions first, then builds one grid per island.
  void Rebuild(std::span<const PathGrid::Obstacle> _obstacles);

  // Bumped when the architecture changes, and only then. One number for the whole world because a
  // Route carries one: a follower that planned against an older one re-plans (World::AdvanceRoute).
  [[nodiscard]] std::uint32_t Version() const noexcept
  {
    return m_version;
  }

  // A route as a short waypoint list, with PathGrid::FindPath's meaning exactly: true when the last
  // waypoint is _to itself, false when the last waypoint is the furthest safe point along the way
  // and the follower re-plans from there.
  //
  // Three cases (Design/RegionalPathfinding.md 3.4). The run is clear of every island, and the
  // answer is the destination. It meets exactly one, and that island plans it. It meets more than
  // one, and the first island plans as far as it can while the answer says the route is unfinished
  // -- because the second island is still in the way however well the first one planned, and an
  // unfinished route is what makes the follower come back for the rest.
  [[nodiscard]] bool FindPath(const WorldPos& _from, const WorldPos& _to, float _requiredClearanceMetres,
                              std::vector<WorldPos>& _outWaypoints);

  // How many islands the architecture came to. For the F1 readout and for the tests that have to
  // say "these two stations are two islands and not one".
  [[nodiscard]] std::size_t IslandCount() const noexcept
  {
    return m_islands.size();
  }

  // How many of them refused to build, because a single island is genuinely wider than
  // PATH_GRID_MAX_CELLS_PER_AXIS allows. Its neighbours keep routing -- that is the whole gain over
  // one grid -- but ships crossing *it* fall back to straight-line steering, and a declining island
  // is otherwise indistinguishable from open space. Counted so it can be read off the screen rather
  // than inferred from ships that stopped avoiding things (Design/RegionalPathfinding.md 3.3).
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

  // The grid over one island, in the world-fixed order Rebuild put them in. For diagnostics and
  // tests; routing goes through FindPath, which is the whole point of the class.
  [[nodiscard]] const PathGrid& Island(std::size_t _at) const noexcept
  {
    return m_islands[_at];
  }

private:
  // One island while it is being found: which union-find root it is, and the lowest path cell any
  // of its members sits in, which is what puts the islands in a world-fixed order.
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

  std::vector<PathGrid> m_islands;
  std::uint32_t m_version = 0;

  // What the last build was built from, so a rebuild with an unchanged obstacle set can leave the
  // version alone and every route with it -- the same gate PathGrid keeps, one level up, because
  // this is now the version a Route is stamped with (Design/MmoScalabilityReview.md U4).
  std::vector<PathGrid::Obstacle> m_built;

  // Partition scratch, reused so a rebuild allocates nothing after the first one. m_parent is the
  // union-find forest over m_built's indices; m_members is those indices grouped by island and
  // m_memberStart indexes into it, which is one allocation rather than one vector per island.
  std::vector<std::uint32_t> m_parent;
  std::vector<Found> m_found;
  std::vector<std::uint32_t> m_islandOf; // which entry of m_found each obstacle landed in
  std::vector<std::uint32_t> m_order;    // m_found's entries, in the world-fixed order
  std::vector<std::uint32_t> m_members;
  std::vector<std::uint32_t> m_memberStart;
  std::vector<PathGrid::Obstacle> m_islandScratch;
};
} // namespace Game
