#include "pch.h"
#include "Publisher.h"

#include "Universe.h"

#include <algorithm>

namespace Game
{
namespace
{
// Whether a list of departed entities names this one. Linear because these lists are a handful of
// ids at most -- what left one subscriber's view on one update -- and sorting them to binary-search
// would cost more than the scan.
[[nodiscard]] bool Names(std::span<const EntityId> _entities, EntityId _entity) noexcept
{
  for (const EntityId entity : _entities)
  {
    if (entity == _entity)
      return true;
  }
  return false;
}

// Whether a faction holds any slot at all. What decides that an update with no records is still
// worth sending, because its header carries the status block.
[[nodiscard]] bool HasAnyFleet(const Universe& _universe, FactionId _faction) noexcept
{
  for (std::uint8_t slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    if (_universe.FleetInSlot(_faction, slot) != Universe::INVALID_FLEET_ID)
      return true;
  }
  return false;
}
} // namespace

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
  // right for a subscriber present from the first tick and wrong for one joining a running universe;
  // that caller passes Universe::DespawnHead() (ADR 0027).
  added.despawnCursor = _desc.openingDespawnCursor;
  added.shotCursor = _desc.openingShotCursor;

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

void Publisher::SetCentre(Handle _handle, const UniversePos& _centre) noexcept
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

void Publisher::ApplyOrders(Universe& _universe)
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

        // Ids resolve here and nowhere else, for every kind. A ship that died between the click and
        // this tick resolves to nothing and the order does nothing with it, rather than acting on
        // whichever ship swap-and-pop moved into its index (ADR 0005) -- and an id minted by another
        // shard resolves to nothing too, which is what stops a client naming what this universe does
        // not own. There is far less of it to do than there was: an order names a fleet now and
        // carries at most two ids, where a ship-list order carried up to 139 (ADR 0049).
        FleetOrder fleetOrder;
        LedgerRequest ledgerRequest;
        ComposeOrder composeOrder;
        if (ReadFleetOrder(message, fleetOrder))
        {
          // The smallest branch here, and deliberately: an order that names a fleet resolves one id
          // instead of a list, and the authority gate below is a comparison rather than a filter
          // over everything the message carried (ADR 0049). The subscriber's faction is what the
          // gate reads, so a client cannot order a slot that is not its own.
          Universe::FleetCommand command;
          command.kind = fleetOrder.kind;
          command.point = fleetOrder.point;
          command.facingRad = fleetOrder.facingRad;
          command.hasFacing = fleetOrder.hasFacing;
          command.station = _universe.ResolveEntity(fleetOrder.station);
          command.target = _universe.ResolveEntity(fleetOrder.target);
          command.gate = _universe.ResolveEntity(fleetOrder.gate);
          (void)_universe.IssueFleetOrder(subscriber.faction, fleetOrder.slot, command);
        }
        else if (ReadLedgerRequest(message, ledgerRequest))
        {
          // Answered here, on the tick the question was read, and not queued for Publish: a reply
          // is an answer rather than an announcement, so it belongs at the question. Holding it for
          // this subscriber's phase would make the assembly screen's opening wait on an update slot
          // for no reason at all (ADR 0051).
          LedgerReply reply;
          reply.station = ledgerRequest.station;

          // A station that is not one, or is gone, is answered with zeros rather than with silence.
          // The screen has to open on something, and a reply that never comes is indistinguishable
          // from a lost one -- which would leave a player looking at a spinner over a dead dock.
          const ShipId structure = _universe.ResolveEntity(ledgerRequest.station);
          _universe.LedgerFor(_universe.StationAt(structure), subscriber.faction, reply.hullCounts);
          (void)WriteLedgerReply(reply, *subscriber.transport);
        }
        else if (ReadComposeOrder(message, composeOrder))
        {
          // Every gate is Universe's, including the standing gate and the slot gate, for ADR 0014's
          // reason; the issuing faction is the subscriber's and never the message's, so a client
          // cannot compose out of somebody else's ledger by saying it is somebody else.
          const ShipId structure = _universe.ResolveEntity(composeOrder.station);
          (void)_universe.ComposeFleet(_universe.StationAt(structure), composeOrder.slot, composeOrder.hullCounts, subscriber.faction);
        }
        // A message this half does not understand is dropped, not fatal.
      }
    }
  }
}

void Publisher::Publish(Universe& _universe)
{
  for (Subscriber& subscriber : m_subscribers)
  {
    if (subscriber.transport == nullptr)
      continue;

    // Rosters first, and on every tick rather than on the due ones. A roster is what says who is in
    // a fleet; the status block that rides the update says where it is and what it is doing, and a
    // block describing a membership the client has not been told yet is a button that cannot draw
    // itself (Design/Archive/Fleets.md 8.1).
    PublishRosters(_universe, subscriber);

    if (subscriber.interest.IsDueOn(_universe.Tick(), subscriber.phase))
      PublishOne(_universe, subscriber);
  }

  // Then, and only then, the log may forget. The minimum across every subscriber is what is safe to
  // drop -- anything newer is still owed to whichever one is furthest behind (ADR 0027). With no
  // subscribers at all the head is the minimum, because nobody is owed anything.
  std::uint64_t minimum = _universe.DespawnHead();
  for (const Subscriber& subscriber : m_subscribers)
    minimum = std::min(minimum, subscriber.despawnCursor);
  _universe.TrimDespawnsBefore(minimum);

  // And the shot log, on the same terms: what remains is what at least one subscriber has still to
  // hear about, and with no subscribers the head is the minimum because nobody is owed anything.
  std::uint64_t oldestShot = _universe.ShotHead();
  for (const Subscriber& subscriber : m_subscribers)
    oldestShot = std::min(oldestShot, subscriber.shotCursor);
  _universe.TrimShotsBefore(oldestShot);
}

void Publisher::PublishRosters(const Universe& _universe, Subscriber& _subscriber)
{
  // The publisher's own scratch rather than a local, so the common case -- five slots, nothing
  // changed -- allocates nothing. Every other buffer here is held for the same reason.
  FleetRoster& roster = m_rosterScratch;
  for (std::uint8_t slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    // The universe's own membership for this slot, as ids. The fleet holds handles because a reference
    // that outlives a tick has to (ADR 0005); the wire holds identities because a client has never
    // been given a handle and could not interpret one (ADR 0047). This is where the two meet, the
    // same meeting SplitTheLost is.
    roster.slot = slot;
    roster.members.clear();

    const Universe::FleetId id = _universe.FleetInSlot(_subscriber.faction, slot);
    if (id != Universe::INVALID_FLEET_ID)
    {
      const Universe::Fleet& fleet = _universe.FleetOf(id);
      for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
      {
        const EntityId entity = _universe.EntityIdOf(fleet.members[at]);
        if (entity != INVALID_ENTITY_ID)
          roster.members.push_back(entity);
      }
    }

    if (roster.members == _subscriber.lastRoster[slot])
      continue;

    // Recorded as sent even if the lane refuses it. Nothing on this seam retries -- a refused leave
    // is counted and dropped, and a roster is the same shape of fact -- and re-sending it next tick
    // would turn one refusal into a message every tick for as long as the lane stayed full, which is
    // the opposite of what a full lane needs. The mask keeps telling the truth about the slot
    // meanwhile, which is the reason occupancy rides the update and membership does not.
    _subscriber.lastRoster[slot] = roster.members;
    (void)WriteFleetRoster(roster, *_subscriber.transport);
  }
}

void Publisher::PublishOne(const Universe& _universe, Subscriber& _subscriber)
{
  _subscriber.interest.Update(_universe, _subscriber.centre);

  // Entered and refreshed go on the wire together: the receiver upserts either way and the
  // distinction is the sender's, not the format's.
  m_sendScratch.clear();
  m_sendScratch.insert(m_sendScratch.end(), _subscriber.interest.Entered().begin(), _subscriber.interest.Entered().end());
  m_sendScratch.insert(m_sendScratch.end(), _subscriber.interest.Refreshed().begin(), _subscriber.interest.Refreshed().end());

  SplitTheLost(_universe, _subscriber);

  // The gunfire, filtered to what this subscriber can see either end of.
  //
  // Either end, and the target end is the one that matters: being shot at from outside your own
  // interest set is exactly the event a player must not be denied. The three departure lists are
  // consulted after the subscribed set because they cover the one case a live handle cannot -- a
  // ship that died in view on this very update, which is the shot a player most wants to have seen.
  //
  // The cursor advances whether or not anything is sent, exactly as the despawn cursor does: gunfire
  // nobody could see has nobody to tell.
  const std::span<const ShipHandle> subscribed = _subscriber.interest.Subscribed();
  const auto sees = [&](EntityId _entity)
  {
    const ShipHandle handle = _universe.HandleOfEntity(_entity);
    if (handle.generation != 0 && std::binary_search(subscribed.begin(), subscribed.end(), handle, HandleOrderBefore))
      return true;
    return Names(m_destroyedScratch, _entity) || Names(m_dockedScratch, _entity) || Names(m_jumpedScratch, _entity) ||
           Names(m_leftScratch, _entity);
  };

  m_fireScratch.clear();
  for (const ShotRecord& shot : _universe.ShotsSince(_subscriber.shotCursor))
  {
    if (sees(shot.shooter) || sees(shot.victim))
      m_fireScratch.push_back(shot);
  }
  _subscriber.shotCursor = _universe.ShotHead();
  if (!m_fireScratch.empty())
    (void)_subscriber.writer.WriteFire(_universe.Tick(), m_fireScratch, *_subscriber.transport);

  // An empty update is not information -- unless this subscriber has a fleet, in which case the
  // header alone is. The case this guard would otherwise break is the one the whole feature is for:
  // four of five fleets are routinely outside the interest set, so a player whose camera is over
  // empty space would be told nothing about any of them. A zero-record fragment carrying a status
  // block is 28 to 98 bytes at the update rate, and it is the only thing that says where they are
  // (Design/Archive/Fleets.md 8.2, Design/Archive/Fleets-slice-5.md 2.8).
  if (m_sendScratch.empty() && _subscriber.interest.Left().empty() && !HasAnyFleet(_universe, _subscriber.faction))
    return;

  // The subscriber's faction is what the header's hostileMask is stated for. The publisher is the
  // only thing that knows whose view an update is; this is not a second authority check.
  (void)_subscriber.writer.WriteInterest(_universe, m_sendScratch, m_leftScratch, m_destroyedScratch, m_dockedScratch, m_jumpedScratch,
                                         *_subscriber.transport, _subscriber.faction);
}

void Publisher::SplitTheLost(const Universe& _universe, Subscriber& _subscriber)
{
  const std::span<const ShipHandle> left = _subscriber.interest.Left();
  m_destroyedScratch.clear();
  m_dockedScratch.clear();
  m_jumpedScratch.clear();
  m_departedScratch.clear();

  // Four sets from two: a departure carries a cause, so this is where a death is told apart from a
  // docking, from a jump, and all three from a ship that merely flew out of range (ADR 0040, 0056).
  //
  // This subscriber's own cursor, so two of them reading the same death both see it. The cursor
  // advances whether or not anything is sent, since a death nobody held has nobody to tell.
  //
  // The two stated causes come off the log as *ids*, because their ships are already gone and the
  // universe can no longer be asked who they were -- which is why the log carries one (ADR 0047). The
  // handles go into a separate list, used only to work out who is left over.
  for (const DespawnRecord& gone : _universe.DespawnsSince(_subscriber.despawnCursor))
  {
    if (!std::binary_search(left.begin(), left.end(), gone.handle, HandleOrderBefore))
      continue;
    m_departedScratch.push_back(gone.handle);
    // Switched rather than tested against one cause, because the default branch is a DESTROY and a
    // cause that fell through it would detonate a ship that is alive somewhere else. That is exactly
    // what a jump did before this run existed (ADR 0056).
    switch (gone.cause)
    {
    case DespawnCause::Docked:
      m_dockedScratch.push_back(gone.entity);
      break;
    case DespawnCause::JumpedOut:
      m_jumpedScratch.push_back(gone.entity);
      break;
    case DespawnCause::Destroyed:
    default:
      m_destroyedScratch.push_back(gone.entity);
      break;
    }
  }
  _subscriber.despawnCursor = _universe.DespawnHead();

  // One sorted list to subtract instead of a set_union of two, which is what the currency split
  // bought: the causes no longer have to be sorted in the same terms they are sent in.
  std::sort(m_departedScratch.begin(), m_departedScratch.end(), HandleOrderBefore);

  m_leftScratch.clear();
  for (const ShipHandle handle : left)
  {
    if (std::binary_search(m_departedScratch.begin(), m_departedScratch.end(), handle, HandleOrderBefore))
      continue;

    // Anything the log did not claim is still alive: the log is trimmed to the minimum cursor across
    // subscribers, so no subscriber can be past a death it has not been shown (ADR 0027). A handle
    // that resolves to nothing here would therefore be a ship this subscriber will never be told
    // about -- a ghost -- so it is dropped rather than sent as a null id, which would remove nothing.
    const EntityId entity = _universe.EntityIdOf(handle);
    if (entity != INVALID_ENTITY_ID)
      m_leftScratch.push_back(entity);
  }
}
} // namespace Game
