#pragma once

#include "Formation.h"
#include "HullSpec.h"
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
// (Design/Archive/Stations.md 7.4, ADR 0040).
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
  // a departure is the right place to keep one anyway (ADR 0047).
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
  // handles naming lives, and what groups already do for any despawn (Design/Archive/Stations.md 7.3).
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
  // (Design/Archive/Stations.md 8.5) -- but the user-station design inherits a table that already tolerates
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
  // than docking and being relaunched a tick later (Design/Archive/Stations-slice-4.md 2.4).
  struct ProtectorDuty
  {
    StationId home = INVALID_STATION_ID;
    ShipHandle target;
    bool active = false;
  };

  // What one mount is doing: where it is pointed, when it may fire again, and what it is pointed at.
  //
  // The held target is avoidHeadingRad's argument at gunnery scale. Without it a mount flickers
  // between two candidates scoring within noise of each other and restarts its traverse every time
  // one of them edges ahead; with it, the mount keeps last tick's target for as long as that target
  // is still live and still valid, and re-chooses the moment it is not. It is a tie-break, never a
  // commitment (Design/Combat.md 5.2).
  //
  // All of it is intent, which is what the snapshot exists to withhold: the view will slew its
  // turrets off the fire block and its own clock, and is allowed to disagree by a degree.
  struct MountState
  {
    float aimBearingRad = 0.0f; // hull frame, inside the mount's authored arc
    std::uint32_t cooldownTicks = 0;
    ShipHandle target;
  };

  // One ship's mounts, fixed capacity, for the reason Route carries its waypoints that way: a dense
  // array is what keeps despawn cheap and iteration order the array's. Entries past the hull's own
  // mountCount are never read and stay at rest, so a reloaded row equals the row that was saved.
  struct ShipMounts
  {
    MountState mount[MAX_MOUNTS];
  };

  // An index into the fleet table. A bare index with no generation, exactly as StationId is: rows
  // do retire here, but nothing holds one across a tick -- an owner and a slot is the name that
  // survives a retirement, and it is the name the player reads off a button.
  using FleetId = std::uint32_t;
  static constexpr FleetId INVALID_FLEET_ID = 0xFFFFFFFFu;

  // A fleet: who owns it, which of that owner's five slots it holds, and who is in it.
  //
  // Simulation state, and not a remembered selection. A control group changes what a tap does and
  // nothing else, which is why one lives in the view; a fleet's caps are rules an adapter must not
  // be able to talk its way past, its defense runs beyond every interest radius, and orders name
  // it -- so it is here, in the replay contract and in the save format with everything else Step
  // reads (ADR 0048, Design/Archive/Fleets.md 4.1).
  //
  // Members are handles rather than a table parallel to m_ships, and that is what stops the despawn
  // repair gaining a fifth table: a member that docked or died simply stops resolving, and the
  // fleet pass prunes it in place. Entries past memberCount are null handles and the pass keeps
  // them that way -- a route deliberately leaves its dead waypoints as they were, because nothing
  // reads or writes them, while a fleet row is small enough to be compared whole and a defined tail
  // is what makes a reloaded row equal the row that was saved.
  //
  // Design/Archive/Fleets.md 4.1 spells the finished row: a launch manifest, a standing order, a threat and
  // an alert. Each arrives with the slice that reads it, so that no field reaches the save format
  // before there is a test that can reach it.
  struct Fleet
  {
    FactionId ownerFaction = FACTION_PLAYER;

    // Which of the owner's FLEET_SLOTS this one holds: unique among that owner's live fleets.
    std::uint8_t slot = 0;

    ShipHandle members[MAX_FLEET_SHIPS];
    std::uint32_t memberCount = 0;

    // The launch manifest: hulls composed into this fleet and still inside the station. The rows
    // left the ledger when the fleet was composed, so nothing else can claim them, and the
    // metronome in StepFleets is what turns them into ships (Design/Archive/Fleets.md 5.2, 5.3).
    //
    // memberCount + manifestCount <= MAX_FLEET_SHIPS is the invariant compose establishes and every
    // launch preserves: a launch moves one hull from the second count to the first, and a loss only
    // lowers the first. The codec checks it, because a file that broke it would launch a ship into a
    // row with nowhere to put it.
    ShipHandle launchStructure; // the station the manifest is inside; its death strands the manifest
    std::uint32_t manifest[MAX_FLEET_SHIPS]{};
    std::uint32_t manifestCount = 0;
    std::uint32_t launchCooldownTicks = 0;

    // The standing order, at fleet grain. Members derive their own intent from it and keep deriving
    // it for as long as it stands, which is what makes a fleet arrive whole through traffic and
    // what makes a hull launched after the order join it rather than the rally (Design/Archive/Fleets.md 6).
    //
    // The station is a handle rather than an id, for ADR 0005's reason and the docking table's: an
    // order that outlives the station it names must stop resolving rather than name whatever ship
    // took that index.
    FleetOrderKind orderKind = FleetOrderKind::Idle;
    WorldPos orderPoint;         // Move
    float orderFacingRad = 0.0f; // Move
    bool orderHasFacing = false; // Move
    ShipHandle orderStation;     // Dock
    ShipHandle orderTarget;      // Attack

    // The defense (Design/Archive/Fleets.md 7). Who was last stated to have attacked a member, where that
    // act was stated, and how long the fleet stays roused by it.
    //
    // The anchor is the ground that was struck and not the fleet or the fight, which is what makes
    // the leash release at all: measured from the pursuers it never would, because chasing keeps the
    // distance small. There is no aim point beside it -- what a pursuer last aimed at is its own
    // route's destination, which is where the protector keeps it and where PURSUIT_REPLAN_METRES is
    // measured from, and a second copy here would be one more thing for the codec to carry and for
    // the two to disagree about.
    ShipHandle threat;
    WorldPos threatAnchorPos;
    std::uint32_t alertTicks = 0;
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

  // --- the shot log ---------------------------------------------------------------------------

  // Every shot that landed, read by cursor and trimmed by whoever is reading -- the despawn log's
  // mechanism with a different record, and ADR 0027's argument unchanged (Design/Combat-slice-2.md 2.1).
  //
  // It is a log rather than one tick's worth because an update goes out every
  // INTEREST_UPDATE_EVERY_TICKS and a view fed only the newest tick would miss five sixths of the
  // gunfire in the game.
  //
  // Deliberately NOT in the save format. Nothing in Step reads it, so it changes no recorded
  // outcome, and a reloaded world with no tracers pending is the correct picture of one that has
  // just resumed. The despawn log is saved because a client that missed a death has a ghost ship for
  // the rest of the match; a client that missed a muzzle flash has nothing at all.
  [[nodiscard]] std::uint64_t ShotHead() const noexcept
  {
    return m_shotBase + m_shotLog.size();
  }

  [[nodiscard]] std::span<const ShotRecord> ShotsSince(std::uint64_t _cursor) const noexcept;

  // Drops every shot before _cursor. The publisher passes the minimum across its subscribers, for
  // the reason TrimDespawnsBefore gives: the log does not know who is reading it.
  void TrimShotsBefore(std::uint64_t _cursor) noexcept;

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
  // (Design/Archive/Stations.md 4.3).
  [[nodiscard]] std::uint8_t HostileMaskFor(FactionId _viewer) const noexcept;

  // The server's judgment on a hostile act against a station: the attacker's faction becomes
  // Hostile in the station owner's eyes, permanently and empire-wide.
  //
  // Permanently, because forgiveness is a standings design of its own and the owner chose
  // permanence over inventing half of one here (Design/Archive/Stations.md 15, decision 3). Empire-wide,
  // because CVC is one government -- so a second station of the same owner refuses the attacker
  // too, without ever having been told.
  //
  // There is no client message for this and there never will be. Aggression is a judgment about
  // acts the server observed; a client that could declare one could make anybody a criminal
  // (Design/Archive/Stations.md 8.1). It arrives either from outside the tick -- an adapter, the
  // composition root, a test -- like any order, or from StepMounts, which observes a landed shot and
  // states what it saw (Design/Combat.md 6, ADR 0052). Those are the only two shapes a call can
  // take, and a client message is neither.
  //
  // A stale attacker handle is a no-op. The attacked station scrambles its garrison off the standing
  // this sets, which is the half Design/Archive/Stations.md 8.2 owns.
  void RecordAggression(ShipHandle _attacker, StationId _station);

  // The server's judgment on a hostile act against a ship: the victim's fleet is roused against the
  // attacker. RecordAggression's sentence one level down, and every word of it applies here --
  // there is no client message for this and there never will be, because a client that could
  // declare one could make anybody a target (Design/Archive/Stations.md 8.1, ADR 0041, ADR 0050).
  //
  // The fleet takes the attacker as its threat, the victim's position now as the leash's anchor, and
  // a full FLEET_ALERT_TICKS. A victim that is not a live ship, or that is in no fleet, is recorded
  // and ignored: a loose ship has no response of its own until some design gives it one.
  //
  // The attacker's liveness is deliberately not checked. Being shot by something that then died is
  // still being shot, so the alert lights either way and the posture finds nothing to pursue and
  // stands down on its own.
  //
  // It arrives either from outside the tick -- an adapter, the composition root, a test -- like any
  // order, or from StepMounts on a landed hit. The sentence that used to stand here, "nothing inside
  // Step states an act", was true until the fire pass gave the simulation something to observe;
  // what is unchanged and load-bearing is that no CLIENT message states one, and none ever will
  // (Design/Combat.md 6, ADR 0041, ADR 0052).
  void RecordHostileAct(ShipHandle _attacker, ShipHandle _victim);

  // --- stations ----------------------------------------------------------------------------------

  // Makes an existing structure ship a station. Returns its id, which is an index into the station
  // table; stations do not despawn this phase, so the index is stable.
  //
  // The ship keeps doing everything it already does -- static index, obstacle set, record on the
  // wire -- and the row is what knows it admits ships. Deliberately not a hull property: a
  // Structure that is scenery and a Structure that is a station must both be expressible, and
  // user-owned stations will be stations on other hulls (Design/Archive/Stations.md 6.1).
  //
  // Naming a ship that is not live returns INVALID_STATION_ID and makes no row.
  StationId MakeStation(ShipId _structure, const StationDesc& _desc);

  // --- docking -----------------------------------------------------------------------------------

  // What happened to a dock order. Returned for the local host's log and for tests; nothing returns
  // over the wire, because an order datagram is fire-and-forget and the client's affordance already
  // knew (Design/Archive/Stations.md 7.1, 9.2).
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

  // A ship's mounts, on the same terms again: an aim is intent, so it is server-side only and is
  // exposed for the tests and for a debug overlay.
  [[nodiscard]] const ShipMounts& MountsOf(ShipId _id) const noexcept;

  // Where a pursuit last aimed: the target's position when this ship's route was planned, which is
  // the point PURSUIT_REPLAN_METRES is measured from.
  //
  // It is exposed because it is the only place that quantity now lives. Until this design it was
  // readable off the route's destination, and a test could state the replan invariant by comparing
  // the target against RouteOf(...).back(); a stand-off put those up to 224 m apart
  // (Route::pursuitAimedAt), so the invariant needs the number itself rather than a stand-in.
  [[nodiscard]] const WorldPos& PursuitAimedAt(ShipId _id) const noexcept;

  // How many of _station's protectors are currently in space.
  //
  // Counted rather than stored, and Design/Archive/Stations.md 8.2 keeps a field for it. Storing it would
  // need a repair path nobody would remember: a protector that *dies* has to decrement it too, or
  // losses are never replaced -- and DespawnShip has no business knowing what a protector is.
  // Counting removes that path, removes a counter from the replay contract's shadow, and cannot
  // drift from the truth because it is the truth (Design/Archive/Stations-slice-4.md 2.3).
  [[nodiscard]] std::uint32_t LaunchedProtectorCount(StationId _station) const noexcept;

  // The station a ship is, or INVALID_STATION_ID if it is not one. A linear scan of a vector with
  // single digits of rows, which is free at this scale and becomes an index the day there are
  // hundreds (Design/Archive/Stations.md 6.1, 13).
  [[nodiscard]] StationId StationAt(ShipId _id) const noexcept;
  [[nodiscard]] bool IsStation(ShipId _id) const noexcept
  {
    return StationAt(_id) != INVALID_STATION_ID;
  }

  // A station's row. Server-side only: the ledger, the garrison numbers and the target list are all
  // intent or private state of the kind the snapshot exists to withhold (Design/Archive/Stations.md 6.2).
  // Exposed for tests and for a debug overlay.
  [[nodiscard]] const Station& StationOf(StationId _id) const noexcept;
  [[nodiscard]] std::uint32_t StationCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_stations.size());
  }

  // --- fleets ------------------------------------------------------------------------------------

  // Makes a fleet in _slot for _ownerFaction out of ships that are already flying. Returns its id,
  // which is an index into the fleet table.
  //
  // Every gate refuses the whole call and changes nothing -- IssueDockOrder's rule rather than
  // IssueMoveOrder's, because a fleet formed from some of the ships asked for is a fleet nobody
  // asked for, and its size is one of the rules being enforced:
  //
  //   1. a slot at or past FLEET_SLOTS, or a faction at or past FACTION_LIMIT;
  //   2. a slot this owner already holds;
  //   3. no ships at all, or more than MAX_FLEET_SHIPS of them;
  //   4. a ship that is not live, or that is not _ownerFaction's;
  //   5. a ship named twice, or one that is already in a fleet.
  //
  // The last is the fleet-only model held where it is cheapest to hold: one ship is in one fleet,
  // checked at the only place that makes a membership (Design/Archive/Fleets.md 15, decision 1).
  //
  // This is not ComposeFleet. Composing takes hulls out of a station's ledger and leaves a manifest
  // for the launch metronome; forming takes ships that are already in space, which is what the
  // starting fleet needs. A composed fleet begins with no members at all, so it cannot go through
  // this function -- what the two share is the slot gate, and they share it by both asking
  // CanTakeSlot rather than by one calling the other.
  FleetId FormFleet(FactionId _ownerFaction, std::uint8_t _slot, std::span<const ShipId> _ships);

  // The fleet in one of an owner's slots, or INVALID_FLEET_ID. This pair is the only reference to a
  // fleet that survives a tick, because a FleetId is an index and rows retire.
  [[nodiscard]] FleetId FleetInSlot(FactionId _ownerFaction, std::uint8_t _slot) const noexcept;

  // The fleet a ship is in, or INVALID_FLEET_ID. StationAt's shape and StationAt's reason: through
  // Resolve rather than by comparing stored ids, because swap-and-pop moves ids and a row holding a
  // raw one would name whichever ship arrived in that index (ADR 0005).
  [[nodiscard]] FleetId FleetAt(ShipId _id) const noexcept;

  // A fleet's row. An id past the end reads back an empty fleet rather than past the end of the
  // table -- PatrolOf's guard rather than StationOf's bare index, because a retirement makes a
  // stale FleetId reachable in a way that nothing yet makes a stale StationId.
  [[nodiscard]] const Fleet& FleetOf(FleetId _id) const noexcept;

  [[nodiscard]] std::uint32_t FleetCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_fleets.size());
  }

  // How many of each hull _asker itself has docked at _station. _outCounts is indexed by hull id and
  // must be at least HULL_COUNT long; anything past that is left alone.
  //
  // Two rules, and they are ComposeFleet's rules rather than this function's: only the asker's OWN
  // rows are counted, because whose is docked in a station is nobody else's business
  // (Design/Archive/Stations.md 6.2), and a station whose owner holds the asker hostile reads all
  // zeros, because you do not take inventory in a port that would not let you assemble in it. They
  // live here, in one function both callers use, precisely so a screen cannot offer what a compose
  // will refuse -- which is the disagreement ComposeFleet's own ledger comment exists to prevent
  // (Design/Archive/Fleets.md 5.2, 8.3).
  //
  // A station id past the table, or one whose structure is gone, reads zeros too. That is what a
  // ledger request over the wire is answered with, and it is the honest reading: an absent station
  // holds nothing.
  void LedgerFor(StationId _station, FactionId _asker, std::span<std::uint32_t> _outCounts) const noexcept;

  // What happened to a compose. Returned for the local host's log and for tests; nothing returns
  // over the wire, because an order is fire-and-forget and the client's affordance already knew --
  // IssueDockOrder's sentence, and for its reason (Design/Archive/Fleets.md 5.2).
  enum class ComposeResult : std::uint8_t
  {
    Composed,
    NotAStation,
    RefusedStanding,
    SlotTaken,
    TooMany,
    NotDocked
  };

  // Makes a fleet in _slot for _issuerFaction out of hulls docked at _station, and leaves them in
  // its manifest for the launch metronome to spawn.
  //
  // _hullCounts is indexed by hull id. Five gates, in this order, each refusing the whole call and
  // changing nothing -- the dock gate's rule rather than the move gate's, because a fleet built from
  // some of what was asked for is a fleet nobody asked for:
  //
  //   NotAStation      _station is not a live station row;
  //   RefusedStanding  the station's owner holds the issuer hostile -- you do not assemble a battle
  //                    group in a hostile port, which is the dock gate's mirror;
  //   SlotTaken        a slot past FLEET_SLOTS, or one this faction already holds. One gate and one
  //                    result: a slot that does not exist is not available either;
  //   TooMany          nothing asked for, or more than MAX_FLEET_SHIPS. One result for one gate, so
  //                    an empty compose reads as TooMany too;
  //   NotDocked        the issuer's OWN rows in this ledger do not cover it -- including any count
  //                    against a hull id past HULL_COUNT, which no ledger can hold.
  //
  // Only the issuer's own rows are counted and drawn: who else is docked in a station is nobody's
  // business, which is the line Design/Archive/Stations.md 6.2 drew around the ledger. The rows leave
  // the ledger now rather than one per launch, so a second compose cannot claim them and a screen
  // that offered them cannot disagree with what the launch finds.
  ComposeResult ComposeFleet(StationId _station, std::uint8_t _slot, std::span<const std::uint32_t> _hullCounts, FactionId _issuerFaction);

  // What happened to a fleet order. Returned for the local host's log and for tests, like every
  // other order result here; nothing returns over the wire.
  enum class FleetOrderResult : std::uint8_t
  {
    Ordered,
    NoSuchFleet,
    NotAStation,
    RefusedStanding,
    NoSuchTarget,
    RefusedFriendly,
    Unsupported
  };

  // Everything one fleet order asks for, with the ids already resolved. The wire names entities and
  // this names ships, and the publisher is where the two meet (ADR 0047).
  struct FleetCommand
  {
    FleetOrderKind kind = FleetOrderKind::Idle;
    WorldPos point;                   // Move
    float facingRad = 0.0f;           // Move
    bool hasFacing = false;           // Move
    ShipId station = INVALID_SHIP_ID; // Dock: the station's structure
    ShipId target = INVALID_SHIP_ID;  // Attack
  };

  // Orders the fleet in _slot. This is the design's title sentence as a signature: an order names a
  // FLEET, so the authority gate is one comparison -- does the issuer's faction own a live fleet in
  // that slot -- where a ship-list order needs a filter over every id in it, and an order stops
  // scaling with the number of ships in the group (ADR 0049).
  //
  // The gate is here and not in the adapter for ADR 0014's reason, and every refusal changes
  // nothing:
  //
  //   NoSuchFleet      no live fleet of _issuerFaction in that slot, a slot past the fifth included;
  //   NotAStation      Dock: the named record is not a live station row;
  //   RefusedStanding  Dock: that station's owner holds the issuer hostile -- IssueDockOrder's own
  //                    gate, surfaced rather than restated;
  //   NoSuchTarget     Attack: the named record is not a live ship;
  //   RefusedFriendly  Attack: the named record is the issuer's own faction's. NoSuchTarget would
  //                    have been a lie, and the gate is here rather than on the sheet for ADR 0014's
  //                    reason -- no mount may resolve to a friend, and neither may an order
  //                    (Design/Combat.md 11);
  //   Unsupported      Mine, which waits for a design that gives it meaning and something to mine.
  //
  // An accepted order replaces whatever standing order was there. Stop is the one kind that leaves
  // the row Idle rather than holding one: it is a brake, and "order the fleet to where it already
  // is" is a formation shuffle rather than a stop.
  FleetOrderResult IssueFleetOrder(FactionId _issuerFaction, std::uint8_t _slot, const FleetCommand& _command);

  // One fixed tick. The only thing in the game that advances simulation state.
  //
  // Standing NPC intent is issued first, before pass 0 -- the position an adapter's incoming orders
  // occupy from outside, so an NPC order and a player order entering on the same tick are
  // indistinguishable to every pass below (Design/Archive/Hostiles.md 5.3). The fire pass is the
  // last of them, so that what shoots this tick is what the postures decided this tick.
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
  // (Design/Archive/MmoScalabilityReview.md U2).
  [[nodiscard]] std::uint64_t GatheredCandidateCount() const noexcept
  {
    return m_gatheredCandidates;
  }
  [[nodiscard]] std::uint64_t QueriedCandidateCount() const noexcept
  {
    return m_queriedCandidates;
  }

  // How many routes have been planned since this world began. A readout, never read by the
  // simulation, and here for the reason the two counters above are: a planner quietly running every
  // tick and one running only when something changed look exactly alike from the outside -- the
  // ships are in the right places either way -- until somebody counts.
  //
  // A route is a pure function of the static set and the two endpoints, so re-planning without one
  // of those changing costs an A* and buys nothing (PlanRoute). The number is what lets a test say
  // that rather than a comment claiming it.
  [[nodiscard]] std::uint64_t RoutePlanCount() const noexcept
  {
    return m_routePlans;
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
  // collection order is array order, which is deterministic (Design/Archive/Stations.md 10).
  void StepDockings();

  void StepPatrols();

  // The protector response, third in the standing-intent slot.
  //
  // Three steps in order: the duty pass re-targets and pursues, the launch pass tops up each
  // station's garrison on its metronome, and the launches are applied after both -- because a spawn
  // appends to the very tables the pass is walking, which is the dock pass's argument for its
  // captures from the other direction (Design/Archive/Stations.md 10).
  void StepProtectors();

  // The fleet pass, last in the standing-intent slot.
  //
  // Last, and that is behavior rather than tidiness: the dock pass despawns and the protector pass
  // spawns, both before this one, so a member that docked or died anywhere in this tick has left
  // its fleet by the end of the tick it left on rather than a tick later.
  //
  // Two halves. Prune compacts each row's members in place, dropping handles that no longer
  // resolve -- StepProtectors' idiom on a station's target list, so the survivors keep the order
  // the fleet was formed in. Retire removes a fleet with nothing left in it, by swap-and-pop and
  // walking backwards, so the row brought in from the end is one this walk has already looked at.
  void StepFleets();

  // The fire pass, and the last thing in the tick.
  //
  // It is the pass that makes this simulation lethal, and the caller ADR 0041 and ADR 0050 were both
  // written waiting for: nothing else in the game observes a shot, so nothing else can honestly
  // state an act.
  //
  // *Last*, rather than beside the other standing behaviours at the top, and the reason is the
  // neighbour list. Opportunistic acquisition reads the list the sense pass built, and a
  // Neighbour names a ShipId -- which is an array index that every despawn moves (ADR 0005). The
  // list is therefore only trustworthy between the gather that built it and the next despawn, and
  // running here is what puts the whole pass inside that window: the postures above have already
  // decided, the gather has already run, and nothing after it can invalidate what it read. It also
  // means a mount fires on where the ships have actually ended the tick rather than on where they
  // began it, which is the more honest of the two readings.
  //
  // Gather-then-apply, the dock pass's idiom, in four steps whose order is load-bearing:
  //
  //   1  walk     per ship, per mount: cool down, choose a target, slew, test the gates, record a
  //               shot. Writes only the visiting ship's own mounts, so it is free of array order.
  //   2  damage   subtract, saturating; an indestructible hull discards its share.
  //   3  acts     RecordHostileAct for every hit, RecordAggression where the victim was a station or
  //               a garrison ship -- BEFORE the deaths below, because RecordHostileAct resolves its
  //               victim and a fleet member killed outright would otherwise rouse nobody.
  //   4  deaths   through DespawnShip, which is where the shatter, the departure runs, the fleet
  //               prune and the protector stand-down already live (ADR 0040).
  void StepMounts();

  // Whether a mount on _shooter could shoot _target at all: live, not itself, not its own faction --
  // ever -- and inside the device's envelope, which is measured to the target's skin.
  //
  // Range belongs here rather than in the firing gates because it decides *selection*: a mount whose
  // ordered target is a kilometre away is not a mount that holds its fire, it is a mount that has
  // nothing stated in reach and falls through to what it can see. An escort therefore defends the
  // fleet it is flying with while the fleet flies at something else.
  [[nodiscard]] bool MountTargetStands(ShipId _shooter, ShipId _target, const DeviceSpec& _device) const noexcept;

  // What one mount will shoot at this tick, by Design/Combat.md 5.2's fixed order: the fleet's
  // threat, the fleet's ordered target, the ship's protector duty, the target it already held, and
  // then the nearest standing-hostile it can see. First that stands.
  [[nodiscard]] ShipId ChooseMountTarget(ShipId _ship, const DeviceSpec& _device, const MountState& _mount) const noexcept;

  // Whether _owner's faction holds _other's hostile. The gate on the one priority that is a sense
  // rather than a statement: no radius may make anyone a criminal, and none may start a war either.
  [[nodiscard]] bool HoldsHostile(ShipId _owner, ShipId _other) const noexcept;

  // Whether _ownerFaction could take _slot right now. The one gate FormFleet and ComposeFleet share:
  // both refuse a slot past the fifth and a slot already held, and neither may invent its own answer
  // to that question.
  [[nodiscard]] bool CanTakeSlot(FactionId _ownerFaction, std::uint8_t _slot) const noexcept;

  // Puts a fleet's standing order onto the ships that are out, through the same calls a player's
  // click has always gone through. Called when the order is given and again whenever a launch adds
  // a member, so a hull born after the order joins the formation solved for it rather than the
  // rally it would otherwise have flown to.
  void LowerFleetOrder(Fleet& _fleet);

  // The slowest member's top speed, or 0 when the fleet has nothing out: what every member's order
  // speed cap is set to while the fleet is going somewhere together (Design/Archive/Fleets.md 6.3).
  [[nodiscard]] float FleetCruiseSpeedMetresPerSec(const Fleet& _fleet) const noexcept;

  // Aims one ship at another and keeps it aimed: re-planned when the ship has nothing to do, or when
  // the target has walked PURSUIT_REPLAN_METRES from the point last aimed at, and never every tick.
  //
  // It stops short by EngageStandoffMetres, so a hull with turrets holds where they all bear instead
  // of closing to contact and parking its guns on its quarry's hull. A hull whose mounts are all
  // fixed keeps the old behaviour and is sent at the target itself, which is what makes it fly
  // attack runs (Design/Combat.md 8).
  //
  // One function with two masters, which is what the design means by the fleet defense being the
  // protector's chassis: the protector duty and the fleet posture both call it, so neither can drift
  // from the other the first time one of them is retuned (Design/Archive/Fleets.md 3).
  void PursueTarget(ShipId _ship, ShipId _target);

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
  // is the number to remember if churn ever makes it matter (ADR 0047).
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
  std::vector<ShotRecord> m_shotLog;       // the same, for gunfire, and not saved (ShotHead)
  std::uint64_t m_shotBase = 0;
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

    // Where the target was when a pursuit planned this route -- and only a pursuit writes it.
    //
    // Design/Archive/Fleets-slice-4.md 2.3 refused a stored aim point on the ground that the point
    // last aimed at was already `destination`. That was true until a pursuit gained a stand-off:
    // the destination now sits up to 224 m from the target, so measuring the target's drift against
    // it reads a constant offset as movement, exceeds PURSUIT_REPLAN_METRES on every tick of every
    // chase, and re-plans an A* sixty times a second -- which is the entire cost that constant
    // exists to avoid. A premise that expired rather than a rule that was wrong (ADR 0052).
    WorldPos pursuitAimedAt;

    // Consecutive ticks the blocking pass has pushed this ship away from its own steer target. Counted
    // by ApplyBlocking, reset by every plan and by every tick that is not blocked, and read by
    // AdvanceRoute: at BLOCKED_WAYPOINT_TICKS the waypoint is taken as reached, because a ship that
    // has pushed at a wall for a second is as close to the point as it is ever going to get.
    std::uint32_t blockedTicks = 0;
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
    // returns to the complement by simply no longer being counted (Design/Archive/Stations.md 8.3).
    bool isGarrison = false;
  };
  std::vector<Capture> m_captureScratch;

  // A protector's duty, parallel to m_ships and swap-and-popped with them exactly as m_routes,
  // m_patrols and m_dockings are. The fourth table the despawn repair covers.
  std::vector<ProtectorDuty> m_protectors;

  // A ship's mounts, and the fifth. Not a field on ShipState for the sentence that struct makes
  // about itself: nothing in it a snapshot could not carry, and an aim is exactly the kind of intent
  // the snapshot withholds.
  std::vector<ShipMounts> m_mounts;

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

  // What the fire pass decided this tick, applied after its walk rather than during it -- the dock
  // pass's argument, and the same reason: step 4 despawns, and every despawn swap-and-pops, so an
  // id collected during the walk names a different ship by the time the shot before it has landed.
  // Handles survive that; ids do not. Reused, so a tick in which nobody fires allocates nothing.
  struct Shot
  {
    ShipHandle shooter;
    ShipHandle victim;
    std::uint32_t damage = 0;
    std::uint32_t mount = 0; // which of the shooter's mounts fired: the view's muzzle, nothing else
  };
  std::vector<Shot> m_shotScratch;

  // Who reached zero, collected while damage is applied and spent after the acts are stated.
  std::vector<ShipHandle> m_deathScratch;

  // The stations. Indexed by StationId and *not* parallel to m_ships, which is the difference worth
  // seeing: a patrol belongs to a ship, a station is a thing a ship happens to be. Nothing removes
  // from it this phase, so an index stays an index.
  std::vector<Station> m_stations;

  // The fleets, of every faction. Dense and walked in array order like everything else here, and
  // deliberately NOT parallel to m_ships: a fleet is a thing ships belong to, where a patrol is
  // something a ship has. Rows retire, so an index is not a name -- FleetInSlot is.
  std::vector<Fleet> m_fleets;

  // A fleet's live members as ship ids, gathered for the rally order so that a launch allocates
  // nothing. Reused, like every other scratch here.
  std::vector<ShipId> m_fleetShipScratch;

  // Who holds whom hostile. Mutated only by RecordAggression -- from outside the tick, or from
  // StepMounts, which is the last pass in it and states its acts after every mount has already
  // chosen (Design/Combat.md 6). So a tick's reads of this table all precede that tick's writes, and
  // it is read pointwise and never iterated, so no pass of Step depends on its contents in array
  // order.
  StandingTable m_standings = DEFAULT_STANDINGS;

  std::vector<WorldPos> m_routeScratch;
  std::vector<PathGrid::Obstacle> m_obstacleScratch;

  // Scratch, all of it sized by ship count and reused, so a tick allocates nothing once the fleet
  // has stopped growing.
  std::vector<SpatialIndex::Entry> m_staticEntries;
  std::vector<SpatialIndex::Entry> m_dynamicEntries;

  // The largest and fastest hulls actually in this world, recomputed as the index rebuilds and read
  // by the gather. Not the hull table's maxima: those size a region's ghost zone and are the
  // ceiling, not the bill (Design/Archive/MmoScalabilityReview.md U2).
  NeighbourhoodExtent m_extent;

  // Counted per tick and reset by each gather: a readout, never read by the simulation.
  std::uint64_t m_routePlans = 0;
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
