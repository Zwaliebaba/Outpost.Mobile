#pragma once

#include "Simulation.h"

#include <cstdint>

namespace Neuron
{
// Owns the tick loop. Given a frame's elapsed time it runs whole simulation ticks and reports how
// much of the next tick is already accumulated, so the renderer can interpolate rather than show a
// stepped world.
//
// It is a separate object from the frame loop for one reason: the tick rate is the game's, not the
// display's. Wire the same host to a headless loop and it ticks identically with no swapchain in
// sight, which is what makes the client-server split a move rather than a rewrite.
class ServerHost
{
public:
  struct Desc
  {
    float tickHz = 60.0f;
    // The longest run of catch-up a single frame may drive. A stall -- a dragged window, a
    // breakpoint, a laptop lid -- must not spiral into a burst of ticks that stalls the next frame
    // too. Time beyond this is dropped, deliberately: the world runs slow for a moment rather than
    // freezing.
    float maxCatchUpSec = 0.25f;
  };

  void Init(const Desc& _desc, Simulation& _simulation) noexcept;

  // Runs whole ticks for the elapsed frame time and returns how many it ran.
  int Advance(float _dtSec);

  // How far into the next tick the accumulator stands, 0..1. What the render pass interpolates by.
  [[nodiscard]] float InterpolationAlpha() const noexcept;

  [[nodiscard]] std::uint64_t Tick() const noexcept;
  [[nodiscard]] float TickDt() const noexcept
  {
    return m_tickDt;
  }

private:
  Simulation* m_simulation = nullptr;
  float m_tickDt = 1.0f / 60.0f;
  float m_maxCatchUpSec = 0.25f;
  float m_accumulatorSec = 0.0f;
};
} // namespace Neuron
