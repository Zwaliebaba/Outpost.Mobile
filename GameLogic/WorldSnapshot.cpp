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
// (Design/Archive/MmoScalabilityReview.md E1, Design/Archive/ReliableFormat-work-order.md). Duplicating them onto
// both lanes was the alternative and lost -- two paths carrying the same fact is two paths to
// reason about, and the unreliable copy would still be the one that arrived first.
constexpr std::uint8_t KIND_LEAVE = 3;

// The dock order, beside the move order. A second kind rather than a flag on the first, because the
// two carry different payloads -- a station handle against a destination and a facing -- and a
// discriminated kind is what the format already uses to say so.
constexpr std::uint8_t KIND_DOCK_ORDER = 4;

// An order that names a fleet rather than the ships in it (ADR 0049). It is the smallest message on
// this lane and the only one whose size does not depend on how many ships it moves.
constexpr std::uint8_t KIND_FLEET_ORDER = 5;

// Who is in a fleet, downward and reliably. It is not in the ship record and never will be:
// a record is per-update and membership changes at human speed, so the roster is the delta and the
// record stays 47 bytes (Design/Fleets.md 8.1).
constexpr std::uint8_t KIND_FLEET_ROSTER = 6;

// The one request/reply pair on this seam. A station's ledger is large, private, slow-changing and
// wanted by exactly one client at exactly one moment -- broadcasting it would put every station's
// contents on every wire ten times a second for a screen nobody has open (ADR 0051).
constexpr std::uint8_t KIND_LEDGER_REQUEST = 7;
constexpr std::uint8_t KIND_LEDGER_REPLY = 8;

// A draft, upward. The fourth order kind and the only one that names no ship.
constexpr std::uint8_t KIND_COMPOSE_ORDER = 9;

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
// and three tests agree on, which is the trade MaxShipsPerOrder already argues for below. It costs
// one record a fragment: 22 rather than 23 (Design/Fleets-slice-5.md 2.2).
constexpr std::uint32_t FLEET_STATUS_BYTES = 4 + 4 + 2 + 2 + 1 + 1;
constexpr std::uint32_t FLEET_BLOCK_MAX_BYTES = 1 + FLEET_SLOTS * FLEET_STATUS_BYTES;

// kind, tick, leaveCount, destroyedCount, dockedCount
//
// The docked handles ride this message and not the snapshot header, and Design/Archive/Stations.md 7.4 says
// otherwise only because it predates ADR 0029 moving departures onto the reliable lane. That ADR's
// argument covers a docking exactly: a snapshot is superseded by the next one and heals itself, a
// departure is stated once, and a lost "it docked" is a ghost ship for the rest of the match. So
// ShipsPerSnapshotFragment does *not* follow this header -- it derives from SNAPSHOT_HEADER_BYTES,
// which a docking never touches (Design/Archive/Stations-slice-3.md 2.1, ADR 0040).
constexpr std::uint32_t LEAVE_HEADER_BYTES = 1 + 8 + 4 + 4 + 4;
// handle, the sector pair, the local offsets, the prevPos delta, two angles, three floats, order,
// faction, flags, hullId.
//
// 47 bytes, from 83. What bought the 36 is below: positions moved onto a 0.125 m lattice, the
// sector pair narrowed from i64 to i32 (ADR 0046), prevPos became a delta against posWorld rather
// than a second whole position, and the two angles became turns16. At 1,152 bytes a datagram less a
// 27-byte header that is 23 ship records per fragment against the 13 this replaces -- so a
// hundred-ship update is 5 fragments instead of 8, which is what finding E1 cares about: at 2%
// datagram loss it completes 90% of the time rather than 85% (Design/Archive/QuantizedWire-work-order.md).
constexpr std::uint32_t SHIP_RECORD_BYTES = 8 + 8 + 4 + 4 + 4 + 12 + 1 + 1 + 1 + 4;
// kind, orderId, hasFacing, facingRad, destination, handleCount
constexpr std::uint32_t ORDER_HEADER_BYTES = 1 + 4 + 1 + 4 + 24 + 4;
// The saved state's magic and format byte. Not a KIND_* value: a state buffer is not a datagram and
// never meets SnapshotReceiver::Accept, so the magic is what turns feeding it to one into a refusal
// rather than a misread. The format byte is what makes a disagreement between two builds a refusal
// too -- there is nothing to migrate from yet, and a version nobody checks is a version nobody has.
constexpr std::uint32_t WORLD_STATE_MAGIC = 0x54535750u; // 'PWST' little-endian: Persisted World STate
constexpr std::uint8_t WORLD_STATE_FORMAT = 5;           // 2: the fleet table. 3: its manifest. 4: its order. 5: its threat

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
// +/-8 million (ADR 0046). Out of range saturates rather than wrapping, so the failure mode is a
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

  void Entity(EntityId _entity)
  {
    U64(_entity);
  }

  void Bool(bool _value)
  {
    U8(_value ? std::uint8_t{1} : std::uint8_t{0});
  }

  // Not on the wire any more -- identity replaced it there (ADR 0047) -- but a saved world is an
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

  WorldPos Pos()
  {
    WorldPos pos;
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

// Still derived from the datagram bound, though an order now travels on the reliable lane and could
// be MAX_RELIABLE_BYTES long (ADR 0029). Keeping the smaller cap is deliberate: it is the number
// every existing test and the client's selection logic already agree on, and raising it is a wire
// change with nobody asking for it. The day a formation of more than this many ships is orderable,
// it moves -- and it moves as its own slice, because the cap is what stops one click from becoming
// an unbounded amount of pathfinding.
std::uint32_t MaxShipsPerOrder() noexcept
{
  return (Neuron::MAX_DATAGRAM_BYTES - ORDER_HEADER_BYTES) / ENTITY_ID_BYTES;
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

  _out.Entity(_world.EntityIdOf(_id));
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

// The status block: the one thing on this seam that tells a player about a fleet the interest set
// has never heard of. Four of five fleets are routinely outside it, which is the point of them.
//
// Everything here is DERIVED and nothing is held. The centroid is a readout -- the publisher
// already derives per subscriber in SplitTheLost, sits outside the replay contract, and a number
// nobody simulates against cannot desynchronize anything (Design/Fleets.md 8.2). The check on that
// claim is the save format: if a field of this block ever had to live on Fleet, WORLD_STATE_FORMAT
// would have to move, and it does not.
//
// A slot is stated when its position can be DERIVED -- a live member, or a live launch structure.
// A fleet with neither is the one tick between a manifest being dropped for a dead station and the
// next tick's retire freeing the slot; clearing the bit there says the truth one tick early rather
// than stating a position that means nothing.
void WriteFleetBlock(ByteWriter& _out, const World& _world, FactionId _viewer)
{
  WorldPos positions[FLEET_SLOTS];
  std::uint8_t statuses[FLEET_SLOTS] = {};
  std::uint8_t counts[FLEET_SLOTS] = {};
  std::uint8_t mask = 0;

  for (std::uint32_t slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    const World::FleetId id = _world.FleetInSlot(_viewer, static_cast<std::uint8_t>(slot));
    if (id == World::INVALID_FLEET_ID)
      continue;
    const World::Fleet& fleet = _world.FleetOf(id);

    // The centroid, through OffsetX/OffsetZ from the first live member rather than by averaging the
    // fields, so it is right with a sector boundary through the middle of a fleet.
    WorldPos centre;
    bool anchored = false;
    float sumX = 0.0f;
    float sumZ = 0.0f;
    std::uint32_t live = 0;
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      const ShipId member = _world.Resolve(fleet.members[at]);
      if (member == INVALID_SHIP_ID)
        continue;
      const WorldPos& pos = _world.Ships()[member].posWorld;
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
      const ShipId structure = _world.Resolve(fleet.launchStructure);
      if (structure == INVALID_SHIP_ID)
        continue;
      centre = _world.Ships()[structure].posWorld;
    }

    // Launching outranks the standing order in what is SHOWN, not in what is done: a fleet ordered
    // to move mid-launch is both, and the launch is the fact the player has no other way to see.
    std::uint8_t status = (fleet.manifestCount != 0) ? FLEET_STATUS_LAUNCHING : static_cast<std::uint8_t>(fleet.orderKind);

    // Engaged is the threat surviving this tick's stand-down check, which World has already run --
    // so the row holds a threat only while the alert, the leash and the target all still hold. The
    // two bits therefore differ exactly when the alert outlives the fight, which is what buying two
    // of them was for (Design/Fleets.md 7.2, 7.3).
    if (fleet.threat.generation != 0)
      status |= FLEET_STATUS_ENGAGED;
    if (fleet.alertTicks > 0)
      status |= FLEET_STATUS_UNDER_ATTACK;

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
    statuses[slot] = status;
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
    _out.U8(statuses[slot]);
    _out.U8(counts[slot]);
  }
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
    WriteFleetBlock(out, _world, _viewer);

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

bool SnapshotWriter::WriteLeaves(std::uint64_t _tick, std::span<const EntityId> _left, std::span<const EntityId> _destroyed,
                                 std::span<const EntityId> _docked, Neuron::Transport& _transport)
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
  if (LEAVE_HEADER_BYTES + (leaveCount + destroyedCount + dockedCount) * ENTITY_ID_BYTES > Neuron::MAX_RELIABLE_BYTES)
    return false;

  m_leaveScratch.clear();
  ByteWriter out(m_leaveScratch);
  out.U8(KIND_LEAVE);
  out.U64(_tick);
  out.U32(leaveCount);
  out.U32(destroyedCount);
  out.U32(dockedCount);
  for (const EntityId entity : _left)
    out.Entity(entity);
  for (const EntityId entity : _destroyed)
    out.Entity(entity);
  for (const EntityId entity : _docked)
    out.Entity(entity);

  return _transport.SendReliable(m_leaveScratch.data(), static_cast<std::uint32_t>(m_leaveScratch.size()));
}

std::uint32_t SnapshotWriter::WriteInterest(const World& _world, std::span<const ShipHandle> _sent, std::span<const EntityId> _left,
                                            std::span<const EntityId> _destroyed, std::span<const EntityId> _docked,
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
    WriteFleetBlock(out, _world, _viewer);

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
  if (kind == KIND_FLEET_ROSTER)
    return AcceptRoster(_datagram);
  if (kind == KIND_LEDGER_REPLY)
    return AcceptLedgerReply(_datagram);
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
    fleets[slot].status = in.U8();
    fleets[slot].count = in.U8();
  }

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
void SnapshotReceiver::Remove(EntityId _gone)
{
  for (std::size_t at = 0; at < m_latest.ships.size(); ++at)
  {
    if (m_latest.ships[at].entity == _gone)
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
std::span<const EntityId> SnapshotReceiver::RosterOf(std::uint8_t _slot) const noexcept
{
  return (_slot < FLEET_SLOTS) ? std::span<const EntityId>(m_rosters[_slot]) : std::span<const EntityId>();
}

bool SnapshotReceiver::AcceptRoster(std::span<const std::uint8_t> _message)
{
  FleetRoster roster;
  if (!ReadFleetRoster(_message, roster))
    return false;

  // Replaces that slot's list whole. A roster is a statement of membership rather than a delta,
  // which is what lets a lost one be repaired by the next instead of compounding into a list that
  // is wrong in a way nothing can correct (Design/Fleets.md 8.1).
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
  if (!in.Ok())
    return false;

  // Read the whole message before touching the set: a truncated one must change nothing rather than
  // remove half of what it names.
  m_leaveScratch.clear();
  m_destroyedScratch.clear();
  m_dockedScratch.clear();
  for (std::uint32_t at = 0; at < leaveCount; ++at)
    m_leaveScratch.push_back(in.Entity());
  for (std::uint32_t at = 0; at < destroyedCount; ++at)
    m_destroyedScratch.push_back(in.Entity());
  for (std::uint32_t at = 0; at < dockedCount; ++at)
    m_dockedScratch.push_back(in.Entity());
  if (!in.Ok())
    return false;

  // All three leave the held set the same way. The lists differ in what they *say*, not in what they
  // do to the set, and only the client's effects care which.
  for (const EntityId gone : m_leaveScratch)
    Remove(gone);
  for (const EntityId dead : m_destroyedScratch)
    Remove(dead);
  for (const EntityId docked : m_dockedScratch)
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
// One ship's simulation state, all fifteen fields -- including the five WorldSnapshot deliberately
// withholds, which is the whole reason this codec had to exist beside it rather than reuse it.
void WriteShipState(ByteWriter& _out, const ShipState& _ship)
{
  _out.Pos(_ship.posWorld);
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
}

void ReadShipState(ByteReader& _in, ShipState& _outShip)
{
  _outShip.posWorld = _in.Pos();
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
}
} // namespace

void WriteWorldState(const World& _world, std::vector<std::uint8_t>& _outBytes)
{
  _outBytes.clear();
  ByteWriter out(_outBytes);
  out.U32(WORLD_STATE_MAGIC);
  out.U8(WORLD_STATE_FORMAT);

  out.U64(_world.m_tick);
  out.U16(_world.m_shard);
  out.U64(_world.m_nextEntitySerial);
  out.U64(_world.m_despawnBase);

  // The standings, row by row and with no count: FACTION_LIMIT is a compile-time constant on both
  // ends, and a build that disagreed about it would have failed the format byte first.
  for (std::uint32_t owner = 0; owner < FACTION_LIMIT; ++owner)
  {
    for (std::uint32_t other = 0; other < FACTION_LIMIT; ++other)
      out.U8(static_cast<std::uint8_t>(_world.m_standings.rows[owner][other]));
  }

  const std::uint32_t shipCount = static_cast<std::uint32_t>(_world.m_ships.size());
  out.U32(shipCount);
  for (const ShipState& ship : _world.m_ships)
    WriteShipState(out, ship);

  // The four tables parallel to m_ships, in the same order, so none of them carries a count of its
  // own: they are the same length as m_ships by construction and a length that disagreed would be a
  // defect this format cannot express.
  const std::uint32_t currentVersion = _world.m_pathIslands.Version();
  for (const World::Route& route : _world.m_routes)
  {
    out.U32(route.count);
    // Only the live waypoints. Entries past `count` are whatever a longer route left behind, they
    // are never read, and writing them would make two worlds that behave identically compare
    // unequal (work order 2.2).
    for (std::uint32_t at = 0; at < route.count; ++at)
      out.Pos(route.waypoint[at]);
    out.Pos(route.destination);
    out.Pos(route.legStart);
    out.F32(route.requiredClearanceMetres);
    out.U32(route.cursor);
    // Arrived with ADR 0042, after this codec's first branch: the ticks a ship has pushed against a
    // wall toward this route's point. Left out, a reload would reset every blocked counter and the
    // replayed world would end orders on different ticks than the run that saved it.
    out.U32(route.blockedTicks);
    // Whether it was planned against the architecture as it stands -- not the number that says so.
    // A grid version is an epoch counter with no meaning outside the run that produced it, and a
    // loaded world's islands get whatever number their rebuild produces (work order 2.1).
    out.Bool(route.gridVersion == currentVersion);
    out.Bool(route.reachesDestination);
  }

  for (const World::Patrol& patrol : _world.m_patrols)
  {
    out.Handle(patrol.anchor);
    out.F32(patrol.ringRadiusMetres);
    out.F32(patrol.cruiseSpeedMetresPerSec);
    out.U32(patrol.waypointIndex);
    out.Bool(patrol.active);
  }

  for (const World::Docking& docking : _world.m_dockings)
  {
    out.Handle(docking.station);
    out.Bool(docking.active);
  }

  for (const World::ProtectorDuty& duty : _world.m_protectors)
  {
    out.U32(duty.home);
    out.Handle(duty.target);
    out.Bool(duty.active);
  }

  // The slot table, which is what makes every handle in the four tables above still mean something
  // after a reload. m_shipSlot and m_entityRows are both inverses of it and are rebuilt rather than
  // written, so there is one statement of this relation in the file and not three.
  out.U32(static_cast<std::uint32_t>(_world.m_slots.size()));
  for (const World::Slot& slot : _world.m_slots)
  {
    out.U32(slot.ship);
    out.U32(slot.generation);
    out.Entity(slot.entity);
  }

  // In order: reuse is last-in-first-out and the order is the reproduction.
  out.U32(static_cast<std::uint32_t>(_world.m_freeSlots.size()));
  for (const std::uint32_t slot : _world.m_freeSlots)
    out.U32(slot);

  out.U32(static_cast<std::uint32_t>(_world.m_despawnLog.size()));
  for (const DespawnRecord& record : _world.m_despawnLog)
  {
    out.Handle(record.handle);
    out.Entity(record.entity);
    out.U8(static_cast<std::uint8_t>(record.cause));
  }

  out.U32(static_cast<std::uint32_t>(_world.m_stations.size()));
  for (const World::Station& station : _world.m_stations)
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
    for (const World::DockedShip& docked : station.docked)
    {
      out.U32(docked.hullId);
      out.U8(docked.factionId);
    }
  }

  // The fleets. Step prunes and retires them, so they are state it reads and the file carries them
  // (AGENTS.md 8). Only the live members are written, for the reason the routes above give about
  // their waypoints: entries past the count are never read, and writing them would make two worlds
  // that behave identically compare unequal.
  out.U32(static_cast<std::uint32_t>(_world.m_fleets.size()));
  for (const World::Fleet& fleet : _world.m_fleets)
  {
    out.U8(fleet.ownerFaction);
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
    out.Handle(fleet.threat);
    out.Pos(fleet.threatAnchorPos);
    out.U32(fleet.alertTicks);
  }
}

bool ReadWorldState(std::span<const std::uint8_t> _bytes, World& _outWorld)
{
  ByteReader in(_bytes);
  if (in.U32() != WORLD_STATE_MAGIC || in.U8() != WORLD_STATE_FORMAT)
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

  // Everything below is read into locals and moved into _outWorld only once the whole buffer has
  // been read and checked. That is what "fails closed" means here: a truncation on the last station
  // cannot leave a world half replaced, with nothing saying which half (AGENTS.md 5).
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

  std::vector<World::Route> routes(shipCount);
  for (World::Route& route : routes)
  {
    route.count = in.U32();
    if (!in.Ok() || route.count > MAX_PATH_WAYPOINTS)
      return false;
    for (std::uint32_t at = 0; at < route.count; ++at)
      route.waypoint[at] = in.Pos();
    route.destination = in.Pos();
    route.legStart = in.Pos();
    route.requiredClearanceMetres = in.F32();
    route.cursor = in.U32();
    route.blockedTicks = in.U32();
    // Held as the relation it was written as; the number it becomes is filled in below, once the
    // islands this world will actually route against have been rebuilt.
    route.gridVersion = in.Bool() ? 1u : 0u;
    route.reachesDestination = in.Bool();
    if (!in.Ok() || route.cursor > route.count)
      return false;
  }

  std::vector<World::Patrol> patrols(shipCount);
  for (World::Patrol& patrol : patrols)
  {
    patrol.anchor = in.Handle();
    patrol.ringRadiusMetres = in.F32();
    patrol.cruiseSpeedMetresPerSec = in.F32();
    patrol.waypointIndex = in.U32();
    patrol.active = in.Bool();
  }

  std::vector<World::Docking> dockings(shipCount);
  for (World::Docking& docking : dockings)
  {
    docking.station = in.Handle();
    docking.active = in.Bool();
  }

  std::vector<World::ProtectorDuty> protectors(shipCount);
  for (World::ProtectorDuty& duty : protectors)
  {
    duty.home = in.U32();
    duty.target = in.Handle();
    duty.active = in.Bool();
  }
  if (!in.Ok())
    return false;

  const std::uint32_t slotCount = in.U32();
  if (!in.Ok() || slotCount > in.Remaining())
    return false;
  std::vector<World::Slot> slots(slotCount);
  for (World::Slot& slot : slots)
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
  std::vector<World::Station> stations(stationCount);
  for (World::Station& station : stations)
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
    for (World::DockedShip& docked : station.docked)
    {
      docked.hullId = in.U32();
      docked.factionId = in.U8();
    }
  }

  // An exact bound rather than a heuristic one: every slot of every faction is the most fleets a
  // world can legitimately hold, so a count past it is a corrupt file and not a big world.
  const std::uint32_t fleetCount = in.U32();
  if (!in.Ok() || fleetCount > FACTION_LIMIT * FLEET_SLOTS)
    return false;

  // Two live fleets claiming one slot would make FleetInSlot's answer depend on which row came
  // first, which is an invariant corrupted rather than a value -- the same kind of defect as a slot
  // naming a ship that is not there, and refused for the same reason.
  std::vector<World::Fleet> fleets(fleetCount);
  bool slotTaken[FACTION_LIMIT][FLEET_SLOTS] = {};
  for (World::Fleet& fleet : fleets)
  {
    fleet.ownerFaction = in.U8();
    fleet.slot = in.U8();
    fleet.memberCount = in.U32();
    if (!in.Ok() || fleet.ownerFaction >= FACTION_LIMIT || fleet.slot >= FLEET_SLOTS || fleet.memberCount > MAX_FLEET_SHIPS)
      return false;
    if (slotTaken[fleet.ownerFaction][fleet.slot])
      return false;
    slotTaken[fleet.ownerFaction][fleet.slot] = true;
    // The handles themselves are not checked against the slot table, and deliberately: Resolve
    // bounds-checks a slot and compares a generation, so a handle this file invented resolves to
    // nothing and the fleet pass prunes it on the first tick. That is the fail-closed direction
    // already, and a second check here would only turn a self-repairing world into a refused load.
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
    if (!in.Ok() || orderKind > static_cast<std::uint8_t>(FleetOrderKind::Mine))
      return false;
    fleet.orderKind = static_cast<FleetOrderKind>(orderKind);
    fleet.orderPoint = in.Pos();
    fleet.orderFacingRad = in.F32();
    fleet.orderHasFacing = in.Bool();
    fleet.orderStation = in.Handle();
    fleet.orderTarget = in.Handle();
    // Nothing bounds the threat: a handle that resolves to nothing is a fleet that stands down on
    // its first tick, which is the fail-closed direction already.
    fleet.threat = in.Handle();
    fleet.threatAnchorPos = in.Pos();
    fleet.alertTicks = in.U32();
  }

  if (!in.Ok())
    return false;

  // Committed, and not before. From here nothing can fail.
  _outWorld.m_tick = tick;
  _outWorld.m_shard = shard;
  _outWorld.m_nextEntitySerial = nextSerial;
  _outWorld.m_despawnBase = despawnBase;
  _outWorld.m_standings = standings;
  _outWorld.m_ships = std::move(ships);
  _outWorld.m_routes = std::move(routes);
  _outWorld.m_patrols = std::move(patrols);
  _outWorld.m_dockings = std::move(dockings);
  _outWorld.m_protectors = std::move(protectors);
  _outWorld.m_slots = std::move(slots);
  _outWorld.m_freeSlots = std::move(freeSlots);
  _outWorld.m_despawnLog = std::move(despawnLog);
  _outWorld.m_stations = std::move(stations);
  _outWorld.m_fleets = std::move(fleets);

  // The two inverses of the slot table, rebuilt rather than read. m_entityRows is sorted by id
  // because that is the invariant its binary search rests on, and building it by walking the slots
  // in order and sorting once is cheaper and harder to get wrong than inserting one at a time.
  _outWorld.m_shipSlot.assign(_outWorld.m_ships.size(), 0);
  _outWorld.m_entityRows.clear();
  for (std::uint32_t slot = 0; slot < _outWorld.m_slots.size(); ++slot)
  {
    const World::Slot& entry = _outWorld.m_slots[slot];
    if (entry.ship == INVALID_SHIP_ID)
      continue;
    _outWorld.m_shipSlot[entry.ship] = slot;
    _outWorld.m_entityRows.push_back(World::EntityRow{entry.entity, slot});
  }
  std::sort(_outWorld.m_entityRows.begin(), _outWorld.m_entityRows.end(),
            [](const World::EntityRow& _a, const World::EntityRow& _b) { return _a.entity < _b.entity; });

  // The architecture, rebuilt here rather than on the first Step. It has to be here: the routes
  // above were written as "current" or "stale" and the number that says which is whatever this
  // rebuild produces, so a rebuild that happened one tick later would bump the version under every
  // route just marked current and re-plan the lot (work order 2.1).
  _outWorld.m_staticIndexDirty = true;
  _outWorld.RebuildStaticIfDirty();

  const std::uint32_t currentVersion = _outWorld.m_pathIslands.Version();
  for (World::Route& route : _outWorld.m_routes)
    route.gridVersion = (route.gridVersion != 0) ? currentVersion : currentVersion - 1u;

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
  for (const EntityId entity : _order.ships)
    out.Entity(entity);

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
  if (!in.Ok() || count == 0 || count > MaxShipsPerOrder() || in.Remaining() < count * ENTITY_ID_BYTES)
    return false;

  _outOrder.ships.clear();
  _outOrder.ships.reserve(count);
  for (std::uint32_t at = 0; at < count; ++at)
    _outOrder.ships.push_back(in.Entity());
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
  // on (Design/Archive/Stations-slice-3.md 2.6).
  if (_order.ships.empty() || _order.ships.size() > MaxShipsPerOrder())
    return false;

  std::vector<std::uint8_t> bytes;
  ByteWriter out(bytes);
  out.U8(KIND_DOCK_ORDER);
  out.U32(0); // order id, reserved: nothing acknowledges an order yet
  out.Entity(_order.station);
  out.U32(static_cast<std::uint32_t>(_order.ships.size()));
  for (const EntityId entity : _order.ships)
    out.Entity(entity);

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
  const EntityId station = in.Entity();
  const std::uint32_t count = in.U32();
  if (!in.Ok() || count == 0 || count > MaxShipsPerOrder() || in.Remaining() < count * ENTITY_ID_BYTES)
    return false;

  _outOrder.ships.clear();
  _outOrder.ships.reserve(count);
  for (std::uint32_t at = 0; at < count; ++at)
    _outOrder.ships.push_back(in.Entity());
  if (!in.Ok())
    return false;

  _outOrder.station = station;
  return true;
}

bool WriteFleetOrder(const FleetOrder& _order, Neuron::Transport& _transport)
{
  if (_order.slot >= FLEET_SLOTS || _order.kind > FleetOrderKind::Mine)
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
  const WorldPos point = in.Pos();
  const EntityId station = in.Entity();
  const EntityId target = in.Entity();
  if (!in.Ok() || slot >= FLEET_SLOTS || kind > static_cast<std::uint8_t>(FleetOrderKind::Mine))
    return false;

  _outOrder.slot = slot;
  _outOrder.kind = static_cast<FleetOrderKind>(kind);
  _outOrder.point = point;
  _outOrder.facingRad = facingRad;
  _outOrder.hasFacing = hasFacing;
  _outOrder.station = station;
  _outOrder.target = target;
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
