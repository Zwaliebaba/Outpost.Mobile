#pragma once

#include "InterestSet.h"
#include "World.h"
#include "WorldSnapshot.h"

#include "Simulation.h"
#include "Transport.h"

#include <array>
#include <span>
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
    PublishInterest();
  }

  [[nodiscard]] std::uint64_t Tick() const override
  {
    return m_world.Tick();
  }

private:
  // What the subscriber can see, published on its own schedule rather than every tick. Design
  // /Collision.md 1 asks for 5-20 Hz against a 60 Hz tick, and the rate is counted in ticks so a
  // measurement reproduces.
  void PublishInterest()
  {
    if (m_transport == nullptr || !m_interest.IsDueOn(m_world.Tick()))
      return;

    m_interest.Update(m_world, SubscriberCentre());

    // Entered and refreshed go on the wire together: the receiver upserts either way and the
    // distinction is the sender's, not the format's.
    m_sendScratch.clear();
    m_sendScratch.insert(m_sendScratch.end(), m_interest.Entered().begin(), m_interest.Entered().end());
    m_sendScratch.insert(m_sendScratch.end(), m_interest.Refreshed().begin(), m_interest.Refreshed().end());
    if (m_sendScratch.empty() && m_interest.Left().empty())
      return; // nothing changed and nothing came due; an empty update is not information

    (void)m_writer.WriteInterest(m_world, m_sendScratch, m_interest.Left(), *m_transport);
  }

  // Where the subscriber is looking. With one client every ship is its own, so this is the fleet's
  // centroid; the day a real player has a camera on the wire, it comes from there instead
  // (Design/Collision-slice-6.md 3.6).
  //
  // Accumulated as offsets from the first ship rather than by averaging fields, so a fleet
  // straddling a sector boundary has a centre between its ships and not a sector away.
  [[nodiscard]] Game::WorldPos SubscriberCentre() const
  {
    const std::span<const Game::ShipState> ships = m_world.Ships();
    if (ships.empty())
      return Game::WorldPos{};

    Game::WorldPos centre = ships[0].posWorld;
    float offsetX = 0.0f;
    float offsetZ = 0.0f;
    for (const Game::ShipState& ship : ships)
    {
      offsetX += Game::OffsetX(centre, ship.posWorld);
      offsetZ += Game::OffsetZ(centre, ship.posWorld);
    }
    Game::Translate(centre, offsetX / static_cast<float>(ships.size()), offsetZ / static_cast<float>(ships.size()));
    return centre;
  }

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
  Game::InterestSet m_interest;
  std::vector<Game::ShipId> m_resolved;
  std::vector<Game::ShipHandle> m_sendScratch;
};
} // namespace Outpost
