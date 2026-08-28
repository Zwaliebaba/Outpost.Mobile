#pragma once

#include "Formation.h"
#include "ShipState.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// The authoritative world. One dense array per entity kind, indexed by id -- no maps, no pointers
// between entities, no iteration order that is not array order, because all three are how a
// simulation stops reproducing itself.
//
// Ownership: whatever thread ticks the World owns it. Nothing else writes to it, and the view
// reads it only between ticks. When the halves separate this class is what moves to the server and
// the view stops holding a reference to it (AGENTS.md 2).
class World
{
public:
  // Adds a ship at rest. Returns its id, which is its index and stays valid for the run.
  ShipId SpawnShip(const DirectX::XMFLOAT3& _posWorld, float _headingRad, std::uint32_t _hullId);

  // One fixed tick. The only thing in the game that advances simulation state.
  void Step();

  // Sends the given ships to _point in formation. Returns the heading the formation was solved
  // onto, which is what the view needs to draw the order marker -- so the rule that decides it
  // lives here, with the order, rather than being guessed at again on the other side.
  //
  // With no ordered facing the formation points along the way the group is about to travel.
  float IssueMoveOrder(std::span<const ShipId> _ships, const DirectX::XMFLOAT3& _point, bool _hasFacing, float _facingRad);

  [[nodiscard]] std::span<const ShipState> Ships() const noexcept
  {
    return m_ships;
  }
  [[nodiscard]] const ShipState& Ship(ShipId _id) const noexcept
  {
    return m_ships[_id];
  }
  [[nodiscard]] std::uint32_t ShipCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_ships.size());
  }
  [[nodiscard]] std::uint64_t Tick() const noexcept
  {
    return m_tick;
  }

private:
  std::vector<ShipState> m_ships;
  std::uint64_t m_tick = 0;
};
} // namespace Game
