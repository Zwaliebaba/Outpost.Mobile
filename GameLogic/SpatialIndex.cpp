#include "pch.h"
#include "SpatialIndex.h"

namespace Game
{
namespace
{
// Integer mixing, not std::hash: the value has to be identical on every machine that replays the
// recording, and a standard library's hash is not required to be.
[[nodiscard]] std::uint32_t CellHash(std::int32_t _cellX, std::int32_t _cellZ) noexcept
{
  std::uint32_t hash = static_cast<std::uint32_t>(_cellX) * 0x9E3779B1u;
  hash ^= (static_cast<std::uint32_t>(_cellZ) * 0x85EBCA77u) + 0x9E3779B9u + (hash << 6) + (hash >> 2);
  hash ^= hash >> 15;
  hash *= 0x2C1B3C6Du;
  hash ^= hash >> 12;
  return hash;
}

[[nodiscard]] std::int32_t CellOf(float _metres, float _cellSizeMetres) noexcept
{
  return static_cast<std::int32_t>(std::floor(_metres / _cellSizeMetres));
}

[[nodiscard]] std::uint32_t BucketCountFor(std::size_t _entryCount) noexcept
{
  std::uint32_t buckets = 16;
  while (buckets < _entryCount * 2 && buckets < (1u << 22))
    buckets <<= 1;
  return buckets;
}
} // namespace

void SpatialIndex::Configure(const Desc& _desc) noexcept
{
  m_baseCellSizeMetres = (_desc.baseCellSizeMetres > 0.0f) ? _desc.baseCellSizeMetres : 256.0f;
  m_staticCellSizeMetres = (_desc.staticCellSizeMetres > 0.0f) ? _desc.staticCellSizeMetres : 512.0f;
  m_dynamicLevelCount = (_desc.dynamicLevelCount > 0) ? _desc.dynamicLevelCount : 1;
  m_dynamic.assign(m_dynamicLevelCount, Grid{});
  for (std::uint32_t level = 0; level < m_dynamicLevelCount; ++level)
    m_dynamic[level].cellSizeMetres = CellSizeMetres(level);
  m_static.cellSizeMetres = m_staticCellSizeMetres;
}

float SpatialIndex::CellSizeMetres(std::uint32_t _level) const noexcept
{
  return m_baseCellSizeMetres * static_cast<float>(1u << _level);
}

std::uint32_t SpatialIndex::LevelForRadius(float _boundingRadiusMetres) const noexcept
{
  for (std::uint32_t level = 0; level < m_dynamicLevelCount; ++level)
  {
    if (CellSizeMetres(level) >= 2.0f * _boundingRadiusMetres)
      return level;
  }
  return m_dynamicLevelCount - 1;
}

void SpatialIndex::Rebuild(Grid& _grid, std::span<const Entry> _entries, float _cellSizeMetres)
{
  _grid.cellSizeMetres = _cellSizeMetres;
  _grid.maxBoundingRadiusMetres = 0.0f;

  const std::uint32_t buckets = BucketCountFor(_entries.size());
  _grid.bucketMask = buckets - 1;
  _grid.bucketStart.assign(static_cast<std::size_t>(buckets) + 1, 0);
  _grid.cells.resize(_entries.size());
  if (_entries.empty())
    return;

  // Count, prefix-sum, then place. Entries are walked in array order at both steps, so within a
  // bucket they end up in ascending entry index -- which is what makes the query's output order a
  // property of the input rather than of the allocator.
  for (const Entry& entry : _entries)
  {
    const std::int32_t cellX = CellOf(entry.pos.localX, _cellSizeMetres);
    const std::int32_t cellZ = CellOf(entry.pos.localZ, _cellSizeMetres);
    ++_grid.bucketStart[(CellHash(cellX, cellZ) & _grid.bucketMask) + 1];
    if (entry.boundingRadiusMetres > _grid.maxBoundingRadiusMetres)
      _grid.maxBoundingRadiusMetres = entry.boundingRadiusMetres;
  }
  for (std::size_t bucket = 1; bucket < _grid.bucketStart.size(); ++bucket)
    _grid.bucketStart[bucket] += _grid.bucketStart[bucket - 1];

  m_cursorScratch.assign(_grid.bucketStart.begin(), _grid.bucketStart.end() - 1);
  for (const Entry& entry : _entries)
  {
    const std::int32_t cellX = CellOf(entry.pos.localX, _cellSizeMetres);
    const std::int32_t cellZ = CellOf(entry.pos.localZ, _cellSizeMetres);
    const std::uint32_t bucket = CellHash(cellX, cellZ) & _grid.bucketMask;
    _grid.cells[m_cursorScratch[bucket]++] = Cell{entry.id, entry.pos, entry.boundingRadiusMetres, cellX, cellZ};
  }
}

void SpatialIndex::RebuildStatic(std::span<const Entry> _entries)
{
  Rebuild(m_static, _entries, m_staticCellSizeMetres);
}

void SpatialIndex::RebuildDynamic(std::span<const Entry> _entries)
{
  if (m_dynamicLevelCount == 1)
  {
    Rebuild(m_dynamic[0], _entries, CellSizeMetres(0));
    return;
  }

  for (std::uint32_t level = 0; level < m_dynamicLevelCount; ++level)
  {
    m_levelScratch.clear();
    for (const Entry& entry : _entries)
    {
      if (LevelForRadius(entry.boundingRadiusMetres) == level)
        m_levelScratch.push_back(entry);
    }
    Rebuild(m_dynamic[level], m_levelScratch, CellSizeMetres(level));
  }
}

void SpatialIndex::Gather(const Grid& _grid, const WorldPos& _centre, float _radiusMetres, std::vector<ShipId>& _out)
{
  if (_grid.cells.empty())
    return;

  // The ring is derived, never a hardcoded 3x3. With per-level cell sizes and a per-ship query
  // radius it is routinely more than one cell, and a hardcoded neighbourhood is the single most
  // common way this class of index goes subtly wrong -- silently, because a missed contact reads
  // as a tuning problem rather than as a bug. The brute-force agreement test is what catches it.
  const float reach = _radiusMetres + _grid.maxBoundingRadiusMetres;
  const std::int32_t ring = static_cast<std::int32_t>(std::ceil(reach / _grid.cellSizeMetres));
  const std::int32_t centreX = CellOf(_centre.localX, _grid.cellSizeMetres);
  const std::int32_t centreZ = CellOf(_centre.localZ, _grid.cellSizeMetres);

  for (std::int32_t offsetZ = -ring; offsetZ <= ring; ++offsetZ)
  {
    for (std::int32_t offsetX = -ring; offsetX <= ring; ++offsetX)
    {
      const std::int32_t cellX = centreX + offsetX;
      const std::int32_t cellZ = centreZ + offsetZ;
      const std::uint32_t bucket = CellHash(cellX, cellZ) & _grid.bucketMask;
      const std::uint32_t end = _grid.bucketStart[bucket + 1];
      for (std::uint32_t at = _grid.bucketStart[bucket]; at < end; ++at)
      {
        const Cell& cell = _grid.cells[at];
        if (cell.cellX != cellX || cell.cellZ != cellZ)
          continue; // a different cell that happens to share this bucket
        const float span = _radiusMetres + cell.boundingRadiusMetres;
        if (DistanceSquared(_centre, cell.pos) <= span * span)
          _out.push_back(cell.id);
      }
    }
  }
}

void SpatialIndex::QueryCircle(const WorldPos& _centre, float _radiusMetres, std::vector<ShipId>& _out) const
{
  _out.clear();
  Gather(m_static, _centre, _radiusMetres, _out);
  for (const Grid& level : m_dynamic)
    Gather(level, _centre, _radiusMetres, _out);
}
} // namespace Game
