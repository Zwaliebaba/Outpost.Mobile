#include "pch.h"
#include "UniverseSnapshot.h"

#include "Universe.h"

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

// 2 and 4 were the ship-list move and dock orders. They are RETIRED rather than reused: a client
// built against an older tree would have its move orders read as whatever took the number, and a
// kind that once meant something is the one value a format must not recycle
// (Design/Archive/Fleets-slice-6.md 2.10).

// Leaves and deaths, on the reliable lane and no longer in the snapshot header.
//
// They moved because a lost leave is a ghost ship for the rest of the match: a snapshot is
// superseded by the next one and heals itself, a leave is stated once and never repeated
// (Design/Archive/MmoScalabilityReview.md E1, Design/Archive/ReliableFormat-work-order.md). Duplicating them onto
// both lanes was the alternative and lost -- two paths carrying the same fact is two paths to
// reason about, and the unreliable copy would still be the one that arrived first.
constexpr std::uint8_t KIND_LEAVE = 3;

// An order that names a fleet rather than the ships in it (ADR 0049). It is the smallest message on
// this lane, and since the two that carried ship lists retired it is the only one that moves ships
// at all.
constexpr std::uint8_t KIND_FLEET_ORDER = 5;

// Who is in a fleet, downward and reliably. It is not in the ship record and never will be:
// a record is per-update and membership changes at human speed, so the roster is the delta and the
// record keeps the fixed width below (Design/Archive/Fleets.md 8.1).
constexpr std::uint8_t KIND_FLEET_ROSTER = 6;

// The one request/reply pair on this seam. A station's ledger is large, private, slow-changing and
// wanted by exactly one client at exactly one moment -- broadcasting it would put every station's
// contents on every wire ten times a second for a screen nobody has open (ADR 0051).
constexpr std::uint8_t KIND_LEDGER_REQUEST = 7;
constexpr std::uint8_t KIND_LEDGER_REPLY = 8;

// A draft, upward. The fourth order kind and the only one that names no ship.
constexpr std::uint8_t KIND_COMPOSE_ORDER = 9;

// Gunfire, downward, on the DATAGRAM lane -- the one message here whose answer to ADR 0029's
// question is "no, a later message does not make a lost one right", and which takes that lane
// anyway. The question is the wrong one for a message whose only consumers are a muzzle flash, a
// tracer and a turret slew: every authoritative consequence of a shot already travels elsewhere and
// reliably -- the damage as a fraction in the ship record, the death in a leave run -- so a lost
// flash is not a lie, while a late one draws a line into empty space (ADR 0053).
constexpr std::uint8_t KIND_FIRE = 10;

// kind, complete, snapshotId, fragmentIndex, fragmentCount, tick, recordCount, hostileMask
//
// The mask is appended rather than inserted, so every field a reader already knew stays where it
// was. It rides every update rather than travelling on change because updates are datagrams: a lost
// "you are now criminal" would leave a client believing itself honest for the rest of the match,
// and one byte per update is the cheapest idempotence there is (Design/Archive/Stations.md 4.3).
constexpr std::uint32_t SNAPSHOT_HEADER_BYTES = 1 + 1 + 4 + 4 + 4 + 8 + 4 + 1;

// One fleet in the status block: the wire's narrowed sector pair, the record's own lattice, the
// status byte and the count. A block is one mask byte plus this per set bit -- 1 byte at no fleets,
// 71 at five, on an update that is already carrying a kilobyte of records.
//
// It rides EVERY fragment header, beside hostileMask and for hostileMask's reason: a lost "your
// fleet is under attack" would leave a button dark through the fight, and stamping it costs less
// than any scheme for repairing it. Which means ShipsPerSnapshotFragment must be sized against the
// WORST case rather than against what an update happens to carry -- one number that every caller
// and every test agree on, rather than the truer number nobody can state. It costs one record a
// fragment: 22 rather than 23 (Design/Archive/Fleets-slice-5.md 2.2).
constexpr std::uint32_t FLEET_STATUS_BYTES = 4 + 4 + 2 + 2 + 1 + 1 + 1 + 1;
constexpr std::uint32_t FLEET_BLOCK_MAX_BYTES = 1 + FLEET_SLOTS * FLEET_STATUS_BYTES;

// kind, tick, leaveCount, destroyedCount, dockedCount, jumpedCount
//
// The docked handles ride this message and not the snapshot header, and Design/Archive/Stations.md 7.4 says
// otherwise only because it predates ADR 0029 moving departures onto the reliable lane. That ADR's
// argument covers a docking exactly: a snapshot is superseded by the next one and heals itself, a
// departure is stated once, and a lost "it docked" is a ghost ship for the rest of the match. So
// ShipsPerSnapshotFragment does *not* follow this header -- it derives from SNAPSHOT_HEADER_BYTES,
// which a docking never touches (Design/Archive/Stations-slice-3.md 2.1, ADR 0040).
constexpr std::uint32_t LEAVE_HEADER_BYTES = 1 + 8 + 4 + 4 + 4 + 4;
// handle, the sector pair, the local offsets, the prevPos delta, two angles, three floats, order,
// faction, flags, hullId, hullFraction.
//
// 48 bytes, from 83. What bought the 35 is below: positions moved onto a 0.125 m lattice, the
// sector pair narrowed from i64 to i32 (ADR 0046), prevPos became a delta against posUniverse rather
// than a second whole position, and the two angles became turns16. The record was 47 until combat
// put a byte of hull fraction in it, which is the one place state that heals belongs
// (Design/Combat.md 9.1) -- and the capacity below re-derived itself, which is the point of deriving
// it. At 1,152 bytes a datagram less the header and the fleet block that rides every fragment, that
// is 21 ship records per fragment against the 13 this replaces -- so a hundred-ship update is 5
// fragments instead of 8, which is what finding E1 cares about: at 2% datagram loss it completes 90%
// of the time rather than 85% (Design/Archive/QuantizedWire-work-order.md).
constexpr std::uint32_t SHIP_RECORD_BYTES = 8 + 8 + 4 + 4 + 4 + 12 + 1 + 1 + 1 + 4 + 1;

// kind, tick, count, then the events. Seventeen bytes each: two entities and a mount index narrowed
// to the byte it fits in, since MAX_MOUNTS is six.
constexpr std::uint32_t FIRE_HEADER_BYTES = 1 + 8 + 2;
constexpr std::uint32_t FIRE_EVENT_BYTES = 8 + 8 + 1;
static_assert(FIRE_HEADER_BYTES + MAX_FIRE_EVENTS * FIRE_EVENT_BYTES <= Neuron::MAX_DATAGRAM_BYTES,
              "a full fire message must fit one datagram, or the cap is meaningless");
// The saved state's magic. Not a KIND_* value: a state buffer is not a datagram and never meets
// SnapshotReceiver::Accept, so the magic is what turns feeding it to one into a refusal rather than
// a misread. The format byte beside it on the wire is UNIVERSE_STATE_FORMAT in the header, and what
// a build does with a byte older than its own is read with it: a format inside the window is
// migrated on read, and one outside it is refused, because a version nobody checks is a version
// nobody has.
constexpr std::uint32_t UNIVERSE_STATE_MAGIC = 0x54535550u; // 'PUST' little-endian: Persisted Universe STate

// The byte after the magic, in both the state and the file. Where PeekSaveFormats looks.
constexpr std::size_t FORMAT_BYTE_OFFSET = 4;

// An EntityId is a u64, which is exactly what a {slot, generation} pair cost. So identity became
// global for no bytes at all: the ship record stays 47, a fragment stays 23 ships, and the order cap
// stays 139 (ADR 0047).
constexpr std::uint32_t ENTITY_ID_BYTES = 8;

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
// localX is in [0, SECTOR_SIZE_METRES) by UniversePos's invariant, so scaling lands in [0, 65536) and
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

[[nodiscard]] LatticePos ToLattice(const UniversePos& _pos) noexcept
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
[[nodiscard]] UniversePos FromLattice(std::int32_t _sectorX, std::int32_t _sectorZ, std::uint16_t _stepX, std::uint16_t _stepZ) noexcept
{
  return UniversePos{_sectorX, _sectorZ, static_cast<float>(_stepX) * POSITION_STEP_METRES,
                     static_cast<float>(_stepZ) * POSITION_STEP_METRES};
}

// The wire's sector index is 32 bits where the simulation's is 64 -- +/-1,858 light years against
// +/-8 million (ADR 0046). Out of range saturates rather than wrapping, so the failure mode is a
// ship pinned at the edge of the addressable universe and not one that appears on the other side
// of it.
[[nodiscard]] std::int32_t ToWireSector(std::int64_t _sector) noexcept
{
  constexpr std::int64_t LOWEST = std::numeric_limits<std::int32_t>::min();
  constexpr std::int64_t HIGHEST = std::numeric_limits<std::int32_t>::max();
  return static_cast<std::int32_t>(std::clamp(_sector, LOWEST, HIGHEST));
}

// prevPos travels as a delta from posUniverse in these same steps, so the receiver reconstructs the
// quantized prevPos exactly and its only error against the true one is that position's own
// rounding. Quantizing the delta against the raw posUniverse would stack two roundings and double the
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

  void Pos(const UniversePos& _pos)
  {
    I64(_pos.sectorX);
    I64(_pos.sectorZ);
    F32(_pos.localX);
    F32(_pos.localZ);
  }

  void Entity(EntityId _entity)
  {
    U64(_entity);
  }

  void Bool(bool _value)
  {
    U8(_value ? std::uint8_t{1} : std::uint8_t{0});
  }

  // Not on the wire any more -- identity replaced it there (ADR 0047) -- but a saved universe is an
  // image of one process, and a handle is what that process's tables are keyed by.
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

  UniversePos Pos()
  {
    UniversePos pos;
    pos.sectorX = I64();
    pos.sectorZ = I64();
    pos.localX = F32();
    pos.localZ = F32();
    return pos;
  }

  EntityId Entity()
  {
    return U64();
  }

  bool Bool()
  {
    return U8() != 0;
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
  return (Neuron::MAX_DATAGRAM_BYTES - SNAPSHOT_HEADER_BYTES - FLEET_BLOCK_MAX_BYTES) / SHIP_RECORD_BYTES;
}

namespace
{
void WriteShipRecord(ByteWriter& _out, const Universe& _universe, ShipId _id)
{
  const ShipState& ship = _universe.Ships()[_id];

  // Both positions go onto the lattice before either is written, because prevPos is stated as a
  // step delta from the *quantized* posUniverse and not from the one the simulation holds.
  const LatticePos pos = ToLattice(ship.posUniverse);
  const LatticePos prev = ToLattice(ship.prevPos);

  _out.Entity(_universe.EntityIdOf(_id));
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
  // A ship is at most one of these today -- MakeStation and MakeGate are never called on the same
  // structure -- but they are separate bits rather than a kind byte, so a thing that is both is
  // expressible the day some design wants a gate you can dock at.
  std::uint8_t flags = 0;
  if (_universe.IsStation(_id))
    flags |= SHIP_FLAG_STATION;
  if (_universe.IsGate(_id))
    flags |= SHIP_FLAG_GATE;
  _out.U8(flags);
  _out.U32(ship.hullId);

  // 255ths of whole, and 255 for a hull that cannot be destroyed: an indestructible thing is
  // undamaged, which is the only honest answer and the one that keeps a station's bar from reading
  // empty. The multiply is done in 32 bits before the divide, so a Carrier's 5,200 points do not
  // overflow on their way to a byte.
  const std::uint32_t maxHullPoints = HullSpecOf(ship.hullId).maxHullPoints;
  const std::uint32_t fraction = (maxHullPoints > 0) ? (ship.hullPoints * 255u) / maxHullPoints : 255u;
  _out.U8(static_cast<std::uint8_t>(fraction));
}

// The status block: the one thing on this seam that tells a player about a fleet the interest set
// has never heard of. Four of five fleets are routinely outside it, which is the point of them.
//
// Everything here is DERIVED and nothing is held. The centroid is a readout -- the publisher
// already derives per subscriber in SplitTheLost, sits outside the replay contract, and a number
// nobody simulates against cannot desynchronize anything (Design/Archive/Fleets.md 8.2). The check on that
// claim is the save format: if a field of this block ever had to live on Fleet, UNIVERSE_STATE_FORMAT
// would have to move, and it does not.
//
// A slot is stated when its position can be DERIVED -- a live member, or a live launch structure.
// A fleet with neither is the one tick between a manifest being dropped for a dead station and the
// next tick's retire freeing the slot; clearing the bit there says the truth one tick early rather
// than stating a position that means nothing.
void WriteFleetBlock(ByteWriter& _out, const Universe& _universe, const Issuer& _viewer)
{
  UniversePos positions[FLEET_SLOTS];
  std::uint8_t kinds[FLEET_SLOTS] = {};
  std::uint8_t flags[FLEET_SLOTS] = {};
  std::uint8_t counts[FLEET_SLOTS] = {};
  std::uint8_t mask = 0;

  for (std::uint32_t slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    const Universe::FleetId id = _universe.FleetInSlot(_viewer.owner, static_cast<std::uint8_t>(slot));
    if (id == Universe::INVALID_FLEET_ID)
      continue;
    const Universe::Fleet& fleet = _universe.FleetOf(id);

    // The centroid, through OffsetX/OffsetZ from the first live member rather than by averaging the
    // fields, so it is right with a sector boundary through the middle of a fleet.
    UniversePos centre;
    bool anchored = false;
    float sumX = 0.0f;
    float sumZ = 0.0f;
    std::uint32_t live = 0;
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      const ShipId member = _universe.Resolve(fleet.members[at]);
      if (member == INVALID_SHIP_ID)
        continue;
      const UniversePos& pos = _universe.Ships()[member].posUniverse;
      if (!anchored)
      {
        centre = pos;
        anchored = true;
      }
      sumX += OffsetX(centre, pos);
      sumZ += OffsetZ(centre, pos);
      ++live;
    }

    if (anchored)
    {
      Translate(centre, sumX / static_cast<float>(live), sumZ / static_cast<float>(live));
    }
    else
    {
      // Nobody out yet: the fleet is where its door is, which is where its first hull will appear.
      const ShipId structure = _universe.Resolve(fleet.launchStructure);
      if (structure == INVALID_SHIP_ID)
        continue;
      centre = _universe.Ships()[structure].posUniverse;
    }

    // The kind and the launch are both stated, which is what splitting them bought: a fleet ordered
    // to move mid-launch is doing both, and the old byte could say only one. Which of the two a
    // reader leads with is the reader's business (UniverseView::FleetActivity).
    std::uint8_t kind = static_cast<std::uint8_t>(fleet.orderKind);
    std::uint8_t bits = 0;
    if (fleet.manifestCount != 0)
      bits |= FLEET_FLAG_LAUNCHING;

    // Engaged is the threat surviving this tick's stand-down check, which Universe has already run --
    // so the row holds a threat only while the alert, the leash and the target all still hold. The
    // two bits therefore differ exactly when the alert outlives the fight, which is what buying two
    // of them was for (Design/Archive/Fleets.md 7.2, 7.3).
    if (fleet.threat.generation != 0)
      bits |= FLEET_FLAG_ENGAGED;
    if (fleet.alertTicks > 0)
      bits |= FLEET_FLAG_UNDER_ATTACK;

    // Members in space plus manifest, so the count is the fleet's composed size throughout a launch
    // and does not climb as the hulls appear. The roster's own count is how many are out, and the
    // difference between the two is what "LAUNCHING 4 OF 8" is drawn from.
    // A byte, because ComposeFleet and FormFleet both refuse past MAX_FLEET_SHIPS and the manifest
    // only ever shrinks -- so this sum has a hard ceiling of 8 and no clamp is a guard against
    // anything reachable.
    static_assert(MAX_FLEET_SHIPS <= 0xFFu, "a fleet's size no longer fits the status block's count byte");
    const std::uint32_t total = live + fleet.manifestCount;

    mask |= static_cast<std::uint8_t>(1u << slot);
    positions[slot] = centre;
    kinds[slot] = kind;
    flags[slot] = bits;
    counts[slot] = static_cast<std::uint8_t>(total);
  }

  _out.U8(mask);
  for (std::uint32_t slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    if ((mask & (1u << slot)) == 0)
      continue;
    const LatticePos pos = ToLattice(positions[slot]);
    _out.I32(ToWireSector(pos.sectorX));
    _out.I32(ToWireSector(pos.sectorZ));
    _out.U16(pos.stepX);
    _out.U16(pos.stepZ);
    _out.U8(kinds[slot]);
    _out.U8(flags[slot]);
    // The stance, reserved. Written from a literal rather than from a variable so that the day it
    // carries something, the compiler names this line.
    _out.U8(0);
    _out.U8(counts[slot]);
  }
}
} // namespace

std::uint32_t SnapshotWriter::Write(const Universe& _universe, Neuron::Transport& _transport, const Issuer& _viewer)
{
  const std::span<const ShipState> ships = _universe.Ships();
  const std::uint32_t perFragment = ShipsPerSnapshotFragment();
  const std::uint32_t shipCount = static_cast<std::uint32_t>(ships.size());

  // An empty universe still sends one fragment. A snapshot that says "no ships" is information; the
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
    out.U8(1); // complete: this is the whole universe, so the receiver replaces rather than upserts
    out.U32(snapshotId);
    out.U32(fragment);
    out.U32(fragmentCount);
    out.U64(_universe.Tick());
    out.U32(count);
    out.U8(_universe.HostileMaskFor(_viewer.faction));
    WriteFleetBlock(out, _universe, _viewer);

    for (std::uint32_t at = 0; at < count; ++at)
      WriteShipRecord(out, _universe, static_cast<ShipId>(first + at));

    if (!_transport.Send(m_scratch.data(), static_cast<std::uint32_t>(m_scratch.size())))
      break;
    m_lastBytes += static_cast<std::uint32_t>(m_scratch.size());
    m_lastRecords += count;
    ++sent;
  }
  return sent;
}

bool SnapshotWriter::WriteFire(std::uint64_t _tick, std::span<const ShotRecord> _shots, Neuron::Transport& _transport)
{
  if (_shots.empty())
    return false; // silence is not information, and an empty message is a datagram spent on nothing

  // Over the cap the OLDEST go, which is why this counts back from the end rather than forward from
  // the start: what a player is looking at is the gunfire that just happened.
  const std::size_t taken = std::min<std::size_t>(_shots.size(), MAX_FIRE_EVENTS);
  const std::span<const ShotRecord> newest = _shots.last(taken);

  m_fireScratch.clear();
  ByteWriter out(m_fireScratch);
  out.U8(KIND_FIRE);
  out.U64(_tick);
  out.U16(static_cast<std::uint16_t>(taken));
  for (const ShotRecord& shot : newest)
  {
    out.Entity(shot.shooter);
    out.Entity(shot.victim);
    // Narrowed to a byte because MAX_MOUNTS is six. A mount index past what the shooter's hull
    // carries is a diagnostic on the far side rather than anything to check here.
    out.U8(static_cast<std::uint8_t>(shot.mount));
  }
  return _transport.Send(m_fireScratch.data(), static_cast<std::uint32_t>(m_fireScratch.size()));
}

bool SnapshotWriter::WriteLeaves(std::uint64_t _tick, std::span<const EntityId> _left, std::span<const EntityId> _destroyed,
                                 std::span<const EntityId> _docked, std::span<const EntityId> _jumped, Neuron::Transport& _transport)
{
  if (_left.empty() && _destroyed.empty() && _docked.empty() && _jumped.empty())
    return true; // nothing to state is not a failure to state it

  const std::uint32_t leaveCount = static_cast<std::uint32_t>(_left.size());
  const std::uint32_t destroyedCount = static_cast<std::uint32_t>(_destroyed.size());
  const std::uint32_t dockedCount = static_cast<std::uint32_t>(_docked.size());
  const std::uint32_t jumpedCount = static_cast<std::uint32_t>(_jumped.size());

  // One message, not fragmented. The lane's bound is what caps it, and at eight bytes a handle that
  // is 1021 departures in one update -- far past what an interest set can shed at 10 Hz, and the
  // day it is not, the answer is a second message and not a silent truncation. The third run costs
  // the bound nothing: 8192 less a 21-byte header is still 1021 handles.
  if (LEAVE_HEADER_BYTES + (leaveCount + destroyedCount + dockedCount + jumpedCount) * ENTITY_ID_BYTES > Neuron::MAX_RELIABLE_BYTES)
    return false;

  m_leaveScratch.clear();
  ByteWriter out(m_leaveScratch);
  out.U8(KIND_LEAVE);
  out.U64(_tick);
  out.U32(leaveCount);
  out.U32(destroyedCount);
  out.U32(dockedCount);
  out.U32(jumpedCount);
  for (const EntityId entity : _left)
    out.Entity(entity);
  for (const EntityId entity : _destroyed)
    out.Entity(entity);
  for (const EntityId entity : _docked)
    out.Entity(entity);
  for (const EntityId entity : _jumped)
    out.Entity(entity);

  return _transport.SendReliable(m_leaveScratch.data(), static_cast<std::uint32_t>(m_leaveScratch.size()));
}

std::uint32_t SnapshotWriter::WriteInterest(const Universe& _universe, std::span<const ShipHandle> _sent, std::span<const EntityId> _left,
                                            std::span<const EntityId> _destroyed, std::span<const EntityId> _docked,
                                            std::span<const EntityId> _jumped, Neuron::Transport& _transport, const Issuer& _viewer)
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
  if (!WriteLeaves(_universe.Tick(), _left, _destroyed, _docked, _jumped, _transport))
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
      const ShipId id = _universe.Resolve(_sent[at + record]);
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
    out.U64(_universe.Tick());
    out.U32(count);
    out.U8(_universe.HostileMaskFor(_viewer.faction));
    WriteFleetBlock(out, _universe, _viewer);

    for (const ShipId id : m_resolvedScratch)
      WriteShipRecord(out, _universe, id);

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
  if (kind == KIND_FLEET_ROSTER)
    return AcceptRoster(_datagram);
  if (kind == KIND_LEDGER_REPLY)
    return AcceptLedgerReply(_datagram);
  if (kind == KIND_FIRE)
    return AcceptFire(_datagram);
  if (kind != KIND_SNAPSHOT)
    return false;

  const bool complete = in.U8() != 0;
  const std::uint32_t snapshotId = in.U32();
  const std::uint32_t fragment = in.U32();
  const std::uint32_t fragmentCount = in.U32();
  const std::uint64_t tick = in.U64();
  const std::uint32_t count = in.U32();
  const std::uint8_t hostileMask = in.U8();

  // Read before the header is judged, because the records begin after it and a block half-read
  // would leave the cursor inside one. A slot the mask does not claim keeps whatever it had, which
  // is nothing until the mask claims it.
  const std::uint8_t fleetMask = in.U8();
  FleetStatus fleets[FLEET_SLOTS];
  for (std::uint32_t slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    if ((fleetMask & (1u << slot)) == 0)
      continue;
    const std::int32_t sectorX = in.I32();
    const std::int32_t sectorZ = in.I32();
    const std::uint16_t stepX = in.U16();
    const std::uint16_t stepZ = in.U16();
    fleets[slot].position = FromLattice(sectorX, sectorZ, stepX, stepZ);
    fleets[slot].kind = in.U8();
    fleets[slot].flags = in.U8();
    fleets[slot].stance = in.U8();
    fleets[slot].count = in.U8();
  }

  if (!in.Ok() || fragmentCount == 0 || fragment >= fragmentCount)
    return false;

  // Fragments of a snapshot older than the one already applied are of no use: the universe has moved
  // on and applying it would step the view backwards.
  if (m_hasSnapshot && tick <= m_latest.tick)
    return false;

  // Taken here rather than in Apply, and the asymmetry with the upserts is the point: an incomplete
  // update is dropped whole because half a set of records is half a universe, but a mask is not coupled
  // to any record, so taking it from whatever fragment arrives is strictly more robust. Below the
  // staleness check, so a late fragment of a superseded update cannot walk it backwards.
  m_hostileMask = hostileMask;

  // The status block takes the mask's treatment for the mask's reason: it is coupled to no record,
  // so taking it from whatever fragment arrives is strictly more robust than waiting for a whole
  // update, and robustness against loss is the entire argument for stamping it on every one. Below
  // the staleness check, so a late fragment of a superseded update cannot walk a fleet backwards.
  m_fleetMask = fleetMask;
  for (std::uint32_t slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    if ((fleetMask & (1u << slot)) != 0)
      m_fleetStatus[slot] = fleets[slot];
  }

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
    ship.entity = in.Entity();
    const std::int32_t sectorX = in.I32();
    const std::int32_t sectorZ = in.I32();
    const std::uint16_t stepX = in.U16();
    const std::uint16_t stepZ = in.U16();
    const std::int16_t prevStepX = in.I16();
    const std::int16_t prevStepZ = in.I16();
    ship.posUniverse = FromLattice(sectorX, sectorZ, stepX, stepZ);

    // Through Translate rather than by adding to the fields, so the sector carry is the simulation's
    // own and a prevPos on the far side of a border lands in the sector it belongs to. Exact: the
    // position and the delta are both multiples of the step, their sum needs 17 bits of mantissa
    // against float's 24, and the carry divides by a power of two.
    ship.prevPos = ship.posUniverse;
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
    ship.hullFraction = in.U8();
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
void SnapshotReceiver::Remove(EntityId _gone)
{
  for (std::size_t at = 0; at < m_latest.ships.size(); ++at)
  {
    if (m_latest.ships[at].entity == _gone)
    {
      // Swap-and-pop. The client's order is its own -- it is a set, not the universe's array -- and
      // UniverseView carries presentation state across by handle rather than by position.
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
std::span<const EntityId> SnapshotReceiver::RosterOf(std::uint8_t _slot) const noexcept
{
  return (_slot < FLEET_SLOTS) ? std::span<const EntityId>(m_rosters[_slot]) : std::span<const EntityId>();
}

bool SnapshotReceiver::AcceptFire(std::span<const std::uint8_t> _message)
{
  ByteReader in(_message);
  in.U8(); // the kind, already read by the dispatch
  const std::uint64_t tick = in.U64();
  const std::uint32_t count = in.U16();
  if (!in.Ok() || count > MAX_FIRE_EVENTS)
    return false;

  // Read whole before any of it is kept, so a truncated message leaves the accumulated list as it
  // was rather than half-appended -- AcceptLeaves' rule, and its reason.
  m_fireScratch.clear();
  for (std::uint32_t at = 0; at < count; ++at)
  {
    FireEvent event;
    event.shooter = in.Entity();
    event.target = in.Entity();
    event.mount = in.U8();
    m_fireScratch.push_back(event);
  }
  if (!in.Ok())
    return false;

  m_lastFireTick = tick;
  // Accumulated rather than replaced: two fire messages in one pump must not lose the first one's
  // tracers, which is why the consumer clears rather than the receiver guessing.
  m_fire.insert(m_fire.end(), m_fireScratch.begin(), m_fireScratch.end());
  return !m_fire.empty();
}

bool SnapshotReceiver::AcceptRoster(std::span<const std::uint8_t> _message)
{
  FleetRoster roster;
  if (!ReadFleetRoster(_message, roster))
    return false;

  // Replaces that slot's list whole. A roster is a statement of membership rather than a delta,
  // which is what lets a lost one be repaired by the next instead of compounding into a list that
  // is wrong in a way nothing can correct (Design/Archive/Fleets.md 8.1).
  m_rosters[roster.slot] = std::move(roster.members);
  return true;
}

bool SnapshotReceiver::AcceptLedgerReply(std::span<const std::uint8_t> _message)
{
  if (!ReadLedgerReply(_message, m_ledger))
    return false;

  // Counted, because two replies for one station are identical and a screen has no other way to
  // tell a fresh answer from the one it is already showing.
  ++m_ledgerReplies;
  return true;
}

bool SnapshotReceiver::AcceptLeaves(std::span<const std::uint8_t> _message)
{
  ByteReader in(_message);
  if (in.U8() != KIND_LEAVE)
    return false;

  const std::uint64_t tick = in.U64();
  const std::uint32_t leaveCount = in.U32();
  const std::uint32_t destroyedCount = in.U32();
  const std::uint32_t dockedCount = in.U32();
  const std::uint32_t jumpedCount = in.U32();
  if (!in.Ok())
    return false;

  // Read the whole message before touching the set: a truncated one must change nothing rather than
  // remove half of what it names.
  m_leaveScratch.clear();
  m_destroyedScratch.clear();
  m_dockedScratch.clear();
  m_jumpedScratch.clear();
  for (std::uint32_t at = 0; at < leaveCount; ++at)
    m_leaveScratch.push_back(in.Entity());
  for (std::uint32_t at = 0; at < destroyedCount; ++at)
    m_destroyedScratch.push_back(in.Entity());
  for (std::uint32_t at = 0; at < dockedCount; ++at)
    m_dockedScratch.push_back(in.Entity());
  for (std::uint32_t at = 0; at < jumpedCount; ++at)
    m_jumpedScratch.push_back(in.Entity());
  if (!in.Ok())
    return false;

  // All four leave the held set the same way. The lists differ in what they *say*, not in what they
  // do to the set, and only the client's effects care which.
  for (const EntityId gone : m_leaveScratch)
    Remove(gone);
  for (const EntityId dead : m_destroyedScratch)
    Remove(dead);
  for (const EntityId docked : m_dockedScratch)
    Remove(docked);
  for (const EntityId jumped : m_jumpedScratch)
    Remove(jumped);

  // Appended, not assigned: several of these can arrive in one drain, and every death in them is one
  // the client owes an explosion. The consumer clears it when it has drawn them.
  m_destroyed.insert(m_destroyed.end(), m_destroyedScratch.begin(), m_destroyedScratch.end());
  m_docked.insert(m_docked.end(), m_dockedScratch.begin(), m_dockedScratch.end());
  m_jumped.insert(m_jumped.end(), m_jumpedScratch.begin(), m_jumpedScratch.end());

  // The lane is ordered, so a later message cannot be overtaken by an earlier one; the tick is
  // carried for diagnostics and for the day a subscriber wants to know how stale a departure is.
  m_lastLeaveTick = tick;
  return true;
}

// Applied only once every fragment is in, never as they land: a universe left half-updated by a
// fragment that never arrived is exactly what "dropped whole" exists to prevent.
void SnapshotReceiver::Apply()
{
  // A complete snapshot IS the universe, so anything it does not carry is gone. An update carries only
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
      if (held.entity == ship.entity)
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

// --- the state codec ------------------------------------------------------------------------------
//
// Everything Step READS is written; everything Step DERIVES is rebuilt from what was. The derived
// list is the spatial index (rebuilt from prevPos on every tick anyway), the path islands (rebuilt
// here at load time, for the reason below), the neighbourhood extent, every scratch vector, and the
// two candidate counters, which are readouts and not inputs
// (Design/Archive/WorldState-work-order.md 2).

namespace
{
// One ship's simulation state, all sixteen fields -- including the six UniverseSnapshot deliberately
// withholds, which is the whole reason this codec had to exist beside it rather than reuse it.
void WriteShipState(ByteWriter& _out, const ShipState& _ship)
{
  _out.Pos(_ship.posUniverse);
  _out.F32(_ship.headingRad);
  _out.F32(_ship.speed);
  _out.F32(_ship.turnRateRadPerSec);
  _out.Pos(_ship.prevPos);
  _out.F32(_ship.prevHeading);
  _out.U8(static_cast<std::uint8_t>(_ship.order));
  _out.Pos(_ship.steerTargetPos);
  _out.F32(_ship.orderFacingRad);
  _out.Bool(_ship.orderHasFacing);
  _out.F32(_ship.orderSpeedCapMetresPerSec);
  _out.F32(_ship.avoidHeadingRad);
  _out.F32(_ship.accelSample);
  _out.U32(_ship.hullId);
  _out.U8(_ship.factionId);
  _out.U32(_ship.hullPoints);
}

void ReadShipState(ByteReader& _in, ShipState& _outShip)
{
  _outShip.posUniverse = _in.Pos();
  _outShip.headingRad = _in.F32();
  _outShip.speed = _in.F32();
  _outShip.turnRateRadPerSec = _in.F32();
  _outShip.prevPos = _in.Pos();
  _outShip.prevHeading = _in.F32();
  _outShip.order = static_cast<OrderState>(_in.U8());
  _outShip.steerTargetPos = _in.Pos();
  _outShip.orderFacingRad = _in.F32();
  _outShip.orderHasFacing = _in.Bool();
  _outShip.orderSpeedCapMetresPerSec = _in.F32();
  _outShip.avoidHeadingRad = _in.F32();
  _outShip.accelSample = _in.F32();
  _outShip.hullId = _in.U32();
  _outShip.factionId = _in.U8();
  // Clamped to what this hull can hold, which is the fail-closed direction: a file claiming an
  // invincible Interceptor is a diagnostic, and one claiming a hull point past the maximum would
  // outlive its own table the day that table is retuned (AGENTS.md 5).
  const std::uint32_t maxHullPoints = HullSpecOf(_outShip.hullId).maxHullPoints;
  _outShip.hullPoints = std::min(_in.U32(), maxHullPoints);
}
} // namespace

void WriteUniverseState(const Universe& _universe, std::vector<std::uint8_t>& _outBytes)
{
  _outBytes.clear();
  ByteWriter out(_outBytes);
  out.U32(UNIVERSE_STATE_MAGIC);
  out.U8(UNIVERSE_STATE_FORMAT);

  out.U64(_universe.m_tick);
  out.U16(_universe.m_shard);
  out.U64(_universe.m_nextEntitySerial);
  out.U64(_universe.m_despawnBase);

  // The standings, row by row and with no count: FACTION_LIMIT is a compile-time constant on both
  // ends, and a build that disagreed about it would have failed the format byte first.
  for (std::uint32_t owner = 0; owner < FACTION_LIMIT; ++owner)
  {
    for (std::uint32_t other = 0; other < FACTION_LIMIT; ++other)
      out.U8(static_cast<std::uint8_t>(_universe.m_standings.rows[owner][other]));
  }

  const std::uint32_t shipCount = static_cast<std::uint32_t>(_universe.m_ships.size());
  out.U32(shipCount);
  for (const ShipState& ship : _universe.m_ships)
    WriteShipState(out, ship);

  // The four tables parallel to m_ships, in the same order, so none of them carries a count of its
  // own: they are the same length as m_ships by construction and a length that disagreed would be a
  // defect this format cannot express.
  for (const Universe::Route& route : _universe.m_routes)
  {
    out.U32(route.count);
    // Only the live waypoints. Entries past `count` are whatever a longer route left behind, they
    // are never read, and writing them would make two universes that behave identically compare
    // unequal (work order 2.2).
    for (std::uint32_t at = 0; at < route.count; ++at)
      out.Pos(route.waypoint[at]);
    out.Pos(route.destination);
    out.Pos(route.legStart);
    // Where the target stood when a pursuit planned this route. Left out, a reloaded chase would
    // measure its quarry's drift against a default position, re-plan on its first tick and end its
    // orders on different ticks than the run that saved it -- blockedTicks' own argument, one
    // mechanism along (Universe.h, Route::pursuitAimedAt).
    out.Pos(route.pursuitAimedAt);
    out.F32(route.requiredClearanceMetres);
    out.U32(route.cursor);
    // Arrived with ADR 0042, after this codec's first branch: the ticks a ship has pushed against a
    // wall toward this route's point. Left out, a reload would reset every blocked counter and the
    // replayed universe would end orders on different ticks than the run that saved it.
    out.U32(route.blockedTicks);
    // The island it was planned against, by the key that survives a run: a lowest path cell is a
    // universe coordinate and means the same thing in every process (ADR 0059).
    out.Bool(route.stamp.scopedToIsland);
    out.I64(route.stamp.islandCellX);
    out.I64(route.stamp.islandCellZ);
    // Whether it was planned against the architecture as it stands -- not the number that says so.
    // A version is an epoch counter with no meaning outside the run that produced it, and a loaded
    // universe's islands get whatever numbers their rebuild produces (work order 2.1).
    out.Bool(_universe.m_pathIslands.IsStampCurrent(route.stamp));
    out.Bool(route.reachesDestination);
  }

  for (const Universe::Patrol& patrol : _universe.m_patrols)
  {
    out.Handle(patrol.anchor);
    out.F32(patrol.ringRadiusMetres);
    out.F32(patrol.cruiseSpeedMetresPerSec);
    out.U32(patrol.waypointIndex);
    out.Bool(patrol.active);
  }

  for (const Universe::Docking& docking : _universe.m_dockings)
  {
    out.Handle(docking.station);
    out.U64(docking.owner);
    out.Bool(docking.active);
  }

  for (const Universe::ProtectorDuty& duty : _universe.m_protectors)
  {
    out.U32(duty.home);
    out.Handle(duty.target);
    out.Bool(duty.active);
  }

  // The fifth parallel table. Every entry of every ship's block is written, including the ones past
  // its hull's own mount count: they are never read, they are held at rest, and writing them is what
  // makes a reloaded row compare equal to the row that was saved -- the fleet row's argument rather
  // than the route's, because this block is compared whole.
  for (const Universe::ShipMounts& mounts : _universe.m_mounts)
  {
    for (const Universe::MountState& mount : mounts.mount)
    {
      out.F32(mount.aimBearingRad);
      out.U32(mount.cooldownTicks);
      out.Handle(mount.target);
    }
  }

  // The slot table, which is what makes every handle in the four tables above still mean something
  // after a reload. m_shipSlot and m_entityRows are both inverses of it and are rebuilt rather than
  // written, so there is one statement of this relation in the file and not three.
  out.U32(static_cast<std::uint32_t>(_universe.m_slots.size()));
  for (const Universe::Slot& slot : _universe.m_slots)
  {
    out.U32(slot.ship);
    out.U32(slot.generation);
    out.Entity(slot.entity);
  }

  // In order: reuse is last-in-first-out and the order is the reproduction.
  out.U32(static_cast<std::uint32_t>(_universe.m_freeSlots.size()));
  for (const std::uint32_t slot : _universe.m_freeSlots)
    out.U32(slot);

  out.U32(static_cast<std::uint32_t>(_universe.m_despawnLog.size()));
  for (const DespawnRecord& record : _universe.m_despawnLog)
  {
    out.Handle(record.handle);
    out.Entity(record.entity);
    out.U8(static_cast<std::uint8_t>(record.cause));
  }

  out.U32(static_cast<std::uint32_t>(_universe.m_stations.size()));
  for (const Universe::Station& station : _universe.m_stations)
  {
    out.Handle(station.structure);
    out.U8(station.ownerFaction);
    out.U32(station.protectorHullId);
    out.U32(station.protectorComplement);
    out.U32(station.launchEveryTicks);
    out.U32(station.targetCap);
    out.U32(station.launchCooldownTicks);
    out.U32(static_cast<std::uint32_t>(station.targets.size()));
    for (const ShipHandle target : station.targets)
      out.Handle(target);
    out.U32(static_cast<std::uint32_t>(station.docked.size()));
    for (const Universe::DockedShip& docked : station.docked)
    {
      out.U32(docked.hullId);
      out.U8(docked.factionId);
      out.U64(docked.owner);
    }
  }

  // The gates. Step reads them -- the jump pass resolves a destination through this table on every
  // tick a fleet is crossing -- so the file carries them (AGENTS.md 8).
  out.U32(static_cast<std::uint32_t>(_universe.m_gates.size()));
  for (const Universe::Gate& gate : _universe.m_gates)
  {
    out.Handle(gate.structure);
    out.Entity(gate.destination);
    out.U8(gate.ownerFaction);
  }

  // The fleets. Step prunes and retires them, so they are state it reads and the file carries them
  // (AGENTS.md 8). Only the live members are written, for the reason the routes above give about
  // their waypoints: entries past the count are never read, and writing them would make two universes
  // that behave identically compare unequal.
  out.U32(static_cast<std::uint32_t>(_universe.m_fleets.size()));
  for (const Universe::Fleet& fleet : _universe.m_fleets)
  {
    out.U8(fleet.ownerFaction);
    out.U64(fleet.owner);
    out.U8(fleet.slot);
    out.U32(fleet.memberCount);
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
      out.Handle(fleet.members[at]);
    out.Handle(fleet.launchStructure);
    out.U32(fleet.manifestCount);
    for (std::uint32_t at = 0; at < fleet.manifestCount; ++at)
      out.U32(fleet.manifest[at]);
    out.U32(fleet.launchCooldownTicks);
    out.U8(static_cast<std::uint8_t>(fleet.orderKind));
    out.Pos(fleet.orderPoint);
    out.F32(fleet.orderFacingRad);
    out.Bool(fleet.orderHasFacing);
    out.Handle(fleet.orderStation);
    out.Handle(fleet.orderTarget);
    out.Handle(fleet.orderGate);
    out.Handle(fleet.threat);
    out.Pos(fleet.threatAnchorPos);
    out.U32(fleet.alertTicks);
  }
}

bool ReadUniverseState(std::span<const std::uint8_t> _bytes, Universe& _outUniverse)
{
  ByteReader in(_bytes);
  const std::uint32_t magic = in.U32();
  // Held for the whole read, because it is what every field added by a later format is gated on:
  // a field written from format N onwards is read as
  //
  //   value = (format >= N) ? in.U32() : <the value it had before it existed>;
  //
  // at the point in the stream where it lives, and nowhere else. That gate is the entire migration
  // (ADR 0061). Format 8's owner fields are the first two, below.
  const std::uint8_t format = in.U8();
  if (magic != UNIVERSE_STATE_MAGIC || format < UNIVERSE_STATE_FORMAT_OLDEST || format > UNIVERSE_STATE_FORMAT)
    return false;

  const std::uint64_t tick = in.U64();
  const ShardId shard = static_cast<ShardId>(in.U16());
  const std::uint64_t nextSerial = in.U64();
  const std::uint64_t despawnBase = in.U64();

  StandingTable standings{};
  for (std::uint32_t owner = 0; owner < FACTION_LIMIT; ++owner)
  {
    for (std::uint32_t other = 0; other < FACTION_LIMIT; ++other)
      standings.rows[owner][other] = static_cast<Standing>(in.U8());
  }
  if (!in.Ok())
    return false;

  // Everything below is read into locals and moved into _outUniverse only once the whole buffer has
  // been read and checked. That is what "fails closed" means here: a truncation on the last station
  // cannot leave a universe half replaced, with nothing saying which half (AGENTS.md 5).
  //
  // Every count is checked against what is actually left before it is used to size anything, so a
  // hostile or corrupt length asks for one allocation of a bounded size rather than a huge one. The
  // smallest record in this format is a byte, so Remaining() is a sound bound on every count.
  const std::uint32_t shipCount = in.U32();
  if (!in.Ok() || shipCount > in.Remaining())
    return false;

  std::vector<ShipState> ships(shipCount);
  for (ShipState& ship : ships)
    ReadShipState(in, ship);

  std::vector<Universe::Route> routes(shipCount);
  for (Universe::Route& route : routes)
  {
    route.count = in.U32();
    if (!in.Ok() || route.count > MAX_PATH_WAYPOINTS)
      return false;
    for (std::uint32_t at = 0; at < route.count; ++at)
      route.waypoint[at] = in.Pos();
    route.destination = in.Pos();
    route.legStart = in.Pos();
    route.pursuitAimedAt = in.Pos();
    route.requiredClearanceMetres = in.F32();
    route.cursor = in.U32();
    route.blockedTicks = in.U32();
    // The key is read as it stands, because it means the same thing here. The currency is held as
    // the relation it was written as; the number it becomes is filled in below, once the islands
    // this universe will actually route against have been rebuilt.
    route.stamp.scopedToIsland = in.Bool();
    route.stamp.islandCellX = in.I64();
    route.stamp.islandCellZ = in.I64();
    route.stamp.version = in.Bool() ? 1u : 0u;
    route.reachesDestination = in.Bool();
    if (!in.Ok() || route.cursor > route.count)
      return false;
  }

  std::vector<Universe::Patrol> patrols(shipCount);
  for (Universe::Patrol& patrol : patrols)
  {
    patrol.anchor = in.Handle();
    patrol.ringRadiusMetres = in.F32();
    patrol.cruiseSpeedMetresPerSec = in.F32();
    patrol.waypointIndex = in.U32();
    patrol.active = in.Bool();
  }

  std::vector<Universe::Docking> dockings(shipCount);
  for (Universe::Docking& docking : dockings)
  {
    docking.station = in.Handle();
    // Format 8, on the fleet row's terms: a docking in flight when an older file was written was
    // the player's if anybody's, because nothing else in that build could issue one.
    docking.owner = (format >= 8) ? in.U64() : OWNER_LOCAL;
    docking.active = in.Bool();
  }

  std::vector<Universe::ProtectorDuty> protectors(shipCount);
  for (Universe::ProtectorDuty& duty : protectors)
  {
    duty.home = in.U32();
    duty.target = in.Handle();
    duty.active = in.Bool();
  }

  std::vector<Universe::ShipMounts> mounts(shipCount);
  for (std::uint32_t id = 0; id < shipCount; ++id)
  {
    const HullSpec& hull = HullSpecOf(ships[id].hullId);
    for (std::uint32_t at = 0; at < MAX_MOUNTS; ++at)
    {
      Universe::MountState& mount = mounts[id].mount[at];
      mount.aimBearingRad = in.F32();
      // Clamped to what the device can actually hold, for the hull points' reason: a file claiming a
      // gun that never reloads is a diagnostic, not a crash. A mount past this hull's own count has
      // no device to ask, and is held at rest.
      const std::uint32_t cooldownTicks = in.U32();
      mount.cooldownTicks =
        (at < hull.MountCount()) ? std::min(cooldownTicks, DeviceSpecOf(hull.loadout.mount[at].device).cooldownTicks) : 0;
      mount.target = in.Handle();
    }
  }
  if (!in.Ok())
    return false;

  const std::uint32_t slotCount = in.U32();
  if (!in.Ok() || slotCount > in.Remaining())
    return false;
  std::vector<Universe::Slot> slots(slotCount);
  for (Universe::Slot& slot : slots)
  {
    slot.ship = in.U32();
    slot.generation = in.U32();
    slot.entity = in.Entity();
    // A slot naming a ship that is not there would make HandleOf and Resolve disagree, which is the
    // one corruption that shows up as a ship steering somebody else's order rather than as a crash.
    if (!in.Ok() || (slot.ship != INVALID_SHIP_ID && slot.ship >= shipCount))
      return false;
  }

  const std::uint32_t freeCount = in.U32();
  if (!in.Ok() || freeCount > in.Remaining())
    return false;
  std::vector<std::uint32_t> freeSlots(freeCount);
  for (std::uint32_t& slot : freeSlots)
  {
    slot = in.U32();
    if (!in.Ok() || slot >= slotCount)
      return false;
  }

  const std::uint32_t despawnCount = in.U32();
  if (!in.Ok() || despawnCount > in.Remaining())
    return false;
  std::vector<DespawnRecord> despawnLog(despawnCount);
  for (DespawnRecord& record : despawnLog)
  {
    record.handle = in.Handle();
    record.entity = in.Entity();
    record.cause = static_cast<DespawnCause>(in.U8());
  }

  const std::uint32_t stationCount = in.U32();
  if (!in.Ok() || stationCount > in.Remaining())
    return false;
  std::vector<Universe::Station> stations(stationCount);
  for (Universe::Station& station : stations)
  {
    station.structure = in.Handle();
    station.ownerFaction = in.U8();
    station.protectorHullId = in.U32();
    station.protectorComplement = in.U32();
    station.launchEveryTicks = in.U32();
    station.targetCap = in.U32();
    station.launchCooldownTicks = in.U32();

    const std::uint32_t targetCount = in.U32();
    if (!in.Ok() || targetCount > in.Remaining())
      return false;
    station.targets.resize(targetCount);
    for (ShipHandle& target : station.targets)
      target = in.Handle();

    const std::uint32_t dockedCount = in.U32();
    if (!in.Ok() || dockedCount > in.Remaining())
      return false;
    station.docked.resize(dockedCount);
    for (Universe::DockedShip& docked : station.docked)
    {
      docked.hullId = in.U32();
      docked.factionId = in.U8();
      // Format 8 added it. A file written before it has rows that meant "the player's" when the
      // faction was the player's and "somebody else's" otherwise, and that is what they come back
      // as -- the row is the same row, read by a build that can now say more about it (ADR 0061).
      docked.owner = (format >= 8) ? in.U64() : ((docked.factionId == FACTION_PLAYER) ? OWNER_LOCAL : OWNER_NOBODY);
    }
  }

  // The gates. Bounded against what is left in the buffer rather than by a count of its own: a gate
  // row is thirteen bytes, so a count past Remaining() is a corrupt file however large the universe.
  const std::uint32_t gateCount = in.U32();
  if (!in.Ok() || gateCount > in.Remaining())
    return false;

  std::vector<Universe::Gate> gates(gateCount);
  for (Universe::Gate& gate : gates)
  {
    gate.structure = in.Handle();
    gate.destination = in.Entity();
    gate.ownerFaction = in.U8();
    // The destination is deliberately not checked against this universe's entities. A gate whose far
    // side is not here is exactly what a half-loaded shard looks like, and the jump pass already
    // refuses to move anybody through one -- so the fail-closed behaviour is the pass's, and a check
    // here would turn a recoverable universe into a refused load (Design/Universe-slice-2.md 4.6).
    if (!in.Ok() || gate.ownerFaction >= FACTION_LIMIT)
      return false;
  }

  // Bounded against what is left in the buffer rather than by an exact ceiling. It was
  // FACTION_LIMIT * FLEET_SLOTS -- every slot of every faction -- and that was exact only while a
  // fleet's owner was a faction. Owners are u64 and unbounded by design (Design/OwnerKey-work-order.md),
  // so the most fleets a universe may hold is the most a player base may have, and the honest bound
  // is the one every other count in this codec uses: a fleet row is more than a byte, so a count
  // past Remaining() is a corrupt file however many players a shard carries.
  const std::uint32_t fleetCount = in.U32();
  if (!in.Ok() || fleetCount > in.Remaining())
    return false;

  // Two live fleets claiming one slot would make FleetInSlot's answer depend on which row came
  // first, which is an invariant corrupted rather than a value -- the same kind of defect as a slot
  // naming a ship that is not there, and refused for the same reason.
  std::vector<Universe::Fleet> fleets(fleetCount);

  // (owner, slot) pairs, checked for duplicates after the read rather than during it. The array of
  // flags this replaces was indexed by faction, which cannot hold an owner: a u64 has no array to
  // be a subscript of, and the pairs are collected and sorted instead. O(n log n) rather than the
  // O(n^2) a pairwise sweep would cost on a file claiming a large count.
  std::vector<std::uint64_t> claims;
  claims.reserve(fleetCount);

  for (Universe::Fleet& fleet : fleets)
  {
    fleet.ownerFaction = in.U8();
    fleet.owner = (format >= 8) ? in.U64() : ((fleet.ownerFaction == FACTION_PLAYER) ? OWNER_LOCAL : OWNER_NOBODY);
    fleet.slot = in.U8();
    fleet.memberCount = in.U32();
    if (!in.Ok() || fleet.ownerFaction >= FACTION_LIMIT || fleet.slot >= FLEET_SLOTS || fleet.memberCount > MAX_FLEET_SHIPS)
      return false;
    // Packed rather than paired, so the sort below needs no comparator: FLEET_SLOTS is 5 and a slot
    // is already bounded above, so the low three bits hold it and the owner keeps the rest. An owner
    // past 2^61 would collide, and no minting scheme in this tree can reach one.
    static_assert(FLEET_SLOTS <= 8, "the slot no longer fits the low three bits of a fleet claim");
    claims.push_back((fleet.owner << 3) | fleet.slot);
    // The handles themselves are not checked against the slot table, and deliberately: Resolve
    // bounds-checks a slot and compares a generation, so a handle this file invented resolves to
    // nothing and the fleet pass prunes it on the first tick. That is the fail-closed direction
    // already, and a second check here would only turn a self-repairing universe into a refused load.
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
      fleet.members[at] = in.Handle();

    fleet.launchStructure = in.Handle();
    fleet.manifestCount = in.U32();
    // The invariant ComposeFleet establishes and every launch preserves. A file that broke it would
    // launch a ship into a row with nowhere to put it, which is memory past the end of a member
    // array rather than a wrong number.
    if (!in.Ok() || fleet.manifestCount > MAX_FLEET_SHIPS || fleet.memberCount + fleet.manifestCount > MAX_FLEET_SHIPS)
      return false;
    for (std::uint32_t at = 0; at < fleet.manifestCount; ++at)
      fleet.manifest[at] = in.U32();
    fleet.launchCooldownTicks = in.U32();

    const std::uint8_t orderKind = in.U8();
    if (!in.Ok() || orderKind > static_cast<std::uint8_t>(FleetOrderKind::Jump))
      return false;
    fleet.orderKind = static_cast<FleetOrderKind>(orderKind);
    fleet.orderPoint = in.Pos();
    fleet.orderFacingRad = in.F32();
    fleet.orderHasFacing = in.Bool();
    fleet.orderStation = in.Handle();
    fleet.orderTarget = in.Handle();
    fleet.orderGate = in.Handle();
    // Nothing bounds the threat: a handle that resolves to nothing is a fleet that stands down on
    // its first tick, which is the fail-closed direction already.
    fleet.threat = in.Handle();
    fleet.threatAnchorPos = in.Pos();
    fleet.alertTicks = in.U32();
  }

  // Two live fleets claiming one owner's slot would make FleetInSlot's answer depend on which row
  // came first, which is an invariant corrupted rather than a value -- the same kind of defect as a
  // slot naming a ship that is not there, and refused for the same reason. Sorted and swept for
  // adjacent equals, which is the whole check.
  std::sort(claims.begin(), claims.end());
  for (std::size_t at = 1; at < claims.size(); ++at)
  {
    if (claims[at] == claims[at - 1])
      return false;
  }

  if (!in.Ok())
    return false;

  // Committed, and not before. From here nothing can fail.
  _outUniverse.m_tick = tick;
  _outUniverse.m_shard = shard;
  _outUniverse.m_nextEntitySerial = nextSerial;
  _outUniverse.m_despawnBase = despawnBase;
  _outUniverse.m_standings = standings;
  _outUniverse.m_ships = std::move(ships);
  _outUniverse.m_routes = std::move(routes);
  _outUniverse.m_patrols = std::move(patrols);
  _outUniverse.m_dockings = std::move(dockings);
  _outUniverse.m_protectors = std::move(protectors);
  _outUniverse.m_mounts = std::move(mounts);
  _outUniverse.m_slots = std::move(slots);
  _outUniverse.m_freeSlots = std::move(freeSlots);
  _outUniverse.m_despawnLog = std::move(despawnLog);
  _outUniverse.m_stations = std::move(stations);
  _outUniverse.m_gates = std::move(gates);
  _outUniverse.m_fleets = std::move(fleets);

  // The two inverses of the slot table, rebuilt rather than read. m_entityRows is sorted by id
  // because that is the invariant its binary search rests on, and building it by walking the slots
  // in order and sorting once is cheaper and harder to get wrong than inserting one at a time.
  _outUniverse.m_shipSlot.assign(_outUniverse.m_ships.size(), 0);
  _outUniverse.m_entityRows.clear();
  for (std::uint32_t slot = 0; slot < _outUniverse.m_slots.size(); ++slot)
  {
    const Universe::Slot& entry = _outUniverse.m_slots[slot];
    if (entry.ship == INVALID_SHIP_ID)
      continue;
    _outUniverse.m_shipSlot[entry.ship] = slot;
    _outUniverse.m_entityRows.push_back(Universe::EntityRow{entry.entity, slot});
  }
  std::sort(_outUniverse.m_entityRows.begin(), _outUniverse.m_entityRows.end(),
            [](const Universe::EntityRow& _a, const Universe::EntityRow& _b) { return _a.entity < _b.entity; });

  // The architecture, rebuilt here rather than on the first Step. It has to be here: the routes
  // above were written as "current" or "stale" and the number that says which is whatever this
  // rebuild produces, so a rebuild that happened one tick later would bump the version under every
  // route just marked current and re-plan the lot (work order 2.1).
  _outUniverse.m_staticIndexDirty = true;
  _outUniverse.RebuildStaticIfDirty();

  const std::uint32_t currentVersion = _outUniverse.m_pathIslands.Version();
  for (Universe::Route& route : _outUniverse.m_routes)
  {
    if (route.stamp.version == 0)
    {
      // It was already stale when it was written, and it stays stale: unscoped against a version
      // that cannot be the current one, so the next tick re-plans it exactly as the saving run's
      // next tick would have.
      route.stamp.scopedToIsland = false;
      route.stamp.version = currentVersion - 1u;
      continue;
    }

    if (!route.stamp.scopedToIsland)
    {
      route.stamp.version = currentVersion;
      continue;
    }

    // It was current against the island with this key, and the same obstacles partition the same
    // way, so that island is here. NO_ISLAND is the fail-closed answer if it somehow is not: an
    // unscoped stale stamp, which re-plans, rather than a number that might match by accident.
    const std::uint32_t version = _outUniverse.m_pathIslands.IslandVersion(route.stamp.islandCellX, route.stamp.islandCellZ);
    if (version == PathIslands::NO_ISLAND)
    {
      route.stamp.scopedToIsland = false;
      route.stamp.version = currentVersion - 1u;
      continue;
    }
    route.stamp.version = version;
  }

  return true;
}

void WriteSaveFile(const Universe& _universe, const SaveHeader& _header, std::vector<std::uint8_t>& _outBytes)
{
  // The state first, into a scratch, because the header carries its length and the length is not
  // known until it exists. One copy of the state, not two: the header is emitted into _outBytes and
  // the state appended to it.
  std::vector<std::uint8_t> state;
  WriteUniverseState(_universe, state);

  _outBytes.clear();
  _outBytes.reserve(SAVE_HEADER_BYTES + state.size());
  ByteWriter out(_outBytes);
  out.U32(SAVE_FILE_MAGIC);
  out.U8(SAVE_FILE_FORMAT);
  out.U64(_header.galaxySeed);
  out.U16(static_cast<std::uint16_t>(_header.shard));
  out.U64(static_cast<std::uint64_t>(state.size()));
  // The header is exactly as long as the constant says. A field added above without moving the
  // constant would put the state at an offset the reader does not look at, and the reader's own
  // arithmetic would then be measured against the wrong place -- so it is asserted here, at compile
  // time, rather than discovered in a file somebody cannot load.
  static_assert(SAVE_HEADER_BYTES == 4u + 1u + 8u + 2u + 8u, "the save header's length has drifted from its fields");

  _outBytes.insert(_outBytes.end(), state.begin(), state.end());
}

bool ReadSaveFile(std::span<const std::uint8_t> _bytes, SaveHeader& _outHeader, Universe& _outUniverse)
{
  ByteReader in(_bytes);
  const std::uint32_t magic = in.U32();
  const std::uint8_t fileFormat = in.U8();
  if (magic != SAVE_FILE_MAGIC || fileFormat < SAVE_FILE_FORMAT_OLDEST || fileFormat > SAVE_FILE_FORMAT)
    return false;

  SaveHeader header;
  header.fileFormat = fileFormat;
  header.galaxySeed = in.U64();
  header.shard = static_cast<ShardId>(in.U16());
  const std::uint64_t stateBytes = in.U64();
  if (!in.Ok())
    return false;

  // The length must account for the file exactly. Short is a torn write; long is a file with
  // something appended, which the state codec would not notice because it stops at the end of the
  // state. Compared against the buffer rather than trusted, and computed in uint64 so a length near
  // the top of the range cannot wrap the addition into a small number that happens to match.
  if (stateBytes != static_cast<std::uint64_t>(_bytes.size()) - static_cast<std::uint64_t>(SAVE_HEADER_BYTES))
    return false;

  // Into a local universe, not the caller's, for ReadUniverseState's own reason one level up: a
  // refusal at the LAST check below -- the shard cross-check -- must leave the caller holding the
  // universe it had, and a read straight into _outUniverse could not offer that.
  Universe loaded;
  if (!ReadUniverseState(_bytes.subspan(SAVE_HEADER_BYTES), loaded))
    return false;

  // The one thing the header claims that the body can contradict. A file that disagrees with itself
  // is refused rather than reconciled: picking either answer would mean shipping the shard that was
  // not saved, and there is no way to tell which of the two is the mistake.
  if (loaded.Shard() != header.shard)
    return false;

  // The state codec has already checked this byte's magic and window, so it is read as a fact here
  // rather than parsed a second time.
  header.stateFormat = _bytes[SAVE_HEADER_BYTES + FORMAT_BYTE_OFFSET];

  _outHeader = header;
  _outUniverse = std::move(loaded);
  return true;
}

bool PeekSaveFormats(std::span<const std::uint8_t> _bytes, std::uint8_t& _outFileFormat, std::uint8_t& _outStateFormat)
{
  if (_bytes.size() <= SAVE_HEADER_BYTES + FORMAT_BYTE_OFFSET)
    return false;
  ByteReader file(_bytes);
  if (file.U32() != SAVE_FILE_MAGIC)
    return false;
  ByteReader state(_bytes.subspan(SAVE_HEADER_BYTES));
  if (state.U32() != UNIVERSE_STATE_MAGIC)
    return false;
  _outFileFormat = _bytes[FORMAT_BYTE_OFFSET];
  _outStateFormat = _bytes[SAVE_HEADER_BYTES + FORMAT_BYTE_OFFSET];
  return true;
}

std::wstring UniverseSaveSidecarName(std::uint8_t _stateFormat)
{
  return std::wstring(UNIVERSE_SAVE_FILE) + L"." + std::to_wstring(static_cast<unsigned>(_stateFormat));
}

// kind, orderId, station handle, handleCount -- 17 bytes, against the move order's 38.
bool WriteFleetOrder(const FleetOrder& _order, Neuron::Transport& _transport)
{
  if (_order.slot >= FLEET_SLOTS || _order.kind > FleetOrderKind::Jump)
    return false;

  std::vector<std::uint8_t> bytes;
  ByteWriter out(bytes);
  out.U8(KIND_FLEET_ORDER);
  out.U32(0); // order id, reserved: nothing acknowledges an order yet
  out.U8(_order.slot);
  out.U8(static_cast<std::uint8_t>(_order.kind));
  out.U8(_order.hasFacing ? 1u : 0u);
  out.F32(_order.facingRad);
  out.Pos(_order.point);
  out.Entity(_order.station);
  out.Entity(_order.target);
  out.Entity(_order.gate);
  return _transport.SendReliable(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

bool ReadFleetOrder(std::span<const std::uint8_t> _datagram, FleetOrder& _outOrder)
{
  ByteReader in(_datagram);
  if (in.U8() != KIND_FLEET_ORDER)
    return false;

  (void)in.U32(); // order id
  const std::uint8_t slot = in.U8();
  const std::uint8_t kind = in.U8();
  const bool hasFacing = in.U8() != 0;
  const float facingRad = in.F32();
  const UniversePos point = in.Pos();
  const EntityId station = in.Entity();
  const EntityId target = in.Entity();
  const EntityId gate = in.Entity();
  if (!in.Ok() || slot >= FLEET_SLOTS || kind > static_cast<std::uint8_t>(FleetOrderKind::Jump))
    return false;

  _outOrder.slot = slot;
  _outOrder.kind = static_cast<FleetOrderKind>(kind);
  _outOrder.point = point;
  _outOrder.facingRad = facingRad;
  _outOrder.hasFacing = hasFacing;
  _outOrder.station = station;
  _outOrder.target = target;
  _outOrder.gate = gate;
  return true;
}

bool WriteFleetRoster(const FleetRoster& _roster, Neuron::Transport& _transport)
{
  if (_roster.slot >= FLEET_SLOTS || _roster.members.size() > MAX_FLEET_SHIPS)
    return false;

  std::vector<std::uint8_t> bytes;
  ByteWriter out(bytes);
  out.U8(KIND_FLEET_ROSTER);
  out.U8(_roster.slot);
  out.U8(static_cast<std::uint8_t>(_roster.members.size()));
  for (const EntityId member : _roster.members)
    out.Entity(member);
  return _transport.SendReliable(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

bool ReadFleetRoster(std::span<const std::uint8_t> _message, FleetRoster& _outRoster)
{
  ByteReader in(_message);
  if (in.U8() != KIND_FLEET_ROSTER)
    return false;

  const std::uint8_t slot = in.U8();
  const std::uint8_t count = in.U8();
  if (!in.Ok() || slot >= FLEET_SLOTS || count > MAX_FLEET_SHIPS)
    return false;

  // Into a local and copied out on success, so a refused read leaves the caller's roster as it was
  // rather than half replaced -- which for a roster is the difference between a stale membership
  // and an invented one.
  std::vector<EntityId> members;
  members.reserve(count);
  for (std::uint8_t at = 0; at < count; ++at)
    members.push_back(in.Entity());
  if (!in.Ok())
    return false;

  _outRoster.slot = slot;
  _outRoster.members = std::move(members);
  return true;
}

bool WriteLedgerRequest(const LedgerRequest& _request, Neuron::Transport& _transport)
{
  std::vector<std::uint8_t> bytes;
  ByteWriter out(bytes);
  out.U8(KIND_LEDGER_REQUEST);
  out.Entity(_request.station);
  return _transport.SendReliable(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

bool ReadLedgerRequest(std::span<const std::uint8_t> _message, LedgerRequest& _outRequest)
{
  ByteReader in(_message);
  if (in.U8() != KIND_LEDGER_REQUEST)
    return false;

  const EntityId station = in.Entity();
  if (!in.Ok())
    return false;

  // No faction field, and its absence is the design: whose rows are counted is the subscriber's own
  // faction, which the server holds and a client cannot state (ADR 0051).
  _outRequest.station = station;
  return true;
}

bool WriteLedgerReply(const LedgerReply& _reply, Neuron::Transport& _transport)
{
  std::vector<std::uint8_t> bytes;
  ByteWriter out(bytes);
  out.U8(KIND_LEDGER_REPLY);
  out.Entity(_reply.station);

  // The hull table's own size, on the wire. It is what makes a reader from a build with a different
  // table refuse rather than read one row's count as another's -- a misread that would show a
  // player a ledger of the wrong hulls and let them compose from it.
  out.U8(static_cast<std::uint8_t>(HULL_COUNT));
  for (std::uint32_t hull = 0; hull < HULL_COUNT; ++hull)
    out.U32(_reply.hullCounts[hull]);
  return _transport.SendReliable(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

bool ReadLedgerReply(std::span<const std::uint8_t> _message, LedgerReply& _outReply)
{
  ByteReader in(_message);
  if (in.U8() != KIND_LEDGER_REPLY)
    return false;

  const EntityId station = in.Entity();
  const std::uint8_t hulls = in.U8();
  if (!in.Ok() || hulls != HULL_COUNT)
    return false;

  std::uint32_t counts[HULL_COUNT] = {};
  for (std::uint32_t hull = 0; hull < HULL_COUNT; ++hull)
    counts[hull] = in.U32();
  if (!in.Ok())
    return false;

  _outReply.station = station;
  for (std::uint32_t hull = 0; hull < HULL_COUNT; ++hull)
    _outReply.hullCounts[hull] = counts[hull];
  return true;
}

bool WriteComposeOrder(const ComposeOrder& _order, Neuron::Transport& _transport)
{
  if (_order.slot >= FLEET_SLOTS)
    return false;

  std::vector<std::uint8_t> bytes;
  ByteWriter out(bytes);
  out.U8(KIND_COMPOSE_ORDER);
  out.U32(0); // order id, reserved: nothing acknowledges an order yet
  out.Entity(_order.station);
  out.U8(_order.slot);
  out.U8(static_cast<std::uint8_t>(HULL_COUNT));
  for (std::uint32_t hull = 0; hull < HULL_COUNT; ++hull)
    out.U32(_order.hullCounts[hull]);
  return _transport.SendReliable(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

bool ReadComposeOrder(std::span<const std::uint8_t> _message, ComposeOrder& _outOrder)
{
  ByteReader in(_message);
  if (in.U8() != KIND_COMPOSE_ORDER)
    return false;

  (void)in.U32(); // order id
  const EntityId station = in.Entity();
  const std::uint8_t slot = in.U8();
  const std::uint8_t hulls = in.U8();
  if (!in.Ok() || slot >= FLEET_SLOTS || hulls != HULL_COUNT)
    return false;

  std::uint32_t counts[HULL_COUNT] = {};
  for (std::uint32_t hull = 0; hull < HULL_COUNT; ++hull)
    counts[hull] = in.U32();
  if (!in.Ok())
    return false;

  // No size gate here, and deliberately: how many ships a fleet may hold is ComposeFleet's rule,
  // and a codec that enforced it too would be a second copy of it to keep in step (ADR 0014).
  _outOrder.station = station;
  _outOrder.slot = slot;
  for (std::uint32_t hull = 0; hull < HULL_COUNT; ++hull)
    _outOrder.hullCounts[hull] = counts[hull];
  return true;
}
} // namespace Game
