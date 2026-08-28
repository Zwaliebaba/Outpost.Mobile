#pragma once

#include <cstdint>

namespace Neuron
{
// What the server host needs a simulation to be, declared by the engine and implemented by the
// game. This is the whole of the engine's knowledge of the game: NeuronServer never names
// GameLogic, never includes a game header, and never links against one. The executable is the only
// place the two meet, and it meets them by handing one to the other (AGENTS.md 2).
//
// The moment an engine project references the game it stops being an engine, and the sibling game
// on the same engine stops building.
class Simulation
{
public:
  virtual ~Simulation() = default;

  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  // Advance by exactly one fixed tick. Called only by ServerHost, only on the owning thread, and
  // never with a variable time step -- the step size is the simulation's own, not the frame's.
  virtual void Step() = 0;

  [[nodiscard]] virtual std::uint64_t Tick() const = 0;

protected:
  Simulation() = default;
};
} // namespace Neuron
