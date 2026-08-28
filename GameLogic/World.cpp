#include "pch.h"
#include "World.h"

#include "Collision.h"
#include "HullSpec.h"
#include "Movement.h"
#include "SimTuning.h"

using namespace DirectX;

namespace Game
{
ShipId World::SpawnShip(const WorldPos& _posWorld, float _headingRad, std::uint32_t _hullId)
{
  ShipState ship;
  ship.posWorld = _posWorld;
  ship.prevPos = _posWorld;
  ship.headingRad = _headingRad;
  ship.prevHeading = _headingRad;
  ship.hullId = _hullId;

  const ShipId id = static_cast<ShipId>(m_ships.size());
  m_ships.push_back(ship);

  std::uint32_t slot = 0;
  if (!m_freeSlots.empty())
  {
    slot = m_freeSlots.back();
    m_freeSlots.pop_back();
  }
  else
  {
    slot = static_cast<std::uint32_t>(m_slots.size());
    m_slots.emplace_back();
  }
  m_slots[slot].ship = id;
  // Generation 0 is the null handle, so a slot's first issue is 1 and a wrap skips back past it.
  if (m_slots[slot].generation == 0)
    m_slots[slot].generation = 1;
  m_shipSlot.push_back(slot);
  // Any spawn or despawn can move a Structure's id, not only one that adds or removes a Structure,
  // because swap-and-pop renumbers whatever was last. Marking every one of them dirty costs a
  // rebuild that only happens when the fleet changes, and gets the id-shift case right for free.
  m_staticIndexDirty = true;
  return id;
}

bool World::DespawnShip(ShipHandle _handle)
{
  const ShipId id = Resolve(_handle);
  if (id == INVALID_SHIP_ID)
    return false;

  const ShipId last = static_cast<ShipId>(m_ships.size() - 1);
  if (id != last)
  {
    m_ships[id] = m_ships[last];
    m_shipSlot[id] = m_shipSlot[last];
    m_slots[m_shipSlot[id]].ship = id; // the moved ship keeps its slot, and its handles keep working
  }
  m_ships.pop_back();
  m_shipSlot.pop_back();

  Slot& freed = m_slots[_handle.slot];
  freed.ship = INVALID_SHIP_ID;
  ++freed.generation;
  if (freed.generation == 0)
    freed.generation = 1;
  m_freeSlots.push_back(_handle.slot);
  m_staticIndexDirty = true;
  return true;
}

ShipHandle World::HandleOf(ShipId _id) const noexcept
{
  if (_id >= m_ships.size())
    return {};
  const std::uint32_t slot = m_shipSlot[_id];
  return ShipHandle{slot, m_slots[slot].generation};
}

ShipId World::Resolve(ShipHandle _handle) const noexcept
{
  if (_handle.generation == 0 || _handle.slot >= m_slots.size())
    return INVALID_SHIP_ID;
  const Slot& slot = m_slots[_handle.slot];
  return (slot.generation == _handle.generation) ? slot.ship : INVALID_SHIP_ID;
}

void World::ConfigureIndex(const SpatialIndex::Desc& _desc) noexcept
{
  m_index.Configure(_desc);
  m_staticIndexDirty = true;
}

std::span<const Neighbour> World::NeighboursOf(ShipId _id) const noexcept
{
  if (_id >= m_neighbourCount.size())
    return {};
  return std::span<const Neighbour>(m_neighbours).subspan(m_neighbourStart[_id], m_neighbourCount[_id]);
}

float World::AuthorityOf(ShipId _id) const noexcept
{
  return AvoidanceAuthorityOf(HullSpecOf(m_ships[_id].hullId), m_ships[_id].order);
}

void World::SnapshotPreviousTick() noexcept
{
  // Two lines, hoisted out of StepShip into a pass of their own, and that hoist is the whole of
  // the order-independence property. prevPos was already written here for the view to interpolate
  // between ticks; making it a whole pass turns it into the authoritative start-of-tick state that
  // every neighbour read below sees, so ship 0 and ship 500 see the same world.
  for (ShipState& ship : m_ships)
  {
    ship.prevPos = ship.posWorld;
    ship.prevHeading = ship.headingRad;
  }
}

void World::RebuildIndex()
{
  if (m_staticIndexDirty)
  {
    m_staticEntries.clear();
    for (ShipId id = 0; id < m_ships.size(); ++id)
    {
      const HullSpec& hull = HullSpecOf(m_ships[id].hullId);
      if (hull.immovable && hull.collidable)
        m_staticEntries.push_back({id, m_ships[id].posWorld, hull.BoundingRadiusMetres()});
    }
    m_index.RebuildStatic(m_staticEntries);
    m_staticIndexDirty = false;
  }

  m_dynamicEntries.clear();
  for (ShipId id = 0; id < m_ships.size(); ++id)
  {
    const HullSpec& hull = HullSpecOf(m_ships[id].hullId);
    if (!hull.immovable && hull.collidable)
      m_dynamicEntries.push_back({id, m_ships[id].prevPos, hull.BoundingRadiusMetres()});
  }
  m_index.RebuildDynamic(m_dynamicEntries);
}

void World::GatherNeighbours()
{
  const std::uint32_t count = ShipCount();
  m_neighbourStart.assign(static_cast<std::size_t>(count) + 1, 0);
  for (ShipId id = 0; id < count; ++id)
    m_neighbourStart[id + 1] = m_neighbourStart[id] + HullSpecOf(m_ships[id].hullId).neighbourCap;
  m_neighbours.assign(m_neighbourStart[count], Neighbour{});
  m_neighbourCount.assign(count, 0);

  for (ShipId id = 0; id < count; ++id)
  {
    const ShipState& ship = m_ships[id];
    const HullSpec& hull = HullSpecOf(ship.hullId);
    if (!hull.collidable)
      continue;

    m_index.QueryCircle(ship.prevPos, QueryRadiusMetres(hull), m_queryScratch);
    m_candidateScratch.clear();
    for (const ShipId other : m_queryScratch)
    {
      if (other == id)
        continue;
      const ShipState& neighbour = m_ships[other];
      Neighbour record;
      record.id = other;
      record.offsetX = OffsetX(ship.prevPos, neighbour.prevPos);
      record.offsetZ = OffsetZ(ship.prevPos, neighbour.prevPos);
      record.velocityX = std::sin(neighbour.prevHeading) * neighbour.speed;
      record.velocityZ = std::cos(neighbour.prevHeading) * neighbour.speed;
      const HullSpec& neighbourHull = HullSpecOf(neighbour.hullId);
      record.boundingRadiusMetres = neighbourHull.BoundingRadiusMetres();
      record.avoidanceAuthority = AvoidanceAuthorityOf(neighbourHull, neighbour.order);
      record.immovable = neighbourHull.immovable;
      record.distanceSquared = record.offsetX * record.offsetX + record.offsetZ * record.offsetZ;
      record.proximityMetres = std::sqrt(record.distanceSquared) - record.boundingRadiusMetres;
      m_candidateScratch.push_back(record);
    }

    // Every candidate from the covering ring, then sorted, and only then truncated. Truncating
    // cell by cell would make cell size part of the replay contract and it could never be retuned
    // again -- not per region, not after profiling. ShipId is the tie-break and is what makes the
    // order total, so std::sort's instability cannot show through (Design/Collision.md 7).
    std::sort(m_candidateScratch.begin(), m_candidateScratch.end(), [](const Neighbour& _a, const Neighbour& _b)
              { return (_a.proximityMetres != _b.proximityMetres) ? _a.proximityMetres < _b.proximityMetres : _a.id < _b.id; });

    const std::uint32_t take = std::min<std::uint32_t>(hull.neighbourCap, static_cast<std::uint32_t>(m_candidateScratch.size()));
    std::copy_n(m_candidateScratch.begin(), take, m_neighbours.begin() + m_neighbourStart[id]);
    m_neighbourCount[id] = take;
  }
}

namespace
{
[[nodiscard]] Capsule CapsuleAt(const ShipState& _ship, const HullSpec& _hull, float _centreX, float _centreZ) noexcept
{
  return Capsule{
    _centreX, _centreZ, std::sin(_ship.headingRad), std::cos(_ship.headingRad), _hull.capsuleHalfLengthMetres, _hull.capsuleRadiusMetres};
}
} // namespace

void World::ApplySeparation()
{
  const std::uint32_t count = ShipCount();
  m_correctionX.assign(count, 0.0f);
  m_correctionZ.assign(count, 0.0f);

  for (ShipId id = 0; id < count; ++id)
  {
    const ShipState& ship = m_ships[id];
    const HullSpec& hull = HullSpecOf(ship.hullId);
    if (hull.immovable || !hull.collidable)
      continue;

    const Capsule self = CapsuleAt(ship, hull, 0.0f, 0.0f);
    float correctionX = 0.0f;
    float correctionZ = 0.0f;
    for (const Neighbour& neighbour : NeighboursOf(id))
    {
      const ShipState& other = m_ships[neighbour.id];
      const HullSpec& otherHull = HullSpecOf(other.hullId);
      if (!otherHull.collidable || otherHull.immovable)
        continue; // architecture is pass 5b's, after this one and without the clamp

      const Capsule against = CapsuleAt(other, otherHull, OffsetX(ship.posWorld, other.posWorld), OffsetZ(ship.posWorld, other.posWorld));
      const Contact contact = CapsuleContact(self, against, id, neighbour.id);
      if (!contact.touching)
        continue;

      // Cap what the pair closes, then split it -- never the other way round. Both sides compute
      // this same cap from the same two radii, so the authority ratio survives the clamp instead of
      // being inverted by it.
      const SeparationShares shares = SeparationSharesFor(AuthorityOf(id), false, AuthorityOf(neighbour.id), false);
      const float pairCap = SEPARATION_PAIR_CLOSE_FRACTION * std::min(hull.capsuleRadiusMetres, otherHull.capsuleRadiusMetres);
      const float closed = std::min(contact.overlapMetres * SEPARATION_STIFFNESS, pairCap);
      correctionX += contact.normalX * closed * shares.a;
      correctionZ += contact.normalZ * closed * shares.a;
    }

    const float limit = SEPARATION_CLAMP_FRACTION * hull.capsuleRadiusMetres;
    const float magnitudeSquared = correctionX * correctionX + correctionZ * correctionZ;
    if (magnitudeSquared > limit * limit)
    {
      const float scale = limit / std::sqrt(magnitudeSquared);
      correctionX *= scale;
      correctionZ *= scale;
    }
    m_correctionX[id] = correctionX;
    m_correctionZ[id] = correctionZ;
  }

  for (ShipId id = 0; id < count; ++id)
    Translate(m_ships[id].posWorld, m_correctionX[id], m_correctionZ[id]);
}

void World::ApplyBlocking()
{
  const std::uint32_t count = ShipCount();
  m_correctionX.assign(count, 0.0f);
  m_correctionZ.assign(count, 0.0f);

  for (ShipId id = 0; id < count; ++id)
  {
    const ShipState& ship = m_ships[id];
    const HullSpec& hull = HullSpecOf(ship.hullId);
    if (hull.immovable || !hull.collidable)
      continue;

    const Capsule self = CapsuleAt(ship, hull, 0.0f, 0.0f);
    for (const Neighbour& neighbour : NeighboursOf(id))
    {
      const ShipState& other = m_ships[neighbour.id];
      const HullSpec& otherHull = HullSpecOf(other.hullId);
      if (!otherHull.collidable || !otherHull.immovable)
        continue;

      const Capsule against = CapsuleAt(other, otherHull, OffsetX(ship.posWorld, other.posWorld), OffsetZ(ship.posWorld, other.posWorld));
      const Contact contact = CapsuleContact(self, against, id, neighbour.id);
      if (!contact.touching)
        continue;

      // The whole overlap, and no clamp. Hard blocking has to be hard or it is decoration: this
      // runs after ship-to-ship separation precisely so that a column of traffic behind a ship
      // cannot squeeze it through a Structure.
      m_correctionX[id] += contact.normalX * contact.overlapMetres;
      m_correctionZ[id] += contact.normalZ * contact.overlapMetres;
    }
  }

  for (ShipId id = 0; id < count; ++id)
    Translate(m_ships[id].posWorld, m_correctionX[id], m_correctionZ[id]);
}

void World::Step()
{
  SnapshotPreviousTick();
  RebuildIndex();
  GatherNeighbours();

  for (ShipId id = 0; id < ShipCount(); ++id)
  {
    ShipState& ship = m_ships[id];
    const HullSpec& hull = HullSpecOf(ship.hullId);
    if (hull.immovable)
      continue;
    // Pass 3 reads only this ship and the start-of-tick copies in its neighbour list; pass 4 writes
    // only this ship. Neither can see another ship half-advanced, which is what keeps the whole
    // tick free of array order.
    const MotionIntent intent = AvoidNeighbours(ship, hull, SolveOrder(ship, hull), NeighboursOf(id));
    IntegrateShip(ship, hull, intent);
  }

  ApplySeparation();
  ApplyBlocking();
  ++m_tick;
}

float World::IssueMoveOrder(std::span<const ShipId> _ships, const WorldPos& _point, bool _hasFacing, float _facingRad)
{
  std::vector<ShipId> chosen;
  chosen.reserve(_ships.size());
  for (const ShipId id : _ships)
  {
    if (id < m_ships.size())
      chosen.push_back(id);
  }
  if (chosen.empty())
    return _facingRad;

  // Point the formation along the ordered facing, or along the way the group is about to travel.
  float heading = _facingRad;
  if (!_hasFacing)
  {
    WorldPos centre = m_ships[chosen[0]].posWorld;
    float centreX = 0.0f;
    float centreZ = 0.0f;
    for (const ShipId id : chosen)
    {
      centreX += OffsetX(centre, m_ships[id].posWorld);
      centreZ += OffsetZ(centre, m_ships[id].posWorld);
    }
    Translate(centre, centreX / static_cast<float>(chosen.size()), centreZ / static_cast<float>(chosen.size()));
    const float dx = OffsetX(centre, _point);
    const float dz = OffsetZ(centre, _point);
    heading = (dx * dx + dz * dz > 1e-4f) ? std::atan2(dx, dz) : m_ships[chosen[0]].headingRad;
  }

  // Hand out slots in the order the ships already lie across the formation, so they do not have to
  // cross each other on the way in.
  const float rightX = std::cos(heading);
  const float rightZ = -std::sin(heading);
  const auto acrossFormation = [&](ShipId _id)
  { return OffsetX(_point, m_ships[_id].posWorld) * rightX + OffsetZ(_point, m_ships[_id].posWorld) * rightZ; };
  std::sort(chosen.begin(), chosen.end(), [&](ShipId _a, ShipId _b) { return acrossFormation(_a) < acrossFormation(_b); });

  // Sized by the largest hull in the group, so a mixed order spaces itself for the Carrier in it
  // rather than for the fighters. A formation plans once and travels as one thing.
  float largestRadius = 0.0f;
  for (const ShipId id : chosen)
    largestRadius = std::max(largestRadius, HullSpecOf(m_ships[id].hullId).BoundingRadiusMetres());
  const float spacing = SlotSpacingMetres(largestRadius);

  const int count = static_cast<int>(chosen.size());
  const FormationShape shape = static_cast<FormationShape>(std::clamp(FORMATION_SHAPE, 0, 3));
  const float cosH = std::cos(heading);
  const float sinH = std::sin(heading);

  for (int slot = 0; slot < count; ++slot)
  {
    const XMFLOAT2 local = FormationOffset(slot, count, shape, spacing);
    ShipState& ship = m_ships[chosen[static_cast<size_t>(slot)]];
    ship.orderPos = _point;
    Translate(ship.orderPos, local.x * cosH + local.y * sinH, -local.x * sinH + local.y * cosH);
    ship.orderFacingRad = heading;
    ship.orderHasFacing = _hasFacing;
    ship.order = OrderState::Moving;
  }
  return heading;
}
} // namespace Game
