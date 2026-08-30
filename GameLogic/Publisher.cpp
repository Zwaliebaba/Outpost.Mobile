#include "pch.h"
#include "Publisher.h"

#include "World.h"

#include <algorithm>
#include <iterator>

namespace Game
{
Publisher::Handle Publisher::Add(const Desc& _desc)
{
  const std::uint32_t index = static_cast<std::uint32_t>(m_subscribers.size());

  std::uint32_t slot = 0;
  if (!m_freeSlots.empty())
  {
    slot = m_freeSlots.back();
    m_freeSlots.pop_back();
  }
  else
  {
    slot = static_cast<std::uint32_t>(m_slots.size());
    m_slots.emplace_back();
  }
  m_slots[slot].subscriber = index;
  if (m_slots[slot].generation == 0)
    m_slots[slot].generation = 1;

  Subscriber& added = m_subscribers.emplace_back();
  added.transport = _desc.transport;
  added.faction = _desc.faction;
  added.ordersPerTick = _desc.ordersPerTick;
  added.centre = _desc.centre;
  added.interest.Configure(_desc.interest);

  // The phase comes from the slot rather than from the index, and that is deliberate: an index moves
  // when somebody else is removed, so a subscriber that had a tick to itself would silently start
  // sharing one. The slot is stable for its life.
  added.phase = slot % std::max(1u, _desc.interest.updateEveryTicks);

  // Whatever the caller said. Zero -- the default -- is "everything the log still holds", which is
  // right for a subscriber present from the first tick and wrong for one joining a running world;
  // that caller passes World::DespawnHead() (ADR 0027).
  added.despawnCursor = _desc.openingDespawnCursor;

  m_subscriberSlot.push_back(slot);
  return Handle{slot, m_slots[slot].generation};
}

bool Publisher::Remove(Handle _handle)
{
  Subscriber* const found = Resolve(_handle);
  if (found == nullptr)
    return false;

  const std::uint32_t index = m_slots[_handle.slot].subscriber;
  const std::uint32_t last = static_cast<std::uint32_t>(m_subscribers.size() - 1);
  if (index != last)
  {
    m_subscribers[index] = std::move(m_subscribers[last]);
    m_subscriberSlot[index] = m_subscriberSlot[last];
    m_slots[m_subscriberSlot[index]].subscriber = index; // the moved one keeps its slot and its phase
  }
  m_subscribers.pop_back();
  m_subscriberSlot.pop_back();

  Slot& freed = m_slots[_handle.slot];
  freed.subscriber = INVALID_SUBSCRIBER;
  ++freed.generation;
  if (freed.generation == 0)
    freed.generation = 1;
  m_freeSlots.push_back(_handle.slot);
  return true;
}

Publisher::Subscriber* Publisher::Resolve(Handle _handle) noexcept
{
  if (_handle.generation == 0 || _handle.slot >= m_slots.size())
    return nullptr;
  const Slot& slot = m_slots[_handle.slot];
  if (slot.generation != _handle.generation || slot.subscriber == INVALID_SUBSCRIBER)
    return nullptr;
  return &m_subscribers[slot.subscriber];
}

const Publisher::Subscriber* Publisher::Resolve(Handle _handle) const noexcept
{
  return const_cast<Publisher*>(this)->Resolve(_handle);
}

void Publisher::SetCentre(Handle _handle, const WorldPos& _centre) noexcept
{
  if (Subscriber* const found = Resolve(_handle))
    found->centre = _centre;
}

std::uint32_t Publisher::ThrottledTickCount(Handle _handle) const noexcept
{
  const Subscriber* const found = Resolve(_handle);
  return (found != nullptr) ? found->throttledTicks : 0u;
}

std::uint32_t Publisher::RefusedLeaveCount(Handle _handle) const noexcept
{
  const Subscriber* const found = Resolve(_handle);
  return (found != nullptr) ? found->writer.RefusedLeaveCount() : 0u;
}

std::uint32_t Publisher::PhaseOf(Handle _handle) const noexcept
{
  const Subscriber* const found = Resolve(_handle);
  return (found != nullptr) ? found->phase : 0u;
}

void Publisher::ApplyOrders(World& _world)
{
  m_messageScratch.resize(Neuron::MAX_RELIABLE_BYTES);

  for (Subscriber& subscriber : m_subscribers)
  {
    if (subscriber.transport == nullptr)
      continue;
    subscriber.transport->Poll();

    // Both lanes, budget shared between them. Orders travel reliably now (ADR 0029); the datagram
    // lane is still drained because a client whose stream is not up yet is still a client, and
    // dropping its clicks silently would be a worse answer than reading them.
    std::uint32_t read = 0;
    for (int lane = 0; lane < 2 && read < subscriber.ordersPerTick; ++lane)
    {
      for (;;)
      {
        if (read >= subscriber.ordersPerTick)
        {
          // Over budget. Nothing is discarded: what is still queued stays in the transport and is
          // read next tick, so this counts a throttled tick rather than a lost order. Dropping here
          // would throw away a click the player made, and the queue filling up is already the
          // backpressure answer for a client that never stops sending.
          ++subscriber.throttledTicks;
          break;
        }

        const std::uint32_t size = (lane == 0) ? subscriber.transport->ReceiveReliable(m_messageScratch.data(), Neuron::MAX_RELIABLE_BYTES)
                                               : subscriber.transport->Receive(m_messageScratch.data(), Neuron::MAX_DATAGRAM_BYTES);
        if (size == 0)
          break;
        ++read;

        const std::span<const std::uint8_t> message(m_messageScratch.data(), size);

        // Handles resolve here and nowhere else, for either kind. A ship that died between the click
        // and this tick resolves to nothing and is left out, rather than steering whichever ship
        // swap-and-pop moved into its index (ADR 0005).
        MoveOrder order;
        DockOrder dockOrder;
        if (ReadMoveOrder(message, order))
        {
          m_resolvedScratch.clear();
          for (const ShipHandle handle : order.ships)
          {
            const ShipId id = _world.Resolve(handle);
            if (id != INVALID_SHIP_ID)
              m_resolvedScratch.push_back(id);
          }
          if (!m_resolvedScratch.empty())
            (void)_world.IssueMoveOrder(m_resolvedScratch, order.destination, order.hasFacing, order.facingRad, subscriber.faction);
        }
        else if (ReadDockOrder(message, dockOrder))
        {
          // The station resolves too, and a dead one makes the whole order a no-op rather than a
          // dock at whatever now occupies that index.
          const ShipId station = _world.Resolve(dockOrder.station);
          if (station == INVALID_SHIP_ID)
            continue;

          m_resolvedScratch.clear();
          for (const ShipHandle handle : dockOrder.ships)
          {
            const ShipId id = _world.Resolve(handle);
            if (id != INVALID_SHIP_ID)
              m_resolvedScratch.push_back(id);
          }
          if (!m_resolvedScratch.empty())
            (void)_world.IssueDockOrder(m_resolvedScratch, station, subscriber.faction);
        }
        // A message this half does not understand is dropped, not fatal.
      }
    }
  }
}

void Publisher::Publish(World& _world)
{
  for (Subscriber& subscriber : m_subscribers)
  {
    if (subscriber.transport != nullptr && subscriber.interest.IsDueOn(_world.Tick(), subscriber.phase))
      PublishOne(_world, subscriber);
  }

  // Then, and only then, the log may forget. The minimum across every subscriber is what is safe to
  // drop -- anything newer is still owed to whichever one is furthest behind (ADR 0027). With no
  // subscribers at all the head is the minimum, because nobody is owed anything.
  std::uint64_t minimum = _world.DespawnHead();
  for (const Subscriber& subscriber : m_subscribers)
    minimum = std::min(minimum, subscriber.despawnCursor);
  _world.TrimDespawnsBefore(minimum);
}

void Publisher::PublishOne(const World& _world, Subscriber& _subscriber)
{
  _subscriber.interest.Update(_world, _subscriber.centre);

  // Entered and refreshed go on the wire together: the receiver upserts either way and the
  // distinction is the sender's, not the format's.
  m_sendScratch.clear();
  m_sendScratch.insert(m_sendScratch.end(), _subscriber.interest.Entered().begin(), _subscriber.interest.Entered().end());
  m_sendScratch.insert(m_sendScratch.end(), _subscriber.interest.Refreshed().begin(), _subscriber.interest.Refreshed().end());

  SplitTheLost(_world, _subscriber);
  if (m_sendScratch.empty() && _subscriber.interest.Left().empty())
    return; // nothing changed and nothing came due; an empty update is not information

  // The subscriber's faction is what the header's hostileMask is stated for. The publisher is the
  // only thing that knows whose view an update is; this is not a second authority check.
  (void)_subscriber.writer.WriteInterest(_world, m_sendScratch, m_leftScratch, m_destroyedScratch, m_dockedScratch, *_subscriber.transport,
                                         _subscriber.faction);
}

void Publisher::SplitTheLost(const World& _world, Subscriber& _subscriber)
{
  const std::span<const ShipHandle> left = _subscriber.interest.Left();
  m_destroyedScratch.clear();
  m_dockedScratch.clear();

  // Three sets from two now: a departure carries a cause, so this is where a death is told apart
  // from a docking and both from a ship that merely flew out of range (ADR 0040).
  //
  // This subscriber's own cursor, so two of them reading the same death both see it. The cursor
  // advances whether or not anything is sent, since a death nobody held has nobody to tell.
  for (const DespawnRecord& gone : _world.DespawnsSince(_subscriber.despawnCursor))
  {
    if (!std::binary_search(left.begin(), left.end(), gone.handle, HandleOrderBefore))
      continue;
    if (gone.cause == DespawnCause::Docked)
      m_dockedScratch.push_back(gone.handle);
    else
      m_destroyedScratch.push_back(gone.handle);
  }
  _subscriber.despawnCursor = _world.DespawnHead();

  std::sort(m_destroyedScratch.begin(), m_destroyedScratch.end(), HandleOrderBefore);
  std::sort(m_dockedScratch.begin(), m_dockedScratch.end(), HandleOrderBefore);

  // The merely-left are what is in neither stated set. Both are subtracted, in two passes, because
  // set_difference wants a sorted range on each side and the two causes are sorted separately.
  m_statedScratch.clear();
  std::set_union(m_destroyedScratch.begin(), m_destroyedScratch.end(), m_dockedScratch.begin(), m_dockedScratch.end(),
                 std::back_inserter(m_statedScratch), HandleOrderBefore);
  m_leftScratch.clear();
  std::set_difference(left.begin(), left.end(), m_statedScratch.begin(), m_statedScratch.end(), std::back_inserter(m_leftScratch),
                      HandleOrderBefore);
}
} // namespace Game
