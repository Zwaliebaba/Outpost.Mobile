#include "pch.h"
#include "WorldSnapshot.h"

#include "World.h"

#include <algorithm>
#include <cstring>

namespace Game
{
namespace
{
// Explicit little-endian byte order, so the format does not depend on the machine that wrote it.
// The two ends are the same binary today and will not be forever, and a format that quietly assumed
// otherwise would fail on the first ARM64 client (Design/Archive/Collision.md 2 puts servers on x64 and
// clients anywhere).
constexpr std::uint8_t KIND_SNAPSHOT = 1;
constexpr std::uint8_t KIND_MOVE_ORDER = 2;

// Leaves and deaths, on the reliable lane and no longer in the snapshot header.
//
// They moved because a lost leave is a ghost ship for the rest of the match: a snapshot is
// superseded by the next one and heals itself, a leave is stated once and never repeated
// (Design/MmoScalabilityReview.md E1, Design/Archive/ReliableFormat-work-order.md). Duplicating them onto
// both lanes was the alternative and lost -- two paths carrying the same fact is two paths to
// reason about, and the unreliable copy would still be the one that arrived first.
constexpr std::uint8_t KIND_LEAVE = 3;

// kind, complete, snapshotId, fragmentIndex, fragmentCount, tick, recordCount, hostileMask
//
// The mask is appended rather than inserted, so every field a reader already knew stays where it
// was. It rides every update rather than travelling on change because updates are datagrams: a lost
// "you are now criminal" would leave a client believing itself honest for the rest of the match,
// and one byte per update is the cheapest idempotence there is (Design/Stations.md 4.3).
constexpr std::uint32_t SNAPSHOT_HEADER_BYTES = 1 + 1 + 4 + 4 + 4 + 8 + 4 + 1;

// kind, tick, leaveCount, destroyedCount
constexpr std::uint32_t LEAVE_HEADER_BYTES = 1 + 8 + 4 + 4;
// handle, posWorld, prevPos, five floats, order, faction, flags, hullId
constexpr std::uint32_t SHIP_RECORD_BYTES = 8 + 24 + 24 + 20 + 1 + 1 + 1 + 4;
// kind, orderId, hasFacing, facingRad, destination, handleCount
constexpr std::uint32_t ORDER_HEADER_BYTES = 1 + 4 + 1 + 4 + 24 + 4;
constexpr std::uint32_t HANDLE_BYTES = 8;

class ByteWriter
{
public:
  explicit ByteWriter(std::vector<std::uint8_t>& _out) noexcept
    : m_out(&_out)
  {
  }

  void U8(std::uint8_t _value)
  {
    m_out->push_back(_value);
  }

  void U32(std::uint32_t _value)
  {
    for (int shift = 0; shift < 32; shift += 8)
      m_out->push_back(static_cast<std::uint8_t>((_value >> shift) & 0xFFu));
  }

  void U64(std::uint64_t _value)
  {
    for (int shift = 0; shift < 64; shift += 8)
      m_out->push_back(static_cast<std::uint8_t>((_value >> shift) & 0xFFu));
  }

  void I64(std::int64_t _value)
  {
    U64(static_cast<std::uint64_t>(_value));
  }

  void F32(float _value)
  {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &_value, sizeof(bits));
    U32(bits);
  }

  void Pos(const WorldPos& _pos)
  {
    I64(_pos.sectorX);
    I64(_pos.sectorZ);
    F32(_pos.localX);
    F32(_pos.localZ);
  }

  void Handle(ShipHandle _handle)
  {
    U32(_handle.slot);
    U32(_handle.generation);
  }

private:
  std::vector<std::uint8_t>* m_out;
};

class ByteReader
{
public:
  explicit ByteReader(std::span<const std::uint8_t> _bytes) noexcept
    : m_bytes(_bytes)
  {
  }

  [[nodiscard]] bool Ok() const noexcept
  {
    return m_ok;
  }

  [[nodiscard]] std::uint32_t Remaining() const noexcept
  {
    return static_cast<std::uint32_t>(m_bytes.size() - m_at);
  }

  std::uint8_t U8()
  {
    if (!Take(1))
      return 0;
    return m_bytes[m_at - 1];
  }

  std::uint32_t U32()
  {
    if (!Take(4))
      return 0;
    std::uint32_t value = 0;
    for (int byte = 0; byte < 4; ++byte)
      value |= static_cast<std::uint32_t>(m_bytes[m_at - 4 + byte]) << (byte * 8);
    return value;
  }

  std::uint64_t U64()
  {
    if (!Take(8))
      return 0;
    std::uint64_t value = 0;
    for (int byte = 0; byte < 8; ++byte)
      value |= static_cast<std::uint64_t>(m_bytes[m_at - 8 + byte]) << (byte * 8);
    return value;
  }

  std::int64_t I64()
  {
    return static_cast<std::int64_t>(U64());
  }

  float F32()
  {
    const std::uint32_t bits = U32();
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  WorldPos Pos()
  {
    WorldPos pos;
    pos.sectorX = I64();
    pos.sectorZ = I64();
    pos.localX = F32();
    pos.localZ = F32();
    return pos;
  }

  ShipHandle Handle()
  {
    ShipHandle handle;
    handle.slot = U32();
    handle.generation = U32();
    return handle;
  }

private:
  bool Take(std::size_t _count) noexcept
  {
    // A short read is a malformed datagram, not a crash. Everything after it reads as zero and the
    // caller checks Ok() once at the end rather than after every field.
    if (!m_ok || m_at + _count > m_bytes.size())
    {
      m_ok = false;
      return false;
    }
    m_at += _count;
    return true;
  }

  std::span<const std::uint8_t> m_bytes;
  std::size_t m_at = 0;
  bool m_ok = true;
};
} // namespace

std::uint32_t ShipsPerSnapshotFragment() noexcept
{
  return (Neuron::MAX_DATAGRAM_BYTES - SNAPSHOT_HEADER_BYTES) / SHIP_RECORD_BYTES;
}

// Still derived from the datagram bound, though an order now travels on the reliable lane and could
// be MAX_RELIABLE_BYTES long (ADR 0029). Keeping the smaller cap is deliberate: it is the number
// every existing test and the client's selection logic already agree on, and raising it is a wire
// change with nobody asking for it. The day a formation of more than this many ships is orderable,
// it moves -- and it moves as its own slice, because the cap is what stops one click from becoming
// an unbounded amount of pathfinding.
std::uint32_t MaxShipsPerOrder() noexcept
{
  return (Neuron::MAX_DATAGRAM_BYTES - ORDER_HEADER_BYTES) / HANDLE_BYTES;
}

namespace
{
void WriteShipRecord(ByteWriter& _out, const World& _world, ShipId _id)
{
  const ShipState& ship = _world.Ships()[_id];
  _out.Handle(_world.HandleOf(_id));
  _out.Pos(ship.posWorld);
  _out.Pos(ship.prevPos);
  _out.F32(ship.headingRad);
  _out.F32(ship.prevHeading);
  _out.F32(ship.speed);
  _out.F32(ship.accelSample);
  _out.F32(ship.turnRateRadPerSec);
  _out.U8(static_cast<std::uint8_t>(ship.order));
  _out.U8(ship.factionId);
  _out.U8(_world.IsStation(_id) ? SHIP_FLAG_STATION : std::uint8_t{0});
  _out.U32(ship.hullId);
}
} // namespace

std::uint32_t SnapshotWriter::Write(const World& _world, Neuron::Transport& _transport, FactionId _viewer)
{
  const std::span<const ShipState> ships = _world.Ships();
  const std::uint32_t perFragment = ShipsPerSnapshotFragment();
  const std::uint32_t shipCount = static_cast<std::uint32_t>(ships.size());

  // An empty world still sends one fragment. A snapshot that says "no ships" is information; the
  // absence of a snapshot is indistinguishable from a stalled server.
  const std::uint32_t fragmentCount = (shipCount + perFragment - 1) / perFragment + (shipCount == 0 ? 1 : 0);
  const std::uint32_t snapshotId = m_nextSnapshotId++;
  m_lastBytes = 0;
  m_lastRecords = 0;

  std::uint32_t sent = 0;
  for (std::uint32_t fragment = 0; fragment < fragmentCount; ++fragment)
  {
    const std::uint32_t first = fragment * perFragment;
    const std::uint32_t count = (first < shipCount) ? std::min(perFragment, shipCount - first) : 0u;

    m_scratch.clear();
    ByteWriter out(m_scratch);
    out.U8(KIND_SNAPSHOT);
    out.U8(1); // complete: this is the whole world, so the receiver replaces rather than upserts
    out.U32(snapshotId);
    out.U32(fragment);
    out.U32(fragmentCount);
    out.U64(_world.Tick());
    out.U32(count);
    out.U8(_world.HostileMaskFor(_viewer));

    for (std::uint32_t at = 0; at < count; ++at)
      WriteShipRecord(out, _world, static_cast<ShipId>(first + at));

    if (!_transport.Send(m_scratch.data(), static_cast<std::uint32_t>(m_scratch.size())))
      break;
    m_lastBytes += static_cast<std::uint32_t>(m_scratch.size());
    m_lastRecords += count;
    ++sent;
  }
  return sent;
}

bool SnapshotWriter::WriteLeaves(std::uint64_t _tick, std::span<const ShipHandle> _left, std::span<const ShipHandle> _destroyed,
                                 Neuron::Transport& _transport)
{
  if (_left.empty() && _destroyed.empty())
    return true; // nothing to state is not a failure to state it

  const std::uint32_t leaveCount = static_cast<std::uint32_t>(_left.size());
  const std::uint32_t destroyedCount = static_cast<std::uint32_t>(_destroyed.size());

  // One message, not fragmented. The lane's bound is what caps it, and at eight bytes a handle that
  // is 1021 departures in one update -- far past what an interest set can shed at 10 Hz, and the
  // day it is not, the answer is a second message and not a silent truncation.
  if (LEAVE_HEADER_BYTES + (leaveCount + destroyedCount) * HANDLE_BYTES > Neuron::MAX_RELIABLE_BYTES)
    return false;

  m_leaveScratch.clear();
  ByteWriter out(m_leaveScratch);
  out.U8(KIND_LEAVE);
  out.U64(_tick);
  out.U32(leaveCount);
  out.U32(destroyedCount);
  for (const ShipHandle handle : _left)
    out.Handle(handle);
  for (const ShipHandle handle : _destroyed)
    out.Handle(handle);

  return _transport.SendReliable(m_leaveScratch.data(), static_cast<std::uint32_t>(m_leaveScratch.size()));
}

std::uint32_t SnapshotWriter::WriteInterest(const World& _world, std::span<const ShipHandle> _sent, std::span<const ShipHandle> _left,
                                            std::span<const ShipHandle> _destroyed, Neuron::Transport& _transport, FactionId _viewer)
{
  m_lastBytes = 0;
  m_lastRecords = 0;

  const std::uint32_t perFragment = ShipsPerSnapshotFragment();
  const std::uint32_t sendCount = static_cast<std::uint32_t>(_sent.size());

  // The leave and destroyed lists go first, on the reliable lane and in one message of their own.
  // First rather than last because a leave that overtakes the upserts is harmless -- removing a
  // handle the receiver does not hold is a no-op -- while an upsert that overtakes its own leave
  // would put a dead ship back. The two lanes have no ordering between them, so the order that is
  // safe under either is the one to send in.
  if (!WriteLeaves(_world.Tick(), _left, _destroyed, _transport))
  {
    // The lane refused: it is full, or not up yet. Nothing is retried and the update goes on --
    // the next one carries these handles again only if the interest set states them again, which
    // it will not. A refused leave is therefore counted, so the gap is visible rather than silent.
    ++m_refusedLeaves;
  }

  // Every fragment now carries records and nothing else, so the first is no different from the rest.
  const std::uint32_t fragmentCount = (sendCount + perFragment - 1) / perFragment + (sendCount == 0 ? 1 : 0);
  const std::uint32_t snapshotId = m_nextSnapshotId++;

  std::uint32_t sent = 0;
  std::uint32_t at = 0;
  for (std::uint32_t fragment = 0; fragment < fragmentCount; ++fragment)
  {
    const std::uint32_t claimed = std::min(perFragment, sendCount - at);

    // Resolve BEFORE writing the header, because the header states how many records follow and a
    // handle can have died between the set being taken and this write. Declaring a count and then
    // skipping one would leave the receiver reading a record's worth of the next field.
    m_resolvedScratch.clear();
    for (std::uint32_t record = 0; record < claimed; ++record)
    {
      const ShipId id = _world.Resolve(_sent[at + record]);
      if (id != INVALID_SHIP_ID)
        m_resolvedScratch.push_back(id);
    }
    const std::uint32_t count = static_cast<std::uint32_t>(m_resolvedScratch.size());

    m_scratch.clear();
    ByteWriter out(m_scratch);
    out.U8(KIND_SNAPSHOT);
    out.U8(0); // an update, so the receiver upserts rather than replacing what it holds
    out.U32(snapshotId);
    out.U32(fragment);
    out.U32(fragmentCount);
    out.U64(_world.Tick());
    out.U32(count);
    out.U8(_world.HostileMaskFor(_viewer));

    for (const ShipId id : m_resolvedScratch)
      WriteShipRecord(out, _world, id);

    if (!_transport.Send(m_scratch.data(), static_cast<std::uint32_t>(m_scratch.size())))
      break;
    m_lastBytes += static_cast<std::uint32_t>(m_scratch.size());
    m_lastRecords += count;
    at += claimed; // claimed, not count: a handle that died is consumed, not retried
    ++sent;
  }
  return sent;
}

void SnapshotReceiver::AbandonInProgress() noexcept
{
  if (m_fragmentCount > 0 && m_fragmentsSeen < m_fragmentCount)
    ++m_dropped;
  m_pendingUpserts.clear();
  m_buildingId = 0;
  m_fragmentsSeen = 0;
  m_fragmentCount = 0;
}

bool SnapshotReceiver::Accept(std::span<const std::uint8_t> _datagram)
{
  ByteReader in(_datagram);
  const std::uint8_t kind = in.U8();
  if (kind == KIND_LEAVE)
    return AcceptLeaves(_datagram);
  if (kind != KIND_SNAPSHOT)
    return false;

  const bool complete = in.U8() != 0;
  const std::uint32_t snapshotId = in.U32();
  const std::uint32_t fragment = in.U32();
  const std::uint32_t fragmentCount = in.U32();
  const std::uint64_t tick = in.U64();
  const std::uint32_t count = in.U32();
  const std::uint8_t hostileMask = in.U8();
  if (!in.Ok() || fragmentCount == 0 || fragment >= fragmentCount)
    return false;

  // Fragments of a snapshot older than the one already applied are of no use: the world has moved
  // on and applying it would step the view backwards.
  if (m_hasSnapshot && tick <= m_latest.tick)
    return false;

  // Taken here rather than in Apply, and the asymmetry with the upserts is the point: an incomplete
  // update is dropped whole because half a set of records is half a world, but a mask is not coupled
  // to any record, so taking it from whatever fragment arrives is strictly more robust. Below the
  // staleness check, so a late fragment of a superseded update cannot walk it backwards.
  m_hostileMask = hostileMask;

  if (snapshotId != m_buildingId)
  {
    AbandonInProgress();
    m_buildingId = snapshotId;
    m_fragmentCount = fragmentCount;
    m_buildingTick = tick;
    m_buildingComplete = complete;
  }

  // Fragments arrive in order over this transport, so an out-of-order one means loss upstream.
  // Abandon rather than guess at where it belongs.
  if (fragment != m_fragmentsSeen)
  {
    AbandonInProgress();
    return false;
  }

  for (std::uint32_t at = 0; at < count; ++at)
  {
    ShipSnapshot ship;
    ship.handle = in.Handle();
    ship.posWorld = in.Pos();
    ship.prevPos = in.Pos();
    ship.headingRad = in.F32();
    ship.prevHeading = in.F32();
    ship.speed = in.F32();
    ship.accelSample = in.F32();
    ship.turnRateRadPerSec = in.F32();
    ship.order = static_cast<OrderState>(in.U8());
    ship.factionId = in.U8();
    ship.flags = in.U8();
    ship.hullId = in.U32();
    if (!in.Ok())
    {
      AbandonInProgress();
      return false;
    }
    m_pendingUpserts.push_back(ship);
  }

  ++m_fragmentsSeen;
  if (m_fragmentsSeen < m_fragmentCount)
    return false;

  Apply();
  m_latest.tick = m_buildingTick;
  m_pendingUpserts.clear();
  m_buildingId = 0;
  m_fragmentsSeen = 0;
  m_fragmentCount = 0;
  m_hasSnapshot = true;
  return true;
}

// Removes a handle from the held set, whether it left or died: the two differ in what they *say*,
// not in what they do to the set, and only the client's effects care which (Design/Archive/Hostiles.md 4.4).
void SnapshotReceiver::Remove(ShipHandle _gone)
{
  for (std::size_t at = 0; at < m_latest.ships.size(); ++at)
  {
    if (m_latest.ships[at].handle == _gone)
    {
      // Swap-and-pop. The client's order is its own -- it is a set, not the world's array -- and
      // WorldView carries presentation state across by handle rather than by position.
      m_latest.ships[at] = m_latest.ships.back();
      m_latest.ships.pop_back();
      return;
    }
  }
}

// One reliable message, applied whole and at once -- there is nothing to reassemble, because the
// lane delivers a message or does not deliver it.
//
// It is applied the moment it arrives rather than being held for a snapshot, and that is the point
// of the split: a departure no longer waits on the fragments of an update that may never complete.
// A handle that is not held is removed by a no-op, which is what makes the two lanes safe in either
// order.
bool SnapshotReceiver::AcceptLeaves(std::span<const std::uint8_t> _message)
{
  ByteReader in(_message);
  if (in.U8() != KIND_LEAVE)
    return false;

  const std::uint64_t tick = in.U64();
  const std::uint32_t leaveCount = in.U32();
  const std::uint32_t destroyedCount = in.U32();
  if (!in.Ok())
    return false;

  // Read the whole message before touching the set: a truncated one must change nothing rather than
  // remove half of what it names.
  m_leaveScratch.clear();
  m_destroyedScratch.clear();
  for (std::uint32_t at = 0; at < leaveCount; ++at)
    m_leaveScratch.push_back(in.Handle());
  for (std::uint32_t at = 0; at < destroyedCount; ++at)
    m_destroyedScratch.push_back(in.Handle());
  if (!in.Ok())
    return false;

  for (const ShipHandle gone : m_leaveScratch)
    Remove(gone);
  for (const ShipHandle dead : m_destroyedScratch)
    Remove(dead);

  // Appended, not assigned: several of these can arrive in one drain, and every death in them is one
  // the client owes an explosion. The consumer clears it when it has drawn them.
  m_destroyed.insert(m_destroyed.end(), m_destroyedScratch.begin(), m_destroyedScratch.end());

  // The lane is ordered, so a later message cannot be overtaken by an earlier one; the tick is
  // carried for diagnostics and for the day a subscriber wants to know how stale a departure is.
  m_lastLeaveTick = tick;
  return true;
}

// Applied only once every fragment is in, never as they land: a world left half-updated by a
// fragment that never arrived is exactly what "dropped whole" exists to prevent.
void SnapshotReceiver::Apply()
{
  // A complete snapshot IS the world, so anything it does not carry is gone. An update carries only
  // what changed, so what it does not mention is untouched. Without this distinction a full snapshot
  // could never remove a despawned ship, because departures no longer travel with it at all -- they
  // are their own message on the reliable lane now (KIND_LEAVE).
  if (m_buildingComplete)
  {
    m_latest.ships.clear();
    m_latest.ships.insert(m_latest.ships.end(), m_pendingUpserts.begin(), m_pendingUpserts.end());
    return;
  }

  for (const ShipSnapshot& ship : m_pendingUpserts)
  {
    bool found = false;
    for (ShipSnapshot& held : m_latest.ships)
    {
      if (held.handle == ship.handle)
      {
        held = ship;
        found = true;
        break;
      }
    }
    if (!found)
      m_latest.ships.push_back(ship);
  }
}

bool WriteMoveOrder(const MoveOrder& _order, Neuron::Transport& _transport)
{
  if (_order.ships.empty() || _order.ships.size() > MaxShipsPerOrder())
    return false;

  std::vector<std::uint8_t> bytes;
  ByteWriter out(bytes);
  out.U8(KIND_MOVE_ORDER);
  out.U32(0); // order id, reserved: nothing acknowledges an order yet
  out.U8(_order.hasFacing ? 1u : 0u);
  out.F32(_order.facingRad);
  out.Pos(_order.destination);
  out.U32(static_cast<std::uint32_t>(_order.ships.size()));
  for (const ShipHandle handle : _order.ships)
    out.Handle(handle);

  // On the reliable lane: a dropped order is a click the player made and the game ignored, which is
  // the one failure mode no amount of interpolation covers up (Design/Archive/QuicTransport.md 8).
  return _transport.SendReliable(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

bool ReadMoveOrder(std::span<const std::uint8_t> _datagram, MoveOrder& _outOrder)
{
  ByteReader in(_datagram);
  if (in.U8() != KIND_MOVE_ORDER)
    return false;

  (void)in.U32(); // order id
  const bool hasFacing = in.U8() != 0;
  const float facingRad = in.F32();
  const WorldPos destination = in.Pos();
  const std::uint32_t count = in.U32();
  if (!in.Ok() || count == 0 || count > MaxShipsPerOrder() || in.Remaining() < count * HANDLE_BYTES)
    return false;

  _outOrder.ships.clear();
  _outOrder.ships.reserve(count);
  for (std::uint32_t at = 0; at < count; ++at)
    _outOrder.ships.push_back(in.Handle());
  if (!in.Ok())
    return false;

  _outOrder.destination = destination;
  _outOrder.facingRad = facingRad;
  _outOrder.hasFacing = hasFacing;
  return true;
}
} // namespace Game
