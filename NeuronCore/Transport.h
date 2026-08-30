#pragma once

#include <cstdint>

namespace Neuron
{
// The declared seam between the client and server halves.
//
// LoopbackTransport and QuicTransport implement it, and the client half reaches the world only
// through one of them: the game still runs both halves in one process, but no longer by letting one
// read the other's memory (AGENTS.md 2 and Design/Archive/Collision.md 2). The plan this comment used to
// describe has happened as written -- a loopback implementation first, a network one after it, and
// neither half changing -- so what remains is a second process, and nothing here is what would
// change on the day there is one.
//
// The rule the seam exists to protect: the two halves communicate only through a Transport. No
// shared memory, no cross-half calls, no singleton bridging them -- because anything that works
// in-process and not over a wire is a bug that only shows up on the day it is expensive to find.

// A datagram that fits inside the smallest MTU worth designing for, with room for headers.
inline constexpr std::uint32_t MAX_DATAGRAM_BYTES = 1152;

// The most one reliable message may carry. Deliberately not MAX_DATAGRAM_BYTES and not derived from
// it: a datagram is bounded by what the path will carry in one piece, a reliable message by what the
// receiver is willing to hold while the rest of it arrives. The lane is a stream underneath, so a
// message may exceed an MTU and be reassembled -- what it may not do is grow without a bound the
// receiver agreed to (Design/ReliableLane-work-order.md).
inline constexpr std::uint32_t MAX_RELIABLE_BYTES = 8192;

enum class ConnectionState : std::uint8_t
{
  Disconnected,
  Connecting,
  Connected,
  Draining,
  Closed
};

class Transport
{
public:
  virtual ~Transport() = default;

  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  // Queues one datagram. False means the send queue is full, which is a normal condition and not
  // an error: the caller drops the message rather than blocking a frame on it.
  [[nodiscard]] virtual bool Send(const std::uint8_t* _bytes, std::uint32_t _count) = 0;

  // Copies the next queued datagram out, returning its size, or 0 when nothing is waiting.
  [[nodiscard]] virtual std::uint32_t Receive(std::uint8_t* _outBytes, std::uint32_t _capacity) = 0;

  // The reliable lane: ordered, never dropped, and framed as messages rather than bytes.
  //
  // Non-pure, and the defaults refuse -- false and 0, exactly as a full queue and an empty one
  // answer on the lane above. A Transport without a lane is a real thing (a capture double in a
  // test, an implementation that predates this) and it should compile and behave rather than be
  // forced to write two stubs. A caller that needs the lane checks the return like any other send.
  //
  // What belongs here: a fact that is wrong forever if it is lost -- a leave, a death, an order.
  // What does not: anything a later message supersedes, above all a position, where a late one is
  // worse than a lost one (Design/QuicTransport.md 8).
  // Level 4 warns on an unused parameter, and these are named rather than left blank because the
  // names are the documentation of what an implementation is being handed.
  [[nodiscard]] virtual bool SendReliable([[maybe_unused]] const std::uint8_t* _bytes, [[maybe_unused]] std::uint32_t _count)
  {
    return false;
  }

  [[nodiscard]] virtual std::uint32_t ReceiveReliable([[maybe_unused]] std::uint8_t* _outBytes, [[maybe_unused]] std::uint32_t _capacity)
  {
    return 0;
  }

  // Delivery happens here, on the owning thread. Foreign threads may only enqueue. It drains both
  // lanes; there is no second Poll.
  virtual void Poll() = 0;

  [[nodiscard]] virtual ConnectionState State() const = 0;

protected:
  Transport() = default;
};
} // namespace Neuron
