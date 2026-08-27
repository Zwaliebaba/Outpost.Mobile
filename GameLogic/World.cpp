#include "pch.h"
#include "World.h"

#include "Movement.h"
#include "SimTuning.h"

using namespace DirectX;

namespace Game
{
ShipId World::SpawnShip(const XMFLOAT3& _posWorld, float _headingRad, std::uint32_t _hullId)
{
  ShipState ship;
  ship.posWorld = _posWorld;
  ship.prevPos = _posWorld;
  ship.headingRad = _headingRad;
  ship.prevHeading = _headingRad;
  ship.hullId = _hullId;
  m_ships.push_back(ship);
  return static_cast<ShipId>(m_ships.size() - 1);
}

void World::Step()
{
  for (ShipState& ship : m_ships)
    StepShip(ship);
  ++m_tick;
}

float World::IssueMoveOrder(std::span<const ShipId> _ships, const XMFLOAT3& _point, bool _hasFacing, float _facingRad)
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
    float centreX = 0.0f;
    float centreZ = 0.0f;
    for (const ShipId id : chosen)
    {
      centreX += m_ships[id].posWorld.x;
      centreZ += m_ships[id].posWorld.z;
    }
    centreX /= static_cast<float>(chosen.size());
    centreZ /= static_cast<float>(chosen.size());
    const float dx = _point.x - centreX;
    const float dz = _point.z - centreZ;
    heading = (dx * dx + dz * dz > 1e-4f) ? std::atan2(dx, dz) : m_ships[chosen[0]].headingRad;
  }

  // Hand out slots in the order the ships already lie across the formation, so they do not have to
  // cross each other on the way in.
  const float rightX = std::cos(heading);
  const float rightZ = -std::sin(heading);
  const auto acrossFormation = [&](ShipId _id)
  {
    return m_ships[_id].posWorld.x * rightX + m_ships[_id].posWorld.z * rightZ;
  };
  std::sort(chosen.begin(), chosen.end(), [&](ShipId _a, ShipId _b)
  {
    return acrossFormation(_a) < acrossFormation(_b);
  });

  const int count = static_cast<int>(chosen.size());
  const FormationShape shape = static_cast<FormationShape>(std::clamp(FORMATION_SHAPE, 0, 3));
  const float cosH = std::cos(heading);
  const float sinH = std::sin(heading);

  for (int slot = 0; slot < count; ++slot)
  {
    const XMFLOAT2 local = FormationOffset(slot, count, shape, FORMATION_SPACING);
    ShipState& ship = m_ships[chosen[static_cast<size_t>(slot)]];
    ship.orderPos = XMFLOAT3(_point.x + local.x * cosH + local.y * sinH, 0.0f, _point.z - local.x * sinH + local.y * cosH);
    ship.orderFacingRad = heading;
    ship.orderHasFacing = _hasFacing;
    ship.order = OrderState::Moving;
  }
  return heading;
}
} // namespace Game
