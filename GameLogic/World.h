#pragma once

#include "Formation.h"
#include "Movement.h"
#include "PathGrid.h"
#include "ShipState.h"
#include "SpatialIndex.h"
#include "WorldPos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// The authoritative world. One dense array per entity kind, indexed by id -- no maps, no pointers
// between entities, no iteration order that is not array order, because all three are how a
// simulation stops reproducing itself.
//
// Ownership: whatever thread ticks the World owns it. Nothing else writes to it, and the view
// reads it only between ticks. When the halves separate this class is what moves to the server and
// the view stops holding a reference to it (AGENTS.md 2).
class World
{
public:
  // Adds a ship at rest. Returns its id, which is its index for as long as nothing is despawned.
  // Take HandleOf if the reference has to outlive a tick.
  ShipId SpawnShip(const WorldPos& _posWorld, float _headingRad, std::uint32_t _hullId);

  // Removes a ship, moving the last one into its slot. False means the handle was already stale.
  //
  // Every stored reference to the removed ship stops resolving; every stored reference to the ship
  // that moved keeps resolving, to the same ship. That second half is the reason handles exist.
  bool DespawnShip(ShipHandle _handle);

  // The handle for a live ship. Null (generation 0) if the id is not one.
  [[nodiscard]] ShipHandle HandleOf(ShipId _id) const noexcept;

  // The ship a handle names, or INVALID_SHIP_ID if it has been despawned.
  [[nodiscard]] ShipId Resolve(ShipHandle _handle) const noexcept;

  // One fixed tick. The only thing in the game that advances simulation state.
  //
  // Five passes over the whole array rather than one fused per-ship loop, and the reason is a
  // property rather than a preference: every read in a pass comes from a snapshot no pass is
  // concurrently writing, so the answer does not depend on array order. Array order stops being
  // stable the moment despawn swap-and-pops, the array is split across worker threads, or entities
  // are handed between region servers -- all of which are on the roadmap, and each of which would
  // otherwise make the simulation quietly produce different answers for the same input
  // (Design/Collision.md 6).
  //
  //   0  snapshot   prevPos and prevHeading; every neighbour read below is start-of-tick
  //   1  broad      rebuild the dynamic index from prevPos; the static one only when dirty
  //   2  sense      up to K neighbours per ship, sorted by (distance^2, ShipId) then truncated
  //   3  intent     what the order wants, then what the neighbourhood allows
  //   4  integrate  the turn-rate and acceleration limiter
  //   5  resolve    separation into scratch, applied after the loop, never in place
  void Step();

  // The index the world keeps. Exposed so a caller can retune it -- cell size and level count are
  // performance knobs, not contract values -- and so tests can reach the query directly.
  void ConfigureIndex(const SpatialIndex::Desc& _desc);
  [[nodiscard]] const SpatialIndex& Index() const noexcept
  {
    return m_index;
  }

  // What a ship sensed this tick. Empty before the first Step.
  [[nodiscard]] std::span<const Neighbour> NeighboursOf(ShipId _id) const noexcept;

  // The remaining waypoints of a ship's planned route, current one first. Server-side only: a path
  // is never wire data, and a client sees the resulting motion through snapshots like any other
  // (Design/Collision.md 12). Exposed for tests and for a debug overlay.
  [[nodiscard]] std::span<const WorldPos> RouteOf(ShipId _id) const noexcept;

  // Sends the given ships to _point in formation. Returns the heading the formation was solved
  // onto, which is what the view needs to draw the order marker -- so the rule that decides it
  // lives here, with the order, rather than being guessed at again on the other side.
  //
  // With no ordered facing the formation points along the way the group is about to travel.
  float IssueMoveOrder(std::span<const ShipId> _ships, const WorldPos& _point, bool _hasFacing, float _facingRad);

  [[nodiscard]] std::span<const ShipState> Ships() const noexcept
  {
    return m_ships;
  }
  [[nodiscard]] const ShipState& Ship(ShipId _id) const noexcept
  {
    return m_ships[_id];
  }
  [[nodiscard]] std::uint32_t ShipCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_ships.size());
  }
  [[nodiscard]] std::uint64_t Tick() const noexcept
  {
    return m_tick;
  }

private:
  void SnapshotPreviousTick() noexcept;
  void RebuildStaticIfDirty();
  void RebuildIndex();
  void GatherNeighbours();

  // Pass 5, in two halves. Both gather -- each ship sums its own corrections by walking its own
  // neighbour list and recomputing the pair term from its own side -- rather than scatter, where
  // one side would compute the pair once and write +d to itself and -d to the other.
  //
  // That is not a style choice and a profiler will argue against it. Scatter halves the arithmetic
  // and, under threading, needs an atomic float add; float addition is not associative, so the sum
  // would depend on the order the threads arrived and determinism would depend on scheduling. That
  // is the worst failure mode available: it reproduces on one machine, not on another, and not
  // twice in a row under load. Paying two cheap closed-form evaluations to keep the replay contract
  // free of the scheduler is not a close call (Design/Collision.md 6, 14).
  void ApplySeparation();
  void ApplyBlocking();

  [[nodiscard]] float AuthorityOf(ShipId _id) const noexcept;

  // Planning is server-side and at order time -- a pure function of the static set and the two
  // endpoints, so it is deterministic with nothing added -- and re-planning happens only when the
  // static set changes or the follower has drifted off its leg. Never per tick.
  void PlanRoute(ShipId _id, const WorldPos& _destination, float _requiredClearanceMetres);
  void AdvanceRoute(ShipId _id);

  // The indirection that makes a handle survive swap-and-pop: the slot is stable for a ship's
  // life, and the ship index inside it is what despawn repairs.
  struct Slot
  {
    ShipId ship = INVALID_SHIP_ID;
    std::uint32_t generation = 0;
  };

  std::vector<ShipState> m_ships;
  std::vector<std::uint32_t> m_shipSlot; // parallel to m_ships: which slot each ship owns
  std::vector<Slot> m_slots;
  std::vector<std::uint32_t> m_freeSlots; // reused last-in-first-out, so reuse is reproducible
  std::uint64_t m_tick = 0;

  SpatialIndex m_index;
  PathGrid m_pathGrid;
  bool m_staticIndexDirty = true;

  // A ship's route, parallel to m_ships and swap-and-popped with them. Fixed capacity rather than a
  // vector per ship: routes through sparse convex architecture are two or three waypoints, and a
  // dense array is what keeps despawn cheap and iteration order the array's.
  struct Route
  {
    WorldPos waypoint[MAX_PATH_WAYPOINTS];
    WorldPos destination;
    WorldPos legStart; // where the current leg began, for the deviation test
    float requiredClearanceMetres = 0.0f;
    std::uint32_t count = 0;
    std::uint32_t cursor = 0;
    std::uint32_t gridVersion = 0;
    bool reachesDestination = true; // false means the list ran out and the rest is still to plan
  };
  std::vector<Route> m_routes;
  std::vector<WorldPos> m_routeScratch;
  std::vector<PathGrid::Obstacle> m_obstacleScratch;

  // Scratch, all of it sized by ship count and reused, so a tick allocates nothing once the fleet
  // has stopped growing.
  std::vector<SpatialIndex::Entry> m_staticEntries;
  std::vector<SpatialIndex::Entry> m_dynamicEntries;
  std::vector<ShipId> m_queryScratch;
  std::vector<Neighbour> m_candidateScratch;
  std::vector<Neighbour> m_neighbours;         // flat, one run per ship
  std::vector<std::uint32_t> m_neighbourStart; // ship count + 1 offsets into it
  std::vector<std::uint32_t> m_neighbourCount; // how much of each run is filled
  // Ship positions gathered for FormationHeading, kept so an order allocates nothing.
  std::vector<WorldPos> m_headingScratch;

  std::vector<float> m_correctionX;
  std::vector<float> m_correctionZ;
  std::vector<float> m_appliedX; // this tick's running total, so k solver steps share one clamp
  std::vector<float> m_appliedZ;
};
} // namespace Game
