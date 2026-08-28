#include "pch.h"
#include "PathGrid.h"

#include "SimTuning.h"

namespace Game
{
namespace
{
// Anywhere off the grid is open. True by construction: the grid is inflated past everything in it.
constexpr float OPEN_CLEARANCE_METRES = 1e9f;
constexpr std::uint32_t NO_CELL = 0xFFFFFFFFu;
} // namespace

bool PathGrid::Worse(const Open& _a, const Open& _b) noexcept
{
  if (_a.estimate != _b.estimate)
    return _a.estimate > _b.estimate;
  if (_a.travelled != _b.travelled)
    return _a.travelled > _b.travelled;
  return _a.cell > _b.cell;
}

void PathGrid::Rebuild(std::span<const Obstacle> _obstacles)
{
  ++m_version;
  m_clearance.clear();
  m_width = 0;
  m_height = 0;
  if (_obstacles.empty())
    return;

  float minX = _obstacles[0].pos.localX;
  float maxX = minX;
  float minZ = _obstacles[0].pos.localZ;
  float maxZ = minZ;
  for (const Obstacle& obstacle : _obstacles)
  {
    minX = std::min(minX, obstacle.pos.localX - obstacle.radiusMetres);
    maxX = std::max(maxX, obstacle.pos.localX + obstacle.radiusMetres);
    minZ = std::min(minZ, obstacle.pos.localZ - obstacle.radiusMetres);
    maxZ = std::max(maxZ, obstacle.pos.localZ + obstacle.radiusMetres);
  }
  minX -= PATH_GRID_MARGIN_METRES;
  maxX += PATH_GRID_MARGIN_METRES;
  minZ -= PATH_GRID_MARGIN_METRES;
  maxZ += PATH_GRID_MARGIN_METRES;

  const std::int64_t width = static_cast<std::int64_t>((maxX - minX) / PATH_CELL_SIZE_METRES) + 1;
  const std::int64_t height = static_cast<std::int64_t>((maxZ - minZ) / PATH_CELL_SIZE_METRES) + 1;

  // A world whose architecture is spread over tens of kilometres would want a grid of millions of
  // cells, and the honest answer today is to decline rather than to silently coarsen: cell size is
  // in the replay contract, so coarsening it here would change recorded outcomes as a side effect
  // of where someone put a building. Declining degrades to exactly the behaviour before this
  // phase -- straight-line steering with local avoidance -- and sectors are what will bound this
  // properly (Collision.md 3).
  if (width > PATH_GRID_MAX_CELLS_PER_AXIS || height > PATH_GRID_MAX_CELLS_PER_AXIS)
    return;

  m_width = static_cast<std::uint32_t>(width);
  m_height = static_cast<std::uint32_t>(height);
  m_originX = minX;
  m_originZ = minZ;
  m_clearance.assign(static_cast<std::size_t>(m_width) * m_height, OPEN_CLEARANCE_METRES);

  for (std::uint32_t cellZ = 0; cellZ < m_height; ++cellZ)
  {
    for (std::uint32_t cellX = 0; cellX < m_width; ++cellX)
    {
      const WorldPos centre{m_originX + static_cast<float>(cellX) * PATH_CELL_SIZE_METRES,
                            m_originZ + static_cast<float>(cellZ) * PATH_CELL_SIZE_METRES};
      float clearance = OPEN_CLEARANCE_METRES;
      for (const Obstacle& obstacle : _obstacles)
        clearance = std::min(clearance, Distance(centre, obstacle.pos) - obstacle.radiusMetres);
      m_clearance[static_cast<std::size_t>(cellZ) * m_width + cellX] = clearance;
    }
  }
}

std::int32_t PathGrid::ClampedCellX(float _metres) const noexcept
{
  const std::int32_t cell = static_cast<std::int32_t>(std::floor((_metres - m_originX) / PATH_CELL_SIZE_METRES + 0.5f));
  return std::clamp(cell, 0, static_cast<std::int32_t>(m_width) - 1);
}

std::int32_t PathGrid::ClampedCellZ(float _metres) const noexcept
{
  const std::int32_t cell = static_cast<std::int32_t>(std::floor((_metres - m_originZ) / PATH_CELL_SIZE_METRES + 0.5f));
  return std::clamp(cell, 0, static_cast<std::int32_t>(m_height) - 1);
}

WorldPos PathGrid::CentreOf(std::uint32_t _cell) const noexcept
{
  const std::uint32_t cellX = _cell % m_width;
  const std::uint32_t cellZ = _cell / m_width;
  return WorldPos{m_originX + static_cast<float>(cellX) * PATH_CELL_SIZE_METRES,
                  m_originZ + static_cast<float>(cellZ) * PATH_CELL_SIZE_METRES};
}

float PathGrid::ClearanceAt(const WorldPos& _pos) const noexcept
{
  if (m_clearance.empty())
    return OPEN_CLEARANCE_METRES;
  const float cellX = (_pos.localX - m_originX) / PATH_CELL_SIZE_METRES;
  const float cellZ = (_pos.localZ - m_originZ) / PATH_CELL_SIZE_METRES;
  if (cellX < -0.5f || cellZ < -0.5f || cellX > static_cast<float>(m_width) - 0.5f || cellZ > static_cast<float>(m_height) - 0.5f)
    return OPEN_CLEARANCE_METRES;
  const std::uint32_t x = static_cast<std::uint32_t>(ClampedCellX(_pos.localX));
  const std::uint32_t z = static_cast<std::uint32_t>(ClampedCellZ(_pos.localZ));
  return m_clearance[static_cast<std::size_t>(z) * m_width + x];
}

bool PathGrid::IsClearBetween(const WorldPos& _from, const WorldPos& _to, float _requiredClearanceMetres) const
{
  if (m_clearance.empty())
    return true;

  // Half a cell per step, so nothing can be stepped over: the clearance field varies over a cell,
  // and sampling at cell spacing would walk straight through the corner of a Structure.
  const float span = Distance(_from, _to);
  const int steps = std::max(1, static_cast<int>(span / (PATH_CELL_SIZE_METRES * 0.5f)) + 1);
  for (int step = 0; step <= steps; ++step)
  {
    const WorldPos at = Lerp(_from, _to, static_cast<float>(step) / static_cast<float>(steps));
    if (ClearanceAt(at) < _requiredClearanceMetres)
      return false;
  }
  return true;
}

bool PathGrid::FindPath(const WorldPos& _from, const WorldPos& _to, float _requiredClearanceMetres, std::vector<WorldPos>& _outWaypoints)
{
  _outWaypoints.clear();
  if (IsClearBetween(_from, _to, _requiredClearanceMetres))
  {
    _outWaypoints.push_back(_to);
    return true;
  }

  const std::uint32_t cells = static_cast<std::uint32_t>(m_clearance.size());
  const std::uint32_t start =
    static_cast<std::uint32_t>(ClampedCellZ(_from.localZ)) * m_width + static_cast<std::uint32_t>(ClampedCellX(_from.localX));
  const std::uint32_t goal =
    static_cast<std::uint32_t>(ClampedCellZ(_to.localZ)) * m_width + static_cast<std::uint32_t>(ClampedCellX(_to.localX));

  m_travelled.assign(cells, OPEN_CLEARANCE_METRES);
  m_cameFrom.assign(cells, NO_CELL);
  m_visited.assign(cells, 0);
  m_open.clear();

  const WorldPos goalCentre = CentreOf(goal);
  m_travelled[start] = 0.0f;
  m_open.push_back({Distance(CentreOf(start), goalCentre), 0.0f, start});

  // The best cell reached, if the goal itself is walled in -- an order into the middle of a
  // Structure is a player's to make, and getting as close as the geometry allows is a better answer
  // than refusing to move.
  std::uint32_t best = start;
  float bestHeuristic = Distance(CentreOf(start), goalCentre);
  bool reached = false;

  while (!m_open.empty())
  {
    std::pop_heap(m_open.begin(), m_open.end(), Worse);
    const Open current = m_open.back();
    m_open.pop_back();
    if (m_visited[current.cell] != 0)
      continue;
    m_visited[current.cell] = 1;

    const WorldPos centre = CentreOf(current.cell);
    const float heuristic = Distance(centre, goalCentre);
    if (heuristic < bestHeuristic || (heuristic == bestHeuristic && current.cell < best))
    {
      bestHeuristic = heuristic;
      best = current.cell;
    }
    if (current.cell == goal)
    {
      reached = true;
      break;
    }

    const std::int32_t cellX = static_cast<std::int32_t>(current.cell % m_width);
    const std::int32_t cellZ = static_cast<std::int32_t>(current.cell / m_width);
    for (std::int32_t stepZ = -1; stepZ <= 1; ++stepZ)
    {
      for (std::int32_t stepX = -1; stepX <= 1; ++stepX)
      {
        if (stepX == 0 && stepZ == 0)
          continue;
        const std::int32_t nextX = cellX + stepX;
        const std::int32_t nextZ = cellZ + stepZ;
        if (nextX < 0 || nextZ < 0 || nextX >= static_cast<std::int32_t>(m_width) || nextZ >= static_cast<std::int32_t>(m_height))
          continue;
        const std::uint32_t next = static_cast<std::uint32_t>(nextZ) * m_width + static_cast<std::uint32_t>(nextX);
        // A ship already pressed against a wall has to be able to leave the cell it is in, so the
        // requirement is on entering a cell rather than on occupying one.
        if (m_clearance[next] < _requiredClearanceMetres)
          continue;

        const float step = (stepX != 0 && stepZ != 0) ? PATH_CELL_SIZE_METRES * 1.41421356f : PATH_CELL_SIZE_METRES;
        const float travelled = current.travelled + step;
        if (travelled >= m_travelled[next])
          continue;
        m_travelled[next] = travelled;
        m_cameFrom[next] = current.cell;
        m_open.push_back({travelled + Distance(CentreOf(next), goalCentre), travelled, next});
        std::push_heap(m_open.begin(), m_open.end(), Worse);
      }
    }
  }

  const std::uint32_t reachedCell = reached ? goal : best;
  if (reachedCell == start && !reached)
  {
    // Walled in with nowhere to go. Steer at the point and let separation hold the line.
    _outWaypoints.push_back(_to);
    return false;
  }

  m_cellPath.clear();
  for (std::uint32_t at = reachedCell; at != NO_CELL; at = m_cameFrom[at])
    m_cellPath.push_back(at);
  std::reverse(m_cellPath.begin(), m_cellPath.end());

  // String-pull: keep a cell only when the run from the last kept point to the next one is not
  // clear. Without it the follower steers at every cell centre and the route reads as a staircase.
  WorldPos anchor = _from;
  for (std::size_t at = 1; at < m_cellPath.size(); ++at)
  {
    if (IsClearBetween(anchor, CentreOf(m_cellPath[at]), _requiredClearanceMetres))
      continue;
    anchor = CentreOf(m_cellPath[at - 1]);
    _outWaypoints.push_back(anchor);
    // A route needing more turns than one list holds ends on its last safe waypoint and reports
    // itself unfinished, so the follower re-plans from there. Appending the destination instead
    // would hand back a shortcut through whatever the remaining waypoints were avoiding.
    if (_outWaypoints.size() >= MAX_PATH_WAYPOINTS)
      return false;
  }
  _outWaypoints.push_back(_to);
  return true;
}
} // namespace Game
