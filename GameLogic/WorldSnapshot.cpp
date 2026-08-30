#include "pch.h"
#include "WorldSnapshot.h"

#include "World.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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

// The dock order, beside the move order. A second kind rather than a flag on the first, because the
// two carry different payloads -- a station handle against a destination and a facing -- and a
// discriminated kind is what the format already uses to say so.
constexpr std::uint8_t KIND_DOCK_ORDER = 4;

// kind, complete, snapshotId, fragmentIndex, fragmentCount, tick, recordCount, hostileMask
//
// The mask is appended rather than inserted, so every field a reader already knew stays where it
// was. It rides every update rather than travelling on change because updates are datagrams: a lost
// "you are now criminal" would leave a client believing itself honest for the rest of the match,
// and one byte per update is the cheapest idempotence there is (Design/Stations.md 4.3).
constexpr std::uint32_t SNAPSHOT_HEADER_BYTES = 1 + 1 + 4 + 4 + 4 + 8 + 4 + 1;

// kind, tick, leaveCount, destroyedCount, dockedCount
//
// The docked handles ride this message and not the snapshot header, and Design/Stations.md 7.4 says
// otherwise only because it predates ADR 0029 moving departures onto the reliable lane. That ADR's
// argument covers a docking exactly: a snapshot is superseded by the next one and heals itself, a
// departure is stated once, and a lost "it docked" is a ghost ship for the rest of the match. So
// ShipsPerSnapshotFragment does *not* follow this header -- it derives from SNAPSHOT_HEADER_BYTES,
// which a docking never touches (Design/Stations-slice-3.md 2.1, ADR 0040).
constexpr std::uint32_t LEAVE_HEADER_BYTES = 1 + 8 + 4 + 4 + 4;
// handle, the sector pair, the local offsets, the prevPos delta, two angles, three floats, order,
// faction, flags, hullId.
//
// 47 bytes, from 83. What bought the 36 is below: positions moved onto a 0.125 m lattice, the
// sector pair narrowed from i64 to i32 (ADR 0042), prevPos became a delta against posWorld rather
// than a second whole position, and the two angles became turns16. At 1,152 bytes a datagram less a
// 27-byte header that is 23 ship records per fragment against the 13 this replaces -- so a
// hundred-ship update is 5 fragments instead of 8, which is what finding E1 cares about: at 2%
// datagram loss it completes 90% of the time rather than 85% (Design/Archive/QuantizedWire-work-order.md).
constexpr std::uint32_t SHIP_RECORD_BYTES = 8 + 8 + 4 + 4 + 4 + 12 + 1 + 1 + 1 + 4;
// kind, orderId, hasFacing, facingRad, destination, handleCount
constexpr std::uint32_t ORDER_HEADER_BYTES = 1 + 4 + 1 + 4 + 24 + 4;
constexpr std::uint32_t HANDLE_BYTES = 8;

// The wire's position lattice: 0.125 m a step.
//
// SECTOR_SIZE_METRES is 8,192, so a sector is exactly 65,536 steps and a local offset is a u16 with
// nothing left over and nothing wasted. A centimetre lattice -- which two comments in this tree
// used to promise -- needs 20 bits for the same span, so it spans a byte boundary or spends three
// bytes on a field that has two. The step is the largest that is invisible at the scale a hull is
// drawn: 6.25 cm at worst, against capsule radii of 1.1 m and up
// (Design/Archive/QuantizedWire-work-order.md 2.1).
constexpr float POSITION_STEP_METRES = 0.125f;
constexpr float POSITION_STEPS_PER_METRE = 8.0f;
constexpr std::int64_t POSITION_STEPS_PER_SECTOR = 65536;
static_assert(POSITION_STEP_METRES * POSITION_STEPS_PER_METRE == 1.0f, "the step and its reciprocal disagree");
static_assert(SECTOR_SIZE_METRES * POSITION_STEPS_PER_METRE == static_cast<float>(POSITION_STEPS_PER_SECTOR),
              "a sector must be exactly 65,536 steps, or a local offset does not fit a u16");

// An angle on the wire: a sixteenth-bit of a turn, so a step is XM_2PI/65536 = 9.59e-5 rad and
// rounding costs at most XM_PI/65536. Fixed point wraps by construction -- there is no
// representable value that is not an angle -- which is the property that lets the encode take a
// fractional turn rather than clamp a range.
constexpr float TURNS16_PER_TURN = 65536.0f;

// A position as the wire states it. The sector pair is still i64 here: narrowing to the wire's i32
// happens at the field, so the delta below is computed in the range the simulation actually uses.
struct LatticePos
{
  std::int64_t sectorX = 0;
  std::int64_t sectorZ = 0;
  std::uint16_t stepX = 0;
  std::uint16_t stepZ = 0;
};

// One axis of the encode, carrying whole sectors out of the offset the way Translate does.
//
// localX is in [0, SECTOR_SIZE_METRES) by WorldPos's invariant, so scaling lands in [0, 65536) and
// rounding to nearest can carry it to exactly 65536 -- which is the next sector's origin and not a
// seventeenth bit. Carrying rather than clamping is what keeps the encode monotonic across a
// border: a ship a millimetre short of one encodes to the border, never to the far corner of the
// sector it is leaving.
//
// The test against zero is not for the invariant, which holds. It is for the day something hands
// this a position it built by hand, because a negative float converted to an unsigned type is
// undefined behaviour and a saturating read is the whole cost of it not being.
void ToLatticeAxis(float _local, std::int64_t& _sector, std::uint16_t& _outStep) noexcept
{
  const float steps = _local * POSITION_STEPS_PER_METRE + 0.5f;
  const std::int64_t whole = (steps > 0.0f) ? static_cast<std::int64_t>(steps) : 0;
  _sector += whole / POSITION_STEPS_PER_SECTOR;
  _outStep = static_cast<std::uint16_t>(whole % POSITION_STEPS_PER_SECTOR);
}

[[nodiscard]] LatticePos ToLattice(const WorldPos& _pos) noexcept
{
  LatticePos out;
  out.sectorX = _pos.sectorX;
  out.sectorZ = _pos.sectorZ;
  ToLatticeAxis(_pos.localX, out.sectorX, out.stepX);
  ToLatticeAxis(_pos.localZ, out.sectorZ, out.stepZ);
  return out;
}

// Back to metres, exactly: a step is a power of two, so the product is representable and decoding an
// encoded lattice point returns the point rather than something near it.
[[nodiscard]] WorldPos FromLattice(std::int32_t _sectorX, std::int32_t _sectorZ, std::uint16_t _stepX, std::uint16_t _stepZ) noexcept
{
  return WorldPos{_sectorX, _sectorZ, static_cast<float>(_stepX) * POSITION_STEP_METRES, static_cast<float>(_stepZ) * POSITION_STEP_METRES};
}

// The wire's sector index is 32 bits where the simulation's is 64 -- +/-1,858 light years against
// +/-8 million (ADR 0042). Out of range saturates rather than wrapping, so the failure mode is a
// ship pinned at the edge of the addressable universe and not one that appears on the other side
// of it.
[[nodiscard]] std::int32_t ToWireSector(std::int64_t _sector) noexcept
{
  constexpr std::int64_t LOWEST = std::numeric_limits<std::int32_t>::min();
  constexpr std::int64_t HIGHEST = std::numeric_limits<std::int32_t>::max();
  return static_cast<std::int32_t>(std::clamp(_sector, LOWEST, HIGHEST));
}

// prevPos travels as a delta from posWorld in these same steps, so the receiver reconstructs the
// quantized prevPos exactly and its only error against the true one is that position's own
// rounding. Quantizing the delta against the raw posWorld would stack two roundings and double the
// bound, which is why both are put on the lattice first
// (Design/Archive/QuantizedWire-work-order.md 2.3).
//
// A tick of travel is 0.567 m for the fastest hull -- four and a half steps -- against a range of
// +/-32,767, so the saturation is about 7,000x from anything the simulation can produce, and costs
// one interpolation sample if it ever fires.
[[nodiscard]] std::int16_t LatticeDelta(std::int64_t _fromSector, std::uint16_t _fromStep, std::int64_t _toSector,
                                        std::uint16_t _toStep) noexcept
{
  constexpr std::int64_t LOWEST = std::numeric_limits<std::int16_t>::min();
  constexpr std::int64_t HIGHEST = std::numeric_limits<std::int16_t>::max();
  const std::int64_t sectorSteps = std::clamp(_toSector - _fromSector, LOWEST, HIGHEST) * POSITION_STEPS_PER_SECTOR;
  const std::int64_t steps = sectorSteps + (static_cast<std::int64_t>(_toStep) - static_cast<std::int64_t>(_fromStep));
  return static_cast<std::int16_t>(std::clamp(steps, LOWEST, HIGHEST));
}

// Radians to turns16. The fractional turn is what makes this total: every float that is an angle
// encodes, whatever its sign or how many turns it has accumulated, and nothing has to normalise
// first. The test against zero catches a NaN as well as a negative -- both compare false -- for the
// unsigned-conversion reason ToLatticeAxis gives.
[[nodiscard]] std::uint16_t ToTurns16(float _radians) noexcept
{
  const float turns = _radians / DirectX::XM_2PI;
  const float fraction = turns - std::floor(turns);
  const float scaled = fraction * TURNS16_PER_TURN + 0.5f;
  const std::uint32_t whole = (scaled > 0.0f) ? static_cast<std::uint32_t>(scaled) : 0u;
  // A fraction that rounds up to a whole turn is zero turns, which is what the mask says.
  return static_cast<std::uint16_t>(whole & 0xFFFFu);
}

// Back to (-pi, pi], which is the range Movement keeps a heading in (XMScalarModAngle) -- so a
// decoded heading is comparable with a simulated one rather than merely equivalent to it. A source
// of exactly -pi comes back as +pi, which is the same angle and not the same number: an assertion
// on a heading is an assertion on the wrapped difference.
[[nodiscard]] float FromTurns16(std::uint16_t _turns) noexcept
{
  const float radians = static_cast<float>(_turns) * (DirectX::XM_2PI / TURNS16_PER_TURN);
  return (radians > DirectX::XM_PI) ? radians - DirectX::XM_2PI : radians;
}

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

  void U16(std::uint16_t _value)
  {
    for (int shift = 0; shift < 16; shift += 8)
      m_out->push_back(static_cast<std::uint8_t>((_value >> shift) & 0xFFu));
  }

  void U32(std::uint32_t _value)
  {
    for (int shift = 0; shift < 32; shift += 8)
      m_out->push_back(static_cast<std::uint8_t>((_value >> shift) & 0xFFu));
  }

  // Two's complement over the unsigned width, which C++20 makes the only representation there is --
  // so the conversion is a reinterpretation and the reader's cast back is its exact inverse.
  void I16(std::int16_t _value)
  {
    U16(static_cast<std::uint16_t>(_value));
  }

  void I32(std::int32_t _value)
  {
    U32(static_cast<std::uint32_t>(_value));
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

  std::uint16_t U16()
  {
    if (!Take(2))
      return 0;
    std::uint32_t value = 0;
    for (int byte = 0; byte < 2; ++byte)
      value |= static_cast<std::uint32_t>(m_bytes[m_at - 2 + byte]) << (byte * 8);
    return static_cast<std::uint16_t>(value);
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

  std::int16_t I16()
  {
    return static_cast<std::int16_t>(U16());
  }

  std::int32_t I32()
  {
    return static_cast<std::int32_t>(U32());
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

  // Both positions go onto the lattice before either is written, because prevPos is stated as a
  // step delta from the *quantized* posWorld and not from the one the simulation holds.
  const LatticePos pos = ToLattice(ship.posWorld);
  const LatticePos prev = ToLattice(ship.prevPos);

  _out.Handle(_world.HandleOf(_id));
  _out.I32(ToWireSector(pos.sectorX));
  _out.I32(ToWireSector(pos.sectorZ));
  _out.U16(pos.stepX);
  _out.U16(pos.stepZ);
  _out.I16(LatticeDelta(pos.sectorX, pos.stepX, prev.sectorX, prev.stepX));
  _out.I16(LatticeDelta(pos.sectorZ, pos.stepZ, prev.sectorZ, prev.stepZ));
  _out.U16(ToTurns16(ship.headingRad));
  _out.U16(ToTurns16(ship.prevHeading));
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
                                 std::span<const ShipHandle> _docked, Neuron::Transport& _transport)
{
  if (_left.empty() && _destroyed.empty() && _docked.empty())
    return true; // nothing to state is not a failure to state it

  const std::uint32_t leaveCount = static_cast<std::uint32_t>(_left.size());
  const std::uint32_t destroyedCount = static_cast<std::uint32_t>(_destroyed.size());
  const std::uint32_t dockedCount = static_cast<std::uint32_t>(_docked.size());

  // One message, not fragmented. The lane's bound is what caps it, and at eight bytes a handle that
  // is 1021 departures in one update -- far past what an interest set can shed at 10 Hz, and the
  // day it is not, the answer is a second message and not a silent truncation. The third run costs
  // the bound nothing: 8192 less a 21-byte header is still 1021 handles.
  if (LEAVE_HEADER_BYTES + (leaveCount + destroyedCount + dockedCount) * HANDLE_BYTES > Neuron::MAX_RELIABLE_BYTES)
    return false;

  m_leaveScratch.clear();
  ByteWriter out(m_leaveScratch);
  out.U8(KIND_LEAVE);
  out.U64(_tick);
  out.U32(leaveCount);
  out.U32(destroyedCount);
  out.U32(dockedCount);
  for (const ShipHandle handle : _left)
    out.Handle(handle);
  for (const ShipHandle handle : _destroyed)
    out.Handle(handle);
  for (const ShipHandle handle : _docked)
    out.Handle(handle);

  return _transport.SendReliable(m_leaveScratch.data(), static_cast<std::uint32_t>(m_leaveScratch.size()));
}

std::uint32_t SnapshotWriter::WriteInterest(const World& _world, std::span<const ShipHandle> _sent, std::span<const ShipHandle> _left,
                                            std::span<const ShipHandle> _destroyed, std::span<const ShipHandle> _docked,
                                            Neuron::Transport& _transport, FactionId _viewer)
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
  if (!WriteLeaves(_world.Tick(), _left, _destroyed, _docked, _transport))
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
    const std::int32_t sectorX = in.I32();
    const std::int32_t sectorZ = in.I32();
    const std::uint16_t stepX = in.U16();
    const std::uint16_t stepZ = in.U16();
    const std::int16_t prevStepX = in.I16();
    const std::int16_t prevStepZ = in.I16();
    ship.posWorld = FromLattice(sectorX, sectorZ, stepX, stepZ);

    // Through Translate rather than by adding to the fields, so the sector carry is the simulation's
    // own and a prevPos on the far side of a border lands in the sector it belongs to. Exact: the
    // position and the delta are both multiples of the step, their sum needs 17 bits of mantissa
    // against float's 24, and the carry divides by a power of two.
    ship.prevPos = ship.posWorld;
    Translate(ship.prevPos, static_cast<float>(prevStepX) * POSITION_STEP_METRES, static_cast<float>(prevStepZ) * POSITION_STEP_METRES);

    ship.headingRad = FromTurns16(in.U16());
    ship.prevHeading = FromTurns16(in.U16());
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
  const std::uint32_t dockedCount = in.U32();
  if (!in.Ok())
    return false;

  // Read the whole message before touching the set: a truncated one must change nothing rather than
  // remove half of what it names.
  m_leaveScratch.clear();
  m_destroyedScratch.clear();
  m_dockedScratch.clear();
  for (std::uint32_t at = 0; at < leaveCount; ++at)
    m_leaveScratch.push_back(in.Handle());
  for (std::uint32_t at = 0; at < destroyedCount; ++at)
    m_destroyedScratch.push_back(in.Handle());
  for (std::uint32_t at = 0; at < dockedCount; ++at)
    m_dockedScratch.push_back(in.Handle());
  if (!in.Ok())
    return false;

  // All three leave the held set the same way. The lists differ in what they *say*, not in what they
  // do to the set, and only the client's effects care which.
  for (const ShipHandle gone : m_leaveScratch)
    Remove(gone);
  for (const ShipHandle dead : m_destroyedScratch)
    Remove(dead);
  for (const ShipHandle docked : m_dockedScratch)
    Remove(docked);

  // Appended, not assigned: several of these can arrive in one drain, and every death in them is one
  // the client owes an explosion. The consumer clears it when it has drawn them.
  m_destroyed.insert(m_destroyed.end(), m_destroyedScratch.begin(), m_destroyedScratch.end());
  m_docked.insert(m_docked.end(), m_dockedScratch.begin(), m_dockedScratch.end());

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

// kind, orderId, station handle, handleCount -- 17 bytes, against the move order's 38.
bool WriteDockOrder(const DockOrder& _order, Neuron::Transport& _transport)
{
  // MaxShipsPerOrder, not a cap of its own. This header is smaller than a move order's, so its own
  // arithmetic would admit two more ships -- and two caps differing by two is a fact nobody would
  // remember and no test would pin. One number, which the client's selection logic already agrees
  // on (Design/Stations-slice-3.md 2.6).
  if (_order.ships.empty() || _order.ships.size() > MaxShipsPerOrder())
    return false;

  std::vector<std::uint8_t> bytes;
  ByteWriter out(bytes);
  out.U8(KIND_DOCK_ORDER);
  out.U32(0); // order id, reserved: nothing acknowledges an order yet
  out.Handle(_order.station);
  out.U32(static_cast<std::uint32_t>(_order.ships.size()));
  for (const ShipHandle handle : _order.ships)
    out.Handle(handle);

  // The reliable lane, for the move order's reason: a dropped order is a click the player made and
  // the game ignored (ADR 0029).
  return _transport.SendReliable(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

bool ReadDockOrder(std::span<const std::uint8_t> _datagram, DockOrder& _outOrder)
{
  ByteReader in(_datagram);
  if (in.U8() != KIND_DOCK_ORDER)
    return false;

  (void)in.U32(); // order id
  const ShipHandle station = in.Handle();
  const std::uint32_t count = in.U32();
  if (!in.Ok() || count == 0 || count > MaxShipsPerOrder() || in.Remaining() < count * HANDLE_BYTES)
    return false;

  _outOrder.ships.clear();
  _outOrder.ships.reserve(count);
  for (std::uint32_t at = 0; at < count; ++at)
    _outOrder.ships.push_back(in.Handle());
  if (!in.Ok())
    return false;

  _outOrder.station = station;
  return true;
}
} // namespace Game
