#include "pch.h"
#include "LoopbackTransport.h"

#include <algorithm>
#include <cstring>

namespace Neuron
{
void LoopbackTransport::Connect(LoopbackTransport& _a, LoopbackTransport& _b, const Desc& _desc)
{
  const std::uint32_t capacity = (_desc.capacityDatagrams > 0) ? _desc.capacityDatagrams : 1;
  const std::uint32_t reliableCapacity = (_desc.capacityReliableMessages > 0) ? _desc.capacityReliableMessages : 1;
  for (LoopbackTransport* end : {&_a, &_b})
  {
    end->m_latencyTicks = _desc.latencyTicks;
    end->m_dropOneInN = _desc.dropOneInN;
    end->m_sendCounter = 0;
    end->m_tick = 0;
    end->m_slots.assign(capacity, Slot{});
    end->m_arena.assign(static_cast<std::size_t>(capacity) * MAX_DATAGRAM_BYTES, 0u);
    end->m_head = 0;
    end->m_count = 0;
    end->m_ready = 0;
    end->m_reliableSlots.assign(reliableCapacity, Slot{});
    end->m_reliableArena.assign(static_cast<std::size_t>(reliableCapacity) * MAX_RELIABLE_BYTES, 0u);
    end->m_reliableHead = 0;
    end->m_reliableCount = 0;
    end->m_reliableReady = 0;
    end->m_state = ConnectionState::Connected;
  }
  _a.m_peer = &_b;
  _b.m_peer = &_a;
}

void LoopbackTransport::AdvanceTo(std::uint64_t _tick) noexcept
{
  m_tick = _tick;
}

bool LoopbackTransport::Send(const std::uint8_t* _bytes, std::uint32_t _count)
{
  if (m_state != ConnectionState::Connected || m_peer == nullptr)
    return false;

  // Refused rather than truncated. A wire that silently shortens a datagram turns a format bug into
  // a corrupt-payload bug, which is far harder to find than a rejected send.
  if (_count > MAX_DATAGRAM_BYTES || (_count > 0 && _bytes == nullptr))
    return false;

  // Counted loss, so the same sequence drops the same datagrams on every run. The counter advances
  // whether or not the datagram is dropped, or the pattern would depend on queue occupancy.
  ++m_sendCounter;
  if (m_dropOneInN > 0 && (m_sendCounter % m_dropOneInN) == 0)
    return true; // dropped in flight, which is not a send failure

  // Due at the SENDER's tick plus the latency, which is what "sent at T arrives at T + L" means.
  // Both ends are advanced together by the composition root, so this equals the peer's clock today;
  // taking it from the peer would measure the delay from receipt instead, which is a different and
  // wrong thing the day the two ends are driven apart.
  return m_peer->Accept(_bytes, _count, m_tick + m_latencyTicks);
}

bool LoopbackTransport::Accept(const std::uint8_t* _bytes, std::uint32_t _count, std::uint64_t _dueTick)
{
  const std::uint32_t capacity = static_cast<std::uint32_t>(m_slots.size());
  if (capacity == 0 || m_count == capacity)
    return false; // a full queue drops the newest and keeps what is already in flight

  const std::uint32_t at = (m_head + m_count) % capacity;
  m_slots[at] = Slot{_dueTick, _count};
  if (_count > 0)
    std::memcpy(m_arena.data() + static_cast<std::size_t>(at) * MAX_DATAGRAM_BYTES, _bytes, _count);
  ++m_count;
  return true;
}

void LoopbackTransport::Poll()
{
  // Delivery happens here and only here, as Transport.h requires. Latency is uniform and sends are
  // ordered, so due order is send order and one cursor over the ring is enough.
  const std::uint32_t capacity = static_cast<std::uint32_t>(m_slots.size());
  while (m_ready < m_count && m_slots[(m_head + m_ready) % capacity].dueTick <= m_tick)
    ++m_ready;

  // Both lanes, one Poll -- there is no second one to forget to call.
  const std::uint32_t reliableCapacity = static_cast<std::uint32_t>(m_reliableSlots.size());
  while (m_reliableReady < m_reliableCount && m_reliableSlots[(m_reliableHead + m_reliableReady) % reliableCapacity].dueTick <= m_tick)
    ++m_reliableReady;
}

std::uint32_t LoopbackTransport::Receive(std::uint8_t* _outBytes, std::uint32_t _capacity)
{
  if (m_ready == 0)
    return 0;

  const Slot& slot = m_slots[m_head];

  // A caller must offer at least MAX_DATAGRAM_BYTES, because that is the most a datagram can be.
  // Returning 0 here would be indistinguishable from an empty queue and would spin any
  // while (Receive(...)) loop forever, so say so instead of stalling quietly.
  ASSERT_TEXT(slot.size <= _capacity, L"Receive was offered a buffer smaller than the waiting datagram");
  if (slot.size > _capacity)
    return 0;

  if (slot.size > 0 && _outBytes != nullptr)
    std::memcpy(_outBytes, m_arena.data() + static_cast<std::size_t>(m_head) * MAX_DATAGRAM_BYTES, slot.size);

  const std::uint32_t size = slot.size;
  m_head = (m_head + 1) % static_cast<std::uint32_t>(m_slots.size());
  --m_count;
  --m_ready;
  return size;
}

bool LoopbackTransport::SendReliable(const std::uint8_t* _bytes, std::uint32_t _count)
{
  if (m_state != ConnectionState::Connected || m_peer == nullptr)
    return false;

  // Refused rather than truncated, exactly as Send does, and against the lane's own bound.
  if (_count > MAX_RELIABLE_BYTES || (_count > 0 && _bytes == nullptr))
    return false;

  // No drop counter here, and m_sendCounter is deliberately not advanced: the datagram lane's loss
  // pattern is a property of that lane, and a test that sends on both would otherwise find its
  // drops moving depending on how many reliable messages happened to go out between them.
  return m_peer->AcceptReliable(_bytes, _count, m_tick + m_latencyTicks);
}

bool LoopbackTransport::AcceptReliable(const std::uint8_t* _bytes, std::uint32_t _count, std::uint64_t _dueTick)
{
  const std::uint32_t capacity = static_cast<std::uint32_t>(m_reliableSlots.size());
  if (capacity == 0 || m_reliableCount == capacity)
    return false; // backpressure: the sender is told, and nothing already in flight is disturbed

  const std::uint32_t at = (m_reliableHead + m_reliableCount) % capacity;
  m_reliableSlots[at] = Slot{_dueTick, _count};
  if (_count > 0)
    std::memcpy(m_reliableArena.data() + static_cast<std::size_t>(at) * MAX_RELIABLE_BYTES, _bytes, _count);
  ++m_reliableCount;
  return true;
}

std::uint32_t LoopbackTransport::ReceiveReliable(std::uint8_t* _outBytes, std::uint32_t _capacity)
{
  if (m_reliableReady == 0)
    return 0;

  const Slot& slot = m_reliableSlots[m_reliableHead];

  // Same rule as Receive: a caller offering too small a buffer is a bug in the caller, and returning
  // 0 would be indistinguishable from an empty lane and would spin a drain loop for ever.
  ASSERT_TEXT(slot.size <= _capacity, L"ReceiveReliable was offered a buffer smaller than the waiting message");
  if (slot.size > _capacity)
    return 0;

  if (slot.size > 0 && _outBytes != nullptr)
    std::memcpy(_outBytes, m_reliableArena.data() + static_cast<std::size_t>(m_reliableHead) * MAX_RELIABLE_BYTES, slot.size);

  const std::uint32_t size = slot.size;
  m_reliableHead = (m_reliableHead + 1) % static_cast<std::uint32_t>(m_reliableSlots.size());
  --m_reliableCount;
  --m_reliableReady;
  return size;
}

ConnectionState LoopbackTransport::State() const
{
  return m_state;
}

std::uint32_t LoopbackTransport::QueuedCount() const noexcept
{
  return m_count;
}

std::uint32_t LoopbackTransport::QueuedReliableCount() const noexcept
{
  return m_reliableCount;
}
} // namespace Neuron
