#include "pch.h"

// MsQuic after pch.h and before anything else, for the include-order reason at the top of
// QuicApi.cpp: pch.h has already put Winsock ahead of <windows.h>, and QUIC_ADDR needs that.
#include <msquic.h>
#include <msquic.hpp>

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

      const std::uint32_t at = write % end.m_capacity;
      if (received->Length > 0)
        std::memcpy(end.m_inArena.data() + static_cast<std::size_t>(at) * MAX_DATAGRAM_BYTES, received->Buffer, received->Length);
      end.m_inSizes[at] = received->Length;
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

  m_outArena.assign(arenaBytes, 0u);
  m_outInFlight = std::vector<std::atomic<bool>>(m_capacity);
  m_outBuffers.assign(m_capacity, SendBuffer{});
  for (std::uint32_t slot = 0; slot < m_capacity; ++slot)
    m_outBuffers[slot].bytes = m_outArena.data() + static_cast<std::size_t>(slot) * MAX_DATAGRAM_BYTES;
  m_outCursor = 0;

  m_dropped.store(0u, std::memory_order_relaxed);
  m_maxSendLength.store(0u, std::memory_order_relaxed);
  m_handshakeDone.store(false, std::memory_order_relaxed);
  m_datagramSendEnabled.store(false, std::memory_order_relaxed);
}

bool QuicTransport::Adopt(void* _connection)
{
  // On an MsQuic worker, inside QuicListener's accept callback. Reserve has already run on the
  // owning thread, so there is nothing to allocate here and nothing to decide.
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

    // The one place this class waits, and it is bounded. MsQuic may still be inside a callback on
    // this object, so the handle cannot be closed until SHUTDOWN_COMPLETE has come back -- and if it
    // never does, the leak of one connection handle is preferred to a deadlock at shutdown.
    std::unique_lock<std::mutex> lock(m_closedLock);
    (void)m_closed.wait_for(lock, std::chrono::milliseconds(m_api->IdleTimeoutMs()),
                            [this] { return m_state.load(std::memory_order_acquire) == ConnectionState::Closed; });
  }

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

  ConnectionState connecting = ConnectionState::Connecting;
  if (limit >= MAX_DATAGRAM_BYTES)
  {
    (void)m_state.compare_exchange_strong(connecting, ConnectionState::Connected);
    return;
  }

  // A peer that would take 900 bytes of a 1152-byte datagram gets the connection ended, because a
  // transport that silently truncates is the bug LoopbackTransport::Send refuses to have. State()
  // and MaxSendLength() together are what the owning thread reads to say why.
  if (m_state.compare_exchange_strong(connecting, ConnectionState::Draining))
    m_api->Table()->ConnectionShutdown(static_cast<HQUIC>(m_connection), QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_ERROR_DATAGRAM_TOO_SMALL);
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
    bool free = false;
    if (m_outInFlight[candidate].compare_exchange_strong(free, true, std::memory_order_acq_rel, std::memory_order_relaxed))
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
  // Delivery happens here and only here, as Transport.h requires. The lock is held for one cursor
  // copy: what the workers have written below this point is now the reader's, and Receive takes it
  // without the lock because nothing above it is touched by anyone but a writer.
  const std::lock_guard<std::mutex> lock(m_inLock);
  m_inReady = m_inWrite.load(std::memory_order_acquire);
}

std::uint32_t QuicTransport::Receive(std::uint8_t* _outBytes, std::uint32_t _capacity)
{
  const std::uint32_t read = m_inRead.load(std::memory_order_relaxed);
  if (read == m_inReady)
    return 0;

  const std::uint32_t at = read % m_capacity;
  const std::uint32_t size = m_inSizes[at];

  // A caller must offer at least MAX_DATAGRAM_BYTES, because that is the most a datagram can be.
  // Returning 0 would be indistinguishable from an empty queue and would spin any while (Receive())
  // loop forever, so say so instead of stalling quietly -- LoopbackTransport::Receive's rule.
  ASSERT_TEXT(size <= _capacity, L"Receive was offered a buffer smaller than the waiting datagram");
  if (size > _capacity)
    return 0;

  if (size > 0 && _outBytes != nullptr)
    std::memcpy(_outBytes, m_inArena.data() + static_cast<std::size_t>(at) * MAX_DATAGRAM_BYTES, size);

  m_inRead.store(read + 1u, std::memory_order_release);
  return size;
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
