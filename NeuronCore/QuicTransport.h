#pragma once

#include "QuicApi.h"
#include "Transport.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Neuron
{
class QuicListener;

// Where a client end dials. Both the composition root and the tests pass 127.0.0.1, because nothing
// in this design listens on anything else (Design/QuicTransport.md 11); the field exists so that the
// day one does, it is an argument and not a rewrite.
struct Endpoint
{
  const char* host = "127.0.0.1";
  std::uint16_t port = 0;
};

// One QUIC connection, as a Transport. Datagrams are QUIC DATAGRAM frames: unreliable, unordered and
// bounded, which is exactly what LoopbackTransport already promised, so the wire format above does
// not change and neither half of the game learns which transport it got.
//
// THE THREADING RULE IS THE WHOLE OF THIS CLASS. MsQuic delivers on its own worker threads and the
// engine is single-threaded, so foreign threads only ever enqueue (AGENTS.md 5, Transport.h:46).
// Inbound, a worker copies into a fixed-stride ring under one lock and bumps a write cursor; Poll,
// on the owning thread, copies that cursor into a ready cursor, and Receive pops below it without
// the lock, because everything below ready belongs to the reader and nothing above it is touched by
// anyone but the writer. Outbound, Send copies into a second ring and hands MsQuic the slot, which a
// worker marks free when the send reaches a final state -- one atomic per slot, no lock at all.
// Nothing else crosses: every other event sets an atomic and returns.
//
// Nothing allocates after Connect. Both rings are capacityDatagrams x MAX_DATAGRAM_BYTES, taken once,
// which is LoopbackTransport's arena exactly -- so the two behave alike under pressure, including
// "a full queue drops the newest, it does not block".
class QuicTransport final : public Transport
{
public:
  struct Desc
  {
    // Per ring, fixed at Connect. Two rings of 256 datagrams is 288 KB each.
    std::uint32_t capacityDatagrams = 256;
  };

  QuicTransport() = default;

  // Waits for SHUTDOWN_COMPLETE, bounded by the idle timeout, before closing the connection: MsQuic
  // may still be calling back into an object that is being torn down. It is the one place this class
  // blocks -- in a destructor, on the owning thread, at shutdown, which AGENTS.md 5 allows to do
  // nothing rather than report.
  ~QuicTransport() override;

  // The client half: opens a connection to _peer and starts the handshake. False and Reason() when
  // MsQuic refuses; the handshake itself completes later, and State() is what says so.
  [[nodiscard]] bool Connect(QuicApi& _api, const Endpoint& _peer, const Desc& _desc);

  // Asks for a graceful shutdown and returns. The state goes Draining now and Closed when MsQuic
  // says so; the destructor is what waits.
  void Close() noexcept;

  [[nodiscard]] bool Send(const std::uint8_t* _bytes, std::uint32_t _count) override;
  [[nodiscard]] std::uint32_t Receive(std::uint8_t* _outBytes, std::uint32_t _capacity) override;
  void Poll() override;
  [[nodiscard]] ConnectionState State() const override;

  // Inbound datagrams the ring had no room for, counted on the worker that dropped them. Diagnostics,
  // and what the full-ring test asserts against.
  [[nodiscard]] std::uint32_t DroppedCount() const noexcept;

  // The largest datagram the peer will take, as MsQuic reports it; 0 until the handshake has said.
  // A peer that will not take MAX_DATAGRAM_BYTES never reaches Connected -- a transport that
  // silently truncates is the bug the loopback refuses to have.
  [[nodiscard]] std::uint32_t MaxSendLength() const noexcept;

  [[nodiscard]] const char* Reason() const noexcept;

private:
  friend class QuicListener;

  // The static thunk MsQuic actually calls lives in the .cpp, because its signature names MsQuic
  // types and no header here may. It reaches this object's state through this declaration, and
  // nothing else does.
  friend struct QuicTransportCallbacks;

  // Allocates the two rings and binds this end to the library, on the owning thread and before any
  // connection exists. QuicListener::Start calls it once per transport in its backlog, which is what
  // keeps the accept path -- an MsQuic worker -- free of allocation.
  void Reserve(QuicApi& _api, const Desc& _desc);

  // Takes an accepted connection, on an MsQuic worker. Allocates nothing; Reserve has been here
  // already. False when this end is already carrying a connection.
  [[nodiscard]] bool Adopt(void* _connection);

  void ReleaseApiHandle() noexcept;

  // Both halves of the state table in Design/QuicTransport.md 4.1. Connected needs the handshake
  // *and* a datagram limit that covers MAX_DATAGRAM_BYTES, so the two events that can grant it both
  // come through here.
  void ReconsiderConnected();

  void SetReason(const char* _format, ...) noexcept;

  QuicApi* m_api = nullptr;
  void* m_connection = nullptr; // HQUIC, void* for the reason at the top of QuicApi.h

  // Written by MsQuic's workers, read by the owning thread. These four are the whole of what crosses
  // apart from the rings.
  std::atomic<ConnectionState> m_state{ConnectionState::Disconnected};
  std::atomic<std::uint32_t> m_maxSendLength{0};
  std::atomic<std::uint32_t> m_dropped{0};
  std::atomic<bool> m_handshakeDone{false};
  std::atomic<bool> m_datagramSendEnabled{false};

  // Inbound: one arena, one size per slot, and free-running cursors. m_inWrite is the worker's,
  // m_inRead is the reader's, and m_inReady is the owning thread's copy of m_inWrite taken by Poll --
  // which is what makes "delivery happens in Poll" true rather than merely intended.
  std::vector<std::uint8_t> m_inArena;
  std::vector<std::uint32_t> m_inSizes;
  std::mutex m_inLock;
  std::atomic<std::uint32_t> m_inWrite{0};
  std::atomic<std::uint32_t> m_inRead{0};
  std::uint32_t m_inReady = 0;

  // The slot each cursor is standing on, carried rather than derived as `cursor % capacity`: the
  // counters above are free-running 32-bit values, and 2^32 is not a multiple of every capacity, so
  // deriving the slot would misplace one ring's worth of datagrams at the wrap. m_inWriteSlot is the
  // workers' and moves under m_inLock; m_inReadSlot is the owning thread's alone.
  std::uint32_t m_inWriteSlot = 0;
  std::uint32_t m_inReadSlot = 0;

  // Outbound. MsQuic keeps the descriptor array *and* the bytes it points at until a send reaches a
  // final state, so both live in this object rather than on Send's stack.
  //
  // SendBuffer is QUIC_BUFFER, spelled out rather than included, because no header in this tree
  // names an MsQuic type. That is a layout claim and it is checked rather than trusted: Reserve
  // static_asserts this against the real QUIC_BUFFER, so the day the package changes it, the build
  // says so instead of the wire going quietly wrong.
  struct SendBuffer
  {
    std::uint32_t length = 0;
    std::uint8_t* bytes = nullptr;
  };

  std::vector<std::uint8_t> m_outArena;
  std::vector<SendBuffer> m_outBuffers;
  std::vector<std::atomic<bool>> m_outInFlight;
  std::uint32_t m_outCursor = 0;

  std::uint32_t m_capacity = 0;
  bool m_holdsApiHandle = false;

  // The destructor's bounded wait for SHUTDOWN_COMPLETE.
  std::mutex m_closedLock;
  std::condition_variable m_closed;

  char m_reason[QUIC_REASON_BYTES] = {};
};
} // namespace Neuron
