#pragma once

#include "Formation.h"
#include "Movement.h"
#include "PathGrid.h"
#include "Patrol.h"
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
  // What one ship's standing patrol is. The anchor is a handle rather than a position so that the
  // station's death ends the patrol instead of leaving three ships solemnly orbiting the site of a
  // building that no longer exists (ADR 0005).
  struct Patrol
  {
    ShipHandle anchor;
    float ringRadiusMetres = 0.0f;
    float cruiseSpeedMetresPerSec = 0.0f;
    std::uint32_t waypointIndex = 0; // the ring point last issued
    bool active = false;
  };

  // Adds a ship at rest. Returns its id, which is its index for as long as nothing is despawned.
  // Take HandleOf if the reference has to outlive a tick.
  //
  // The faction defaults because every existing caller -- the starting fleet, every test -- spawns
  // the player's own ships, so the default states what those call sites already mean rather than
  // papering over them. A caller that means someone else has to say so (Design/Archive/Hostiles.md 11).
  ShipId SpawnShip(const WorldPos& _posWorld, float _headingRad, std::uint32_t _hullId, FactionId _factionId = FACTION_PLAYER);

  // Removes a ship, moving the last one into its slot. False means the handle was already stale.
  //
  // Every stored reference to the removed ship stops resolving; every stored reference to the ship
  // that moved keeps resolving, to the same ship. That second half is the reason handles exist.
  bool DespawnShip(ShipHandle _handle);

  // The despawn log, read by sequence rather than drained.
  //
  // It exists so that the wire can say *destroyed* where it previously said only *left*: a client
  // that infers a death from an absence detonates every ship that merely flies out of its interest
  // radius, which is where a hostile patrol lives (Design/Archive/Hostiles.md 4.4). Step never touches it --
  // it is the publisher's, and each of its subscribers reads it at its own pace (ADR 0027).
  //
  // Deaths are numbered for the life of the World and the numbering is never reset, so a cursor
  // stays comparable across a trim and the difference between two cursors is exactly the number of
  // deaths between them.

  // The sequence one past the last death logged. A subscriber joining a running world opens its
  // cursor here, so it is told about every death from now on and about none of the ships it never
  // held.
  [[nodiscard]] std::uint64_t DespawnHead() const noexcept
  {
    return m_despawnBase + m_despawnLog.size();
  }

  // The handles despawned at or after _cursor, in despawn order.
  //
  // A cursor older than what the log still holds returns everything held rather than reporting the
  // gap. That is the over-report direction on purpose: the publisher intersects these with the
  // subscriber's own interest set, so a handle it never knew about is dropped there, while a death
  // silently skipped here would be a ship that never dies on one client's screen.
  [[nodiscard]] std::span<const ShipHandle> DespawnsSince(std::uint64_t _cursor) const noexcept;

  // Drops every death before _cursor. The publisher passes the minimum cursor across its
  // subscribers, so what remains is exactly what at least one of them has still to hear -- which is
  // why the log cannot do this for itself: it does not know who is reading it.
  void TrimDespawnsBefore(std::uint64_t _cursor) noexcept;

  // The handle for a live ship. Null (generation 0) if the id is not one.
  [[nodiscard]] ShipHandle HandleOf(ShipId _id) const noexcept;

  // The ship a handle names, or INVALID_SHIP_ID if it has been despawned.
  [[nodiscard]] ShipId Resolve(ShipHandle _handle) const noexcept;

  // Assigns _ship to walk the ring around _anchorStation for as long as the anchor lives.
  //
  // The first leg goes to the ring point nearest the ship's current bearing from the anchor, so an
  // assignment never teleports intent. A call naming a ship that is not live, or naming the ship as
  // its own anchor, does nothing: a station does not patrol itself.
  //
  // The ring radius and cruise speed are arguments rather than tuning constants because they are
  // content -- what the composition root spawns, the way a spawn position is (Design/Archive/Hostiles.md 5.1).
  void AssignPatrol(ShipId _ship, ShipId _anchorStation, float _ringRadiusMetres, float _cruiseSpeedMetresPerSec);

  // A ship's standing patrol. Server-side only, like a route: an assignment is intent, and the
  // snapshot exists to withhold intent. Exposed for tests and for a debug overlay.
  [[nodiscard]] const Patrol& PatrolOf(ShipId _id) const noexcept;

  // One fixed tick. The only thing in the game that advances simulation state.
  //
  // Standing NPC intent is issued first, before pass 0 -- the position an adapter's incoming orders
  // occupy from outside, so an NPC order and a player order entering on the same tick are
  // indistinguishable to every pass below (Design/Archive/Hostiles.md 5.3).
  //
  // Five passes over the whole array rather than one fused per-ship loop, and the reason is a
  // property rather than a preference: every read in a pass comes from a snapshot no pass is
  // concurrently writing, so the answer does not depend on array order. Array order stops being
  // stable the moment despawn swap-and-pops, the array is split across worker threads, or entities
  // are handed between region servers -- all of which are on the roadmap, and each of which would
  // otherwise make the simulation quietly produce different answers for the same input
  // (Design/Archive/Collision.md 6).
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

  // How many candidates the last gather built a record for, across the whole fleet, and how many the
  // index returned before the pair filter looked at them. The query is over-inclusive by design, so
  // the gap between these two is the work the filter is not doing; the second number grows with the
  // widest pairing in the neighbourhood and the first with how crowded it actually is
  // (Design/MmoScalabilityReview.md U2).
  [[nodiscard]] std::uint64_t GatheredCandidateCount() const noexcept
  {
    return m_gatheredCandidates;
  }
  [[nodiscard]] std::uint64_t QueriedCandidateCount() const noexcept
  {
    return m_queriedCandidates;
  }

  // What the last gather asked the index for, so a test can see the radius narrow rather than infer
  // it from a timing.
  [[nodiscard]] const NeighbourhoodExtent& Extent() const noexcept
  {
    return m_extent;
  }

  // The remaining waypoints of a ship's planned route, current one first. Server-side only: a path
  // is never wire data, and a client sees the resulting motion through snapshots like any other
  // (Design/Archive/Collision.md 12). Exposed for tests and for a debug overlay.
  [[nodiscard]] std::span<const WorldPos> RouteOf(ShipId _id) const noexcept;

  // Sends the given ships to _point in formation. Returns the heading the formation was solved
  // onto, which is what the view needs to draw the order marker -- so the rule that decides it
  // lives here, with the order, rather than being guessed at again on the other side.
  //
  // With no ordered facing the formation points along the way the group is about to travel.
  //
  // A ship whose faction is not the issuer's is dropped from the order exactly as a stale id is --
  // left out, not an error. The gate is here rather than in the host's adapter because the adapter
  // has no test suite and every future host, a dedicated server or a replay driver among them, would
  // otherwise have to remember the check: the simulation refusing is a property, an adapter refusing
  // is a convention (Design/Archive/Hostiles.md 4.3).
  float IssueMoveOrder(std::span<const ShipId> _ships, const WorldPos& _point, bool _hasFacing, float _facingRad,
                       FactionId _issuerFaction = FACTION_PLAYER);

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
  // Standing NPC intent, issued through the same order machinery a player's click uses.
  //
  // Order-independent by construction: it runs before anything in the tick moves, so every read --
  // the ship's own state, the anchor's posWorld -- is end-of-last-tick state whatever the array
  // order, and it writes only the ship it is visiting. It draws no randomness, reads no other ship,
  // and reacts to nothing; the first behavior that responds to what it sees is a different design
  // with senses and thresholds (Design/Archive/Hostiles.md 5.5, 8).
  void StepPatrols();

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
  // free of the scheduler is not a close call (Design/Archive/Collision.md 6, 14).
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
  std::vector<ShipHandle> m_despawnLog;   // read by cursor, trimmed by the publisher, never by Step
  std::uint64_t m_despawnBase = 0;        // the sequence of m_despawnLog[0]; rises with every trim
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

  // A ship's patrol, parallel to m_ships and swap-and-popped with them exactly as m_routes is. It is
  // not a field on ShipState, deliberately: that struct promises nothing in it a snapshot could not
  // carry, and an assignment is the kind of intent the snapshot exists to withhold.
  std::vector<Patrol> m_patrols;

  std::vector<WorldPos> m_routeScratch;
  std::vector<PathGrid::Obstacle> m_obstacleScratch;

  // Scratch, all of it sized by ship count and reused, so a tick allocates nothing once the fleet
  // has stopped growing.
  std::vector<SpatialIndex::Entry> m_staticEntries;
  std::vector<SpatialIndex::Entry> m_dynamicEntries;

  // The largest and fastest hulls actually in this world, recomputed as the index rebuilds and read
  // by the gather. Not the hull table's maxima: those size a region's ghost zone and are the
  // ceiling, not the bill (Design/MmoScalabilityReview.md U2).
  NeighbourhoodExtent m_extent;

  // Counted per tick and reset by each gather: a readout, never read by the simulation.
  std::uint64_t m_gatheredCandidates = 0;
  std::uint64_t m_queriedCandidates = 0;
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
