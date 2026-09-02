#pragma once

#include "ServerConfig.h"
#include "TickStats.h"

#include "Publisher.h"
#include "Universe.h"

#include "Simulation.h"

#include "Transport.h"

#include <cstdint>

namespace Outpost
{
// The one place the engine's server half and the game meet. NeuronServer declares Simulation and
// knows nothing else; GameLogic declares Universe and knows nothing about being hosted; this adapter,
// in the executable, is what makes one drive the other.
//
// It also owns the server end of the transport, because the tick is where both directions have to
// happen and in this order: take the orders that arrived, run the tick, publish what the tick
// produced. Draining orders after the step would give every click an extra tick of latency that no
// configured latency accounts for, and publishing before the step would send last tick's universe
// (Design/Archive/Collision-slice-2b.md 2.5).
//
// What it no longer owns is the fan-out. An interest set, a writer, a faction and a despawn cursor
// per subscriber live in Game::Publisher now, and this class holds a Publisher with one entry in it
// (ADR 0030). The behavior is the same to the byte; the shape is what changed, and it changed here
// rather than the day a second client arrives, because that is the day it would be expensive.
class UniverseSimulation final : public Neuron::Simulation
{
public:
  explicit UniverseSimulation(Game::Universe& _universe) noexcept
    : m_universe(_universe)
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
    // Both halves of who this subscriber is. The owner is a placeholder for what a login will
    // supply and is named as one: OWNER_LOCAL is the single player this build has, and a grep for it
    // finds every place that assumes there is only one (Design/Archive/OwnerKey-work-order.md).
    desc.issuer = Game::Issuer{m_subscriberOwner, m_subscriberFaction};
    desc.interest.radiusMetres = _config.interestRadiusMetres;
    desc.interest.updateEveryTicks = _config.interestUpdateEveryTicks;
    desc.ordersPerTick = _config.ordersPerTick;

    // From the head, not from zero: the fleet is spawned before the link is opened, and a ship that
    // died during boot is not something this client ever held (ADR 0027).
    desc.openingDespawnCursor = m_universe.DespawnHead();
    m_subscriber = m_publisher.Add(desc);
  }

  void Step() override
  {
    // Bracketed, not reordered. The order below is argued at the top of this class and the timing
    // must not become a reason to touch it: orders in, tick, publish. The split is where the two
    // halves genuinely divide -- everything before SetCentre is the simulation, everything from it
    // is the seam -- because they scale with different things and the review asks about them
    // separately (Design/Archive/TickTelemetry-work-order.md 1.2).
    const TickStats::Clock::time_point began = TickStats::Clock::now();
    m_publisher.ApplyOrders(m_universe);
    m_universe.Step();

    const TickStats::Clock::time_point stepped = TickStats::Clock::now();
    m_publisher.SetCentre(m_subscriber, m_viewCentre);
    m_publisher.Publish(m_universe);
    const TickStats::Clock::time_point published = TickStats::Clock::now();

    m_stats.Record(TickStats::ElapsedMs(began, stepped), TickStats::ElapsedMs(stepped, published));
  }

  // What the ticks since the last sample cost. The levels on it are filled by the root at write
  // time rather than here, because each is a level and not something a tick accumulates.
  [[nodiscard]] TickStats& Stats() noexcept
  {
    return m_stats;
  }

  [[nodiscard]] std::uint32_t SubscriberCount() const noexcept
  {
    return m_publisher.Count();
  }

  // Where this subscriber is looking, pushed in by the composition root each frame from the
  // camera's ground target.
  //
  // It replaced the centroid of the subscriber's own ships, and the fleet bar is why. Tapping fleet
  // 3 flies the camera fifty kilometres; under a centre that averaged every own ship, the interest
  // set stayed where that average was and not one hull of fleet 3 was ever sent, so the button flew
  // the camera to an empty sky. The design says the interest set follows the camera
  // (Design/Archive/Fleets.md 9.1) and this class's own note anticipated it: the day a real player has a
  // camera, the centre comes from there.
  //
  // It is not on the wire and does not need to be. The composition root holds both halves and may
  // read the camera, which is the standing the debug keys already have; a dedicated server gets it
  // from the session instead, and only this setter's caller changes.
  //
  // What the old centre bought is not lost. A player whose camera is over empty space is still told
  // where all five fleets are -- that is the status block, stamped on every update, and it is why
  // this change was not safe before slice 5. And a fleet that docks no longer drags the view: the
  // camera does not move when a ship docks, so the station the player flew into stays on screen,
  // which Design/Archive/Stations-slice-6.md 5 fixed by hand and this makes structural.
  void SetViewCentre(const Game::UniversePos& _centre) noexcept
  {
    m_viewCentre = _centre;
  }

  [[nodiscard]] std::uint64_t Tick() const override
  {
    return m_universe.Tick();
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
  Game::Universe& m_universe;

  // Wall-clock durations, which is why they are here and not one layer down: GameLogic may not read
  // a clock and this slice must not be the reason it starts (Outpost/TickStats.h).
  TickStats m_stats;
  Game::Publisher m_publisher;
  Game::Publisher::Handle m_subscriber;

  // Whose orders this subscriber may give. One subscriber today, so it is the player's; the day a
  // login exists it arrives with the session and only Connect changes.
  Game::FactionId m_subscriberFaction = Game::FACTION_PLAYER;

  // Who this client is, as against what it is. There is no login and no account, so the one player
  // this executable serves is named by a constant; a dedicated server would take it from whatever
  // authenticated the session, which is the one thing that has to arrive before a second player can.
  Game::OwnerId m_subscriberOwner = Game::OWNER_LOCAL;

  // Where the camera is looking, as of the last frame that said. The universe origin until one
  // does, which is where the boot scene stands.
  Game::UniversePos m_viewCentre;
};
} // namespace Outpost
