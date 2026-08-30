#pragma once

#include "ShipState.h"
#include "WorldPos.h"

#include "Transport.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
class World;

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
// angle is a sixteenth-bit of a turn, and prevPos is an integer step delta from posWorld rather than
// a second whole position. That is 23 records in a datagram against 13 (ADR 0046). The decoded types
// below are unchanged floats and WorldPos, so nothing above this layer knows the wire quantizes --
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
  // Who this is, not where it is. A ShipHandle would have been enough while there was one World; an
  // id is what survives being handed between two, and it is what a client may key its own state on
  // without that state evaporating at a shard boundary (ADR 0047). The server still uses handles
  // everywhere a reference outlives a tick -- ADR 0005 is untouched -- and the publisher is where
  // the two currencies meet.
  EntityId entity = INVALID_ENTITY_ID;

  // The four quantized fields, decoded back to the types the view already reads. A position is
  // within 6.25 cm of the simulation's and an angle within pi/2^16 of it, and a decoded heading is
  // in (-pi, pi] like a simulated one -- so a source of exactly -pi arrives as +pi, the same bearing
  // and a different number (ADR 0046).
  WorldPos posWorld;
  WorldPos prevPos;
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
  // station before an order is worth sending (Design/Stations.md 6.2). What is deliberately *not*
  // here: the ledger, the garrison numbers and the target list -- private state of the kind the
  // snapshot exists to withhold. Seven bits are unspoken for; user stations and conquerable ones
  // are what they are being kept for.
  std::uint8_t flags = 0;

  std::uint32_t hullId = 0;
};

// Bit 0 of ShipSnapshot::flags.
inline constexpr std::uint8_t SHIP_FLAG_STATION = 0x01;

// A decoded snapshot: what the client renders instead of reaching into World.
struct WorldSnapshot
{
  std::uint64_t tick = 0;
  std::vector<ShipSnapshot> ships;
};

// What one move order carries up the wire. Ids, not handles: the client has never been given a
// handle and could not interpret one (ADR 0047).
struct MoveOrder
{
  std::vector<EntityId> ships;
  WorldPos destination;
  float facingRad = 0.0f;
  bool hasFacing = false;
};

// What one dock order carries up the wire. A second instance of the move order's shape: a datagram
// kind, a write/read pair, handles resolved in the adapter and a faction gate in World (ADR 0014).
struct DockOrder
{
  std::vector<EntityId> ships;
  EntityId station = INVALID_ENTITY_ID; // the station's structure
};

// How many ships fit in one datagram of each kind. Derived from MAX_DATAGRAM_BYTES rather than
// chosen, so the day the record grows these follow it.
[[nodiscard]] std::uint32_t ShipsPerSnapshotFragment() noexcept;
[[nodiscard]] std::uint32_t MaxShipsPerOrder() noexcept;

// Sends a world as one or more datagrams.
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
  // _viewer is whose standing the header's hostileMask states. It is defaulted because Write is the
  // whole-world path -- a benchmark and a test harness, with no subscriber to speak of -- while
  // WriteInterest is always written for somebody, and Publisher is the only caller that knows who.
  std::uint32_t Write(const World& _world, Neuron::Transport& _transport, FactionId _viewer = FACTION_PLAYER);

  // One subscriber's update. _sent are handles to carry in full -- entered and refreshed together,
  // because the wire cannot tell them apart and the receiver upserts either way. _left are dropped.
  //
  // _destroyed are dropped too, and additionally stated to have died: a leave means "no longer in
  // your view" and nothing more, which is what it always meant, so a client can stop inferring a
  // death from an absence (Design/Archive/Hostiles.md 4.4). A handle belongs in one list, never both; the
  // caller decides which and the writer does not check.
  //
  // _sent are handles because the writer resolves them against the world to build records; the three
  // departure lists are ids because they name ships that are already gone, which is why the despawn
  // log carries one (ADR 0047).
  std::uint32_t WriteInterest(const World& _world, std::span<const ShipHandle> _sent, std::span<const EntityId> _left,
                              std::span<const EntityId> _destroyed, std::span<const EntityId> _docked, Neuron::Transport& _transport,
                              FactionId _viewer = FACTION_PLAYER);

  // The leave and destroyed lists, as one message on the reliable lane. Public because a caller
  // that is not sending an interest update -- a subscriber leaving, a world shutting down -- still
  // has departures to state. Returns false when the lane refused it, which is a full lane or one
  // that is not up yet, and never a partial send.
  bool WriteLeaves(std::uint64_t _tick, std::span<const EntityId> _left, std::span<const EntityId> _destroyed,
                   std::span<const EntityId> _docked, Neuron::Transport& _transport);

  // How many leave messages the lane has refused. Nothing repeats a refused one, so this is the
  // count of departures a subscriber was never told about -- a number that should be zero, and a
  // diagnostic when it is not.
  [[nodiscard]] std::uint32_t RefusedLeaveCount() const noexcept
  {
    return m_refusedLeaves;
  }

  // Bytes the last Write or WriteInterest put on the wire, and how many ship records it carried.
  // The benchmark in slice 6's acceptance is this pair against a growing world.
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
  // have to know which it is holding, which is what keeps the two drains in WorldView identical.
  //
  // An update UPSERTS rather than replaces: a record for a handle already held updates it in place,
  // one for a handle not held appends it, and a handle in the leave list removes it. That is what
  // lets a datagram carry a change to a world rather than a whole world -- and it is also why an
  // incomplete update is dropped entire, since unlike a full snapshot a delta stream cannot
  // resynchronise by waiting for the next one (Design/Archive/Collision-slice-6.md 3.5).
  bool Accept(std::span<const std::uint8_t> _datagram);

  [[nodiscard]] const WorldSnapshot& Latest() const noexcept
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
  // records is a half-updated world; a mask has no such coupling, so taking it from whatever
  // arrives is strictly more robust, and robustness against loss is the entire argument for
  // spending the byte (Design/Stations.md 4.3).
  [[nodiscard]] std::uint8_t HostileMask() const noexcept
  {
    return m_hostileMask;
  }

  // Whether _faction holds this client hostile. What the livery table and the dock affordance ask.
  [[nodiscard]] bool IsHostileToMe(FactionId _faction) const noexcept
  {
    return _faction < FACTION_LIMIT && (m_hostileMask & static_cast<std::uint8_t>(1u << _faction)) != 0;
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

  // The handles the last applied departure message said had docked: gone from the world, but not
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

  // What arrived in the update being assembled, held until every fragment is in. Applying as
  // fragments land would leave the world half-updated if one never arrived.
  std::vector<ShipSnapshot> m_pendingUpserts;
  std::vector<EntityId> m_leaveScratch;     // one departure message, read before any of it applies
  std::vector<EntityId> m_destroyedScratch; // the same, for the deaths in it
  std::vector<EntityId> m_dockedScratch;    // and for the dockings
  std::vector<EntityId> m_destroyed;        // deaths since the consumer last cleared them
  std::vector<EntityId> m_docked;           // dockings since the consumer last cleared them
  std::uint64_t m_lastLeaveTick = 0;
  std::uint8_t m_hostileMask = 0;
  WorldSnapshot m_latest;
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
// save or a handoff, and until now nothing else could either (Design/MmoScalabilityReview.md U3).
// These two carry all of it, at full fidelity: this is a save, so the wire's 0.125 m lattice and its
// turns16 have no business here and every position is a whole WorldPos.
//
// What is written and what is rebuilt is argued in Design/Archive/WorldState-work-order.md 2. The
// short version: everything Step READS is written, and everything Step DERIVES -- the spatial index,
// the path islands, the neighbourhood extent, every scratch vector -- is rebuilt from what was.
//
// Read refuses and changes nothing on a buffer that is truncated, that carries the wrong magic, or
// that carries a format byte this build does not know. It never throws and never asserts, which is
// AGENTS.md 5's rule for anything parsing content and the discipline SnapshotReceiver already keeps.
void WriteWorldState(const World& _world, std::vector<std::uint8_t>& _outBytes);
[[nodiscard]] bool ReadWorldState(std::span<const std::uint8_t> _bytes, World& _outWorld);

// Orders travel the other way. Written by the client half, read and applied by the server half.
[[nodiscard]] bool WriteMoveOrder(const MoveOrder& _order, Neuron::Transport& _transport);
[[nodiscard]] bool ReadMoveOrder(std::span<const std::uint8_t> _datagram, MoveOrder& _outOrder);

// A dock order's own header is smaller than a move order's -- a station handle in place of a
// destination and a facing -- so it would admit two more ships. It deliberately does not: the cap
// stays MaxShipsPerOrder(), because the client's selection logic already agrees on one number and
// two caps differing by two is a fact nobody will remember and no test would pin.
[[nodiscard]] bool WriteDockOrder(const DockOrder& _order, Neuron::Transport& _transport);
[[nodiscard]] bool ReadDockOrder(std::span<const std::uint8_t> _datagram, DockOrder& _outOrder);
} // namespace Game
