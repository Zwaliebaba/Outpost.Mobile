#pragma once

#include "WorldPos.h"

#include <cstdint>

namespace Game
{
using ShipId = std::uint32_t;
inline constexpr ShipId INVALID_SHIP_ID = 0xFFFFFFFFu;

// Whose a ship is. The server states an identity and every client maps that identity to a relation
// of its own -- "hostile" is the client's word, never the wire's, which is what lets standings,
// diplomacy or a spoofed transponder arrive later as a mapping change rather than as a new field.
// It is not an NPC flag and must not become one: a client cannot tell a player's ship from an NPC's,
// which is the correct amount of knowledge (Design/Archive/Hostiles.md 4.1).
using FactionId = std::uint8_t;
inline constexpr FactionId FACTION_PLAYER = 0;

// The Vandal Collective -- "Vandal", and VANDAL where the HUD needs a word. It was FACTION_HOSTILE
// until Stations.md 4.1, and the rename is not cosmetic: Standing::Hostile below is a *relation*,
// and a faction named for it made StandingOf(FACTION_HOSTILE, ...) == Standing::Hostile a sentence
// nobody should have to parse twice. Identity constants name identities and standing values name
// relations, which is ADR 0013's split spelled into the identifiers.
inline constexpr FactionId FACTION_VANDAL = 1;

// Core Vanguard Command -- CVC, the Vanguard -- the government of known space (Design/Archive/Stations.md 4).
// Its ships and stations are ordinary records with this id: no government flag, no station-faction
// special case anywhere in the engine, for ADR 0013's reason -- the server states whose a thing is,
// and what that *means* is a mapping each side owns.
inline constexpr FactionId FACTION_VANGUARD = 2;

// How many factions the standing table below holds, and therefore how many the wire's hostileMask
// can name: the mask is a u8 (Design/Archive/Stations.md 4.3). The day factions outgrow a byte the mask
// becomes a small standings record and this limit moves with it -- widen both together.
inline constexpr std::uint32_t FACTION_LIMIT = 8;

// The player's whole command surface, and the size of one fleet in it.
//
// Both are in the replay contract: they decide which FormFleet calls are accepted, so a build that
// disagreed about either would form a fleet this one refuses. They sit beside FACTION_LIMIT because
// all three are per-faction ceilings, and because the fleet wire's slot mask will lean on
// FLEET_SLOTS exactly as the hostileMask leans on FACTION_LIMIT (Design/Archive/Fleets.md 8.2).
//
// Eight is measured rather than liked (Design/Archive/Fleets.md 4.2). A Carrier-led wedge of eight spans
// about 1 km, inside the 2,000 m interest radius, while twelve is outside it -- a fleet whose far
// wing its own player cannot see. A compressed pack of eight unjams in 0.4 s against 2.5 s at
// sixteen, which is SEPARATION_ITERATIONS' own measurement read as a fleet size. And five fleets of
// eight is forty player ships, which is the envelope every number in this tree was measured in.
inline constexpr std::uint32_t FLEET_SLOTS = 5;
inline constexpr std::uint32_t MAX_FLEET_SHIPS = 8;

// What one faction is to another.
//
// Simulation state by AGENTS.md 5's own test: it changes recorded outcomes -- who may dock, who
// gets hunted -- and a spectator would need it. It is a relation and not an identity, which is the
// whole reason the faction above stopped being called HOSTILE.
enum class Standing : std::uint8_t
{
  Neutral,
  Hostile
};

// The standings table, read as StandingOf(owner, other): *the owner's opinion of the other*.
//
// Directional, because "CVC despises you" and "you despise CVC" are different facts and the second
// is none of the simulation's business. A fixed-size array indexed by two integers -- no map, no
// hashing, no iteration at all on the hot path (Design/Archive/Stations.md 4.2, 10).
struct StandingTable
{
  // Standing::Neutral is zero, so the authored default is "everyone is neutral" and the function
  // below writes only what is not.
  Standing rows[FACTION_LIMIT][FACTION_LIMIT]{};
};

// The standings the world starts with.
//
// Built by a loop rather than written out as sixty-four literals, because the rule is one sentence
// and a grid of Neutrals is not: the Vandal Collective holds every other faction Hostile and is
// held Hostile by every other faction. The Vandals were never neutral -- the tree just had no word
// for it until now. "Every other" excludes the diagonal: a faction is not its own enemy.
[[nodiscard]] constexpr StandingTable AuthoredStandings() noexcept
{
  StandingTable table{};
  for (std::uint32_t other = 0; other < FACTION_LIMIT; ++other)
  {
    if (other == FACTION_VANDAL)
      continue;
    table.rows[FACTION_VANDAL][other] = Standing::Hostile;
    table.rows[other][FACTION_VANDAL] = Standing::Hostile;
  }
  return table;
}

inline constexpr StandingTable DEFAULT_STANDINGS = AuthoredStandings();

// A reference to a ship that survives a despawn.
//
// ShipId is a dense array index, which is what makes iteration cheap and is why it stays. The cost
// is that despawn moves the last ship into the freed slot, so any id stored across a tick boundary
// silently retargets to a stranger -- and the place that hurts is a snapshot delta baseline, where
// the symptom is not a crash but one ship smoothly interpolating into a different ship while a
// player watches (Design/Archive/Collision.md 6).
//
// So: anything that stores a reference across a tick boundary, or sends one over a wire, stores a
// ShipHandle; anything iterating within a tick uses the index. Resolving is one indexed load and a
// compare, and a stale handle resolves to nothing rather than to a stranger.
//
// The field is a slot rather than the ship's index because the design's shorter form -- generation
// alongside the index itself -- only makes the *despawned* ship's handles safe. The ship that
// swap-and-pop moved has a live handle naming an index that is now past the end, so it would
// resolve to nothing too, and a weapon would drop its target because something unrelated died. The
// slot is stable for the ship's life and costs one more indirection.
struct ShipHandle
{
  std::uint32_t slot = 0;
  std::uint32_t generation = 0; // bumped on despawn; 0 is never issued, so a default handle is null
};

[[nodiscard]] inline bool operator==(ShipHandle _a, ShipHandle _b) noexcept
{
  return _a.slot == _b.slot && _a.generation == _b.generation;
}

// Who a ship is, as against where it is. This is the one thing about an entity that never changes.
//
// A ShipHandle is a reference into one World and is allocated by it: a ship handed from one region
// server to another gets a fresh slot and generation at the destination, so a wire that named
// handles could not say "same ship, new region" -- a client keyed on them would see a destroy and an
// enter, which is exactly the continuity ADR 0005 exists to provide, lost at the shard boundary
// (Design/Archive/MmoScalabilityReview.md U3). So the wire names an EntityId and the process names a handle,
// and the publisher is where the two meet.
//
// Sixteen bits of shard and forty-eight of serial. The shard says who minted it and never who holds
// it; the serial is a per-shard counter that is issued once and never reused, which is why there is
// no generation here and why an id is not derivable from a handle. Both properties are deliberate
// and argued in ADR 0047: a slot-and-generation id looks derivable and stops being so the moment the
// entity moves, and a 24-bit generation reissues a live id after 16.7 million reuses of one slot --
// a weekend of churn. Forty-eight bits is 281 trillion, which is 8.9 million years at a million
// spawns a second.
using EntityId = std::uint64_t;
using ShardId = std::uint16_t;

// Never issued: every shard's serial starts at 1, so no shard mints zero. A default-constructed id
// is therefore null the way a default ShipHandle is.
inline constexpr EntityId INVALID_ENTITY_ID = 0;

inline constexpr int ENTITY_SERIAL_BITS = 48;
inline constexpr EntityId ENTITY_SERIAL_MASK = (EntityId{1} << ENTITY_SERIAL_BITS) - 1;

[[nodiscard]] constexpr EntityId MakeEntityId(ShardId _shard, std::uint64_t _serial) noexcept
{
  return (static_cast<EntityId>(_shard) << ENTITY_SERIAL_BITS) | (_serial & ENTITY_SERIAL_MASK);
}

[[nodiscard]] constexpr ShardId EntityShardOf(EntityId _entity) noexcept
{
  return static_cast<ShardId>(_entity >> ENTITY_SERIAL_BITS);
}

[[nodiscard]] constexpr std::uint64_t EntitySerialOf(EntityId _entity) noexcept
{
  return _entity & ENTITY_SERIAL_MASK;
}

// One shot that landed.
//
// It lives here rather than beside the despawn log it is modelled on, and the reason is the wire:
// SnapshotWriter names this type, and a header that describes the seam must not have to include the
// whole authoritative World to do it. ShipState.h is what both sides already share.
//
// Entities rather than handles or ids, and taken while both ships are still live: a shot is read by
// a subscriber several ticks after it happened, by which time either end may have been despawned,
// and an id is an identity where a handle is a reference into one World (ADR 0047).
//
// The mount index is here because the view needs to know which muzzle to flash. It is the only
// piece of a mount that ever reaches a client -- the aim, the cooldown and the held target stay
// server-side as the intent they are (Design/Combat.md 3.2).
struct ShotRecord
{
  EntityId shooter = INVALID_ENTITY_ID;
  EntityId victim = INVALID_ENTITY_ID;
  std::uint32_t mount = 0;
};

enum class OrderState : std::uint8_t
{
  Idle,
  Moving,  // steering towards steerTargetPos
  Aligning // arrived; turning onto the ordered facing
};

// What a fleet was told to do, as against what one ship is doing about it. OrderState is a ship's
// business and this is a fleet's, and the two never mean the same thing: every member of a fleet
// under Move is Moving, Aligning or Idle at its own slot at different moments of the same order.
//
// Declared whole so the byte never renumbers, and one of the six is still refused: Mine waits for a
// mining design and for something in the world to mine, since a rock is presentation and a ship
// flies through it (ADR 0016, Design/Archive/Fleets.md 6.6). Attack was refused beside it until it
// had the protector's pursuit chassis and a combatant flag to aim with, and then until the guns it
// aims were real; both have landed, and it now means what it says (Design/Combat.md 6).
//
// Stop is a kind a message carries and never a standing order a row holds: stopping is asking for
// Idle, and what the row stores is what it was left in.
enum class FleetOrderKind : std::uint8_t
{
  Idle,
  Move,
  Dock,
  Attack,
  Stop,
  Mine
};

// One ship, as the simulation sees it. Everything here is advanced only in World::Step, and there
// is nothing in it a renderer needs that a snapshot could not carry over a wire.
//
// prevPos/prevHeading are the values from the tick before, kept so that the view can interpolate
// between two ticks rather than sampling a half-stepped state. They are also the start-of-tick
// snapshot every neighbour query reads, which is what makes the tick order-independent -- see
// World::Step.
struct ShipState
{
  WorldPos posWorld;
  float headingRad = 0.0f; // 0 points north (+Z); forward is (sin h, 0, cos h)
  float speed = 0.0f;      // metres per second along the facing
  float turnRateRadPerSec = 0.0f;

  WorldPos prevPos;
  float prevHeading = 0.0f;

  OrderState order = OrderState::Idle;

  // The single point the intent layer steers at. Before pathfinding this was always the ordered
  // destination; with a planner in front of it, it is the current waypoint of a route and the
  // planner changes *which point* is steered at, never *how* (Design/Archive/Collision.md 12).
  WorldPos steerTargetPos;
  float orderFacingRad = 0.0f;
  bool orderHasFacing = false;

  // A ceiling on the speed an order asks for; 0 is uncapped. It is a property of the order, not of
  // the hull -- the same Interceptor cruises at 10 m/s on patrol and burns at 34 m/s under a player
  // -- and it is intent, so it stays off the wire beside steerTargetPos (Design/Archive/Hostiles.md 5.4).
  float orderSpeedCapMetresPerSec = 0.0f;

  // The heading the avoidance pass committed to last tick. It is the one piece of state the
  // steering carries between ticks, and it is what stops a plain per-tick argmax chattering when
  // two candidate headings score within noise of each other. Simulation state, so it goes over the
  // wire like everything else here (Design/Archive/Collision.md 10).
  float avoidHeadingRad = 0.0f;

  // Last tick's acceleration. Simulation output, read by the view to drive thruster response --
  // which is why it is here and not derived per frame: per frame it would be zero on every frame
  // that did not happen to land on a tick.
  float accelSample = 0.0f;

  // Which hull this ship uses. The simulation resolves it to a HullSpec; the view resolves it to a
  // mesh, and neither knows about the other's table.
  std::uint32_t hullId = 0;

  // Whose ship this is. Simulation state by AGENTS.md 5's own test -- a spectator would need it --
  // and it travels in the snapshot record like everything else here.
  FactionId factionId = FACTION_PLAYER;

  // What this ship has left, spawned at its hull's maxHullPoints and subtracted saturating at zero
  // by the fire pass. A hull whose maximum is zero is indestructible and this stays zero for its
  // whole life -- the one reading under which "at zero" does not mean "dead" (HullSpec.h).
  //
  // An unsigned integer rather than a float, and that is a determinism result rather than a taste:
  // every damage number in the device table is whole, so the damage path holds no float at all and
  // is bit-exact under every summation order on every machine. A float would have been fine today
  // and a liability the first afternoon somebody added a fractional resist.
  std::uint32_t hullPoints = 0;
};
} // namespace Game
