#pragma once

#include <chrono>
#include <cstdint>

namespace Outpost
{
// What a tick cost, sampled by the composition root.
//
// It lives here and not in GameLogic, and that placement is the whole of its argument. Everything
// in this block is a wall-clock duration, and a wall clock is the first thing GameLogic.h's
// determinism list forbids: "no wall clock -- the only time in here is TICK_DT and the tick count".
// The root may read a clock and does so every frame; the simulation may not, and measuring it must
// not become the reason that stops being true (Design/TickTelemetry-work-order.md 1.1).
//
// Two spans, because the tick has two halves that scale differently and the review's scale track
// asks about them separately: the step is the simulation and grows with entities, the publish is
// the seam and grows with subscribers. A block that summed them would answer neither question.
struct TickStats
{
  using Clock = std::chrono::steady_clock;

  // The window: reset whenever the block is written, so a file is one window rather than a running
  // total since boot. A mean over the life of a process stops moving after ten minutes and hides
  // exactly the load spike it was added to find.
  std::uint64_t ticks = 0;
  double stepSumMs = 0.0;
  double publishSumMs = 0.0;
  double stepWorstMs = 0.0;
  double publishWorstMs = 0.0;

  // The last tick alone, which is what the F1 readout shows: a mean is the shape of the load and
  // the last tick is what the frame in front of you just paid.
  double stepLastMs = 0.0;
  double publishLastMs = 0.0;

  // Read at write time rather than accumulated, because each is a level and not a rate.
  std::uint64_t tick = 0;
  std::uint32_t shipCount = 0;
  std::uint32_t subscriberCount = 0;

  // Where the economy's faucets and sinks will be counted (GameDesignReview.md, Economy 7). Zero,
  // unread, and here rather than in a file of their own so that the first faucet has somewhere to
  // be counted instead of a reason to invent a second instrument. The review's own finding is that
  // neither panel owned this block; two blocks would be the same failure twice.
  std::uint64_t issued = 0;
  std::uint64_t sunk = 0;

  void Record(double _stepMs, double _publishMs) noexcept
  {
    ++ticks;
    stepLastMs = _stepMs;
    publishLastMs = _publishMs;
    stepSumMs += _stepMs;
    publishSumMs += _publishMs;
    if (_stepMs > stepWorstMs)
      stepWorstMs = _stepMs;
    if (_publishMs > publishWorstMs)
      publishWorstMs = _publishMs;
  }

  [[nodiscard]] double StepMeanMs() const noexcept
  {
    return (ticks != 0) ? stepSumMs / static_cast<double>(ticks) : 0.0;
  }

  [[nodiscard]] double PublishMeanMs() const noexcept
  {
    return (ticks != 0) ? publishSumMs / static_cast<double>(ticks) : 0.0;
  }

  // The window only. The levels and the counters are not a window's worth of anything and are
  // rewritten wholesale at the next sample.
  void ResetWindow() noexcept
  {
    ticks = 0;
    stepSumMs = 0.0;
    publishSumMs = 0.0;
    stepWorstMs = 0.0;
    publishWorstMs = 0.0;
  }

  // Milliseconds between two steady-clock reads. A steady clock and never a wall one: a wall clock
  // steps backwards over an NTP correction and would report a negative tick.
  [[nodiscard]] static double ElapsedMs(Clock::time_point _from, Clock::time_point _to) noexcept
  {
    return std::chrono::duration<double, std::milli>(_to - _from).count();
  }
};

// The block as a file, beside Universe.sav. `key = value` lines in the shape Server.cfg already
// uses, because a person reads this and because a binary one would need a version byte, a reader
// and a test, which is a second save file rather than a readout.
[[nodiscard]] bool WriteTickStatsFile(const TickStats& _stats);

// What it is called, beside UNIVERSE_SAVE_FILE and for that constant's reason.
inline constexpr const wchar_t* UNIVERSE_STATS_FILE = L"Universe.stats";
} // namespace Outpost
