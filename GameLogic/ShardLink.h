#pragma once

#include "Universe.h"

#include "Transport.h"

#include <cstdint>
#include <vector>

namespace Game
{
// How often an unacknowledged handoff is re-sent, in ticks.
//
// Not every tick, and the number matters less than the fact that there is one: a fleet of eight in
// the outbox re-sent sixty times a second is 480 messages a second on a lane that carries orders and
// leaves. One second is far inside any save period and far outside any round trip.
inline constexpr std::uint64_t HANDOFF_RESEND_TICKS = 60;

// One shard's end of a link to another shard.
//
// It owns neither the universe nor the transport, exactly as SnapshotPublisher owns neither: both
// are handed in per pump, so a link can be re-pointed and nothing here outlives a frame it did not
// see. It is the piece that moves a Handoff, and the four calls it drives -- Outbox, DeliverHandoff,
// DrainInbox, AcknowledgeHandoffs -- all existed before it (Design/CrossShard-slice-4.md 1).
//
// **An acknowledgement means "this is in my file", not "this reached my process"**
// ([ADR 0066](../Design/Decisions/0066-an-acknowledgement-means-durable-not-delivered.md)). Acking on
// arrival would let the departing shard forget an entry that the arriving shard then loses in a
// crash before its next save -- and idempotence cannot help, because there would be nothing left to
// replay. So the owner tells the link which tick it has saved through, and nothing is acked past it.
class ShardLink
{
public:
  // What the last pump did, for a caller that wants to log or test it rather than infer it.
  struct Pumped
  {
    std::uint32_t handoffsSent = 0;
    std::uint32_t handoffsReceived = 0;
    std::uint32_t acksSent = 0;
    std::uint32_t entriesCleared = 0;
    bool laneRefused = false; // a send the transport would not take; the entry stays and is re-sent
  };

  // Tells the link that _tick and everything before it is on disk. Until an entry's arrival is
  // covered by this, no acknowledgement for it is sent.
  //
  // Called by whoever writes the save, which is the composition root -- the universe does not know
  // it is ever saved, and this class must not be the thing that decides when it is.
  void NoteDurableThrough(std::uint64_t _tick) noexcept
  {
    m_durableTick = _tick;
  }

  [[nodiscard]] std::uint64_t DurableTick() const noexcept
  {
    return m_durableTick;
  }

  // One turn of the loop: receive, then acknowledge what is durable, then re-send what is not
  // acknowledged. In that order on purpose -- a message received this pump can be acked in the same
  // one if the save already covers it, and re-sending last means a send never races an ack it just
  // made unnecessary.
  //
  // It does NOT drain the inbox. Applying a handoff is a tick-boundary act and belongs to whoever
  // owns the stepping, which is the same rule an order queue keeps (Design/CrossShard.md 4).
  Pumped Pump(Universe& _universe, Neuron::Transport& _transport);

private:
  // Sequence, and the tick this end had when the entry arrived. The tick is what the durability rule
  // compares against; without it an ack would be sent for something queued a moment ago.
  struct Arrived
  {
    std::uint64_t sequence = 0;
    std::uint64_t tick = 0;
  };

  std::uint64_t m_durableTick = 0;
  std::uint64_t m_lastSendTick = 0;

  // Entries delivered into the universe's inbox and not yet acknowledged, with when they landed.
  std::vector<Arrived> m_owed;

  // Reused between pumps, like every other scratch in this library.
  std::vector<std::uint8_t> m_messageScratch;
  std::vector<Universe::Handoff> m_handoffScratch;
  std::vector<std::uint64_t> m_ackScratch;
};
} // namespace Game
