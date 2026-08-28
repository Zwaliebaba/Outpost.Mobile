#include "pch.h"
#include "World.h"

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

void World::Step()
{
  for (ShipState& ship : m_ships)
    StepShip(ship);
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

  const int count = static_cast<int>(chosen.size());
  const FormationShape shape = static_cast<FormationShape>(std::clamp(FORMATION_SHAPE, 0, 3));
  const float cosH = std::cos(heading);
  const float sinH = std::sin(heading);

  for (int slot = 0; slot < count; ++slot)
  {
    const XMFLOAT2 local = FormationOffset(slot, count, shape, FORMATION_SPACING);
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
