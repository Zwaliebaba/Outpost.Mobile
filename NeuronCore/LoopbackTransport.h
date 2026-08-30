#pragma once

#include "Transport.h"

#include <cstdint>
#include <vector>

namespace Neuron
{
// A Transport whose far end is in this process, with latency and loss you can dial in.
//
// It is the first implementation of the seam Transport.h declares, and it exists for two reasons.
// The first is structural: from here the client half reaches the world only through datagrams, so
// the day a socket replaces this class neither half notices. The second is that it is an
// instrument. Several open questions in Design/Collision.md -- whether ships should hard-block,
// whether avoidance can be predicted, how large the separation clamp should be -- are questions
// about how a correction feels under lag, and none of them is answerable without lag in the loop.
//
// LATENCY IS COUNTED IN TICKS, NOT SECONDS, and that is deliberate. A wall clock would make
// delivery depend on how fast the machine ran, which makes a measurement irreproducible and a test
// flaky, and AGENTS.md 5 bans a clock in the simulation for that reason. The tick is also the unit
// the question is really asked in: "seven ticks at 60 Hz" is what 120 ms of lag means to a
// simulation that only advances on ticks. AdvanceTo is on this class rather than on Transport
// because a socket has real latency and no tick -- this is an instrument's knob, not a wire's
// (Design/Archive/Collision-slice-2b.md 2.1).
class LoopbackTransport final : public Transport
{
public:
  struct Desc
  {
    // 0 is the single-player default and has to mean genuinely zero: a datagram sent this tick is
    // readable this tick, or the game gains a frame of lag it did not have before.
    std::uint32_t latencyTicks = 0;

    // Fixed at Connect, so nothing allocates once the game is running.
    std::uint32_t capacityDatagrams = 256;

    // The reliable lane's depth, counted separately and much shallower on purpose: a message on
    // that lane is up to MAX_RELIABLE_BYTES where a datagram is up to MAX_DATAGRAM_BYTES, so 256 of
    // them would be 2 MB an end for a lane that carries leaves and orders rather than a position per
    // ship per tick. Deep enough that an update's worth queues; shallow enough to be free.
    std::uint32_t capacityReliableMessages = 32;

    // Drop every Nth datagram; 0 disables. Counted rather than randomised because AGENTS.md 5 bans
    // unseeded randomness, and because reproducible loss is the only kind worth testing against.
    std::uint32_t dropOneInN = 0;
  };

  // Wires two ends to each other. Neither owns the other, and both must outlive the pair.
  static void Connect(LoopbackTransport& _a, LoopbackTransport& _b, const Desc& _desc);

  // The tick this end believes it is on. Datagrams it sends are stamped due at _tick + latency, and
  // Poll delivers what has come due.
  void AdvanceTo(std::uint64_t _tick) noexcept;

  [[nodiscard]] std::uint64_t Tick() const noexcept
  {
    return m_tick;
  }

  [[nodiscard]] bool Send(const std::uint8_t* _bytes, std::uint32_t _count) override;
  [[nodiscard]] std::uint32_t Receive(std::uint8_t* _outBytes, std::uint32_t _capacity) override;

  // The reliable lane. It carries the same latency as the datagram lane -- a wire's delay is the
  // wire's, whichever lane a message took -- and none of its loss: dropOneInN does not apply here,
  // because a lane that drops is not the lane Transport.h declares. A full queue still refuses,
  // which is backpressure rather than loss and is the one false this may return.
  [[nodiscard]] bool SendReliable(const std::uint8_t* _bytes, std::uint32_t _count) override;
  [[nodiscard]] std::uint32_t ReceiveReliable(std::uint8_t* _outBytes, std::uint32_t _capacity) override;

  void Poll() override;
  [[nodiscard]] ConnectionState State() const override;

  // How many datagrams this end has been handed and not yet delivered. Diagnostics, and what the
  // queue-full test asserts against.
  [[nodiscard]] std::uint32_t QueuedCount() const noexcept;

  // The same, for the reliable lane.
  [[nodiscard]] std::uint32_t QueuedReliableCount() const noexcept;

private:
  struct Slot
  {
    std::uint64_t dueTick = 0;
    std::uint32_t size = 0;
  };

  // Called on the receiving end by the sending one.
  [[nodiscard]] bool Accept(const std::uint8_t* _bytes, std::uint32_t _count, std::uint64_t _dueTick);
  [[nodiscard]] bool AcceptReliable(const std::uint8_t* _bytes, std::uint32_t _count, std::uint64_t _dueTick);

  LoopbackTransport* m_peer = nullptr;
  ConnectionState m_state = ConnectionState::Disconnected;
  std::uint64_t m_tick = 0;
  std::uint32_t m_latencyTicks = 0;
  std::uint32_t m_dropOneInN = 0;
  std::uint32_t m_sendCounter = 0;

  // One allocation at Connect: a ring of fixed-width slots over one flat byte arena. A datagram is
  // MAX_DATAGRAM_BYTES at most, so a fixed stride costs memory and buys never allocating again.
  std::vector<Slot> m_slots;
  std::vector<std::uint8_t> m_arena;
  std::uint32_t m_head = 0;  // oldest queued
  std::uint32_t m_count = 0; // queued, delivered or not
  std::uint32_t m_ready = 0; // of those, how many Poll has delivered and Receive may take

  // The reliable lane's own ring, laid out the same way and sized the same at Connect. Separate
  // rather than a flag on a shared ring: one lane filling up must not refuse the other, and a
  // reliable message is up to MAX_RELIABLE_BYTES where a datagram is up to MAX_DATAGRAM_BYTES, so
  // the strides differ.
  std::vector<Slot> m_reliableSlots;
  std::vector<std::uint8_t> m_reliableArena;
  std::uint32_t m_reliableHead = 0;
  std::uint32_t m_reliableCount = 0;
  std::uint32_t m_reliableReady = 0;
};
} // namespace Neuron
