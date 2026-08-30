#include "pch.h"

// MsQuic after pch.h and before anything else, for the include-order reason at the top of
// QuicApi.cpp: pch.h has already put Winsock ahead of <windows.h>, and QUIC_ADDR needs that.
// msquic.hpp is deliberately not here: nothing in this file uses its wrapper types, and it drags in
// msquicp.h, the private header MsQuic reserves the right to change without warning.
#include <msquic.h>

#include "QuicTransport.h"

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace Neuron
{
namespace
{
// The one application error code this transport shuts a connection down with: the peer would not
// take a datagram of MAX_DATAGRAM_BYTES.
constexpr QUIC_UINT62 QUIC_ERROR_DATAGRAM_TOO_SMALL = 1;

// A stream is bytes and this lane's contract is messages, so every message goes out behind a
// two-byte little-endian length. Two bytes and not four because MAX_RELIABLE_BYTES fits in one --
// the static_assert below is what keeps that true if the bound ever moves.
constexpr std::uint32_t RELIABLE_HEADER_BYTES = 2;
static_assert(MAX_RELIABLE_BYTES <= 0xFFFFu, "a reliable frame header is two bytes and could not carry MAX_RELIABLE_BYTES");

// A slot index travels to the send-completion callback as MsQuic's ClientContext, which is a void*.
// Biased by one, because MsQuic also accepts a null context and slot 0 would be indistinguishable
// from it.
[[nodiscard]] void* SlotToContext(std::uint32_t _slot) noexcept
{
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(_slot) + 1u);
}

[[nodiscard]] std::uint32_t ContextToSlot(void* _context) noexcept
{
  return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(_context) - 1u);
}
} // namespace

// MsQuic's connection callback: the only code in this file that runs on a foreign thread.
//
// It allocates nothing and logs nothing. Everything it learns it writes to an atomic or copies into
// the inbound ring, and the owning thread picks it up in Poll. It calls exactly one MsQuic API, in
// exactly one case -- shutting the connection down when the peer's datagram limit is too small,
// which is what Design/QuicTransport.md 4.1 asks for and is the ordinary way to refuse a connection
// from inside its own callback.
struct QuicTransportCallbacks
{
  static QUIC_STATUS QUIC_API OnConnectionEvent(HQUIC, void* _context, QUIC_CONNECTION_EVENT* _event)
  {
    QuicTransport& end = *static_cast<QuicTransport*>(_context);

    switch (_event->Type)
    {
    case QUIC_CONNECTION_EVENT_CONNECTED:
      end.m_handshakeDone.store(true, std::memory_order_release);
      end.ReconsiderConnected();
      break;

    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
      end.m_datagramSendEnabled.store(_event->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE, std::memory_order_release);
      end.m_maxSendLength.store(_event->DATAGRAM_STATE_CHANGED.MaxSendLength, std::memory_order_release);
      end.ReconsiderConnected();
      break;

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
    {
      const QUIC_BUFFER* const received = _event->DATAGRAM_RECEIVED.Buffer;
      if (received == nullptr || received->Length > MAX_DATAGRAM_BYTES || end.m_capacity == 0)
        break;

      const std::lock_guard<std::mutex> lock(end.m_inLock);
      const std::uint32_t write = end.m_inWrite.load(std::memory_order_relaxed);
      const std::uint32_t read = end.m_inRead.load(std::memory_order_acquire);
      if (write - read >= end.m_capacity)
      {
        // A full ring drops the newest and counts it, which is LoopbackTransport::Accept's rule.
        // Keeping what is already in flight matters more than taking one more.
        end.m_dropped.fetch_add(1u, std::memory_order_relaxed);
        break;
      }

      // The slot is carried, not computed from the counter. `write % capacity` would be wrong for one
      // ring's worth of datagrams every time the 32-bit counter wraps, for any capacity that does not
      // divide 2^32 -- 13 years away at 10 Hz, and not a thing to leave in a ring.
      const std::uint32_t at = end.m_inWriteSlot;
      if (received->Length > 0)
        std::memcpy(end.m_inArena.data() + static_cast<std::size_t>(at) * MAX_DATAGRAM_BYTES, received->Buffer, received->Length);
      end.m_inSizes[at] = received->Length;
      end.m_inWriteSlot = (at + 1u) % end.m_capacity;
      end.m_inWrite.store(write + 1u, std::memory_order_release);
      break;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
      if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(_event->DATAGRAM_SEND_STATE_CHANGED.State))
      {
        const std::uint32_t slot = ContextToSlot(_event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
        if (slot < end.m_capacity)
          end.m_outInFlight[slot].store(false, std::memory_order_release);
      }
      break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
      // The accepting end's half of the lane. SetCallbackHandler is the one MsQuic call ADR 0022
      // sanctions on a worker, and it is what AdoptReliableStream does; nothing else here allocates
      // or calls back into the library.
      end.AdoptReliableStream(_event->PEER_STREAM_STARTED.Stream);
      break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
    {
      ConnectionState draining = end.m_state.load(std::memory_order_acquire);
      while (draining != ConnectionState::Draining && draining != ConnectionState::Closed &&
             !end.m_state.compare_exchange_weak(draining, ConnectionState::Draining))
      {
      }
      break;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
    {
      // Under the lock the destructor's wait uses, so that a state set here cannot land between that
      // wait's predicate test and its sleep.
      const std::lock_guard<std::mutex> lock(end.m_closedLock);
      end.m_state.store(ConnectionState::Closed, std::memory_order_release);
      end.m_closed.notify_all();
      break;
    }

    default:
      break;
    }

    return QUIC_STATUS_SUCCESS;
  }

  // The lane's stream callback, on a worker like the one above and under the same rule: it copies
  // into a ring and sets atomics, and calls no MsQuic API at all.
  static QUIC_STATUS QUIC_API OnStreamEvent(HQUIC, void* _context, QUIC_STREAM_EVENT* _event)
  {
    QuicTransport& end = *static_cast<QuicTransport*>(_context);

    switch (_event->Type)
    {
    case QUIC_STREAM_EVENT_START_COMPLETE:
      // The dialling end's stream is usable once MsQuic says it started. The accepting end sets this
      // in AdoptReliableStream, because a stream it was handed is already started.
      if (QUIC_SUCCEEDED(_event->START_COMPLETE.Status))
        end.m_streamReady.store(true, std::memory_order_release);
      break;

    case QUIC_STREAM_EVENT_RECEIVE:
    {
      // A stream delivers bytes, in as many buffers as it likes and split wherever it likes, so the
      // frames are reassembled here rather than assumed. The whole event is handled under the ring's
      // lock because the partial-frame buffer is shared between whichever workers deliver.
      const std::lock_guard<std::mutex> lock(end.m_streamInLock);
      for (std::uint32_t at = 0; at < _event->RECEIVE.BufferCount; ++at)
      {
        const QUIC_BUFFER& buffer = _event->RECEIVE.Buffers[at];
        std::uint32_t consumed = 0;
        while (consumed < buffer.Length)
        {
          const std::uint32_t room = static_cast<std::uint32_t>(end.m_streamPartial.size()) - end.m_streamPartialCount;
          const std::uint32_t take = (buffer.Length - consumed < room) ? buffer.Length - consumed : room;
          if (take == 0)
          {
            // Cannot happen while the buffer is one frame plus its header, and dropping the
            // reassembly is the only safe answer if it ever does: a desynchronised stream would
            // otherwise deliver garbage as messages for the rest of the connection.
            end.m_streamPartialCount = 0;
            end.m_dropped.fetch_add(1u, std::memory_order_relaxed);
            break;
          }
          std::memcpy(end.m_streamPartial.data() + end.m_streamPartialCount, buffer.Buffer + consumed, take);
          end.m_streamPartialCount += take;
          consumed += take;

          // Then take out every whole frame the buffer now holds, and shuffle the remainder down.
          for (;;)
          {
            if (end.m_streamPartialCount < RELIABLE_HEADER_BYTES)
              break;
            const std::uint32_t size =
              static_cast<std::uint32_t>(end.m_streamPartial[0]) | (static_cast<std::uint32_t>(end.m_streamPartial[1]) << 8);
            if (size > MAX_RELIABLE_BYTES)
            {
              // A frame the sender could not have written. The stream is no longer trustworthy, so
              // the reassembly is dropped rather than resynchronised by guessing.
              end.m_streamPartialCount = 0;
              end.m_dropped.fetch_add(1u, std::memory_order_relaxed);
              break;
            }
            const std::uint32_t frame = RELIABLE_HEADER_BYTES + size;
            if (end.m_streamPartialCount < frame)
              break;
            end.PushReliable(end.m_streamPartial.data() + RELIABLE_HEADER_BYTES, size);
            const std::uint32_t left = end.m_streamPartialCount - frame;
            if (left > 0)
              std::memmove(end.m_streamPartial.data(), end.m_streamPartial.data() + frame, left);
            end.m_streamPartialCount = left;
          }
        }
      }
      break;
    }

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
    {
      const std::uint32_t slot = ContextToSlot(_event->SEND_COMPLETE.ClientContext);
      if (slot < end.m_reliableCapacity)
        end.m_streamOutInFlight[slot].store(false, std::memory_order_release);
      break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      // The lane is gone. SendReliable refuses from here on, which the caller reads as backpressure
      // and the connection's own state explains.
      end.m_streamReady.store(false, std::memory_order_release);
      break;

    default:
      break;
    }

    return QUIC_STATUS_SUCCESS;
  }
};

QuicTransport::~QuicTransport()
{
  Close();
}

bool QuicTransport::Connect(QuicApi& _api, const Endpoint& _peer, const Desc& _desc)
{
  if (m_connection != nullptr)
  {
    SetReason("this end is already carrying a connection");
    return false;
  }
  if (!_api.IsOpen())
  {
    SetReason("the QUIC library is not open: %s", _api.Reason());
    return false;
  }

  Reserve(_api, _desc);

  const QUIC_API_TABLE* const table = _api.Table();
  HQUIC connection = nullptr;
  QUIC_STATUS status =
    table->ConnectionOpen(static_cast<HQUIC>(_api.Registration()), QuicTransportCallbacks::OnConnectionEvent, this, &connection);
  if (QUIC_FAILED(status))
  {
    SetReason("ConnectionOpen failed (0x%08x)", static_cast<unsigned>(status));
    return false;
  }

  // Set before the start, not after: MsQuic may call back on a worker before ConnectionStart has
  // returned, and every one of those callbacks reads this state.
  m_connection = connection;
  m_state.store(ConnectionState::Connecting, std::memory_order_release);
  _api.AcquireHandle();
  m_holdsApiHandle = true;

  // This end dialed, so this end opens the reliable lane's stream once the handshake lands. Set
  // before ConnectionStart, because Poll may run against a Connected state as soon as it returns.
  m_isDialer = true;

  status =
    table->ConnectionStart(connection, static_cast<HQUIC>(_api.ClientConfiguration()), QUIC_ADDRESS_FAMILY_INET, _peer.host, _peer.port);
  if (QUIC_FAILED(status))
  {
    SetReason("ConnectionStart to %s:%u failed (0x%08x)", _peer.host, static_cast<unsigned>(_peer.port), static_cast<unsigned>(status));
    Close();
    return false;
  }

  m_reason[0] = '\0';
  return true;
}

void QuicTransport::Reserve(QuicApi& _api, const Desc& _desc)
{
  // The one place this class allocates. Both rings are fixed-stride over a flat arena, exactly as
  // LoopbackTransport::Connect lays them out, so a datagram costs a memcpy and never a new.
  static_assert(sizeof(QuicTransport::SendBuffer) == sizeof(QUIC_BUFFER), "SendBuffer has drifted from QUIC_BUFFER's size");
  static_assert(offsetof(QuicTransport::SendBuffer, length) == offsetof(QUIC_BUFFER, Length),
                "SendBuffer::length has drifted from QUIC_BUFFER::Length");
  static_assert(offsetof(QuicTransport::SendBuffer, bytes) == offsetof(QUIC_BUFFER, Buffer),
                "SendBuffer::bytes has drifted from QUIC_BUFFER::Buffer");

  m_api = &_api;
  m_capacity = (_desc.capacityDatagrams > 0) ? _desc.capacityDatagrams : 1;

  const std::size_t arenaBytes = static_cast<std::size_t>(m_capacity) * MAX_DATAGRAM_BYTES;
  m_inArena.assign(arenaBytes, 0u);
  m_inSizes.assign(m_capacity, 0u);
  m_inWrite.store(0u, std::memory_order_relaxed);
  m_inRead.store(0u, std::memory_order_relaxed);
  m_inReady = 0;
  m_inWriteSlot = 0;
  m_inReadSlot = 0;

  m_outArena.assign(arenaBytes, 0u);
  m_outInFlight = std::vector<std::atomic<bool>>(m_capacity);
  m_outBuffers.assign(m_capacity, SendBuffer{});
  for (std::uint32_t slot = 0; slot < m_capacity; ++slot)
    m_outBuffers[slot].bytes = m_outArena.data() + static_cast<std::size_t>(slot) * MAX_DATAGRAM_BYTES;
  m_outCursor = 0;

  // The reliable lane's own rings, shallower because a message on it may be MAX_RELIABLE_BYTES.
  m_reliableCapacity = (_desc.capacityReliableMessages > 0) ? _desc.capacityReliableMessages : 1;
  const std::size_t reliableArenaBytes = static_cast<std::size_t>(m_reliableCapacity) * MAX_RELIABLE_BYTES;
  m_streamInArena.assign(reliableArenaBytes, 0u);
  m_streamInSizes.assign(m_reliableCapacity, 0u);
  m_streamInWrite.store(0u, std::memory_order_relaxed);
  m_streamInRead.store(0u, std::memory_order_relaxed);
  m_streamInReady = 0;
  m_streamInWriteSlot = 0;
  m_streamInReadSlot = 0;

  // One frame's worth plus its header: the most a reassembler can be holding when a message is one
  // byte short of complete.
  m_streamPartial.assign(static_cast<std::size_t>(MAX_RELIABLE_BYTES) + RELIABLE_HEADER_BYTES, 0u);
  m_streamPartialCount = 0;

  // Each outbound slot carries its header in front of its payload, so one buffer covers the frame.
  const std::size_t outSlotBytes = static_cast<std::size_t>(MAX_RELIABLE_BYTES) + RELIABLE_HEADER_BYTES;
  m_streamOutArena.assign(static_cast<std::size_t>(m_reliableCapacity) * outSlotBytes, 0u);
  m_streamOutInFlight = std::vector<std::atomic<bool>>(m_reliableCapacity);
  m_streamOutBuffers.assign(m_reliableCapacity, SendBuffer{});
  for (std::uint32_t slot = 0; slot < m_reliableCapacity; ++slot)
    m_streamOutBuffers[slot].bytes = m_streamOutArena.data() + static_cast<std::size_t>(slot) * outSlotBytes;
  m_streamOutCursor = 0;

  m_stream.store(nullptr, std::memory_order_relaxed);

  // Reserve is the pool's call, and a pooled transport accepts rather than dials. Connect sets this
  // for the end that dials; the recycle path in QuicListener re-Reserves and so clears it back.
  m_isDialer = false;
  m_streamReady.store(false, std::memory_order_relaxed);
  m_streamRequested.store(false, std::memory_order_relaxed);

  m_dropped.store(0u, std::memory_order_relaxed);
  m_maxSendLength.store(0u, std::memory_order_relaxed);
  m_handshakeDone.store(false, std::memory_order_relaxed);
  m_datagramSendEnabled.store(false, std::memory_order_relaxed);

  // Back to Disconnected, which matters only on the second call: QuicListener re-Reserves a
  // transport whose connection has closed so the slot can serve another client (ADR 0031), and one
  // that still reported Closed would be a pooled transport lying about being finished. On the first
  // call this is the value it already had.
  m_state.store(ConnectionState::Disconnected, std::memory_order_relaxed);
  m_reason[0] = '\0';
}

bool QuicTransport::Adopt(void* _connection)
{
  // On an MsQuic worker, inside QuicListener's accept callback. Reserve has already run on the
  // owning thread, so there is nothing to allocate here and nothing to decide.
  //
  // No QuicApi::AcquireHandle here, on purpose: that counter is owner-thread-only (see QuicApi.h),
  // and the listener's own handle already covers everything it accepts, because Stop closes this
  // whole pool before it releases that handle.
  if (m_connection != nullptr || m_api == nullptr || _connection == nullptr)
    return false;

  HQUIC connection = static_cast<HQUIC>(_connection);
  const QUIC_API_TABLE* const table = m_api->Table();

  m_connection = _connection;
  m_state.store(ConnectionState::Connecting, std::memory_order_release);
  table->SetCallbackHandler(connection, reinterpret_cast<void*>(QuicTransportCallbacks::OnConnectionEvent), this);

  const QUIC_STATUS status = table->ConnectionSetConfiguration(connection, static_cast<HQUIC>(m_api->ServerConfiguration()));
  if (QUIC_FAILED(status))
  {
    m_connection = nullptr;
    m_state.store(ConnectionState::Disconnected, std::memory_order_release);
    return false;
  }
  return true;
}

void QuicTransport::Close() noexcept
{
  if (m_connection == nullptr)
  {
    ReleaseApiHandle();
    return;
  }

  const QUIC_API_TABLE* const table = m_api->Table();
  HQUIC connection = static_cast<HQUIC>(m_connection);

  if (m_state.load(std::memory_order_acquire) != ConnectionState::Closed)
  {
    table->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);

    // The one place this class waits, and it is bounded by the idle timeout: MsQuic may still be
    // inside a callback on this object, and waiting for SHUTDOWN_COMPLETE is what makes the close
    // below safe rather than a race against a live upcall. If it never comes, the close happens
    // anyway -- ConnectionClose drains the callbacks itself, so it is the backstop and not a leak.
    std::unique_lock<std::mutex> lock(m_closedLock);
    (void)m_closed.wait_for(lock, std::chrono::milliseconds(m_api->IdleTimeoutMs()),
                            [this] { return m_state.load(std::memory_order_acquire) == ConnectionState::Closed; });
  }

  // The lane's stream goes before its connection, for the same reason the connection goes before the
  // registration: a handle cannot close over a live child. It is closed rather than shut down --
  // the connection shutdown above has already ended it, and StreamClose is what returns the handle.
  m_streamReady.store(false, std::memory_order_release);
  if (void* const stream = m_stream.exchange(nullptr, std::memory_order_acq_rel))
    table->StreamClose(static_cast<HQUIC>(stream));

  // Closing here rather than in the destructor, because Outpost's shutdown order closes the client
  // end, then the listener, then the library -- and that order only means anything if this call is
  // what ends the connection (Design/QuicTransport.md 6). A registration cannot close over a live
  // connection, and MsQuic's answer to being asked is to wait rather than to fail.
  table->ConnectionClose(connection);
  m_connection = nullptr;
  m_state.store(ConnectionState::Closed, std::memory_order_release);
  ReleaseApiHandle();
}

void QuicTransport::ReleaseApiHandle() noexcept
{
  if (m_holdsApiHandle && m_api != nullptr)
    m_api->ReleaseHandle();
  m_holdsApiHandle = false;
}

void QuicTransport::ReconsiderConnected()
{
  // Connected takes both halves: the handshake has completed, and the peer has said it will take a
  // datagram at least as large as the format's fragments are. Either event can be the second to
  // arrive, so both come through here.
  if (!m_handshakeDone.load(std::memory_order_acquire) || !m_datagramSendEnabled.load(std::memory_order_acquire))
    return;

  const std::uint32_t limit = m_maxSendLength.load(std::memory_order_acquire);
  if (limit == 0)
    return; // MsQuic has not settled on a number yet; the handshake timeout bounds the wait

  if (limit >= MAX_DATAGRAM_BYTES)
  {
    ConnectionState connecting = ConnectionState::Connecting;
    (void)m_state.compare_exchange_strong(connecting, ConnectionState::Connected);
    return;
  }

  // A peer that would take 900 bytes of a 1152-byte datagram gets the connection ended, because a
  // transport that silently truncates is the bug LoopbackTransport::Send refuses to have. State()
  // and MaxSendLength() together are what the owning thread reads to say why.
  //
  // Demoted from Connected as well as from Connecting, and that is not belt and braces: MsQuic
  // raises DATAGRAM_STATE_CHANGED again every time the limit moves -- an MTU probe settling, a path
  // changing -- so a connection wide enough at the handshake can stop being wide enough later.
  // Refusing only on the way up would leave that connection reporting Connected while every Send
  // returned false, which the contract reads as "the send queue is full" (Transport.h) and which the
  // caller answers by dropping the message and trying again forever.
  //
  // The margin here is not large. At QUIC's 1280-byte MTU floor, IP, UDP, the short header, the AEAD
  // tag and the DATAGRAM frame leave a little over 1180 bytes, so 1152 has tens of bytes of headroom
  // and a longer peer connection ID eats into it. If this ever fires on localhost, that budget is
  // the first thing to measure -- do not lower MAX_DATAGRAM_BYTES, which the wire format's fragment
  // sizes are derived from.
  ConnectionState carrying = m_state.load(std::memory_order_acquire);
  while (carrying == ConnectionState::Connecting || carrying == ConnectionState::Connected)
  {
    if (m_state.compare_exchange_weak(carrying, ConnectionState::Draining))
    {
      m_api->Table()->ConnectionShutdown(static_cast<HQUIC>(m_connection), QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                         QUIC_ERROR_DATAGRAM_TOO_SMALL);
      return;
    }
  }
}

bool QuicTransport::Send(const std::uint8_t* _bytes, std::uint32_t _count)
{
  if (m_state.load(std::memory_order_acquire) != ConnectionState::Connected)
    return false;

  // Refused rather than truncated, exactly as the loopback refuses: a wire that silently shortens a
  // datagram turns a format bug into a corrupt-payload bug.
  if (_count > MAX_DATAGRAM_BYTES || (_count > 0 && _bytes == nullptr))
    return false;

  // Completion is nearly in send order, so one cursor finds a free slot on the first try almost
  // always; the wrap is what makes "almost" safe.
  std::uint32_t at = m_capacity;
  for (std::uint32_t probe = 0; probe < m_capacity; ++probe)
  {
    const std::uint32_t candidate = (m_outCursor + probe) % m_capacity;
    bool expected = false;
    if (m_outInFlight[candidate].compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed))
    {
      at = candidate;
      break;
    }
  }
  if (at == m_capacity)
    return false; // every slot is still in flight, which is the contract's "the queue is full"

  m_outCursor = (at + 1u) % m_capacity;
  if (_count > 0)
    std::memcpy(m_outArena.data() + static_cast<std::size_t>(at) * MAX_DATAGRAM_BYTES, _bytes, _count);
  m_outBuffers[at].length = _count;

  const QUIC_STATUS status = m_api->Table()->DatagramSend(
    static_cast<HQUIC>(m_connection), reinterpret_cast<const QUIC_BUFFER*>(&m_outBuffers[at]), 1, QUIC_SEND_FLAG_NONE, SlotToContext(at));
  if (QUIC_FAILED(status))
  {
    m_outInFlight[at].store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void QuicTransport::Poll()
{
  // The dialling end opens the lane's stream here rather than in the CONNECTED callback: StreamOpen
  // and StreamStart are MsQuic calls, and ADR 0022 keeps those off the workers. One Poll after the
  // handshake is the earliest the owning thread can do it, which is why ReliableReady is a separate
  // question from State().
  OpenReliableStream();

  // Delivery happens here and only here, as Transport.h requires. The lock is held for one cursor
  // copy: what the workers have written below this point is now the reader's, and Receive takes it
  // without the lock because nothing above it is touched by anyone but a writer.
  {
    const std::lock_guard<std::mutex> lock(m_inLock);
    m_inReady = m_inWrite.load(std::memory_order_acquire);
  }

  // Both lanes, one Poll -- there is no second one to forget to call.
  {
    const std::lock_guard<std::mutex> lock(m_streamInLock);
    m_streamInReady = m_streamInWrite.load(std::memory_order_acquire);
  }
}

std::uint32_t QuicTransport::Receive(std::uint8_t* _outBytes, std::uint32_t _capacity)
{
  const std::uint32_t read = m_inRead.load(std::memory_order_relaxed);
  if (read == m_inReady)
    return 0;

  const std::uint32_t at = m_inReadSlot;
  const std::uint32_t size = m_inSizes[at];

  // A caller must offer at least MAX_DATAGRAM_BYTES, because that is the most a datagram can be.
  // Returning 0 would be indistinguishable from an empty queue and would spin any while (Receive())
  // loop forever, so say so instead of stalling quietly -- LoopbackTransport::Receive's rule.
  ASSERT_TEXT(size <= _capacity, L"Receive was offered a buffer smaller than the waiting datagram");
  if (size > _capacity)
    return 0;

  if (size > 0 && _outBytes != nullptr)
    std::memcpy(_outBytes, m_inArena.data() + static_cast<std::size_t>(at) * MAX_DATAGRAM_BYTES, size);

  m_inReadSlot = (at + 1u) % m_capacity;
  m_inRead.store(read + 1u, std::memory_order_release);
  return size;
}

void QuicTransport::OpenReliableStream()
{
  // The dialling end only, and once. The accepting end gets its handle through PEER_STREAM_STARTED,
  // because a bidirectional stream carries both directions and one of them is all the handshake
  // reserved (QuicApi.cpp, QUIC_PEER_BIDI_STREAMS).
  // The dialing end only. The accepting end gets its handle through PEER_STREAM_STARTED, and if it
  // opened one of its own it would be a second bidirectional stream against a peer that negotiated
  // room for exactly one -- which is the defect ADR 0032 records.
  if (!m_isDialer || m_api == nullptr || m_connection == nullptr)
    return;
  if (m_stream.load(std::memory_order_acquire) != nullptr)
    return;
  if (m_state.load(std::memory_order_acquire) != ConnectionState::Connected)
    return;
  if (m_streamRequested.exchange(true, std::memory_order_acq_rel))
    return;

  const QUIC_API_TABLE* const table = m_api->Table();
  HQUIC stream = nullptr;
  if (QUIC_FAILED(table->StreamOpen(static_cast<HQUIC>(m_connection), QUIC_STREAM_OPEN_FLAG_NONE, QuicTransportCallbacks::OnStreamEvent,
                                    this, &stream)))
  {
    SetReason("StreamOpen failed");
    return;
  }

  // IMMEDIATE, so the peer learns the stream exists now rather than when the first byte is sent.
  // Without it the accepting end sees no PEER_STREAM_STARTED until traffic flows, so its lane never
  // comes up and anything waiting on ReliableReady waits for ever.
  if (QUIC_FAILED(table->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE)))
  {
    SetReason("StreamStart failed");
    table->StreamClose(stream);
    return;
  }
  m_stream.store(stream, std::memory_order_release);
}

void QuicTransport::AdoptReliableStream(void* _stream)
{
  // On a worker. SetCallbackHandler is the one MsQuic call this file makes from one, and it is the
  // same exception ADR 0022 already carries for accepting a connection.
  if (_stream == nullptr || m_api == nullptr)
    return;

  // Claim the slot, and refuse a second stream rather than overwrite the handle we already hold:
  // overwriting would leak the first one, which MsQuic requires this side to close exactly once.
  void* expected = nullptr;
  if (!m_stream.compare_exchange_strong(expected, _stream, std::memory_order_acq_rel))
    return;
  m_api->Table()->SetCallbackHandler(static_cast<HQUIC>(_stream), reinterpret_cast<void*>(QuicTransportCallbacks::OnStreamEvent), this);

  // A stream handed over by the peer is already started, so there is no START_COMPLETE coming.
  m_streamReady.store(true, std::memory_order_release);
}

void QuicTransport::PushReliable(const std::uint8_t* _bytes, std::uint32_t _count)
{
  // Called under m_streamInLock, from a worker. Same ring discipline as the datagram lane: the slot
  // is carried rather than derived, so the free-running cursor's wrap cannot misplace a message.
  if (m_reliableCapacity == 0)
    return;
  const std::uint32_t write = m_streamInWrite.load(std::memory_order_relaxed);
  const std::uint32_t read = m_streamInRead.load(std::memory_order_acquire);
  if (write - read >= m_reliableCapacity)
  {
    // A full ring on this lane is worse than on the other one -- what is dropped here is a fact
    // nothing will repeat -- so it is counted and the reader is expected to be keeping up.
    m_dropped.fetch_add(1u, std::memory_order_relaxed);
    return;
  }

  const std::uint32_t at = m_streamInWriteSlot;
  if (_count > 0)
    std::memcpy(m_streamInArena.data() + static_cast<std::size_t>(at) * MAX_RELIABLE_BYTES, _bytes, _count);
  m_streamInSizes[at] = _count;
  m_streamInWriteSlot = (at + 1u) % m_reliableCapacity;
  m_streamInWrite.store(write + 1u, std::memory_order_release);
}

bool QuicTransport::SendReliable(const std::uint8_t* _bytes, std::uint32_t _count)
{
  void* const stream = m_stream.load(std::memory_order_acquire);
  if (!m_streamReady.load(std::memory_order_acquire) || stream == nullptr || m_api == nullptr)
    return false;
  if (_count > MAX_RELIABLE_BYTES || (_count > 0 && _bytes == nullptr))
    return false; // refused rather than truncated, which is Send's rule on the other lane

  // The first slot not still in MsQuic's hands. Same scan as the datagram lane, over a shallower
  // ring: a full ring means the peer is not reading, which is backpressure and not loss.
  std::uint32_t slot = m_reliableCapacity;
  for (std::uint32_t step = 0; step < m_reliableCapacity; ++step)
  {
    const std::uint32_t candidate = (m_streamOutCursor + step) % m_reliableCapacity;
    bool expected = false;
    if (m_streamOutInFlight[candidate].compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
      slot = candidate;
      break;
    }
  }
  if (slot == m_reliableCapacity)
    return false;
  m_streamOutCursor = (slot + 1u) % m_reliableCapacity;

  // The header goes in front of the payload in the same slot, so the whole frame is one buffer and
  // MsQuic has one thing to keep alive until the send completes.
  std::uint8_t* const frame = m_streamOutBuffers[slot].bytes;
  frame[0] = static_cast<std::uint8_t>(_count & 0xFFu);
  frame[1] = static_cast<std::uint8_t>((_count >> 8) & 0xFFu);
  if (_count > 0)
    std::memcpy(frame + RELIABLE_HEADER_BYTES, _bytes, _count);
  m_streamOutBuffers[slot].length = RELIABLE_HEADER_BYTES + _count;

  const QUIC_BUFFER* const buffer = reinterpret_cast<const QUIC_BUFFER*>(&m_streamOutBuffers[slot]);
  if (QUIC_FAILED(m_api->Table()->StreamSend(static_cast<HQUIC>(stream), buffer, 1, QUIC_SEND_FLAG_NONE, SlotToContext(slot))))
  {
    m_streamOutInFlight[slot].store(false, std::memory_order_release);
    return false;
  }
  return true;
}

std::uint32_t QuicTransport::ReceiveReliable(std::uint8_t* _outBytes, std::uint32_t _capacity)
{
  const std::uint32_t read = m_streamInRead.load(std::memory_order_relaxed);
  if (read == m_streamInReady)
    return 0;

  const std::uint32_t at = m_streamInReadSlot;
  const std::uint32_t size = m_streamInSizes[at];

  // Same rule as Receive: too small a buffer is the caller's bug, and returning 0 would be
  // indistinguishable from an empty lane and would spin a drain loop for ever.
  ASSERT_TEXT(size <= _capacity, L"ReceiveReliable was offered a buffer smaller than the waiting message");
  if (size > _capacity)
    return 0;

  if (size > 0 && _outBytes != nullptr)
    std::memcpy(_outBytes, m_streamInArena.data() + static_cast<std::size_t>(at) * MAX_RELIABLE_BYTES, size);

  m_streamInReadSlot = (at + 1u) % m_reliableCapacity;
  m_streamInRead.store(read + 1u, std::memory_order_release);
  return size;
}

bool QuicTransport::ReliableReady() const noexcept
{
  return m_streamReady.load(std::memory_order_acquire);
}

ConnectionState QuicTransport::State() const
{
  return m_state.load(std::memory_order_acquire);
}

std::uint32_t QuicTransport::DroppedCount() const noexcept
{
  return m_dropped.load(std::memory_order_relaxed);
}

std::uint32_t QuicTransport::MaxSendLength() const noexcept
{
  return m_maxSendLength.load(std::memory_order_acquire);
}

const char* QuicTransport::Reason() const noexcept
{
  return m_reason;
}

void QuicTransport::SetReason(const char* _format, ...) noexcept
{
  // Written on the owning thread only. A worker that wants to explain itself sets an atomic instead,
  // which is why the too-small-datagram case is read back as State() plus MaxSendLength().
  std::va_list arguments;
  va_start(arguments, _format);
  (void)std::vsnprintf(m_reason, sizeof(m_reason), _format, arguments);
  va_end(arguments);
}
} // namespace Neuron
