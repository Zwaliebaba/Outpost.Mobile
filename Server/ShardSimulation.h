#pragma once

#include "Simulation.h"

#include "Universe.h"

namespace Shard
{
// A universe, as the engine's run loop wants one.
//
// Deliberately NOT `Outpost::UniverseSimulation`, and deliberately not a copy of it. That one owns a
// `Publisher` and a subscriber, because the game executable has exactly one client; a shard has none
// this slice and many later, and the two ticks differ in a way that matters:
//
//   **a server drains its inbox at the tick boundary and a client never will** (ADR 0065).
//
// Slice 2 adds sessions here, and when it does it must MOVE the publisher wiring rather than grow a
// second copy of it -- which is the thing `Design/ShardServer-slice-1.md` §7 says is not allowed and
// is the first place this design could rot.
class ShardSimulation final : public Neuron::Simulation
{
public:
  explicit ShardSimulation(Game::Universe& _universe) noexcept
    : m_universe(_universe)
  {
  }

  // One tick: apply what arrived, then step.
  //
  // The drain is FIRST and outside `Step`, which is the whole of ADR 0065's determinism argument --
  // a handoff arrives on a stated tick or the replay gates stop meaning anything. Nothing delivers
  // into this universe until slice 3 opens a link, so today it drains an empty queue and costs a
  // branch; it is here now because the ordering is what this slice exists to establish.
  void Step() override
  {
    (void)m_universe.DrainInbox();
    m_universe.Step();
  }

  [[nodiscard]] std::uint64_t Tick() const override
  {
    return m_universe.Tick();
  }

private:
  Game::Universe& m_universe;
};
} // namespace Shard
