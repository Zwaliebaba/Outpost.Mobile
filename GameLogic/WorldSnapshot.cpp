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

// kind, snapshotId, fragmentIndex, fragmentCount, tick, recordCount
constexpr std::uint32_t SNAPSHOT_HEADER_BYTES = 1 + 4 + 4 + 4 + 8 + 4;
// handle, posWorld, prevPos, five floats, order, hullId
constexpr std::uint32_t SHIP_RECORD_BYTES = 8 + 24 + 24 + 20 + 1 + 4;
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

std::uint32_t SnapshotWriter::Write(const World& _world, Neuron::Transport& _transport)
{
  const std::span<const ShipState> ships = _world.Ships();
  const std::uint32_t perFragment = ShipsPerSnapshotFragment();
  const std::uint32_t shipCount = static_cast<std::uint32_t>(ships.size());

  // An empty world still sends one fragment. A snapshot that says "no ships" is information; the
  // absence of a snapshot is indistinguishable from a stalled server.
  const std::uint32_t fragmentCount = (shipCount + perFragment - 1) / perFragment + (shipCount == 0 ? 1 : 0);
  const std::uint32_t snapshotId = m_nextSnapshotId++;

  std::uint32_t sent = 0;
  for (std::uint32_t fragment = 0; fragment < fragmentCount; ++fragment)
  {
    const std::uint32_t first = fragment * perFragment;
    const std::uint32_t count = (first < shipCount) ? std::min(perFragment, shipCount - first) : 0u;

    m_scratch.clear();
    ByteWriter out(m_scratch);
    out.U8(KIND_SNAPSHOT);
    out.U32(snapshotId);
    out.U32(fragment);
    out.U32(fragmentCount);
    out.U64(_world.Tick());
    out.U32(count);

    for (std::uint32_t at = 0; at < count; ++at)
    {
      const ShipId id = static_cast<ShipId>(first + at);
      const ShipState& ship = ships[id];
      out.Handle(_world.HandleOf(id));
      out.Pos(ship.posWorld);
      out.Pos(ship.prevPos);
      out.F32(ship.headingRad);
      out.F32(ship.prevHeading);
      out.F32(ship.speed);
      out.F32(ship.accelSample);
      out.F32(ship.turnRateRadPerSec);
      out.U8(static_cast<std::uint8_t>(ship.order));
      out.U32(ship.hullId);
    }

    if (!_transport.Send(m_scratch.data(), static_cast<std::uint32_t>(m_scratch.size())))
      break;
    ++sent;
  }
  return sent;
}

void SnapshotReceiver::AbandonInProgress() noexcept
{
  if (m_fragmentCount > 0 && m_fragmentsSeen < m_fragmentCount)
    ++m_dropped;
  m_building.ships.clear();
  m_buildingId = 0;
  m_fragmentsSeen = 0;
  m_fragmentCount = 0;
}

bool SnapshotReceiver::Accept(std::span<const std::uint8_t> _datagram)
{
  ByteReader in(_datagram);
  if (in.U8() != KIND_SNAPSHOT)
    return false;

  const std::uint32_t snapshotId = in.U32();
  const std::uint32_t fragment = in.U32();
  const std::uint32_t fragmentCount = in.U32();
  const std::uint64_t tick = in.U64();
  const std::uint32_t count = in.U32();
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
    m_building.tick = tick;
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
    ship.hullId = in.U32();
    if (!in.Ok())
    {
      AbandonInProgress();
      return false;
    }
    m_building.ships.push_back(ship);
  }

  ++m_fragmentsSeen;
  if (m_fragmentsSeen < m_fragmentCount)
    return false;

  m_latest = std::move(m_building);
  m_building = WorldSnapshot{};
  m_buildingId = 0;
  m_fragmentsSeen = 0;
  m_fragmentCount = 0;
  m_hasSnapshot = true;
  return true;
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
