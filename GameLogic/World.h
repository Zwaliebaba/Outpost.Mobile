#pragma once

#include "Formation.h"
#include "Movement.h"
#include "PathIslands.h"
#include "Patrol.h"
#include "ShipState.h"
#include "SpatialIndex.h"
#include "WorldPos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// Why a ship stopped existing.
//
// Hostiles 4.4 opened this door "the width of one list and no wider" so that a client could stop
// inferring a death from an absence; docking is the second cause through the same door rather than
// a parallel mechanism beside it. Jump-out, wreck-and-salvage and capture are each one more
// (Design/Stations.md 7.4, ADR 0040).
enum class DespawnCause : std::uint8_t
{
  Destroyed,
  Docked
};

struct DespawnRecord
{
  ShipHandle handle;

  // Who departed, as against which slot it was in. The three departure runs on the wire name ships
  // that are already gone, so the publisher cannot ask the world for their ids -- and the record of
  // a departure is the right place to keep one anyway (ADR 0044).
  EntityId entity = INVALID_ENTITY_ID;

  DespawnCause cause = DespawnCause::Destroyed;
};

// The authoritative world. One dense array per entity kind, indexed by id -- no maps, no pointers
// between entities, no iteration order that is not array order, because all three are how a
// simulation stops reproducing itself.
//
// Ownership: whatever thread ticks the World owns it. Nothing else writes to it, and the view
// reads it only between ticks. When the halves separate this class is what moves to the server and
// the view stops holding a reference to it (AGENTS.md 2).
class World
{
  // The state codec reads and writes every field below, which is what a save and a shard handoff
  // are made of. A friendship naming two functions is a targeted, reviewable grant; the alternative
  // is thirty accessors that exist for one caller and widen this class's surface for every other
  // caller forever (Design/Archive/WorldState-work-order.md 2.3).
  friend void WriteWorldState(const World& _world, std::vector<std::uint8_t>& _outBytes);
  friend bool ReadWorldState(std::span<const std::uint8_t> _bytes, World& _outWorld);

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

  // A ship's intent to dock somewhere. A handle rather than a station id, so a station whose
  // structure died ends the approach instead of sending a ship at an index that now means something
  // else -- the same reason Patrol anchors on one (ADR 0005).
  struct Docking
  {
    ShipHandle station;
    bool active = false;
  };

  // An index into the station table. Nested beside the types it names, as Patrol is: a station is
  // World's concept, where ShipId is everybody's. Stations do not despawn this phase, so an index
  // stays an index and needs no generation.
  using StationId = std::uint32_t;
  static constexpr StationId INVALID_STATION_ID = 0xFFFFFFFFu;

  // One ship inside a station's ledger. Hull and faction is the whole of what a ship *is* today;
  // when undocking arrives it spawns a fresh ship from this row, with a fresh handle, so control
  // groups will have pruned the docked member and will not reclaim it -- the honest consequence of
  // handles naming lives, and what groups already do for any despawn (Design/Stations.md 7.3).
  struct DockedShip
  {
    std::uint32_t hullId = 0;
    FactionId factionId = FACTION_PLAYER;
  };

  // What a station is made with. All content, passed in by whoever makes the station, the way a
  // patrol's ring radius and cruise speed are -- not tuning constants, because what the composition
  // root spawns is content (Design/Archive/Hostiles.md 5.1).
  struct StationDesc
  {
    FactionId ownerFaction = FACTION_VANGUARD;

    // The garrison. Nothing reads these until the protector response lands; they are here now
    // because the composition root registers the stations before that slice exists, and a desc that
    // could not carry them would make the root unable to say what it means.
    std::uint32_t protectorHullId = 0;
    std::uint32_t protectorComplement = 0; // 0: this station never launches anything
    std::uint32_t launchEveryTicks = 90;
    std::uint32_t targetCap = 4; // the most aggressors one station tracks at once
  };

  // A station: the structure ship, who owns it, what it would launch, and who is inside.
  //
  // The handle is a handle rather than an id for ADR 0005's reason, and every read of it goes
  // through Resolve, so a row whose ship is gone reports inactive instead of dangling. Nothing can
  // destroy anything this phase and a Vanguard station is indestructible as a rule
  // (Design/Stations.md 8.5) -- but the user-station design inherits a table that already tolerates
  // death, which is the point of doing it now.
  struct Station
  {
    ShipHandle structure;
    FactionId ownerFaction = FACTION_VANGUARD;
    std::uint32_t protectorHullId = 0;
    std::uint32_t protectorComplement = 0;
    std::uint32_t launchEveryTicks = 90;
    std::uint32_t targetCap = 4;

    // Who this station's protectors are hunting, capped at targetCap and pruned of stale handles as
    // it is read, so it stays dense and in arrival order. A full list drops the *newest*: the
    // standing flip has already happened, which is the part that matters.
    std::vector<ShipHandle> targets;

    // Ticks until the next protector may launch. Reset while there is nobody to hunt, so a station
    // at peace is not running a metronome.
    std::uint32_t launchCooldownTicks = 0;

    std::vector<DockedShip> docked; // the ledger: who is inside. Filled by the dock pass.
  };

  // What one protector is for.
  //
  // active means "this ship is a garrison ship of home, and it is in space" -- not "it is hunting".
  // The distinction is load-bearing: a protector flying home is still out, still counts against the
  // complement, and still has to be told apart from a visitor when it reaches the door. A stale
  // target with nothing to replace it is what standing down means; the duty ends when the ship
  // docks, and it ends by being swap-and-popped away with it.
  //
  // It also means a protector already on its way home picks up a new target and turns round rather
  // than docking and being relaunched a tick later (Design/Stations-slice-4.md 2.4).
  struct ProtectorDuty
  {
    StationId home = INVALID_STATION_ID;
    ShipHandle target;
    bool active = false;
  };

  // Adds a ship at rest. Returns its id, which is its index for as long as nothing is despawned.
  // Take HandleOf if the reference has to outlive a tick.
  //
  // The faction defaults because every existing caller -- the starting fleet, every test -- spawns
  // the player's own ships, so the default states what those call sites already mean rather than
  // papering over them. A caller that means someone else has to say so (Design/Archive/Hostiles.md 11).
  ShipId SpawnShip(const WorldPos& _posWorld, float _headingRad, std::uint32_t _hullId, FactionId _factionId = FACTION_PLAYER);

  // Spawns under an identity issued somewhere else: a handoff from another shard, or a saved world
  // being reloaded. Returns INVALID_SHIP_ID if the id is null or already here, because an entity
  // existing twice in one world is the failure this whole mechanism exists to make impossible.
  //
  // It also advances this world's serial counter past any *local* id it is handed, so a reload
  // cannot go on to mint an id the file already used. That is one line written for a slice that
  // does not exist yet, and it is here because it is one line now and a corruption bug later
  // (Design/Archive/EntityIdentity-work-order.md 3.4).
  ShipId SpawnShipAs(EntityId _entity, const WorldPos& _posWorld, float _headingRad, std::uint32_t _hullId,
                     FactionId _factionId = FACTION_PLAYER);

  // Removes a ship, moving the last one into its slot. False means the handle was already stale.
  //
  // Every stored reference to the removed ship stops resolving; every stored reference to the ship
  // that moved keeps resolving, to the same ship. That second half is the reason handles exist.
  //
  // The cause is defaulted because every caller that existed before docking meant Destroyed, and a
  // default states what those call sites already mean rather than papering over them -- the same
  // argument SpawnShip's faction default makes.
  bool DespawnShip(ShipHandle _handle, DespawnCause _cause = DespawnCause::Destroyed);

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
  [[nodiscard]] std::span<const DespawnRecord> DespawnsSince(std::uint64_t _cursor) const noexcept;

  // Drops every death before _cursor. The publisher passes the minimum cursor across its
  // subscribers, so what remains is exactly what at least one of them has still to hear -- which is
  // why the log cannot do this for itself: it does not know who is reading it.
  void TrimDespawnsBefore(std::uint64_t _cursor) noexcept;

  // The handle for a live ship. Null (generation 0) if the id is not one.
  [[nodiscard]] ShipHandle HandleOf(ShipId _id) const noexcept;

  // The ship a handle names, or INVALID_SHIP_ID if it has been despawned.
  [[nodiscard]] ShipId Resolve(ShipHandle _handle) const noexcept;

  // --- identity ------------------------------------------------------------------------------------

  // Which shard this world mints ids for. Configured by the composition root before anything spawns,
  // beside ConfigureIndex and for AGENTS.md 5's reason: a library takes a plain value from the root
  // and never reads a file or an environment. Ids already issued keep the shard they were issued
  // under, so calling this after a spawn changes nothing that exists.
  void ConfigureShard(ShardId _shard) noexcept;

  [[nodiscard]] ShardId Shard() const noexcept
  {
    return m_shard;
  }

  // Who a live ship is. INVALID_ENTITY_ID for an id or a handle that is not one.
  [[nodiscard]] EntityId EntityIdOf(ShipId _id) const noexcept;
  [[nodiscard]] EntityId EntityIdOf(ShipHandle _handle) const noexcept;

  // The other direction, for the two places that hold an id rather than a handle: the publisher
  // reading an order off the wire, and the composition root's debug despawn. O(log N) over a sorted
  // vector rather than O(1) over a map, because World holds no maps and ADR 0010 already chose this
  // shape for the same reason.
  // Named rather than overloaded, deliberately: ShipId is a u32 and EntityId a u64, so Resolve(id)
  // with a ShipId in hand would compile and silently mean the other thing.
  [[nodiscard]] ShipId ResolveEntity(EntityId _entity) const noexcept;
  [[nodiscard]] ShipHandle HandleOfEntity(EntityId _entity) const noexcept;

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

  // --- standings ---------------------------------------------------------------------------------

  // The owner's opinion of the other. Directional: "CVC despises you" and "you despise CVC" are
  // different facts, and only the first decides whether CVC lets you dock.
  //
  // A faction id at or past FACTION_LIMIT reads back Hostile. Nobody authored it, every caller of
  // this is a gate or a warning colour, and a stranger admitted as a friend is the one mistake this
  // table must not make -- the same direction WorldView::LiveryOf takes, for the same reason.
  [[nodiscard]] Standing StandingOf(FactionId _owner, FactionId _other) const noexcept;

  // Bit f set: faction f currently holds _viewer's faction hostile. What the wire states to each
  // subscriber so a client's affordances can tell the truth without inferring anything
  // (Design/Stations.md 4.3).
  [[nodiscard]] std::uint8_t HostileMaskFor(FactionId _viewer) const noexcept;

  // The server's judgment on a hostile act against a station: the attacker's faction becomes
  // Hostile in the station owner's eyes, permanently and empire-wide.
  //
  // Permanently, because forgiveness is a standings design of its own and the owner chose
  // permanence over inventing half of one here (Design/Stations.md 15, decision 3). Empire-wide,
  // because CVC is one government -- so a second station of the same owner refuses the attacker
  // too, without ever having been told.
  //
  // There is no client message for this and there never will be. Aggression is a judgment about
  // acts the server observed; a client that could declare one could make anybody a criminal
  // (Design/Stations.md 8.1). It arrives from outside the tick -- an adapter, the composition root,
  // a test -- like any order.
  //
  // A stale attacker handle is a no-op. Slice 4 adds the second half: the attacked station
  // scrambles its garrison.
  void RecordAggression(ShipHandle _attacker, StationId _station);

  // --- stations ----------------------------------------------------------------------------------

  // Makes an existing structure ship a station. Returns its id, which is an index into the station
  // table; stations do not despawn this phase, so the index is stable.
  //
  // The ship keeps doing everything it already does -- static index, obstacle set, record on the
  // wire -- and the row is what knows it admits ships. Deliberately not a hull property: a
  // Structure that is scenery and a Structure that is a station must both be expressible, and
  // user-owned stations will be stations on other hulls (Design/Stations.md 6.1).
  //
  // Naming a ship that is not live returns INVALID_STATION_ID and makes no row.
  StationId MakeStation(ShipId _structure, const StationDesc& _desc);

  // --- docking -----------------------------------------------------------------------------------

  // What happened to a dock order. Returned for the local host's log and for tests; nothing returns
  // over the wire, because an order datagram is fire-and-forget and the client's affordance already
  // knew (Design/Stations.md 7.1, 9.2).
  enum class DockOrderResult : std::uint8_t
  {
    Ordered,
    NotAStation,
    RefusedStanding
  };

  // Sends the given ships to dock at _station. Three gates, here and not in the adapter, for
  // ADR 0014's reason -- the simulation refusing is a property, an adapter refusing is a convention:
  //
  //   1. _station must be a live station row, or the order is a no-op;
  //   2. ships that are not _issuerFaction's are dropped, exactly as the move gate drops them;
  //   3. an issuer the station's owner holds hostile is refused entirely -- an aggressor is not
  //      allowed to dock, and refused means nothing changes at all.
  //
  // An accepted order clears each ship's patrol (an explicit order outranks a standing behavior) and
  // issues the first approach leg immediately, so an order feels like an order rather than like a
  // next-tick suggestion.
  DockOrderResult IssueDockOrder(std::span<const ShipId> _ships, ShipId _station, FactionId _issuerFaction);

  // A ship's docking intent. Server-side only, like a route and like a patrol: an intent is what the
  // snapshot exists to withhold. Exposed for tests and for a debug overlay.
  [[nodiscard]] const Docking& DockingOf(ShipId _id) const noexcept;

  // A ship's protector duty, on the same terms.
  [[nodiscard]] const ProtectorDuty& ProtectorOf(ShipId _id) const noexcept;

  // How many of _station's protectors are currently in space.
  //
  // Counted rather than stored, and Design/Stations.md 8.2 keeps a field for it. Storing it would
  // need a repair path nobody would remember: a protector that *dies* has to decrement it too, or
  // losses are never replaced -- and DespawnShip has no business knowing what a protector is.
  // Counting removes that path, removes a counter from the replay contract's shadow, and cannot
  // drift from the truth because it is the truth (Design/Stations-slice-4.md 2.3).
  [[nodiscard]] std::uint32_t LaunchedProtectorCount(StationId _station) const noexcept;

  // The station a ship is, or INVALID_STATION_ID if it is not one. A linear scan of a vector with
  // single digits of rows, which is free at this scale and becomes an index the day there are
  // hundreds (Design/Stations.md 6.1, 13).
  [[nodiscard]] StationId StationAt(ShipId _id) const noexcept;
  [[nodiscard]] bool IsStation(ShipId _id) const noexcept
  {
    return StationAt(_id) != INVALID_STATION_ID;
  }

  // A station's row. Server-side only: the ledger, the garrison numbers and the target list are all
  // intent or private state of the kind the snapshot exists to withhold (Design/Stations.md 6.2).
  // Exposed for tests and for a debug overlay.
  [[nodiscard]] const Station& StationOf(StationId _id) const noexcept;
  [[nodiscard]] std::uint32_t StationCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_stations.size());
  }

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

  // How many islands the architecture came to, and how many of those refused to build because a
  // single island is wider than one grid may be. A declining island keeps its neighbours routing --
  // that is the whole gain over one grid for the world -- but it is indistinguishable from open
  // space to everything that reads it, so the count is surfaced rather than left to be inferred from
  // ships that quietly stopped avoiding things (Design/Archive/RegionalPathfinding.md 3.3).
  [[nodiscard]] std::size_t PathIslandCount() const noexcept
  {
    return m_pathIslands.IslandCount();
  }
  [[nodiscard]] std::size_t DeclinedPathIslandCount() const noexcept
  {
    return m_pathIslands.DeclinedCount();
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
  // The dock pass, first in the standing-intent slot and therefore before StepPatrols.
  //
  // First because it is the pass that despawns: applying its captures before the others run means
  // they iterate the repaired arrays, exactly as if the despawn had arrived from outside between
  // ticks, which is a case every table already survives.
  //
  // Captures are collected during the walk and applied after it. DespawnShip swap-and-pops four
  // parallel tables, so removing mid-iteration would make the visit order depend on who docked;
  // collection order is array order, which is deterministic (Design/Stations.md 10).
  void StepDockings();

  void StepPatrols();

  // The protector response, last in the standing-intent slot.
  //
  // Three steps in order: the duty pass re-targets and pursues, the launch pass tops up each
  // station's garrison on its metronome, and the launches are applied after both -- because a spawn
  // appends to the very tables the pass is walking, which is the dock pass's argument for its
  // captures from the other direction (Design/Stations.md 10).
  void StepProtectors();

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

    // The identity, kept here rather than in an array parallel to m_ships: the slot is already the
    // indirection that survives swap-and-pop, so this is one fewer table for despawn to keep in
    // step. Exactly 16 bytes with the two above it.
    EntityId entity = INVALID_ENTITY_ID;
  };

  // An id to the slot that holds it, sorted by id so that a lookup is a binary search.
  //
  // Not a map: World.h's own rule at the top of this class, and ADR 0010's answer for interest sets.
  // Locally minted serials increase, so a spawn appends and is O(1); only an id issued elsewhere
  // inserts in the middle. A despawn is a memmove of the tail -- 60 kB at five thousand ships, which
  // is the number to remember if churn ever makes it matter (ADR 0044).
  struct EntityRow
  {
    EntityId entity = INVALID_ENTITY_ID;
    std::uint32_t slot = 0;
  };

  [[nodiscard]] std::uint32_t FindEntityRow(EntityId _entity) const noexcept;
  void InsertEntityRow(EntityId _entity, std::uint32_t _slot);
  void EraseEntityRow(EntityId _entity) noexcept;

  std::vector<ShipState> m_ships;
  std::vector<std::uint32_t> m_shipSlot; // parallel to m_ships: which slot each ship owns
  std::vector<Slot> m_slots;
  std::vector<EntityRow> m_entityRows;     // sorted by entity id; one row per live ship
  std::vector<std::uint32_t> m_freeSlots;  // reused last-in-first-out, so reuse is reproducible
  std::vector<DespawnRecord> m_despawnLog; // read by cursor, trimmed by the publisher, never by Step
  std::uint64_t m_despawnBase = 0;         // the sequence of m_despawnLog[0]; rises with every trim
  std::uint64_t m_tick = 0;

  // Issued once each and never reused, which is what makes an id an identity rather than a
  // reference. Starts at 1 so that no shard ever mints INVALID_ENTITY_ID.
  std::uint64_t m_nextEntitySerial = 1;
  ShardId m_shard = 0;

  SpatialIndex m_index;
  PathIslands m_pathIslands;
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

  // A ship's docking intent, parallel to m_ships and swap-and-popped with them exactly as m_routes
  // and m_patrols are. Not a field on ShipState for the sentence that struct makes about itself:
  // nothing in it that a snapshot could not carry, and an intent is what the snapshot withholds.
  std::vector<Docking> m_dockings;

  // What the dock pass decided this tick, applied after its walk rather than during it.
  //
  // Everything needed to complete a capture is taken while the ship is still there -- its handle,
  // its station, and the two fields the ledger row is -- so applying them needs no lookup at all.
  // Ids would not do: each despawn swap-and-pops, so an id collected during the walk names a
  // different ship by the time the one before it has been applied. Reused, so a tick that docks
  // nobody allocates nothing.
  struct Capture
  {
    ShipHandle ship;
    StationId station = INVALID_STATION_ID;
    std::uint32_t hullId = 0;
    FactionId factionId = FACTION_PLAYER;

    // A garrison ship coming home writes no ledger row: a garrison is not a guest, and the hull
    // returns to the complement by simply no longer being counted (Design/Stations.md 8.3).
    bool isGarrison = false;
  };
  std::vector<Capture> m_captureScratch;

  // A protector's duty, parallel to m_ships and swap-and-popped with them exactly as m_routes,
  // m_patrols and m_dockings are. The fourth table the despawn repair covers.
  std::vector<ProtectorDuty> m_protectors;

  // What the launch pass decided this tick, applied after it for the reason captures are.
  struct Launch
  {
    StationId home = INVALID_STATION_ID;
    WorldPos posWorld;
    float headingRad = 0.0f;
    std::uint32_t hullId = 0;
    FactionId factionId = FACTION_VANGUARD;
  };
  std::vector<Launch> m_launchScratch;

  // The stations. Indexed by StationId and *not* parallel to m_ships, which is the difference worth
  // seeing: a patrol belongs to a ship, a station is a thing a ship happens to be. Nothing removes
  // from it this phase, so an index stays an index.
  std::vector<Station> m_stations;

  // Who holds whom hostile. Mutated only by RecordAggression, which arrives from outside the tick;
  // read pointwise and never iterated, so no pass of Step depends on its contents in array order.
  StandingTable m_standings = DEFAULT_STANDINGS;

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
