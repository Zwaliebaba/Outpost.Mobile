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

// Floor division for a signed numerator, because C++ truncates towards zero and the lattice runs
// west and south of the origin as freely as east and north: cell -1 is the last cell of sector -1,
// not the first of sector 0.
[[nodiscard]] std::int64_t FloorDivide(std::int64_t _numerator, std::int64_t _divisor) noexcept
{
  const std::int64_t quotient = _numerator / _divisor;
  return (_numerator % _divisor < 0) ? quotient - 1 : quotient;
}

// One axis of the lattice. The sector contributes whole cells and the local offset the rest, which
// is exact only because a cell never straddles a sector boundary (SimTuning.h,
// PATH_CELLS_PER_SECTOR). std::floor rather than a cast because the invariant on WorldPos keeps
// _localMetres non-negative today and a cast would be the thing that broke silently if it stopped.
[[nodiscard]] std::int64_t CellOnAxis(std::int64_t _sector, float _localMetres) noexcept
{
  return _sector * PATH_CELLS_PER_SECTOR + static_cast<std::int64_t>(std::floor(_localMetres / PATH_CELL_SIZE_METRES));
}
} // namespace

std::int64_t PathCellX(const WorldPos& _pos) noexcept
{
  return CellOnAxis(_pos.sectorX, _pos.localX);
}

std::int64_t PathCellZ(const WorldPos& _pos) noexcept
{
  return CellOnAxis(_pos.sectorZ, _pos.localZ);
}

WorldPos PathCellCentre(std::int64_t _cellX, std::int64_t _cellZ) noexcept
{
  // Built field by field rather than through LocalPos, because LocalPos takes metres from the
  // universe origin and a cell index far from it does not survive the trip through a float. The
  // invariant holds by construction: the remainder is in [0, PATH_CELLS_PER_SECTOR), so the local
  // offset is half a cell to half a cell short of a sector.
  const std::int64_t sectorX = FloorDivide(_cellX, PATH_CELLS_PER_SECTOR);
  const std::int64_t sectorZ = FloorDivide(_cellZ, PATH_CELLS_PER_SECTOR);
  return WorldPos{sectorX, sectorZ, (static_cast<float>(_cellX - sectorX * PATH_CELLS_PER_SECTOR) + 0.5f) * PATH_CELL_SIZE_METRES,
                  (static_cast<float>(_cellZ - sectorZ * PATH_CELLS_PER_SECTOR) + 0.5f) * PATH_CELL_SIZE_METRES};
}

bool PathGrid::Worse(const Open& _a, const Open& _b) noexcept
{
  if (_a.estimate != _b.estimate)
    return _a.estimate > _b.estimate;
  if (_a.travelled != _b.travelled)
    return _a.travelled > _b.travelled;
  return _a.cell > _b.cell;
}

namespace
{
// Two obstacle sets are the same when they are the same obstacles in the same order. Order counts
// because the set comes out of the static store in array order, and a set that has been permuted is
// a set whose ids have moved -- which is a change a router has to hear about.
//
// All four fields, compared exactly. Two positions a whole sector apart share their local offsets,
// so comparing those alone would call them equal; and exact is what is wanted here rather than a
// tolerance, because the question is "did this change" and not "is this close".
[[nodiscard]] bool SameObstacles(std::span<const PathGrid::Obstacle> _a, std::span<const PathGrid::Obstacle> _b) noexcept
{
  if (_a.size() != _b.size())
    return false;
  for (std::size_t at = 0; at < _a.size(); ++at)
  {
    const PathGrid::Obstacle& one = _a[at];
    const PathGrid::Obstacle& other = _b[at];
    if (one.pos.sectorX != other.pos.sectorX || one.pos.sectorZ != other.pos.sectorZ || one.pos.localX != other.pos.localX ||
        one.pos.localZ != other.pos.localZ || one.radiusMetres != other.radiusMetres)
      return false;
  }
  return true;
}
} // namespace

void PathGrid::Rebuild(std::span<const Obstacle> _obstacles)
{
  // A rebuild that finds the same obstacles leaves the version alone: the version is what makes
  // every routed ship re-plan, and a rebuild triggered by something that did not move the
  // architecture must not cost the fleet its routes (Design/MmoScalabilityReview.md U4).
  const bool changed = !SameObstacles(_obstacles, m_built);
  if (changed)
    ++m_version;
  m_built.assign(_obstacles.begin(), _obstacles.end());

  m_clearance.clear();
  m_width = 0;
  m_height = 0;
  if (_obstacles.empty())
    return;

  // The extent is swept as offsets from the first obstacle rather than as coordinates, so that a
  // set of obstacles straddling a sector boundary gets a bounding box the width of the obstacles
  // and not the width of a sector.
  const WorldPos& reference = _obstacles[0].pos;
  float minX = 0.0f;
  float maxX = 0.0f;
  float minZ = 0.0f;
  float maxZ = 0.0f;
  for (const Obstacle& obstacle : _obstacles)
  {
    const float offsetX = OffsetX(reference, obstacle.pos);
    const float offsetZ = OffsetZ(reference, obstacle.pos);
    minX = std::min(minX, offsetX - obstacle.radiusMetres);
    maxX = std::max(maxX, offsetX + obstacle.radiusMetres);
    minZ = std::min(minZ, offsetZ - obstacle.radiusMetres);
    maxZ = std::max(maxZ, offsetZ + obstacle.radiusMetres);
  }
  minX -= PATH_GRID_MARGIN_METRES;
  maxX += PATH_GRID_MARGIN_METRES;
  minZ -= PATH_GRID_MARGIN_METRES;
  maxZ += PATH_GRID_MARGIN_METRES;

  // The bounding box, snapped outward onto the world lattice. This is the whole of what a build
  // decides: which cells it holds, never where they are. Both corners go through the lattice
  // rather than being divided by the cell size here, so a box that spans a sector boundary is
  // measured in cells and not in the offsets it happens to be expressed in.
  WorldPos corner = reference;
  Translate(corner, minX, minZ);
  const std::int64_t originCellX = PathCellX(corner);
  const std::int64_t originCellZ = PathCellZ(corner);
  corner = reference;
  Translate(corner, maxX, maxZ);
  const std::int64_t width = PathCellX(corner) - originCellX + 1;
  const std::int64_t height = PathCellZ(corner) - originCellZ + 1;

  // A world whose architecture is spread over tens of kilometres would want a grid of millions of
  // cells, and the honest answer today is to decline rather than to silently coarsen: cell size is
  // in the replay contract, so coarsening it here would change recorded outcomes as a side effect
  // of where someone put a building. Declining degrades to exactly the behaviour before this
  // phase -- straight-line steering with local avoidance -- and one grid per island of architecture
  // is what will bound this properly (Design/RegionalPathfinding.md 3.3).
  if (width > PATH_GRID_MAX_CELLS_PER_AXIS || height > PATH_GRID_MAX_CELLS_PER_AXIS)
    return;

  m_width = static_cast<std::uint32_t>(width);
  m_height = static_cast<std::uint32_t>(height);
  m_originCellX = originCellX;
  m_originCellZ = originCellZ;
  m_clearance.assign(static_cast<std::size_t>(m_width) * m_height, OPEN_CLEARANCE_METRES);

  for (std::uint32_t cellZ = 0; cellZ < m_height; ++cellZ)
  {
    for (std::uint32_t cellX = 0; cellX < m_width; ++cellX)
    {
      const WorldPos centre = PathCellCentre(m_originCellX + cellX, m_originCellZ + cellZ);
      float clearance = OPEN_CLEARANCE_METRES;
      for (const Obstacle& obstacle : _obstacles)
        clearance = std::min(clearance, Distance(centre, obstacle.pos) - obstacle.radiusMetres);
      m_clearance[static_cast<std::size_t>(cellZ) * m_width + cellX] = clearance;
    }
  }
}

std::uint32_t PathGrid::ClampedCell(const WorldPos& _pos) const noexcept
{
  const std::int64_t cellX = std::clamp(PathCellX(_pos) - m_originCellX, std::int64_t{0}, static_cast<std::int64_t>(m_width) - 1);
  const std::int64_t cellZ = std::clamp(PathCellZ(_pos) - m_originCellZ, std::int64_t{0}, static_cast<std::int64_t>(m_height) - 1);
  return static_cast<std::uint32_t>(cellZ) * m_width + static_cast<std::uint32_t>(cellX);
}

WorldPos PathGrid::CentreOf(std::uint32_t _cell) const noexcept
{
  return PathCellCentre(m_originCellX + (_cell % m_width), m_originCellZ + (_cell / m_width));
}

float PathGrid::ClearanceAt(const WorldPos& _pos) const noexcept
{
  if (m_clearance.empty())
    return OPEN_CLEARANCE_METRES;

  // Which cell of the lattice, then whether this grid holds it. In that order: the cell a point is
  // in is the world's answer and the same for every grid, and holding it is this grid's.
  const std::int64_t cellX = PathCellX(_pos) - m_originCellX;
  const std::int64_t cellZ = PathCellZ(_pos) - m_originCellZ;
  if (cellX < 0 || cellZ < 0 || cellX >= static_cast<std::int64_t>(m_width) || cellZ >= static_cast<std::int64_t>(m_height))
    return OPEN_CLEARANCE_METRES;
  return m_clearance[static_cast<std::size_t>(cellZ) * m_width + static_cast<std::size_t>(cellX)];
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
  const std::uint32_t start = ClampedCell(_from);
  const std::uint32_t goal = ClampedCell(_to);

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
