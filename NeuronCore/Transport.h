#pragma once

#include <cstdint>

namespace Neuron
{
// The declared seam between the client and server halves.
//
// NOTHING IMPLEMENTS THIS YET. The game currently runs both halves in one process and the client
// reads the world directly (AGENTS.md 2 and Design/Collision.md 2). This interface is here so that the seam
// is a named thing with an owner rather than a plan: when the halves are separated, a loopback
// implementation lands first and a network one after it, and neither half changes.
//
// The rule the seam exists to protect: the two halves communicate only through a Transport. No
// shared memory, no cross-half calls, no singleton bridging them -- because anything that works
// in-process and not over a wire is a bug that only shows up on the day it is expensive to find.

// A datagram that fits inside the smallest MTU worth designing for, with room for headers.
inline constexpr std::uint32_t MAX_DATAGRAM_BYTES = 1152;

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

  // Delivery happens here, on the owning thread. Foreign threads may only enqueue.
  virtual void Poll() = 0;

  [[nodiscard]] virtual ConnectionState State() const = 0;

protected:
  Transport() = default;
};
} // namespace Neuron
