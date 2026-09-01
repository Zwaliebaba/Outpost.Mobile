#include "pch.h"
#include "PathIslands.h"

#include "HullSpec.h"
#include "SimTuning.h"

namespace Game
{
namespace
{
// A run that never meets this grid. Greater than one, so it sorts after every blocked run and
// compares false against "clear?" -- the same convention PathGrid::FirstBlockedFraction returns.
constexpr float NEVER_BLOCKED = 2.0f;

// Whether two pieces of architecture are close enough to be one island: the gap between their
// surfaces, which is the centre distance less both radii, against the widest corridor a hull could
// need (HullSpec.h, IslandGapMetres). Overlapping obstacles give a negative gap and are of course
// the same island.
[[nodiscard]] bool SameIsland(const PathGrid::Obstacle& _a, const PathGrid::Obstacle& _b) noexcept
{
  const float gap = Distance(_a.pos, _b.pos) - _a.radiusMetres - _b.radiusMetres;
  return gap < IslandGapMetres();
}
} // namespace

std::uint32_t PathIslands::RootOf(std::uint32_t _at) noexcept
{
  // Path halving rather than full compression, because the forest is rebuilt from scratch every
  // time and the shortening only has to pay for itself within one Rebuild.
  while (m_parent[_at] != _at)
  {
    m_parent[_at] = m_parent[m_parent[_at]];
    _at = m_parent[_at];
  }
  return _at;
}

void PathIslands::Partition(std::span<const PathGrid::Obstacle> _obstacles)
{
  const std::uint32_t count = static_cast<std::uint32_t>(_obstacles.size());
  m_parent.resize(count);
  for (std::uint32_t at = 0; at < count; ++at)
    m_parent[at] = at;

  // Every pair, which is O(n^2) in the architecture. That is the same order as the clearance field
  // this feeds -- which is cells times obstacles -- and it runs on the same rare cadence, so the
  // partition is not what will need a broad-phase first.
  for (std::uint32_t at = 0; at < count; ++at)
  {
    for (std::uint32_t other = at + 1; other < count; ++other)
    {
      if (!SameIsland(_obstacles[at], _obstacles[other]))
        continue;
      const std::uint32_t left = RootOf(at);
      const std::uint32_t right = RootOf(other);
      if (left != right)
        m_parent[left] = right;
    }
  }

  // The components, and the lowest path cell each one occupies.
  m_found.clear();
  m_islandOf.assign(count, 0);
  for (std::uint32_t at = 0; at < count; ++at)
  {
    const std::uint32_t root = RootOf(at);
    std::uint32_t island = static_cast<std::uint32_t>(m_found.size());
    for (std::uint32_t seen = 0; seen < m_found.size(); ++seen)
    {
      if (m_found[seen].root == root)
      {
        island = seen;
        break;
      }
    }
    const std::int64_t cellX = PathCellX(_obstacles[at].pos);
    const std::int64_t cellZ = PathCellZ(_obstacles[at].pos);
    if (island == m_found.size())
      m_found.push_back(Found{.root = root, .cellX = cellX, .cellZ = cellZ});
    else if (cellZ < m_found[island].cellZ || (cellZ == m_found[island].cellZ && cellX < m_found[island].cellX))
      m_found[island] = Found{.root = root, .cellX = cellX, .cellZ = cellZ};
    m_islandOf[at] = island;
  }

  // Their order. The relation is symmetric so the components themselves do not depend on the walk --
  // but the obstacles arrive in ShipId order and ShipIds move under swap-and-pop (ADR 0005), so the
  // order they are *found* in does. Sorting by the lowest path cell any member sits in fixes that: a
  // cell index is a universe coordinate and moves for nobody (Design/Archive/RegionalPathfinding.md 3.2, 5).
  //
  // That key is unique to its island, so the order is total rather than merely usually total. Two
  // obstacles in one cell are at most a cell's diagonal apart -- 45 m -- and the smallest
  // architecture in the hull table is over 100 m across, so their surfaces overlap and the gap rule
  // has already made them one island. Two islands therefore cannot share a lowest cell.
  m_order.resize(m_found.size());
  for (std::uint32_t at = 0; at < m_order.size(); ++at)
    m_order[at] = at;
  const std::vector<Found>& found = m_found;
  std::sort(m_order.begin(), m_order.end(), [&found](std::uint32_t _a, std::uint32_t _b)
            { return (found[_a].cellZ != found[_b].cellZ) ? (found[_a].cellZ < found[_b].cellZ) : (found[_a].cellX < found[_b].cellX); });

  // Grouped into one array with a start index per island, so the members of island i are the range
  // [m_memberStart[i], m_memberStart[i + 1]). The obstacles within an island keep their store order,
  // which is what lets PathGrid's own rebuild gate recognise an island that did not change.
  m_members.clear();
  m_memberStart.clear();
  m_memberStart.reserve(m_order.size() + 1);
  for (const std::uint32_t island : m_order)
  {
    m_memberStart.push_back(static_cast<std::uint32_t>(m_members.size()));
    for (std::uint32_t at = 0; at < count; ++at)
    {
      if (m_islandOf[at] == island)
        m_members.push_back(at);
    }
  }
  m_memberStart.push_back(static_cast<std::uint32_t>(m_members.size()));
}

void PathIslands::Rebuild(std::span<const PathGrid::Obstacle> _obstacles)
{
  // A rebuild that finds the same architecture leaves the version alone, and every route with it:
  // the version is what makes a routed ship re-plan, and a rebuild triggered by something that did
  // not move a building must not cost the fleet its routes (Design/Archive/MmoScalabilityReview.md U4).
  if (SameObstacles(_obstacles, m_built))
    return;
  ++m_version;
  m_built.assign(_obstacles.begin(), _obstacles.end());

  m_rebuiltIslands = 0;
  if (_obstacles.empty())
  {
    m_islands.clear();
    m_islandBuilt.clear();
    return;
  }

  Partition(_obstacles);

  // The grids from the last build, set aside so this one can claim the ones whose islands did not
  // change. Moved out rather than copied: a grid is a clearance field of thousands of floats, and
  // the point of the exercise is not to touch the ones that are still right.
  //
  // Swapped out *before* anything clears them, which is the whole of it: clearing m_islands first
  // and swapping after leaves nothing to claim and quietly rebuilds the universe every time. The
  // benchmark is what caught that, which is why it is part of this slice.
  m_keptIslands.clear();
  m_keptBuilt.clear();
  m_keptIslands.swap(m_islands);
  m_keptBuilt.swap(m_islandBuilt);
  m_claimed.assign(m_keptIslands.size(), 0);

  const std::size_t count = m_memberStart.size() - 1;
  m_islands.resize(count);
  m_islandBuilt.resize(count);
  m_rebuiltIslands = 0;
  for (std::size_t island = 0; island < count; ++island)
  {
    m_islandScratch.clear();
    for (std::uint32_t at = m_memberStart[island]; at < m_memberStart[island + 1]; ++at)
      m_islandScratch.push_back(_obstacles[m_members[at]]);

    // Matched by content, not by position: the islands are ordered by where they sit in the universe,
    // so building anything renumbers every island after it and the slot an island held last time
    // says nothing about which island is here now (ADR 0034).
    std::size_t kept = m_keptIslands.size();
    for (std::size_t old = 0; old < m_keptBuilt.size(); ++old)
    {
      if (m_claimed[old] == 0 && SameObstacles(m_islandScratch, m_keptBuilt[old]))
      {
        kept = old;
        break;
      }
    }

    if (kept < m_keptIslands.size())
    {
      m_claimed[kept] = 1;
      m_islands[island] = std::move(m_keptIslands[kept]);
      m_islandBuilt[island].swap(m_keptBuilt[kept]);
      continue;
    }

    ++m_rebuiltIslands;
    m_islands[island].Rebuild(m_islandScratch);
    m_islandBuilt[island] = m_islandScratch;
  }
}

bool PathIslands::FindPath(const UniversePos& _from, const UniversePos& _to, float _requiredClearanceMetres,
                           std::vector<UniversePos>& _outWaypoints)
{
  _outWaypoints.clear();
  if (m_islands.empty())
  {
    _outWaypoints.push_back(_to);
    return true;
  }

  // Where each island blocks the run, and which of them blocks it soonest. Asking every island is
  // right rather than merely simple: an island the run passes clear of answers "never" from its own
  // clearance field, so there is no separate test for "is this island near the line" to disagree
  // with the one that decides the route.
  std::size_t first = m_islands.size();
  float firstAt = NEVER_BLOCKED;
  float firstEndsAt = NEVER_BLOCKED;
  float nextAt = NEVER_BLOCKED; // where the soonest *other* island starts blocking
  std::size_t met = 0;
  for (std::size_t at = 0; at < m_islands.size(); ++at)
  {
    const PathGrid::BlockedSpan blocked = m_islands[at].BlockedAlong(_from, _to, _requiredClearanceMetres);
    if (blocked.first > 1.0f)
      continue;
    ++met;
    // Strictly less, so a tie goes to the earlier island -- and the islands are already in a
    // universe-fixed order, which is what makes that a rule and not an accident.
    if (blocked.first < firstAt)
    {
      nextAt = std::min(nextAt, firstAt); // the one it displaces is now the next
      firstAt = blocked.first;
      firstEndsAt = blocked.last;
      first = at;
    }
    else
    {
      nextAt = std::min(nextAt, blocked.first);
    }
  }

  if (met == 0)
  {
    _outWaypoints.push_back(_to);
    return true;
  }

  if (met == 1)
    return m_islands[first].FindPath(_from, _to, _requiredClearanceMetres, _outWaypoints);

  // More than one island in the way, and no single grid can plan the whole run: the first island
  // cannot see the second. So the first island plans a leg, and the route reports itself unfinished
  // -- which is what makes Universe::AdvanceRoute come back for the rest on arrival, exactly as it
  // already did for a route too long for one waypoint list (Design/Archive/RegionalPathfinding.md 3.4).
  //
  // The leg is aimed at the middle of the open water between the two: past where the first island
  // stops blocking the run, and short of where the next one starts. Aiming at the destination
  // instead and truncating whatever came back was the first shape of this, and it ends the route at
  // the last *turn* rather than past the island -- the string-pull stops adding waypoints once the
  // way ahead is clear, so the far side is only ever reached by the destination waypoint that has to
  // be dropped. Measured on two stations 3 km apart, that left the ship 261.8 m from the second one
  // against a 251.2 m capsule; aiming at the gap leaves 304 m.
  if (firstEndsAt < nextAt)
  {
    const UniversePos openWater = Lerp(_from, _to, (firstEndsAt + nextAt) * 0.5f);
    (void)m_islands[first].FindPath(_from, openWater, _requiredClearanceMetres, _outWaypoints);
    return false;
  }

  // The two islands overlap along the run -- the second starts blocking before the first stops --
  // so there is no open water on the line to aim at and the midpoint would be inside a wall. Fall
  // back to the first shape: plan at the destination and drop the waypoint that aims past the second
  // island. Only where there is something left to steer at, because a single waypoint is A*'s
  // "walled in with nowhere to go" answer and dropping it would leave a route of none at all, which
  // the follower reads as "not routed" and which would leave the ship standing still.
  (void)m_islands[first].FindPath(_from, _to, _requiredClearanceMetres, _outWaypoints);
  if (_outWaypoints.size() > 1)
    _outWaypoints.pop_back();
  return false;
}
} // namespace Game
