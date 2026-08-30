#pragma once

#include "ShipState.h"
#include "WorldPos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// The spatial index World owns. Collision is its first customer, not its purpose.
//
// Its second customer is interest management -- deciding which entities each connected player's
// snapshot contains -- which is why the query is QueryCircle and not NeighboursOf: weapons range,
// blast radius and region ownership are all already expressible in that signature and none of them
// has a ship at the centre. Get the query, its determinism and its sharding right and the algorithm
// behind it is replaceable (Design/Archive/Collision.md 1, 7).
//
// That second customer has arrived: InterestSet is built entirely on QueryCircle and nothing in
// this class changed to accommodate it, which is the claim Design/Archive/Collision.md 1 made and the
// evidence that the signature was the right one.
//
// Three stores behind one query, walked in a fixed order:
//
//   static   -- Structures and anything else that never moves. A Structure spans thousands of fine
//               cells; stamping it once on spawn is free, stamping it sixty times a second is not,
//               and the largest things in the hull table happen to be the ones that never move.
//   dynamic  -- one grid per size level, rebuilt whole every tick. Powers of two from a base cell
//               size; a hull goes in the level whose cell is at least twice its bounding radius.
//
// One dynamic level is configured by default. The level machinery is here and tested, because
// adding the second level once the structure exists is an afternoon and the structure is what has
// to be right -- but whether a Battleship stamping into fine cells actually costs anything is a
// projection, and the benchmark in the test suite is what should pay for it rather than an
// argument.
class SpatialIndex
{
public:
  // One indexed entity. The broad phase works in bounding circles only; capsule fidelity is a
  // narrow-phase concern, which is what keeps this class indifferent to whatever shapes arrive
  // later.
  struct Entry
  {
    ShipId id = INVALID_SHIP_ID;
    WorldPos pos;
    float boundingRadiusMetres = 0.0f;
  };

  // None of this is in the replay contract, and that is deliberate: cell size and level count are
  // pure performance knobs, free to differ between a dense-battle region and an empty one, and the
  // sort-then-truncate rule in the sense pass is what buys that freedom. Changing them must not be
  // able to change an answer -- the brute-force agreement test runs at two cell sizes for exactly
  // that reason.
  // 256 m is the benchmark's answer rather than a guess: across N = 100, 1,000 and 5,000 a 250 m
  // sweep costs 4.7x, 3.1x and 3.8x more at 64 m cells than at 256 m, and 512 m buys under 10% more
  // while packing a fighter engagement into one cell. It also happens to make the size rule fall
  // out -- a Carrier's 107.5 m bounding radius is under half a 256 m cell, so one dynamic level
  // holds the entire mobile fleet and the second one has nothing to do yet.
  struct Desc
  {
    float baseCellSizeMetres = 256.0f;
    std::uint32_t dynamicLevelCount = 1;
    float staticCellSizeMetres = 512.0f; // at least twice the largest static bounding radius
  };

  // Not noexcept: it sizes the level array, so it allocates. A throw out of a noexcept function is
  // std::terminate rather than the single try in wWinMain that AGENTS.md 5 says every error path
  // reaches.
  void Configure(const Desc& _desc);

  // Rebuilt only when something immovable spawns or despawns.
  void RebuildStatic(std::span<const Entry> _entries);

  // Rebuilt from scratch every tick: O(N), cache-linear, no incremental state that can drift, no
  // tombstones, and trivially correct.
  void RebuildDynamic(std::span<const Entry> _entries);

  // Every indexed entity whose bounding circle intersects the circle, appended to _out, which is
  // cleared first. Over-inclusive by design -- an entity 300 m away with a 107 m radius is within
  // a 250 m query -- because that is the conservative direction and the narrow phase is exact.
  void QueryCircle(const WorldPos& _centre, float _radiusMetres, std::vector<ShipId>& _out) const;

  [[nodiscard]] std::uint32_t DynamicLevelCount() const noexcept
  {
    return m_dynamicLevelCount;
  }

  [[nodiscard]] float CellSizeMetres(std::uint32_t _level) const noexcept;

  // Which dynamic level a hull belongs in: the first whose cell is at least twice the bounding
  // radius, so nothing ever stamps into more than a handful of cells at its own level. Returns 0
  // for everything while one level is configured, which is the point of it existing now.
  [[nodiscard]] std::uint32_t LevelForRadius(float _boundingRadiusMetres) const noexcept;

private:
  // A dense counting sort into a fixed bucket table, never std::unordered_map: a hash map's
  // iteration order depends on hashing and on allocation addresses, which is exactly the
  // "iteration order that is not dense-array order" AGENTS.md 5 bans, and it would take the replay
  // gate red in a way that reproduces on one machine and not another.
  //
  // Buckets rather than a grid over the entries' bounding box because the bounding box is
  // unbounded: two ships a hundred kilometres apart would otherwise allocate a cell table between
  // them. Two cells can share a bucket, so each entry carries the cell it is really in and the
  // query rejects the rest -- which also stops one entity being returned twice when two ring cells
  // collide.
  struct Cell
  {
    ShipId id = INVALID_SHIP_ID;
    WorldPos pos;
    float boundingRadiusMetres = 0.0f;
    std::int32_t cellX = 0;
    std::int32_t cellZ = 0;
  };

  struct Grid
  {
    float cellSizeMetres = 256.0f;
    float maxBoundingRadiusMetres = 0.0f;
    std::vector<Cell> cells;                // counting-sorted: bucket-major, ascending entry index within
    std::vector<std::uint32_t> bucketStart; // bucketCount + 1 offsets into cells
    std::uint32_t bucketMask = 0;
  };

  void Rebuild(Grid& _grid, std::span<const Entry> _entries, float _cellSizeMetres);
  static void Gather(const Grid& _grid, const WorldPos& _centre, float _radiusMetres, std::vector<ShipId>& _out);

  Grid m_static;
  std::vector<Grid> m_dynamic{1};
  float m_baseCellSizeMetres = 256.0f;
  float m_staticCellSizeMetres = 512.0f;
  std::uint32_t m_dynamicLevelCount = 1;

  // Scratch, reused across rebuilds so nothing allocates after the first tick.
  std::vector<Entry> m_levelScratch;
  std::vector<std::uint32_t> m_cursorScratch;
};
} // namespace Game
