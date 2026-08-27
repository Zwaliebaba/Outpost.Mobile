#pragma once

#include "World.h"

#include "Simulation.h"

namespace Outpost
{
// The one place the engine's server half and the game meet. NeuronServer declares Simulation and
// knows nothing else; GameLogic declares World and knows nothing about being hosted; this adapter,
// in the executable, is what makes one drive the other.
//
// It is three lines because that is the point: if it ever needs to be more than this, the seam is
// in the wrong place.
class WorldSimulation final : public Neuron::Simulation
{
public:
  explicit WorldSimulation(Game::World& _world) noexcept : m_world(_world) {}

  void Step() override { m_world.Step(); }
  [[nodiscard]] std::uint64_t Tick() const override { return m_world.Tick(); }

private:
  Game::World& m_world;
};
} // namespace Outpost
