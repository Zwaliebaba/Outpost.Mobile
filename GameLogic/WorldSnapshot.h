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
// Fields are written one at a time into a byte buffer rather than memcpy'd. ShipState is 120 bytes
// of padded struct whose layout has already changed once in this design and will again; a
// field-by-field format survives that, and survives the day the two ends are different binaries.

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
  ShipHandle handle; // not ShipId -- that is an array index, and despawn moves it (ADR 0005)
  WorldPos posWorld;
  WorldPos prevPos;
  float headingRad = 0.0f;
  float prevHeading = 0.0f;
  float speed = 0.0f;
  float accelSample = 0.0f;
  float turnRateRadPerSec = 0.0f;
  OrderState order = OrderState::Idle;
  FactionId factionId = FACTION_PLAYER;
  std::uint32_t hullId = 0;
};

// A decoded snapshot: what the client renders instead of reaching into World.
struct WorldSnapshot
{
  std::uint64_t tick = 0;
  std::vector<ShipSnapshot> ships;
};

// What one move order carries up the wire.
struct MoveOrder
{
  std::vector<ShipHandle> ships;
  WorldPos destination;
  float facingRad = 0.0f;
  bool hasFacing = false;
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
  std::uint32_t Write(const World& _world, Neuron::Transport& _transport);

  // One subscriber's update. _sent are handles to carry in full -- entered and refreshed together,
  // because the wire cannot tell them apart and the receiver upserts either way. _left are dropped.
  //
  // _destroyed are dropped too, and additionally stated to have died: a leave means "no longer in
  // your view" and nothing more, which is what it always meant, so a client can stop inferring a
  // death from an absence (Design/Archive/Hostiles.md 4.4). A handle belongs in one list, never both; the
  // caller decides which and the writer does not check.
  std::uint32_t WriteInterest(const World& _world, std::span<const ShipHandle> _sent, std::span<const ShipHandle> _left,
                              std::span<const ShipHandle> _destroyed, Neuron::Transport& _transport);

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
  std::vector<std::uint8_t> m_scratch;
  std::vector<ShipId> m_resolvedScratch;
};

// Reassembles fragments into whole snapshots.
//
// A snapshot missing any fragment when the next one begins is dropped entire and never rendered.
// Half a snapshot is worse than a stale one: stale reads as lag, partial reads as ships vanishing.
class SnapshotReceiver
{
public:
  // Feeds one datagram. True when a complete, newer-than-current update became available.
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

  // The handles the last applied update said were destroyed, as distinct from those that merely left
  // this subscriber's view. Valid until the next update applies; empty for a full snapshot.
  [[nodiscard]] std::span<const ShipHandle> Destroyed() const noexcept
  {
    return m_destroyed;
  }

  // Diagnostics: snapshots abandoned because a fragment never arrived.
  [[nodiscard]] std::uint32_t DroppedSnapshotCount() const noexcept
  {
    return m_dropped;
  }

private:
  void AbandonInProgress() noexcept;
  void Apply();

  // What arrived in the update being assembled, held until every fragment is in. Applying as
  // fragments land would leave the world half-updated if one never arrived.
  std::vector<ShipSnapshot> m_pendingUpserts;
  std::vector<ShipHandle> m_pendingLeaves;
  std::vector<ShipHandle> m_pendingDestroyed;
  std::vector<ShipHandle> m_destroyed; // what the last applied update stated, for Destroyed()
  WorldSnapshot m_latest;
  std::uint32_t m_buildingId = 0;
  std::uint64_t m_buildingTick = 0;
  bool m_buildingComplete = false;
  std::uint32_t m_fragmentsSeen = 0;
  std::uint32_t m_fragmentCount = 0;
  std::uint32_t m_dropped = 0;
  bool m_hasSnapshot = false;
};

// Orders travel the other way. Written by the client half, read and applied by the server half.
[[nodiscard]] bool WriteMoveOrder(const MoveOrder& _order, Neuron::Transport& _transport);
[[nodiscard]] bool ReadMoveOrder(std::span<const std::uint8_t> _datagram, MoveOrder& _outOrder);
} // namespace Game
