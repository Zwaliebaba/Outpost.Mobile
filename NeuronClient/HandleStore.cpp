#include "pch.h"
#include "HandleStore.h"

namespace Neuron
{
namespace
{
[[nodiscard]] std::uint32_t Pack(std::uint32_t _slot, std::uint16_t _generation) noexcept
{
  return (static_cast<std::uint32_t>(_generation) << 16) | (_slot & 0xFFFFu);
}
} // namespace

HandleStore::Allocation HandleStore::Alloc()
{
  std::uint32_t slot = 0;
  if (!m_freeSlots.empty())
  {
    slot = m_freeSlots.back();
    m_freeSlots.pop_back();
  }
  else
  {
    if (m_slots.size() >= MAX_SLOTS)
      return {}; // full: a content load reports and carries on rather than crashing (AGENTS.md 5)
    slot = static_cast<std::uint32_t>(m_slots.size());
    m_slots.emplace_back();
  }

  m_slots[slot].live = true;
  ++m_liveCount;
  return Allocation{Pack(slot, m_slots[slot].generation), slot};
}

std::uint32_t HandleStore::SlotOf(std::uint32_t _handle) const noexcept
{
  const std::uint32_t slot = SlotBitsOf(_handle);
  if (slot >= m_slots.size())
    return INVALID_SLOT;
  const Entry& entry = m_slots[slot];
  // Both tests matter and neither implies the other. The generation catches a handle this store
  // issued and has since retired -- the ordinary stale handle. `live` catches one it never issued:
  // Free bumps the generation, so the value a freed slot is now on has never been handed to anybody,
  // and a fabricated handle carrying it would otherwise resolve to a slot whose payload has been
  // released. The first is the case that happens; the second is the one that would be silent.
  if (!entry.live || entry.generation != GenerationOf(_handle))
    return INVALID_SLOT;
  return slot;
}

std::uint32_t HandleStore::Free(std::uint32_t _handle) noexcept
{
  const std::uint32_t slot = SlotOf(_handle);
  if (slot == INVALID_SLOT)
    return INVALID_SLOT; // a double free is a no-op that says so, not a second release
  Retire(slot);
  return slot;
}

void HandleStore::Retire(std::uint32_t _slot) noexcept
{
  Entry& entry = m_slots[_slot];
  entry.live = false;
  // Bumped on free rather than on the next alloc, so every handle to this slot goes stale the
  // moment the payload it names is released -- not the moment somebody happens to want the slot.
  ++entry.generation;
  if (entry.generation == 0)
    entry.generation = 1;
  m_freeSlots.push_back(_slot);
  --m_liveCount;
}
} // namespace Neuron
