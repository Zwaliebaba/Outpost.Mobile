#pragma once

#include "HullSpec.h"
#include "ShipState.h"
#include "UniversePos.h"

#include "Transport.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
class Universe;

// The wire format between the two halves.
//
// It lives in GameLogic and not in an engine library, and the three-way elimination is worth having
// here rather than in a commit message: NeuronCore may hold no game semantics, so it cannot name a
// ShipState; NeuronServer never names GameLogic at all, which its own Simulation.h says in as many
// words; and Outpost could, but then the executable owns the wire format and the day there are two
// executables it is in the wrong one. GameLogic owns the types being encoded and already depends on
// NeuronCore, so it may include Transport.h. Same test AGENTS.md 2 applies to content readers: the
// code lives with what it is about (Design/Archive/Collision-slice-2b.md 2.2).
//
// Fields are written one at a time into a byte buffer rather than memcpy'd. ShipState is 128 bytes
// of padded struct -- it was 120 when this was written, which is the argument making itself -- whose
// layout has already changed twice in this design and will again; a field-by-field format survives
// that, and survives the day the two ends are different binaries.
//
// Field-by-field is also what let the record be quantized without a rework. A ship record is 47
// bytes, not 83: a position is a sector pair narrowed to i32 plus an offset on a 0.125 m lattice, an
// angle is a sixteenth-bit of a turn, and prevPos is an integer step delta from posUniverse rather than
// a second whole position. That is 23 records in a datagram against 13 (ADR 0046). The decoded types
// below are unchanged floats and UniversePos, so nothing above this layer knows the wire quantizes --
// what it costs is 6.25 cm on a position and pi/2^16 on an angle, stated here because it is the only
// place a reader would find it.

// One ship as a client is allowed to see it.
//
// Not ShipState. The seam exists to make "what the client may know" a reviewable list rather than
// whatever happens to be in a struct: steerTargetPos, orderFacingRad, orderHasFacing,
// avoidHeadingRad and the order's speed cap are deliberately absent, because together they tell any
// client exactly what every ship intends to do next. That is a decision to take once, here, rather
// than to discover later.
//
// factionId is on the list on purpose, and what is *not* beside it matters as much: there is no NPC
// flag, so a client cannot tell a player's ship from an NPC's -- the day real players fly beside
// NPCs in one faction, nothing on the wire changes (Design/Archive/Hostiles.md 4.2).
struct ShipSnapshot
{
  // Who this is, not where it is. A ShipHandle would have been enough while there was one Universe; an
  // id is what survives being handed between two, and it is what a client may key its own state on
  // without that state evaporating at a shard boundary (ADR 0047). The server still uses handles
  // everywhere a reference outlives a tick -- ADR 0005 is untouched -- and the publisher is where
  // the two currencies meet.
  EntityId entity = INVALID_ENTITY_ID;

  // The four quantized fields, decoded back to the types the view already reads. A position is
  // within 6.25 cm of the simulation's and an angle within pi/2^16 of it, and a decoded heading is
  // in (-pi, pi] like a simulated one -- so a source of exactly -pi arrives as +pi, the same bearing
  // and a different number (ADR 0046).
  UniversePos posUniverse;
  UniversePos prevPos;
  float headingRad = 0.0f;
  float prevHeading = 0.0f;

  float speed = 0.0f;
  float accelSample = 0.0f;
  float turnRateRadPerSec = 0.0f;
  OrderState order = OrderState::Idle;
  FactionId factionId = FACTION_PLAYER;

  // Bit 0: this record is a station that admits ships.
  //
  // Identity, which is why it is on the reviewable list beside factionId and hullId rather than
  // left to be worked out: "immovable hull of faction 2" is the client inferring server state, the
  // exact sin the destroyed list exists to prevent, and a client has to know it is tapping a
  // station before an order is worth sending (Design/Archive/Stations.md 6.2). What is deliberately *not*
  // here: the ledger, the garrison numbers and the target list -- private state of the kind the
  // snapshot exists to withhold. Seven bits are unspoken for; user stations and conquerable ones
  // are what they are being kept for.
  std::uint8_t flags = 0;

  std::uint32_t hullId = 0;

  // What this ship has left, as 255ths of whole. A fraction rather than the number, because a
  // fraction is what a pip row and a target bar draw and the number itself is server-side like every
  // other quantity the simulation reasons with.
  //
  // In the record rather than in an event, and ADR 0029's own question is what settles it: a lost
  // fraction is corrected by the next update six ticks later, so this is state that heals and late
  // is worse than lost. A hull that cannot be destroyed reads 255 -- undamaged is the only honest
  // answer for a thing with nothing to lose (Design/Archive/Combat-slice-2.md 2.2).
  std::uint8_t hullFraction = 255;
};

// Bits of ShipSnapshot::flags.
//
// What a Structure *is* travels as a flag rather than being worked out from the hull table, and that
// is the rule rather than an economy: a client that inferred "station" from "immovable" would be
// making exactly the inference the flag exists to end -- and a gate is a Structure too, so the
// inference would now be wrong as well as fragile (UniverseView::PickStation).
inline constexpr std::uint8_t SHIP_FLAG_STATION = 0x01;
inline constexpr std::uint8_t SHIP_FLAG_GATE = 0x02;

// One shot, as a client is told about it: who fired, at what, and from which mount.
//
// It is an event rather than state, and it is the only thing on this seam that is. What it does NOT
// carry is as deliberate as what it does: no damage number, because the fraction in the record
// already says what happened; and no kill attribution, because a leave run states that a ship was
// destroyed and never by whom (Design/Archive/Combat.md 9.2, 14).
struct FireEvent
{
  EntityId shooter = INVALID_ENTITY_ID;
  EntityId target = INVALID_ENTITY_ID;
  std::uint32_t mount = 0;
};

// The most one fire message carries. Seventeen bytes each, so sixty-four is comfortably inside a
// datagram, and past what any battle this envelope holds produces in one update period. Over it the
// OLDEST are dropped: the newest gunfire is the gunfire a player is looking at.
inline constexpr std::uint32_t MAX_FIRE_EVENTS = 64;

// A decoded snapshot: what the client renders instead of reaching into Universe.
struct UniverseSnapshot
{
  std::uint64_t tick = 0;
  std::vector<ShipSnapshot> ships;
};

// What one fleet order carries up the wire.
//
// It carries no ship list at all, and that is the whole point rather than an economy: one small
// fixed-size message whatever the fleet's size, so there is no cap for one to exceed and never will
// be, and a fleet of eight costs a client exactly what a fleet of one does (ADR 0049). It is the
// only order that moves ships now -- the two that carried lists retired with the control groups
// that sent them (Design/Archive/Fleets-slice-6.md 2.10).
struct FleetOrder
{
  std::uint8_t slot = 0;
  FleetOrderKind kind = FleetOrderKind::Idle;
  UniversePos point;                    // Move
  float facingRad = 0.0f;               // Move
  bool hasFacing = false;               // Move
  EntityId station = INVALID_ENTITY_ID; // Dock
  EntityId target = INVALID_ENTITY_ID;  // Attack
  EntityId gate = INVALID_ENTITY_ID;    // Jump
};

// Who is in one fleet, as the owning client is told it.
//
// A statement of membership rather than a delta: a roster for a slot replaces that slot's list
// whole, so a lost one is repaired by the next rather than compounding. It carries no position and
// no order -- those ride the status block in every update, because they change every tick and this
// changes at human speed (Design/Archive/Fleets.md 8.1).
//
// An empty members list is a fleet with nobody in space: a composed one whose manifest has not
// begun to pour, or one that has just lost its last ship. It is NOT "the slot is free" -- that is
// the status block's mask, which rides every update and cannot be lost.
struct FleetRoster
{
  std::uint8_t slot = 0;
  std::vector<EntityId> members;
};

// What one station holds, asked for rather than broadcast (ADR 0051).
//
// The request names a station and nothing else: whose rows are counted is the asker's own faction,
// which the server takes from the subscriber and never from the message, so there is no field here
// for a client to lie in.
struct LedgerRequest
{
  EntityId station = INVALID_ENTITY_ID;
};

// The answer: how many of each hull the asker has docked there, indexed by hull id.
//
// A fixed array rather than a vector of rows, because the array IS the format -- and because
// Universe::ComposeFleet already takes a span in exactly this shape, so the assembly screen's draft
// goes back down the wire without being repacked on either side.
struct LedgerReply
{
  EntityId station = INVALID_ENTITY_ID;
  std::uint32_t hullCounts[HULL_COUNT]{};
};

// A draft, sent. The slot and the counts the assembly screen assembled; every gate is Universe's
// (Design/Archive/Fleets.md 5.2), and the issuing faction is the subscriber's rather than anything stated
// here -- the same rule every other order on this lane follows.
struct ComposeOrder
{
  EntityId station = INVALID_ENTITY_ID;
  std::uint8_t slot = 0;
  std::uint32_t hullCounts[HULL_COUNT]{};
};

// One fleet as the status block states it: where it is, what it is doing, and how big it is.
//
// Not a message. It is decoded out of the snapshot header, which is where it rides so that a
// player is told about all five fleets whether or not any of them is in the interest set -- four of
// five routinely are not, which is the whole point of a fleet being able to be elsewhere
// (Design/Archive/Fleets.md 8.2).
struct FleetStatus
{
  UniversePos position; // the centroid of live members, or the launch station while none is out

  // What the fleet is doing, as a whole FleetOrderKind byte rather than a field of a shared one.
  //
  // It was three bits with a seventh value borrowed for LAUNCHING, and that borrowing was the
  // defect slice 2 of Design/GameDesignPlan.md repaired: FLEET_STATUS_LAUNCHING was 6 because 6 was
  // "the first value no FleetOrderKind uses", which stopped being true the day Jump was appended as
  // 6 -- so a fleet holding a Jump order drew as LAUNCHING on both readers. A byte also has room for
  // the verbs the order grammar is scheduled to grow, where three bits had one value left
  // (Design/Archive/FleetStatus-work-order.md).
  std::uint8_t kind = 0;

  // Everything that is true ALONGSIDE the kind rather than instead of it, which is why a launching
  // fleet can now state both. Bits 3 to 7 are reserved: written zero, read as zero, and the room
  // this block was re-laid once to leave rather than three times to find.
  std::uint8_t flags = 0;

  // Reserved for the fleet's rules of engagement, which the order grammar's design owns. Written
  // zero and read as zero until it means something; it rides here rather than arriving in its own
  // ALPN bump, which is the whole argument for laying the block once.
  std::uint8_t stance = 0;

  std::uint8_t count = 0; // members in space plus manifest
};

// Bits of FleetStatus::flags.
//
// LAUNCHING is a flag and not a kind because a fleet pouring out of a dock is also doing whatever
// it was ordered to do, and the old byte could hold only one of the two.
inline constexpr std::uint8_t FLEET_FLAG_LAUNCHING = 0x01;
inline constexpr std::uint8_t FLEET_FLAG_ENGAGED = 0x02;
inline constexpr std::uint8_t FLEET_FLAG_UNDER_ATTACK = 0x04;

// How many ships fit in one snapshot fragment. Derived from MAX_DATAGRAM_BYTES rather than chosen,
// so the day the record grows this follows it.
//
// Its counterpart MaxShipsPerOrder is gone with the ship-list orders it capped. An order names a
// fleet now and carries no ships at all, so there is no size for one to exceed -- which is the
// property ADR 0049 was for, and the cap's retirement is the visible proof of it.
[[nodiscard]] std::uint32_t ShipsPerSnapshotFragment() noexcept;

// Sends a universe as one or more datagrams.
//
// Two shapes, and the difference is what slice 6 added. Write sends every entity every time, which
// is what slice 2b built and what the benchmark measures against. WriteInterest sends one
// subscriber's view: the entities that entered or came due, the bare handles of those that left, and
// separately the handles of those that were destroyed.
//
// Returns the number of fragments sent, or 0 if the transport refused the first one. A refusal
// part-way through is not retried: the receiver drops an incomplete snapshot whole, and the next
// update brings another.
class SnapshotWriter
{
public:
  // _viewer is who this is written FOR, and it takes both halves because the header states both: the
  // hostileMask is that viewer's faction's standing, and the fleet status block is that viewer's
  // OWNER's five slots. They were one byte until slice 3 of Design/GameDesignPlan.md, which is why
  // this parameter grew rather than gained a neighbour (Design/Archive/OwnerKey-work-order.md).
  //
  // It is defaulted because Write is the whole-universe path -- a benchmark and a test harness, with
  // no subscriber to speak of -- while WriteInterest is always written for somebody, and Publisher is
  // the only caller that knows who. The default names the single player this build has, the same
  // assumption the faction default always made.
  std::uint32_t Write(const Universe& _universe, Neuron::Transport& _transport,
                      const Issuer& _viewer = Issuer{OWNER_LOCAL, FACTION_PLAYER});

  // One subscriber's update. _sent are handles to carry in full -- entered and refreshed together,
  // because the wire cannot tell them apart and the receiver upserts either way. _left are dropped.
  //
  // _destroyed are dropped too, and additionally stated to have died: a leave means "no longer in
  // your view" and nothing more, which is what it always meant, so a client can stop inferring a
  // death from an absence (Design/Archive/Hostiles.md 4.4). A handle belongs in one list, never both; the
  // caller decides which and the writer does not check.
  //
  // _sent are handles because the writer resolves them against the universe to build records; the three
  // departure lists are ids because they name ships that are already gone, which is why the despawn
  // log carries one (ADR 0047).
  std::uint32_t WriteInterest(const Universe& _universe, std::span<const ShipHandle> _sent, std::span<const EntityId> _left,
                              std::span<const EntityId> _destroyed, std::span<const EntityId> _docked, std::span<const EntityId> _jumped,
                              Neuron::Transport& _transport, const Issuer& _viewer = Issuer{OWNER_LOCAL, FACTION_PLAYER});

  // The leave and destroyed lists, as one message on the reliable lane. Public because a caller
  // that is not sending an interest update -- a subscriber leaving, a universe shutting down -- still
  // has departures to state. Returns false when the lane refused it, which is a full lane or one
  // that is not up yet, and never a partial send.
  bool WriteLeaves(std::uint64_t _tick, std::span<const EntityId> _left, std::span<const EntityId> _destroyed,
                   std::span<const EntityId> _docked, std::span<const EntityId> _jumped, Neuron::Transport& _transport);

  // The gunfire since this subscriber last heard, as one datagram. False when there was none to
  // send or the lane refused it; nothing is retried, because a lost muzzle flash is not a lie and a
  // late one draws a line into empty space (Design/Archive/Combat-slice-2.md 2.3).
  //
  // More than MAX_FIRE_EVENTS keeps the newest. It is a separate message rather than a block in the
  // fragment header for one reason worth stating: the fleet status block rides every fragment so it
  // heals, and a list of events stamped on every fragment would draw every tracer once per fragment.
  bool WriteFire(std::uint64_t _tick, std::span<const ShotRecord> _shots, Neuron::Transport& _transport);

  // How many leave messages the lane has refused. Nothing repeats a refused one, so this is the
  // count of departures a subscriber was never told about -- a number that should be zero, and a
  // diagnostic when it is not.
  [[nodiscard]] std::uint32_t RefusedLeaveCount() const noexcept
  {
    return m_refusedLeaves;
  }

  // Bytes the last Write or WriteInterest put on the wire, and how many ship records it carried.
  // The benchmark in slice 6's acceptance is this pair against a growing universe.
  [[nodiscard]] std::uint32_t LastByteCount() const noexcept
  {
    return m_lastBytes;
  }

  [[nodiscard]] std::uint32_t LastRecordCount() const noexcept
  {
    return m_lastRecords;
  }

private:
  std::uint32_t m_nextSnapshotId = 1;
  std::uint32_t m_lastBytes = 0;
  std::uint32_t m_lastRecords = 0;
  std::uint32_t m_refusedLeaves = 0;
  std::vector<std::uint8_t> m_scratch;
  std::vector<std::uint8_t> m_leaveScratch;
  std::vector<std::uint8_t> m_fireScratch;
  std::vector<ShipId> m_resolvedScratch;
};

// Reassembles fragments into whole snapshots.
//
// A snapshot missing any fragment when the next one begins is dropped entire and never rendered.
// Half a snapshot is worse than a stale one: stale reads as lag, partial reads as ships vanishing.
class SnapshotReceiver
{
public:
  // Feeds one message from either lane, dispatching on its kind byte. True when something changed
  // that the view should redraw from.
  //
  // A datagram carries snapshot fragments; the reliable lane carries departures. The caller does not
  // have to know which it is holding, which is what keeps the two drains in UniverseView identical.
  //
  // An update UPSERTS rather than replaces: a record for a handle already held updates it in place,
  // one for a handle not held appends it, and a handle in the leave list removes it. That is what
  // lets a datagram carry a change to a universe rather than a whole universe -- and it is also why an
  // incomplete update is dropped entire, since unlike a full snapshot a delta stream cannot
  // resynchronise by waiting for the next one (Design/Archive/Collision-slice-6.md 3.5).
  bool Accept(std::span<const std::uint8_t> _datagram);

  [[nodiscard]] const UniverseSnapshot& Latest() const noexcept
  {
    return m_latest;
  }

  [[nodiscard]] bool HasSnapshot() const noexcept
  {
    return m_hasSnapshot;
  }

  // Bit f set: faction f holds this client's faction hostile, as of the last header that arrived.
  //
  // Taken from every fragment that passes the header and staleness checks, rather than on apply
  // like the upserts -- deliberately. An incomplete update is dropped because a half-applied set of
  // records is a half-updated universe; a mask has no such coupling, so taking it from whatever
  // arrives is strictly more robust, and robustness against loss is the entire argument for
  // spending the byte (Design/Archive/Stations.md 4.3).
  [[nodiscard]] std::uint8_t HostileMask() const noexcept
  {
    return m_hostileMask;
  }

  // Whether _faction holds this client hostile. What the livery table and the dock affordance ask.
  [[nodiscard]] bool IsHostileToMe(FactionId _faction) const noexcept
  {
    return _faction < FACTION_LIMIT && (m_hostileMask & static_cast<std::uint8_t>(1u << _faction)) != 0;
  }

  // Bit s set: this client's faction holds a fleet in slot s, as of the last header that arrived.
  //
  // This, and not an empty roster, is what says a slot is held. A roster is stated once on the
  // reliable lane and can be refused; the mask rides every update, so it heals itself -- the same
  // trade hostileMask made, for the same reason (Design/Archive/Fleets.md 8.1, 8.2).
  [[nodiscard]] std::uint8_t FleetMask() const noexcept
  {
    return m_fleetMask;
  }

  // What the last header said about one slot. A slot the mask does not hold reads back a default,
  // which is a position at the origin and no bits -- so a caller that draws without checking the
  // mask draws something obviously wrong rather than something plausibly wrong.
  [[nodiscard]] const FleetStatus& FleetStatusOf(std::uint8_t _slot) const noexcept
  {
    return m_fleetStatus[(_slot < FLEET_SLOTS) ? _slot : 0];
  }

  // Who is in one slot, as of the last roster for it. Empty until one arrives, and empty for a
  // composed fleet whose manifest has not begun to pour.
  [[nodiscard]] std::span<const EntityId> RosterOf(std::uint8_t _slot) const noexcept;

  // The last ledger reply that arrived, and how many have. The count is what lets a screen tell a
  // fresh answer from the one still on display: replies are not ordered against anything and a
  // second request for the same station is answered identically, so the payload alone cannot say
  // whether the wire has spoken since the screen opened.
  [[nodiscard]] const LedgerReply& Ledger() const noexcept
  {
    return m_ledger;
  }

  [[nodiscard]] std::uint32_t LedgerReplyCount() const noexcept
  {
    return m_ledgerReplies;
  }

  // The handles the last applied update said were destroyed, as distinct from those that merely left
  // this subscriber's view. Valid until the next update applies; empty for a full snapshot.
  [[nodiscard]] std::span<const EntityId> Destroyed() const noexcept
  {
    return m_destroyed;
  }

  // Deaths accumulate across every message in a drain, so the consumer says when it has drawn them
  // rather than the receiver guessing. Without this, two leave messages in one pump would leave the
  // first one's dead unexploded (Design/Archive/ReliableFormat-work-order.md).
  void ClearDestroyed() noexcept
  {
    m_destroyed.clear();
  }

  // The handles the last applied departure message said had docked: gone from the universe, but not
  // dead. The client removes the hull silently -- no explosion, no shake, no SHIP LOST -- which is
  // the entire reason a departure carries a cause (ADR 0040).
  [[nodiscard]] std::span<const EntityId> Docked() const noexcept
  {
    return m_docked;
  }

  void ClearDocked() noexcept
  {
    m_docked.clear();
  }

  // The handles the last applied departure message said had jumped out: gone from this system,
  // alive somewhere else, under the same identity. The fourth run rather than a reuse of the docked
  // one, and the reason is what a client does with it: a docking is a silent removal, a jump is a
  // thing a player watched happen and should see. Without its own run a jump would arrive as a
  // DESTROY -- SplitTheLost routes everything that is not a docking there -- and a fleet crossing a
  // gate would explode on every screen that watched it leave (ADR 0056, ADR 0040).
  [[nodiscard]] std::span<const EntityId> Jumped() const noexcept
  {
    return m_jumped;
  }

  void ClearJumped() noexcept
  {
    m_jumped.clear();
  }

  // The gunfire the last messages carried. Accumulated across a drain and cleared by the consumer,
  // which is Destroyed()'s idiom and its reason: two fire messages in one pump must not leave the
  // first one's tracers undrawn.
  [[nodiscard]] std::span<const FireEvent> Fire() const noexcept
  {
    return m_fire;
  }

  void ClearFire() noexcept
  {
    m_fire.clear();
  }

  // The tick the last departure message was written on. Diagnostics: how stale a departure was.
  [[nodiscard]] std::uint64_t LastLeaveTick() const noexcept
  {
    return m_lastLeaveTick;
  }

  // Diagnostics: snapshots abandoned because a fragment never arrived.
  [[nodiscard]] std::uint32_t DroppedSnapshotCount() const noexcept
  {
    return m_dropped;
  }

private:
  void AbandonInProgress() noexcept;
  void Apply();
  void Remove(EntityId _gone);
  [[nodiscard]] bool AcceptLeaves(std::span<const std::uint8_t> _message);
  [[nodiscard]] bool AcceptFire(std::span<const std::uint8_t> _message);
  [[nodiscard]] bool AcceptRoster(std::span<const std::uint8_t> _message);
  [[nodiscard]] bool AcceptLedgerReply(std::span<const std::uint8_t> _message);

  // What arrived in the update being assembled, held until every fragment is in. Applying as
  // fragments land would leave the universe half-updated if one never arrived.
  std::vector<ShipSnapshot> m_pendingUpserts;
  std::vector<EntityId> m_leaveScratch;     // one departure message, read before any of it applies
  std::vector<EntityId> m_destroyedScratch; // the same, for the deaths in it
  std::vector<EntityId> m_dockedScratch;    // and for the dockings
  std::vector<EntityId> m_jumpedScratch;    // and for the jumps
  std::vector<EntityId> m_destroyed;        // deaths since the consumer last cleared them
  std::vector<EntityId> m_docked;           // dockings since the consumer last cleared them
  std::vector<EntityId> m_jumped;           // jumps since the consumer last cleared them
  std::vector<FireEvent> m_fire;            // gunfire since the consumer last cleared it
  std::vector<FireEvent> m_fireScratch;     // one message, read whole before any of it is kept
  std::uint64_t m_lastFireTick = 0;
  std::uint64_t m_lastLeaveTick = 0;
  std::uint8_t m_hostileMask = 0;
  std::uint8_t m_fleetMask = 0;
  FleetStatus m_fleetStatus[FLEET_SLOTS];
  std::vector<EntityId> m_rosters[FLEET_SLOTS];
  LedgerReply m_ledger;
  std::uint32_t m_ledgerReplies = 0;
  UniverseSnapshot m_latest;
  std::uint32_t m_buildingId = 0;
  std::uint64_t m_buildingTick = 0;
  bool m_buildingComplete = false;
  std::uint32_t m_fragmentsSeen = 0;
  std::uint32_t m_fragmentCount = 0;
  std::uint32_t m_dropped = 0;
  bool m_hasSnapshot = false;
};

// The authoritative state, as against the view of it.
//
// A snapshot exists to WITHHOLD -- steerTargetPos, the order's facing and speed cap, the avoidance
// heading, and every intent table beside m_ships -- so the snapshot path structurally cannot carry a
// save or a handoff, and until now nothing else could either (Design/Archive/MmoScalabilityReview.md U3).
// These two carry all of it, at full fidelity: this is a save, so the wire's 0.125 m lattice and its
// turns16 have no business here and every position is a whole UniversePos.
//
// What is written and what is rebuilt is argued in Design/Archive/WorldState-work-order.md 2. The
// short version: everything Step READS is written, and everything Step DERIVES -- the spatial index,
// the path islands, the neighbourhood extent, every scratch vector -- is rebuilt from what was.
//
// Read refuses and changes nothing on a buffer that is truncated, that carries the wrong magic, or
// that carries a format byte this build does not know. It never throws and never asserts, which is
// AGENTS.md 5's rule for anything parsing content and the discipline SnapshotReceiver already keeps.
void WriteUniverseState(const Universe& _universe, std::vector<std::uint8_t>& _outBytes);
[[nodiscard]] bool ReadUniverseState(std::span<const std::uint8_t> _bytes, Universe& _outUniverse);

// What a save file carries in front of the state.
//
// The seed is here because the client-visible galaxy derives from it: a binary whose compiled
// GALAXY_SEED has moved on still boots the universe the file holds, laid out from this seed, and a
// shard and its clients never disagree about where anything is (Design/Archive/Universe.md 8).
//
// The shard is here as well as in the state, and that duplication is deliberate: a header exists to
// be readable WITHOUT decoding the body, so a launcher can ask which shard a file belongs to for
// the price of fifteen bytes. The reader cross-checks the two and refuses a file where they
// disagree, which is what stops a second source of truth becoming a second answer
// (Design/Archive/Universe-slice-5.md 7).
struct SaveHeader
{
  std::uint64_t galaxySeed = 0;
  ShardId shard = 0;

  // What the file was READ at, filled by ReadSaveFile and ignored by WriteSaveFile, which always
  // writes the current bytes. A boot reads these to say which format it restored and to notice that
  // it migrated one; a test reads them to prove a fixture was taken at the format its name claims
  // (Design/Archive/SaveMigration-work-order.md 1.2).
  std::uint8_t fileFormat = 0;
  std::uint8_t stateFormat = 0;

  // How many shards the galaxy this universe belongs to was written for, and therefore how many
  // ShardOfSystem was asked about. It is here and not only in Server.cfg because a universe
  // generated for four shards is not a universe four OTHER shards can run: the partition is a
  // function of the layout and the count, so a mismatch silently moves every system
  // (Design/CrossShard.md 2). A file format 1 predates it and reads back as 1, which is what those
  // files meant.
  std::uint32_t shardCount = 1;
};

// The state's format: what WriteUniverseState writes, and the newest byte ReadUniverseState accepts.
// Bumped whenever a field is added to what Step reads. The list is the history of the bumps, and
// the rule since slice 1 of the game design plan is that the commit bumping this to N+1 commits the
// fixture Tests/GameLogicTests/Assets/UniverseFormatN.sav, written by UniverseGen at its parent
// commit, so that the reader below is proven against a file and not against itself.
//
// 2: the fleet table. 3: its manifest. 4: its order. 5: its threat. 6: the guns. 7: the plan stamp.
// 8: the owner, on a fleet and on a ledger row. 9: the cross-shard handoff queues.
inline constexpr std::uint8_t UNIVERSE_STATE_FORMAT = 9; // 9: the handoff queues

// The oldest format ReadUniverseState still accepts. A file in a format between this and the
// current one is read with every later field gated on the byte the file carries and defaulted where
// the file predates it, and is a current universe the moment the read returns; the next save writes
// it at UNIVERSE_STATE_FORMAT. This is the whole of migration: no reader per format, no rewrite
// step, no transform table (ADR 0061). Moving this byte retires a format and deletes its fixture,
// and only a decision record moves it.
//
// 7 rather than 1 because formats 1 to 6 never had a file on disk to keep: the save file arrived at
// format 7 (Design/Archive/Universe-slice-5.md), and OLDEST starts where a file first existed.
inline constexpr std::uint8_t UNIVERSE_STATE_FORMAT_OLDEST = 7;
static_assert(UNIVERSE_STATE_FORMAT_OLDEST <= UNIVERSE_STATE_FORMAT, "the oldest accepted state format is newer than the one written");

// 'VASU' little-endian: Universe SAVe. Distinct from UNIVERSE_STATE_MAGIC because these are two
// formats and not one: this byte says how to find the state, the state's own says how to read it,
// and bumping either need not bump the other. The same window applies: ReadSaveFile accepts a file
// format from SAVE_FILE_FORMAT_OLDEST to SAVE_FILE_FORMAT, for the state format's reason.
inline constexpr std::uint32_t SAVE_FILE_MAGIC = 0x55534156u;

// 2: the shard count.
inline constexpr std::uint8_t SAVE_FILE_FORMAT = 2;
inline constexpr std::uint8_t SAVE_FILE_FORMAT_OLDEST = 1;
static_assert(SAVE_FILE_FORMAT_OLDEST <= SAVE_FILE_FORMAT, "the oldest accepted file format is newer than the one written");

// magic 4 + format 1 + galaxy seed 8 + shard 2 + state length 8, and from format 2 a shard count of
// 4 on the end.
//
// A function of the format rather than one constant, because the header is what says where the state
// begins and an older file's header is shorter. It was a constant while there was one format, and
// the day there were two it had to stop being one -- which is the same shape ADR 0061 gave the state
// codec, applied to the thing in front of it.
inline constexpr std::size_t SAVE_HEADER_BYTES = 27;

[[nodiscard]] constexpr std::size_t SaveHeaderBytes(std::uint8_t _fileFormat) noexcept
{
  return (_fileFormat >= 2) ? 27u : 23u;
}

static_assert(SaveHeaderBytes(SAVE_FILE_FORMAT) == SAVE_HEADER_BYTES, "the current header's length has drifted from its fields");

// What the file is called.
//
// Beside the magic rather than in whichever program opens one, because it is the same kind of fact:
// two programs that disagree about a save file's NAME fail exactly as completely as two that
// disagree about its format, and now there are two -- the tool that writes a universe and the game
// that runs it (ADR 0058). GameLogic still opens nothing; it only says what the thing is called.
//
// A bare name, so FileSys::ResolvePath puts it under <exe>\Assets\. That is wrong for a real
// install -- a read-only program directory cannot be saved into -- and it is deliberately one
// constant, so the day there is a writable data directory this line moves and nothing else does
// (Design/Archive/Universe-slice-5.md 6).
inline constexpr const wchar_t* UNIVERSE_SAVE_FILE = L"Universe.sav";

// The state codec given a file.
//
// The length is written even though the state is self-delimiting, and it is what makes "torn"
// checkable in BOTH directions. ReadUniverseState stops at the end of the state and never looks
// past it, so a file with rubbish appended would otherwise load and be believed; and a file cut
// short is caught here, before the body parser allocates anything at all.
//
// Read refuses -- and changes NEITHER out-parameter -- on a buffer too short for the header, the
// wrong magic, a format byte outside the window this build reads, a length that disagrees with the
// buffer, a state the state codec refuses, or a header shard that disagrees with the state's. It
// never throws and never asserts, which is AGENTS.md 5's rule for anything parsing content: what a
// refusal MEANS is the caller's business, and for the composition root it means the boot stops
// (ADR 0057).
void WriteSaveFile(const Universe& _universe, const SaveHeader& _header, std::vector<std::uint8_t>& _outBytes);
[[nodiscard]] bool ReadSaveFile(std::span<const std::uint8_t> _bytes, SaveHeader& _outHeader, Universe& _outUniverse);

// The two format bytes of a file, without parsing it. For the sentence a refused boot prints: a
// file ReadSaveFile refuses has changed nothing, so nothing else can say which format it was in.
// Answers false when either magic is wrong or the buffer is too short to hold both bytes, and then
// writes neither out-parameter.
[[nodiscard]] bool PeekSaveFormats(std::span<const std::uint8_t> _bytes, std::uint8_t& _outFileFormat, std::uint8_t& _outStateFormat);

// Where a boot that migrated a file keeps the file it migrated from: the save's own name plus the
// state format it was in, "Universe.sav.7". One function beside UNIVERSE_SAVE_FILE for the same
// reason that constant is here -- two programs that disagreed about the name would keep two
// different files. GameLogic still opens nothing; the composition root does the write
// (Design/Archive/SaveMigration-work-order.md 1.4).
[[nodiscard]] std::wstring UniverseSaveSidecarName(std::uint8_t _stateFormat);

// Orders travel the other way. Written by the client half, read and applied by the server half.
//
// There is exactly one kind of them that moves ships, and it names a fleet (ADR 0049). The
// ship-list move and dock orders that stood here retired with the control groups that sent them:
// nothing wrote one once selection was fleet-grain, and a wire message nothing writes is a second
// way to command waiting to be found (Design/Archive/Fleets-slice-6.md 2.10).
//
// The reader refuses a slot past FLEET_SLOTS and a kind past the last one it knows, because a
// malformed message is content and content fails closed rather than being passed on to a gate that
// would have to guess (AGENTS.md 5).
[[nodiscard]] bool WriteFleetOrder(const FleetOrder& _order, Neuron::Transport& _transport);
[[nodiscard]] bool ReadFleetOrder(std::span<const std::uint8_t> _datagram, FleetOrder& _outOrder);

// The roster, downward. Every reader below fails closed on anything it cannot mean -- a slot past
// FLEET_SLOTS, a member count past MAX_FLEET_SHIPS, a hull table that is not this build's, a buffer
// that ends early -- because a malformed message is content, and content fails closed rather than
// being handed to a gate that would have to guess (AGENTS.md 5).
[[nodiscard]] bool WriteFleetRoster(const FleetRoster& _roster, Neuron::Transport& _transport);
[[nodiscard]] bool ReadFleetRoster(std::span<const std::uint8_t> _message, FleetRoster& _outRoster);

// The ledger exchange: the only request/reply pair on this seam (ADR 0051). Everything else here
// announces a fact; this one asks a question, because a station's contents are large, private,
// slow-changing and wanted by exactly one client at exactly one moment.
[[nodiscard]] bool WriteLedgerRequest(const LedgerRequest& _request, Neuron::Transport& _transport);
[[nodiscard]] bool ReadLedgerRequest(std::span<const std::uint8_t> _message, LedgerRequest& _outRequest);
[[nodiscard]] bool WriteLedgerReply(const LedgerReply& _reply, Neuron::Transport& _transport);
[[nodiscard]] bool ReadLedgerReply(std::span<const std::uint8_t> _message, LedgerReply& _outReply);

// --- the shard-to-shard seam ----------------------------------------------------------------------

// A batch of handoffs, and the acknowledgement that clears them.
//
// Both ride the RELIABLE lane. A lost handoff is a fleet that has left one universe and reached no
// other, which is the one thing this design must not do -- so it is the opposite of the fire message,
// whose argument for the datagram lane was that a lost flash is not a lie (ADR 0053, ADR 0029).
//
// They are shard-to-shard and no client sends or understands either, which is why the ALPN does not
// move: a kind a reader does not know is already refused rather than misread.
//
// A message carries as many entries as MAX_RELIABLE_BYTES holds; the rest wait for the next send,
// which the re-send loop was going to make anyway. WriteHandoffs reports how many it took, so a
// caller can tell "the lane refused" from "the batch did not fit".
[[nodiscard]] bool WriteHandoffs(std::span<const Universe::Handoff> _handoffs, Neuron::Transport& _transport, std::uint32_t& _outSent);
[[nodiscard]] bool ReadHandoffs(std::span<const std::uint8_t> _message, std::vector<Universe::Handoff>& _outHandoffs);

[[nodiscard]] bool WriteHandoffAck(std::span<const std::uint64_t> _sequences, Neuron::Transport& _transport);
[[nodiscard]] bool ReadHandoffAck(std::span<const std::uint8_t> _message, std::vector<std::uint64_t>& _outSequences);

// The draft, upward. The last of the four order kinds, and the only one that names no ship at all.
[[nodiscard]] bool WriteComposeOrder(const ComposeOrder& _order, Neuron::Transport& _transport);
[[nodiscard]] bool ReadComposeOrder(std::span<const std::uint8_t> _message, ComposeOrder& _outOrder);
} // namespace Game
