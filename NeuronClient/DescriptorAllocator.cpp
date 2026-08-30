#include "pch.h"
#include "DescriptorAllocator.h"

namespace Neuron
{
void DescriptorAllocator::Init(std::uint32_t _capacity)
{
  m_capacity = _capacity;
  m_free.clear();
  m_live.assign(_capacity, false);
  m_next = 0;
  m_liveCount = 0;
}

std::uint32_t DescriptorAllocator::Allocate()
{
  std::uint32_t slot = INVALID_SLOT;
  if (!m_free.empty())
  {
    slot = m_free.back();
    m_free.pop_back();
  }
  else if (m_next < m_capacity)
  {
    slot = m_next++;
  }
  else
  {
    // Exhaustion is a content diagnostic: the texture that asked simply is not drawn, the same
    // treatment a missing mesh gets, and the capacity constant is what to raise.
    DebugTrace("descriptors: all {} slots live; the allocation is refused\n", m_capacity);
    return INVALID_SLOT;
  }
  m_live[slot] = true;
  ++m_liveCount;
  return slot;
}

void DescriptorAllocator::Free(std::uint32_t _slot) noexcept
{
  if (_slot >= m_capacity || !m_live[_slot])
  {
    DebugTrace("descriptors: slot {} freed while not live; ignored\n", _slot);
    return;
  }
  m_live[_slot] = false;
  --m_liveCount;
  m_free.push_back(_slot);
}
} // namespace Neuron
