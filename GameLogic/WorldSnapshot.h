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
// code lives with what it is about (Design/Collision-slice-2b.md 2.2).
//
// Fields are written one at a time into a byte buffer rather than memcpy'd. ShipState is 120 bytes
// of padded struct whose layout has already changed once in this design and will again; a
// field-by-field format survives that, and survives the day the two ends are different binaries.

// One ship as a client is allowed to see it.
//
// Not ShipState. The seam exists to make "what the client may know" a reviewable list rather than
// whatever happens to be in a struct: steerTargetPos, orderFacingRad, orderHasFacing and
// avoidHeadingRad are deliberately absent, because together they tell any client exactly what every
// ship intends to do next. That is a decision to take once, here, rather than to discover later.
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

// Sends a world as one or more datagrams. Every entity, every time: no interest management and no
// delta encoding, which is deliberate -- slice 6 measures itself against this.
//
// Returns the number of fragments sent, or 0 if the transport refused the first one. A refusal
// part-way through is not retried: the receiver drops an incomplete snapshot whole, and the next
// tick brings another.
class SnapshotWriter
{
public:
  std::uint32_t Write(const World& _world, Neuron::Transport& _transport);

private:
  std::uint32_t m_nextSnapshotId = 1;
  std::vector<std::uint8_t> m_scratch;
};

// Reassembles fragments into whole snapshots.
//
// A snapshot missing any fragment when the next one begins is dropped entire and never rendered.
// Half a snapshot is worse than a stale one: stale reads as lag, partial reads as ships vanishing.
class SnapshotReceiver
{
public:
  // Feeds one datagram. True when a complete, newer-than-current snapshot became available.
  bool Accept(std::span<const std::uint8_t> _datagram);

  [[nodiscard]] const WorldSnapshot& Latest() const noexcept
  {
    return m_latest;
  }

  [[nodiscard]] bool HasSnapshot() const noexcept
  {
    return m_hasSnapshot;
  }

  // Diagnostics: snapshots abandoned because a fragment never arrived.
  [[nodiscard]] std::uint32_t DroppedSnapshotCount() const noexcept
  {
    return m_dropped;
  }

private:
  void AbandonInProgress() noexcept;

  WorldSnapshot m_building;
  WorldSnapshot m_latest;
  std::uint32_t m_buildingId = 0;
  std::uint32_t m_fragmentsSeen = 0;
  std::uint32_t m_fragmentCount = 0;
  std::uint32_t m_dropped = 0;
  bool m_hasSnapshot = false;
};

// Orders travel the other way. Written by the client half, read and applied by the server half.
[[nodiscard]] bool WriteMoveOrder(const MoveOrder& _order, Neuron::Transport& _transport);
[[nodiscard]] bool ReadMoveOrder(std::span<const std::uint8_t> _datagram, MoveOrder& _outOrder);
} // namespace Game
