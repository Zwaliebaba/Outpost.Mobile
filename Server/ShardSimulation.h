#pragma once

#include "ShardSession.h"

#include "Simulation.h"

#include "Publisher.h"
#include "ServerConfig.h"
#include "Universe.h"

#include "Transport.h"

#include <cstdint>
#include <vector>

namespace Shard
{
// A universe, as the engine's run loop wants one, with the sessions watching it.
//
// Deliberately NOT `Outpost::UniverseSimulation`, and deliberately not a copy of it. That one owns a
// `Publisher` with exactly one subscriber in it, because the game executable has exactly one client;
// this one owns a `Publisher` with none or one, and later many. The two ticks differ in two ways
// that matter and neither can be abstracted away:
//
//   **a server drains its inbox at the tick boundary and a client never will** (ADR 0065), and
//   **a server has no camera to read**, so a session's interest centre is derived rather than pushed.
//
// What is NOT duplicated is the publisher wiring: `OpenSession` below is the one place a
// `Publisher::Desc` is filled from a `ServerConfig` on this side of the tree, which is what
// `Design/ShardServer-slice-1.md` §7 meant by moving it rather than copying it.
class ShardSimulation final : public Neuron::Simulation
{
public:
  explicit ShardSimulation(Game::Universe& _universe) noexcept
    : m_universe(_universe)
  {
  }

  // Adds one session and returns whether it took. The three knobs come out of the configuration file
  // rather than out of the Desc defaults, which is what makes them a property of the deployment
  // instead of the build (ADR 0043), and they are per subscriber rather than global -- so a
  // spectator or a distant region can be given different ones without touching anybody else's.
  //
  // Both cursors are opened at the head, not at zero. A client joining a running shard is told about
  // deaths and gunfire from now on and about none of the battle it did not watch (ADR 0027, 0053);
  // at zero it would receive the entire log the universe still holds on its first update, which the
  // game never noticed because its one client connects on tick 0 and the logs are empty there.
  bool OpenSession(Neuron::Transport& _transport, const Game::ServerConfig& _config)
  {
    if (!m_sessions.empty())
      return false; // one session, and the refusal is the feature (Design/ShardServer-slice-2.md 2.5)

    ShardSession session;
    session.transport = &_transport;

    Game::Publisher::Desc desc;
    desc.transport = &_transport;
    desc.issuer = Game::Issuer{session.owner, Game::FACTION_PLAYER};
    desc.interest.radiusMetres = _config.interestRadiusMetres;
    desc.interest.updateEveryTicks = _config.interestUpdateEveryTicks;
    desc.ordersPerTick = _config.ordersPerTick;
    desc.openingDespawnCursor = m_universe.DespawnHead();
    desc.openingShotCursor = m_universe.ShotHead();

    // Where it starts looking, before a tick has derived one: its own fleets if it has any, and the
    // universe origin if it does not.
    (void)m_universe.TryCentreOfOwnedFleets(session.owner, desc.centre);

    session.subscriber = m_publisher.Add(desc);
    m_sessions.push_back(session);
    return true;
  }

  // Drops the session on this transport, if there is one. False means there was not, which is what a
  // listener recycling a slot this server never took looks like.
  bool CloseSession(const Neuron::Transport* _transport)
  {
    for (std::size_t at = 0; at < m_sessions.size(); ++at)
    {
      if (m_sessions[at].transport != _transport)
        continue;
      (void)m_publisher.Remove(m_sessions[at].subscriber);
      m_sessions.erase(m_sessions.begin() + static_cast<std::ptrdiff_t>(at));
      return true;
    }
    return false;
  }

  [[nodiscard]] bool HasSessionOn(const Neuron::Transport* _transport) const noexcept
  {
    for (const ShardSession& session : m_sessions)
    {
      if (session.transport == _transport)
        return true;
    }
    return false;
  }

  [[nodiscard]] std::uint32_t SessionCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_sessions.size());
  }

  // The wire one session is on, by index. The root needs it to reconcile against what the listener
  // says is still live, and it is the transport rather than the session because a session is this
  // class's business and the pointer is the only half the root can check.
  [[nodiscard]] Neuron::Transport* SessionTransportAt(std::uint32_t _at) const noexcept
  {
    return (_at < m_sessions.size()) ? m_sessions[_at].transport : nullptr;
  }

  // One tick: orders in, what arrived from the neighbours, the step, then out.
  //
  // The order is the design and none of it may be reordered for convenience. Orders before the step
  // because draining them after would give every click an extra tick of latency (the game's own
  // argument, Design/Archive/Collision-slice-2b.md §2.5). The inbox drain between them and the step,
  // outside `Universe::Step`, which is the whole of ADR 0065's determinism argument -- a handoff
  // arrives on a stated tick or the replay gates stop meaning anything. Publish last, because
  // publishing before the step would send last tick's universe.
  //
  // Nothing delivers into this universe until slice 3 opens a link, so the drain costs a branch
  // today; it is here because the ordering is what slice 1 existed to establish.
  void Step() override
  {
    m_publisher.ApplyOrders(m_universe);
    (void)m_universe.DrainInbox();
    m_universe.Step();

    // Derived, not pushed. The game's root reads its camera and calls SetCentre; a server has none,
    // so a session follows its own fleets -- which is the number the status block already derives,
    // and is right for every client whose camera is on its fleet (Design/ShardServer-slice-2.md 2.6).
    for (const ShardSession& session : m_sessions)
    {
      Game::UniversePos centre;
      if (m_universe.TryCentreOfOwnedFleets(session.owner, centre))
        m_publisher.SetCentre(session.subscriber, centre);
    }

    m_publisher.Publish(m_universe);
  }

  [[nodiscard]] std::uint64_t Tick() const override
  {
    return m_universe.Tick();
  }

private:
  Game::Universe& m_universe;
  Game::Publisher m_publisher;

  // One today and the refusal in OpenSession is why. A vector rather than an optional because the
  // ceiling is a decision this slice took and not a shape the code should have to be rewritten out
  // of when a login lifts it.
  std::vector<ShardSession> m_sessions;
};
} // namespace Shard
