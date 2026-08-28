#pragma once

#include <cstdint>

namespace Neuron
{
// The one place the wall clock is read. Everything above it works in seconds, which keeps QPC out
// of the rest of the tree and gives the deterministic layers nothing to reach for: GameLogic never
// sees this type, and a replay can drive the same frame code from recorded deltas instead.
class FrameClock
{
public:
  FrameClock() noexcept;

  // Seconds since the previous Tick, clamped so a stall (a dragged window, a breakpoint) reports a
  // long frame rather than a jump the simulation would have to catch up on.
  float Tick() noexcept;

  [[nodiscard]] std::int64_t Now() const noexcept;
  [[nodiscard]] float ElapsedMs(std::int64_t _fromQpc, std::int64_t _toQpc) const noexcept;

  // Eased readouts, for a HUD that should not flicker at the refresh rate.
  [[nodiscard]] float Fps() const noexcept
  {
    return m_fpsSmoothed;
  }
  [[nodiscard]] float FrameMs() const noexcept
  {
    return m_frameMsSmoothed;
  }

  static constexpr float MAX_FRAME_SEC = 0.25f;
  static constexpr float READOUT_HALF_LIFE_SEC = 0.25f;

private:
  std::int64_t m_frequency = 1;
  std::int64_t m_previousQpc = 0;
  float m_fpsSmoothed = 0.0f;
  float m_frameMsSmoothed = 0.0f;
};
} // namespace Neuron
