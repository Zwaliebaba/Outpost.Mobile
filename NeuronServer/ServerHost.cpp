#include "pch.h"
#include "ServerHost.h"

namespace Neuron
{
void ServerHost::Init(const Desc& _desc, Simulation& _simulation) noexcept
{
  m_simulation = &_simulation;
  m_tickDt = 1.0f / std::max(1.0f, _desc.tickHz);
  m_maxCatchUpSec = std::max(m_tickDt, _desc.maxCatchUpSec);
  m_accumulatorSec = 0.0f;
}

int ServerHost::Advance(float _dtSec)
{
  if (!m_simulation)
    return 0;

  m_accumulatorSec = std::min(m_accumulatorSec + std::max(0.0f, _dtSec), m_maxCatchUpSec);

  int steps = 0;
  while (m_accumulatorSec >= m_tickDt)
  {
    m_simulation->Step();
    m_accumulatorSec -= m_tickDt;
    ++steps;
  }
  return steps;
}

float ServerHost::InterpolationAlpha() const noexcept
{
  return m_accumulatorSec / m_tickDt;
}

std::uint64_t ServerHost::Tick() const noexcept
{
  return m_simulation ? m_simulation->Tick() : 0;
}
} // namespace Neuron
