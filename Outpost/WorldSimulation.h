#pragma once

#include "World.h"
#include "WorldSnapshot.h"

#include "Simulation.h"
#include "Transport.h"

#include <array>
#include <vector>

namespace Outpost
{
// The one place the engine's server half and the game meet. NeuronServer declares Simulation and
// knows nothing else; GameLogic declares World and knows nothing about being hosted; this adapter,
// in the executable, is what makes one drive the other.
//
// It also owns the server end of the transport, because the tick is where both directions have to
// happen and in this order: take the orders that arrived, run the tick, publish what the tick
// produced. Draining orders after the step would give every click an extra tick of latency that no
// configured latency accounts for, and publishing before the step would send last tick's world
// (Design/Collision-slice-2b.md 2.5).
class WorldSimulation final : public Neuron::Simulation
{
public:
  explicit WorldSimulation(Game::World& _world) noexcept
    : m_world(_world)
  {
  }

  void Connect(Neuron::Transport& _transport) noexcept
  {
    m_transport = &_transport;
  }

  void Step() override
  {
    ApplyIncomingOrders();
    m_world.Step();
    if (m_transport != nullptr)
      (void)m_writer.Write(m_world, *m_transport);
  }

  [[nodiscard]] std::uint64_t Tick() const override
  {
    return m_world.Tick();
  }

private:
  void ApplyIncomingOrders()
  {
    if (m_transport == nullptr)
      return;

    m_transport->Poll();
    std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> datagram{};
    for (;;)
    {
      const std::uint32_t size = m_transport->Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size()));
      if (size == 0)
        break;

      Game::MoveOrder order;
      if (!Game::ReadMoveOrder(std::span<const std::uint8_t>(datagram.data(), size), order))
        continue; // a datagram this half does not understand is dropped, not fatal

      // Handles resolve here and nowhere else. A ship that died between the click and this tick
      // resolves to nothing and is simply left out of the order, rather than steering whichever
      // ship swap-and-pop moved into its index (ADR 0005).
      m_resolved.clear();
      m_resolved.reserve(order.ships.size());
      for (const Game::ShipHandle handle : order.ships)
      {
        const Game::ShipId id = m_world.Resolve(handle);
        if (id != Game::INVALID_SHIP_ID)
          m_resolved.push_back(id);
      }
      if (!m_resolved.empty())
        (void)m_world.IssueMoveOrder(m_resolved, order.destination, order.hasFacing, order.facingRad);
    }
  }

  Game::World& m_world;
  Neuron::Transport* m_transport = nullptr;
  Game::SnapshotWriter m_writer;
  std::vector<Game::ShipId> m_resolved;
};
} // namespace Outpost
