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
// otherwise would fail on the first ARM64 client (Design/Collision.md 2 puts servers on x64 and
// clients anywhere).
constexpr std::uint8_t KIND_SNAPSHOT = 1;
constexpr std::uint8_t KIND_MOVE_ORDER = 2;

// kind, complete, snapshotId, fragmentIndex, fragmentCount, tick, recordCount, leaveCount, destroyedCount
constexpr std::uint32_t SNAPSHOT_HEADER_BYTES = 1 + 1 + 4 + 4 + 4 + 8 + 4 + 4 + 4;
// handle, posWorld, prevPos, five floats, order, faction, hullId
constexpr std::uint32_t SHIP_RECORD_BYTES = 8 + 24 + 24 + 20 + 1 + 1 + 4;
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
  _out.U32(ship.hullId);
}
} // namespace

std::uint32_t SnapshotWriter::Write(const World& _world, Neuron::Transport& _transport)
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
    out.U32(0); // and therefore nothing to say left -- anything absent is gone by construction
    out.U32(0); // nor destroyed: a full snapshot states the world, not what changed in it

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

std::uint32_t SnapshotWriter::WriteInterest(const World& _world, std::span<const ShipHandle> _sent, std::span<const ShipHandle> _left,
                                            std::span<const ShipHandle> _destroyed, Neuron::Transport& _transport)
{
  m_lastBytes = 0;
  m_lastRecords = 0;

  const std::uint32_t perFragment = ShipsPerSnapshotFragment();
  const std::uint32_t sendCount = static_cast<std::uint32_t>(_sent.size());
  const std::uint32_t leaveCount = static_cast<std::uint32_t>(_left.size());
  const std::uint32_t destroyedCount = static_cast<std::uint32_t>(_destroyed.size());

  // Leaves and destroyed handles ride in the first fragment, and a fragment carries fewer records
  // when they do. A handle is eight bytes against a record's eighty-two, so this costs at most one
  // record's room.
  const std::uint32_t handleBytes = (leaveCount + destroyedCount) * HANDLE_BYTES;
  const std::uint32_t firstFragmentRoom = (Neuron::MAX_DATAGRAM_BYTES > SNAPSHOT_HEADER_BYTES + handleBytes)
                                            ? (Neuron::MAX_DATAGRAM_BYTES - SNAPSHOT_HEADER_BYTES - handleBytes) / SHIP_RECORD_BYTES
                                            : 0u;

  std::uint32_t remaining = (sendCount > firstFragmentRoom) ? sendCount - firstFragmentRoom : 0u;
  const std::uint32_t fragmentCount = 1 + (remaining + perFragment - 1) / perFragment;
  const std::uint32_t snapshotId = m_nextSnapshotId++;

  std::uint32_t sent = 0;
  std::uint32_t at = 0;
  for (std::uint32_t fragment = 0; fragment < fragmentCount; ++fragment)
  {
    const std::uint32_t room = (fragment == 0) ? firstFragmentRoom : perFragment;
    const std::uint32_t claimed = std::min(room, sendCount - at);

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
    out.U8(0); // an update, so the receiver upserts and applies the leave list
    out.U32(snapshotId);
    out.U32(fragment);
    out.U32(fragmentCount);
    out.U64(_world.Tick());
    out.U32(count);
    out.U32((fragment == 0) ? leaveCount : 0u);
    out.U32((fragment == 0) ? destroyedCount : 0u);

    for (const ShipId id : m_resolvedScratch)
      WriteShipRecord(out, _world, id);

    if (fragment == 0)
    {
      for (const ShipHandle handle : _left)
        out.Handle(handle);
      for (const ShipHandle handle : _destroyed)
        out.Handle(handle);
    }

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
  m_pendingLeaves.clear();
  m_pendingDestroyed.clear();
  m_buildingId = 0;
  m_fragmentsSeen = 0;
  m_fragmentCount = 0;
}

bool SnapshotReceiver::Accept(std::span<const std::uint8_t> _datagram)
{
  ByteReader in(_datagram);
  if (in.U8() != KIND_SNAPSHOT)
    return false;

  const bool complete = in.U8() != 0;
  const std::uint32_t snapshotId = in.U32();
  const std::uint32_t fragment = in.U32();
  const std::uint32_t fragmentCount = in.U32();
  const std::uint64_t tick = in.U64();
  const std::uint32_t count = in.U32();
  const std::uint32_t leaveCount = in.U32();
  const std::uint32_t destroyedCount = in.U32();
  if (!in.Ok() || fragmentCount == 0 || fragment >= fragmentCount)
    return false;

  // Fragments of a snapshot older than the one already applied are of no use: the world has moved
  // on and applying it would step the view backwards.
  if (m_hasSnapshot && tick <= m_latest.tick)
    return false;

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
    ship.hullId = in.U32();
    if (!in.Ok())
    {
      AbandonInProgress();
      return false;
    }
    m_pendingUpserts.push_back(ship);
  }

  for (std::uint32_t at = 0; at < leaveCount; ++at)
  {
    const ShipHandle handle = in.Handle();
    if (!in.Ok())
    {
      AbandonInProgress();
      return false;
    }
    m_pendingLeaves.push_back(handle);
  }

  for (std::uint32_t at = 0; at < destroyedCount; ++at)
  {
    const ShipHandle handle = in.Handle();
    if (!in.Ok())
    {
      AbandonInProgress();
      return false;
    }
    m_pendingDestroyed.push_back(handle);
  }

  ++m_fragmentsSeen;
  if (m_fragmentsSeen < m_fragmentCount)
    return false;

  Apply();
  m_latest.tick = m_buildingTick;
  m_pendingUpserts.clear();
  m_pendingLeaves.clear();
  m_pendingDestroyed.clear();
  m_buildingId = 0;
  m_fragmentsSeen = 0;
  m_fragmentCount = 0;
  m_hasSnapshot = true;
  return true;
}

// Applied only once every fragment is in, never as they land: a world left half-updated by a
// fragment that never arrived is exactly what "dropped whole" exists to prevent.
void SnapshotReceiver::Apply()
{
  m_destroyed.clear(); // it describes the update being applied, and nothing older

  // A complete snapshot IS the world, so anything it does not carry is gone. An update carries only
  // what changed, so what it does not mention is untouched. Without this distinction a full snapshot
  // could never remove a despawned ship, because it has no leave list to put one in.
  if (m_buildingComplete)
  {
    m_latest.ships.clear();
    m_latest.ships.insert(m_latest.ships.end(), m_pendingUpserts.begin(), m_pendingUpserts.end());
    return;
  }

  // Destroyed removes exactly as a leave does. The two lists differ in what they *say*, not in what
  // they do to the set: one is a departure and the other a death, and only the client's effects care
  // which (Design/Hostiles.md 4.4).
  const auto remove = [this](ShipHandle _gone)
  {
    for (std::size_t at = 0; at < m_latest.ships.size(); ++at)
    {
      if (m_latest.ships[at].handle == _gone)
      {
        // Swap-and-pop. The client's order is its own -- it is a set, not the world's array -- and
        // WorldView carries presentation state across by handle rather than by position.
        m_latest.ships[at] = m_latest.ships.back();
        m_latest.ships.pop_back();
        break;
      }
    }
  };
  for (const ShipHandle gone : m_pendingLeaves)
    remove(gone);
  for (const ShipHandle dead : m_pendingDestroyed)
    remove(dead);
  m_destroyed.insert(m_destroyed.end(), m_pendingDestroyed.begin(), m_pendingDestroyed.end());

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

  return _transport.Send(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
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
