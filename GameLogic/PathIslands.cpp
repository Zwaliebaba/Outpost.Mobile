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
  // cell index is a world coordinate and moves for nobody (Design/RegionalPathfinding.md 3.2, 5).
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
  // not move a building must not cost the fleet its routes (Design/MmoScalabilityReview.md U4).
  if (SameObstacles(_obstacles, m_built))
    return;
  ++m_version;
  m_built.assign(_obstacles.begin(), _obstacles.end());

  m_islands.clear();
  if (_obstacles.empty())
    return;

  Partition(_obstacles);
  m_islands.resize(m_memberStart.size() - 1);
  for (std::size_t island = 0; island + 1 < m_memberStart.size(); ++island)
  {
    m_islandScratch.clear();
    for (std::uint32_t at = m_memberStart[island]; at < m_memberStart[island + 1]; ++at)
      m_islandScratch.push_back(_obstacles[m_members[at]]);
    m_islands[island].Rebuild(m_islandScratch);
  }
}

bool PathIslands::FindPath(const WorldPos& _from, const WorldPos& _to, float _requiredClearanceMetres, std::vector<WorldPos>& _outWaypoints)
{
  _outWaypoints.clear();
  if (m_islands.empty())
  {
    _outWaypoints.push_back(_to);
    return true;
  }

  // Which islands the run actually meets, and which of them it meets first. Asking every island is
  // right rather than merely simple: an island the run passes clear of answers "never" from its own
  // clearance field, so there is no separate test for "is this island near the line" to disagree
  // with the one that decides the route.
  std::size_t first = 0;
  float firstAt = NEVER_BLOCKED;
  std::size_t met = 0;
  for (std::size_t at = 0; at < m_islands.size(); ++at)
  {
    const float blocked = m_islands[at].FirstBlockedFraction(_from, _to, _requiredClearanceMetres);
    if (blocked > 1.0f)
      continue;
    ++met;
    // Strictly less, so a tie goes to the earlier island -- and the islands are already in a
    // world-fixed order, which is what makes that a rule and not an accident.
    if (blocked < firstAt)
    {
      firstAt = blocked;
      first = at;
    }
  }

  if (met == 0)
  {
    _outWaypoints.push_back(_to);
    return true;
  }

  const bool complete = m_islands[first].FindPath(_from, _to, _requiredClearanceMetres, _outWaypoints);
  if (met == 1)
    return complete;

  // More than one island in the way. The first one planned as far as it could see and then aimed its
  // last waypoint at the destination, which is past the second island -- so steering at it would fly
  // the ship into exactly what the first grid cannot see. Dropping that waypoint ends the route on
  // the far side of the first island instead; the follower arrives there and World::AdvanceRoute
  // re-plans, which meets the second island and plans through that. Incremental, and it is the
  // behaviour the follower already had for a route too long for one waypoint list
  // (Design/RegionalPathfinding.md 3.4).
  //
  // Only where there is something left to steer at. A single waypoint is A*'s "walled in with
  // nowhere to go" answer, and dropping it would leave a route of none at all, which the follower
  // reads as "not routed" and which would leave the ship standing still.
  if (_outWaypoints.size() > 1)
    _outWaypoints.pop_back();
  return false;
}
} // namespace Game
