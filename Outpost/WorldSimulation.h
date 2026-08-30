#pragma once

#include "ServerConfig.h"

#include "Publisher.h"
#include "World.h"

#include "Simulation.h"

#include "Transport.h"

#include <cstdint>
#include <span>

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
//
// What it no longer owns is the fan-out. An interest set, a writer, a faction and a despawn cursor
// per subscriber live in Game::Publisher now, and this class holds a Publisher with one entry in it
// (ADR 0030). The behavior is the same to the byte; the shape is what changed, and it changed here
// rather than the day a second client arrives, because that is the day it would be expensive.
class WorldSimulation final : public Neuron::Simulation
{
public:
  explicit WorldSimulation(Game::World& _world) noexcept
    : m_world(_world)
  {
  }

  // Connects the one subscriber this executable has. A dedicated server would call Publisher::Add
  // once per session instead, which is the whole of what changes there.
  //
  // The three knobs come out of the configuration file rather than out of the Desc defaults, which
  // is what makes them a property of the deployment instead of the build (ADR 0043). They are per
  // subscriber and not global, which is the shape Publisher::Desc already had -- so a spectator or
  // a distant region can be given different ones without touching anybody else's.
  void Connect(Neuron::Transport& _transport, const ServerConfig& _config)
  {
    Game::Publisher::Desc desc;
    desc.transport = &_transport;
    desc.faction = m_subscriberFaction;
    desc.interest.radiusMetres = _config.interestRadiusMetres;
    desc.interest.updateEveryTicks = _config.interestUpdateEveryTicks;
    desc.ordersPerTick = _config.ordersPerTick;

    // From the head, not from zero: the fleet is spawned before the link is opened, and a ship that
    // died during boot is not something this client ever held (ADR 0027).
    desc.openingDespawnCursor = m_world.DespawnHead();
    m_subscriber = m_publisher.Add(desc);
  }

  void Step() override
  {
    m_publisher.ApplyOrders(m_world);
    m_world.Step();
    m_publisher.SetCentre(m_subscriber, SubscriberCentre());
    m_publisher.Publish(m_world);
  }

  [[nodiscard]] std::uint64_t Tick() const override
  {
    return m_world.Tick();
  }

  // Diagnostics the HUD does not show yet, exposed because the numbers are worth having a name for:
  // both should be zero.
  [[nodiscard]] std::uint32_t ThrottledTickCount() const noexcept
  {
    return m_publisher.ThrottledTickCount(m_subscriber);
  }

  [[nodiscard]] std::uint32_t RefusedLeaveCount() const noexcept
  {
    return m_publisher.RefusedLeaveCount(m_subscriber);
  }

private:
  // Where the subscriber is looking: the centroid of its own fleet. The day a real player has a
  // camera on the wire, it comes from there instead (Design/Archive/Collision-slice-6.md 3.6).
  //
  // Its own, not every ship's. That distinction was free while every ship was the subscriber's and
  // stopped being the moment a hostile base existed: four hostiles 1.2 km out drag an unfiltered
  // centroid some 690 m toward the enemy, which moves what the player is sent (Design/Archive/Hostiles.md 6).
  //
  // Accumulated as offsets from the first own ship rather than by averaging fields, so a fleet
  // straddling a sector boundary has a center between its ships and not a sector away.
  //
  // With no own ship at all -- every hull docked, which Stations made an ordinary way for a fleet
  // to leave the world -- the centre holds where it last was. It used to fall back to a default
  // WorldPos, the universe origin, and the moment the last ship docked the interest set jumped
  // 3.5 km away: the station the player had just flown into left the client's view and, on
  // screen, vanished. A player looking at a station keeps looking at it; the day undocking exists
  // the ship comes out under a centre that never moved (Design/Stations-slice-6.md 5).
  [[nodiscard]] Game::WorldPos SubscriberCentre()
  {
    const std::span<const Game::ShipState> ships = m_world.Ships();
    Game::WorldPos centre;
    bool haveFirst = false;
    float sumX = 0.0f;
    float sumZ = 0.0f;
    std::uint32_t count = 0;
    for (const Game::ShipState& ship : ships)
    {
      if (ship.factionId != m_subscriberFaction)
        continue;
      if (!haveFirst)
      {
        centre = ship.posWorld;
        haveFirst = true;
      }
      sumX += Game::OffsetX(centre, ship.posWorld);
      sumZ += Game::OffsetZ(centre, ship.posWorld);
      ++count;
    }
    if (count == 0)
      return m_lastCentre;
    Game::Translate(centre, sumX / static_cast<float>(count), sumZ / static_cast<float>(count));
    m_lastCentre = centre;
    return centre;
  }

  Game::World& m_world;
  Game::Publisher m_publisher;
  Game::Publisher::Handle m_subscriber;

  // Whose orders this subscriber may give. One subscriber today, so it is the player's; the day a
  // login exists it arrives with the session and only Connect changes.
  Game::FactionId m_subscriberFaction = Game::FACTION_PLAYER;

  // Where the subscriber last had a fleet to look from (SubscriberCentre).
  Game::WorldPos m_lastCentre;
};
} // namespace Outpost
