#pragma once

#include "InterestSet.h"
#include "World.h"
#include "WorldSnapshot.h"

#include "Simulation.h"
#include "Transport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
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
// (Design/Archive/Collision-slice-2b.md 2.5).
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
    SplitTheLost();
    if (m_sendScratch.empty() && m_interest.Left().empty())
      return; // nothing changed and nothing came due; an empty update is not information

    (void)m_writer.WriteInterest(m_world, m_sendScratch, m_leftScratch, m_destroyedScratch, *m_transport);
  }

  // Which of this update's leaves were deaths. A despawned ship the subscriber held always turns up
  // in Left() -- InterestTests::ADespawnedShipLeavesTheSet is that guarantee -- so the world's
  // despawn log intersected with Left() is exactly the set that died in view; the rest merely went
  // out of range (Design/Hostiles.md 4.4).
  //
  // Left() is sorted (ADR 0010) and the log is a handful of handles, so this is a walk of the log
  // with a binary search into Left(). The cursor advances on every due update rather than only on
  // the ones that send, since a despawn no subscriber held has nobody left to tell and would
  // otherwise sit in the log for the rest of the match.
  //
  // One subscriber, so the minimum cursor across subscribers is this one's and the trim follows the
  // read immediately. With a table of them the read stays here and the trim moves to whoever knows
  // every cursor (ADR 0026).
  void SplitTheLost()
  {
    const std::span<const Game::ShipHandle> left = m_interest.Left();
    m_destroyedScratch.clear();
    for (const Game::ShipHandle dead : m_world.DespawnsSince(m_despawnCursor))
    {
      if (std::binary_search(left.begin(), left.end(), dead, Game::HandleOrderBefore))
        m_destroyedScratch.push_back(dead);
    }
    m_despawnCursor = m_world.DespawnHead();
    m_world.TrimDespawnsBefore(m_despawnCursor);

    std::sort(m_destroyedScratch.begin(), m_destroyedScratch.end(), Game::HandleOrderBefore);
    m_leftScratch.clear();
    std::set_difference(left.begin(), left.end(), m_destroyedScratch.begin(), m_destroyedScratch.end(), std::back_inserter(m_leftScratch),
                        Game::HandleOrderBefore);
  }

  // Where the subscriber is looking: the centroid of its own fleet. The day a real player has a
  // camera on the wire, it comes from there instead (Design/Archive/Collision-slice-6.md 3.6).
  //
  // Its own, not every ship's. That distinction was free while every ship was the subscriber's and
  // stopped being the moment a hostile base existed: four hostiles 1.2 km out drag an unfiltered
  // centroid some 690 m toward the enemy, which moves what the player is sent (Design/Hostiles.md 6).
  //
  // Accumulated as offsets from the first own ship rather than by averaging fields, so a fleet
  // straddling a sector boundary has a center between its ships and not a sector away.
  [[nodiscard]] Game::WorldPos SubscriberCentre() const
  {
    const std::span<const Game::ShipState> ships = m_world.Ships();
    Game::WorldPos centre;
    std::uint32_t counted = 0;
    float offsetX = 0.0f;
    float offsetZ = 0.0f;
    for (const Game::ShipState& ship : ships)
    {
      if (ship.factionId != m_subscriberFaction)
        continue;
      if (counted == 0)
        centre = ship.posWorld;
      offsetX += Game::OffsetX(centre, ship.posWorld);
      offsetZ += Game::OffsetZ(centre, ship.posWorld);
      ++counted;
    }
    if (counted == 0)
      return Game::WorldPos{}; // nothing of its own to look from, as an empty world already returned

    Game::Translate(centre, offsetX / static_cast<float>(counted), offsetZ / static_cast<float>(counted));
    return centre;
  }

  void ApplyIncomingOrders()
  {
    if (m_transport == nullptr)
      return;

    m_transport->Poll();

    // Orders arrive on the reliable lane (ADR 0028): a dropped order is a click the player made and
    // the game ignored. The datagram lane is still drained, because a client that predates the lane
    // -- or one whose lane is not up yet -- may still be sending there, and dropping its orders
    // silently would be a worse answer than reading them.
    m_orderScratch.resize(Neuron::MAX_RELIABLE_BYTES);
    for (int lane = 0; lane < 2; ++lane)
    {
      for (;;)
      {
        const std::uint32_t size = (lane == 0) ? m_transport->ReceiveReliable(m_orderScratch.data(), Neuron::MAX_RELIABLE_BYTES)
                                               : m_transport->Receive(m_orderScratch.data(), Neuron::MAX_DATAGRAM_BYTES);
        if (size == 0)
          break;

        Game::MoveOrder order;
        if (!Game::ReadMoveOrder(std::span<const std::uint8_t>(m_orderScratch.data(), size), order))
          continue; // a message this half does not understand is dropped, not fatal

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
          (void)m_world.IssueMoveOrder(m_resolved, order.destination, order.hasFacing, order.facingRad, m_subscriberFaction);
      }
    }
  }

  Game::World& m_world;
  Neuron::Transport* m_transport = nullptr;
  Game::SnapshotWriter m_writer;
  Game::InterestSet m_interest;

  // Whose orders this subscriber may give. One subscriber today, so it is the player's; the day a
  // real player connects, this comes from the session -- the same sentence SubscriberCentre carries.
  Game::FactionId m_subscriberFaction = Game::FACTION_PLAYER;

  // How far through the world's despawn log this subscriber has been told. Zero is the right start:
  // a world that has killed nothing has a head of zero, and a subscriber joining a running world
  // takes DespawnHead() instead so it is not told about ships it never held (ADR 0026).
  std::uint64_t m_despawnCursor = 0;

  std::vector<Game::ShipId> m_resolved;

  // One message's worth, reused so a tick allocates nothing. Sized to the reliable lane's bound
  // because it is the larger of the two.
  std::vector<std::uint8_t> m_orderScratch;
  std::vector<Game::ShipHandle> m_sendScratch;
  std::vector<Game::ShipHandle> m_leftScratch;
  std::vector<Game::ShipHandle> m_destroyedScratch;
};
} // namespace Outpost
