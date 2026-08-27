#include "pch.h"
#include "FrameClock.h"

#include "Ease.h"

namespace Neuron
{
FrameClock::FrameClock() noexcept
{
  LARGE_INTEGER frequency = {};
  QueryPerformanceFrequency(&frequency);
  m_frequency = frequency.QuadPart != 0 ? frequency.QuadPart : 1;
  m_previousQpc = Now();
}

std::int64_t FrameClock::Now() const noexcept
{
  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

float FrameClock::ElapsedMs(std::int64_t _fromQpc, std::int64_t _toQpc) const noexcept
{
  return static_cast<float>(static_cast<double>(_toQpc - _fromQpc) / static_cast<double>(m_frequency) * 1000.0);
}

float FrameClock::Tick() noexcept
{
  const std::int64_t nowQpc = Now();
  float dtSec = static_cast<float>(static_cast<double>(nowQpc - m_previousQpc) / static_cast<double>(m_frequency));
  m_previousQpc = nowQpc;
  if (dtSec < 0.0f)
    dtSec = 0.0f;
  if (dtSec > MAX_FRAME_SEC)
    dtSec = MAX_FRAME_SEC;

  m_fpsSmoothed = SmoothTowards(m_fpsSmoothed, dtSec > 0.0f ? 1.0f / dtSec : 0.0f, dtSec, READOUT_HALF_LIFE_SEC);
  m_frameMsSmoothed = SmoothTowards(m_frameMsSmoothed, dtSec * 1000.0f, dtSec, READOUT_HALF_LIFE_SEC);
  return dtSec;
}
} // namespace Neuron
