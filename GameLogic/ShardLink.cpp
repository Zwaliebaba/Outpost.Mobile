#include "pch.h"
#include "ShardLink.h"

#include "UniverseSnapshot.h"

namespace Game
{
ShardLink::Pumped ShardLink::Pump(Universe& _universe, Neuron::Transport& _transport)
{
  Pumped pumped;

  // --- receive ------------------------------------------------------------------------------------
  m_messageScratch.resize(Neuron::MAX_RELIABLE_BYTES);
  for (;;)
  {
    const std::uint32_t size = _transport.ReceiveReliable(m_messageScratch.data(), Neuron::MAX_RELIABLE_BYTES);
    if (size == 0)
      break;
    const std::span<const std::uint8_t> message(m_messageScratch.data(), size);

    if (ReadHandoffs(message, m_handoffScratch))
    {
      for (const Universe::Handoff& handoff : m_handoffScratch)
      {
        _universe.DeliverHandoff(handoff);
        ++pumped.handoffsReceived;

        // Owed an acknowledgement, once this end has saved past it. Recorded once per sequence: a
        // re-send of something already queued must not owe a second ack, or the list would grow for
        // as long as the far side keeps re-sending -- which is exactly as long as it is not acked.
        bool known = false;
        for (const Arrived& owed : m_owed)
          known = known || owed.sequence == handoff.sequence;
        if (!known)
          m_owed.push_back(Arrived{.sequence = handoff.sequence, .tick = _universe.Tick()});
      }
      continue;
    }

    if (ReadHandoffAck(message, m_ackScratch))
    {
      const std::size_t before = _universe.Outbox().size();
      _universe.AcknowledgeHandoffs(m_ackScratch);
      pumped.entriesCleared += static_cast<std::uint32_t>(before - _universe.Outbox().size());
      continue;
    }

    // Anything else on this lane is not ours. Refused rather than guessed at, which is what makes
    // adding a kind cheap (UniverseSnapshot.cpp's dispatch keeps the same rule).
  }

  // --- acknowledge what is durable ----------------------------------------------------------------
  //
  // The rule ADR 0066 exists for: an ack says the entry is in this shard's FILE. Anything queued
  // after the last save stays owed, is not acked, and is therefore re-sent by the far side -- which
  // is at-least-once delivery doing the job it was chosen for.
  m_ackScratch.clear();
  for (const Arrived& owed : m_owed)
  {
    if (owed.tick <= m_durableTick)
      m_ackScratch.push_back(owed.sequence);
  }
  if (!m_ackScratch.empty() && WriteHandoffAck(m_ackScratch, _transport))
  {
    pumped.acksSent = static_cast<std::uint32_t>(m_ackScratch.size());
    // Struck off only once the lane took them. A refused ack is re-offered next pump, and the far
    // side re-sends in the meantime, which costs a message and loses nothing.
    std::vector<Arrived> stillOwed;
    for (const Arrived& owed : m_owed)
    {
      bool acked = false;
      for (const std::uint64_t sequence : m_ackScratch)
        acked = acked || sequence == owed.sequence;
      if (!acked)
        stillOwed.push_back(owed);
    }
    m_owed.swap(stillOwed);
  }

  // --- re-send what is not acknowledged -----------------------------------------------------------
  //
  // On a cadence. Every tick would put a fleet's worth of messages on the lane sixty times a second
  // for as long as one crossing is unacknowledged.
  const std::uint64_t tick = _universe.Tick();
  const bool due = m_lastSendTick == 0 || tick >= m_lastSendTick + HANDOFF_RESEND_TICKS;
  if (due && !_universe.Outbox().empty())
  {
    m_lastSendTick = tick;
    std::uint32_t sent = 0;
    if (WriteHandoffs(_universe.Outbox(), _transport, sent))
      pumped.handoffsSent = sent;
    else
      pumped.laneRefused = true; // the entry stays in the outbox, which is the whole point
  }
  return pumped;
}
} // namespace Game
