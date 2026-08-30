#include "pch.h"

// MsQuic after pch.h and before anything else, for the include-order reason at the top of
// QuicApi.cpp.
#include <msquic.h>
#include <msquic.hpp>

#include "QuicListener.h"

#include <cstdarg>
#include <cstdio>

namespace Neuron
{
// MsQuic's listener callback, on an MsQuic worker. Like the connection callback it allocates
// nothing: Start reserved the backlog and both accept vectors, so all this does is take a slot and
// hand the connection over.
struct QuicListenerCallbacks
{
  static QUIC_STATUS QUIC_API OnListenerEvent(HQUIC, void* _context, QUIC_LISTENER_EVENT* _event)
  {
    if (_event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
      return QUIC_STATUS_SUCCESS;

    QuicListener& listener = *static_cast<QuicListener*>(_context);

    // Refused rather than dropped. Returning a failure here is how MsQuic is told to close the
    // connection itself, which is what keeps an exhausted backlog from leaking a handle.
    if (!listener.OnNewConnection(_event->NEW_CONNECTION.Connection))
      return QUIC_STATUS_CONNECTION_REFUSED;

    return QUIC_STATUS_SUCCESS;
  }
};

QuicListener::~QuicListener()
{
  Stop();
}

bool QuicListener::Start(QuicApi& _api, std::uint16_t _port, const Desc& _desc)
{
  Stop();

  if (!_api.IsOpen())
  {
    SetReason("the QUIC library is not open: %s", _api.Reason());
    return false;
  }

  m_api = &_api;

  // Everything the accept path will touch, taken here on the owning thread. A worker that had to
  // allocate would be a worker doing more than enqueueing (AGENTS.md 5).
  const std::uint32_t backlog = (_desc.backlog > 0) ? _desc.backlog : 1;
  m_pool.reserve(backlog);
  for (std::uint32_t slot = 0; slot < backlog; ++slot)
  {
    m_pool.push_back(std::make_unique<QuicTransport>());
    m_pool.back()->Reserve(_api, _desc.transport);
  }
  m_pending.reserve(backlog);
  m_accepted.reserve(backlog);
  m_acceptedSlot.reserve(backlog);

  // Every slot free, highest first, so the first accept takes slot 0 and the pool fills in order --
  // which keeps a test's expectations readable and costs nothing.
  m_freeSlots.clear();
  m_freeSlots.reserve(backlog);
  for (std::uint32_t slot = backlog; slot > 0; --slot)
    m_freeSlots.push_back(slot - 1);

  m_transportDesc = _desc.transport;
  m_recycled = 0;

  const QUIC_API_TABLE* const table = _api.Table();
  HQUIC listener = nullptr;
  QUIC_STATUS status =
    table->ListenerOpen(static_cast<HQUIC>(_api.Registration()), QuicListenerCallbacks::OnListenerEvent, this, &listener);
  if (QUIC_FAILED(status))
  {
    SetReason("ListenerOpen failed (0x%08x)", static_cast<unsigned>(status));
    Stop();
    return false;
  }
  m_listener = listener;
  _api.AcquireHandle();

  // Loopback only. The address is not a parameter because nothing in this design listens anywhere
  // else, and a field that could hold 0.0.0.0 is a field somebody eventually sets to it.
  QUIC_ADDR address{};
  QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_INET);
  QuicAddrSetToLoopback(&address);
  QuicAddrSetPort(&address, _port);

  const MsQuicAlpn alpn(_api.Alpn());
  status = table->ListenerStart(listener, alpn, alpn.Length(), &address);
  if (QUIC_FAILED(status))
  {
    SetReason("port %u is refused (0x%08x) -- another process is already listening on it", static_cast<unsigned>(_port),
              static_cast<unsigned>(status));
    Stop();
    return false;
  }

  // Which port was actually bound, which is the only way to know after a Start on port 0.
  QUIC_ADDR bound{};
  std::uint32_t boundBytes = static_cast<std::uint32_t>(sizeof(bound));
  status = table->GetParam(listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &boundBytes, &bound);
  if (QUIC_FAILED(status))
  {
    SetReason("the listener would not report its local address (0x%08x)", static_cast<unsigned>(status));
    Stop();
    return false;
  }
  m_port = QuicAddrGetPort(&bound);

  m_reason[0] = '\0';
  return true;
}

void QuicListener::Stop() noexcept
{
  bool releaseHandle = false;
  if (m_listener != nullptr)
  {
    const QUIC_API_TABLE* const table = m_api->Table();
    HQUIC listener = static_cast<HQUIC>(m_listener);

    // ListenerStop first so that no further accept can race the teardown; ListenerClose then waits
    // for the callbacks already inside, as MsQuic documents.
    table->ListenerStop(listener);
    table->ListenerClose(listener);
    m_listener = nullptr;
    releaseHandle = true;
  }

  // After the listener and before QuicApi::Close: destroying a transport is what closes the
  // connection handle it accepted, and the registration cannot close over one of those.
  m_pool.clear();
  m_pending.clear();
  m_accepted.clear();
  m_acceptedSlot.clear();
  m_freeSlots.clear();
  m_recycled = 0;
  m_port = 0;

  if (releaseHandle)
    m_api->ReleaseHandle();
  m_api = nullptr;
}

std::uint16_t QuicListener::Port() const noexcept
{
  return m_port;
}

void QuicListener::Poll()
{
  {
    const std::lock_guard<std::mutex> lock(m_acceptLock);
    for (const std::uint32_t slot : m_pending)
    {
      m_accepted.push_back(m_pool[slot].get());
      m_acceptedSlot.push_back(slot);
    }
    m_pending.clear();
  }

  RecycleClosed();
}

void QuicListener::RecycleClosed()
{
  // Owning thread. A connection that has reached Closed has already had its SHUTDOWN_COMPLETE, so
  // no worker is still inside a callback on it and the transport can be taken apart here -- which is
  // why this is in Poll and not in the callback that saw the shutdown (ADR 0022).
  std::size_t live = 0;
  for (std::size_t at = 0; at < m_accepted.size(); ++at)
  {
    QuicTransport* const end = m_accepted[at];
    if (end->State() != ConnectionState::Closed)
    {
      // Kept: compacted down in place, so the order of what remains is still the order it arrived.
      m_accepted[live] = end;
      m_acceptedSlot[live] = m_acceptedSlot[at];
      ++live;
      continue;
    }

    // Close first -- it is what releases the connection handle MsQuic still holds -- then Reserve,
    // which resets the rings and the state to what Start left them. Reserve reuses the vectors it
    // already sized, so the only allocation is the in-flight flags, which are atomics and cannot be
    // assigned; that is once per client that has left, on the owning thread, and not per datagram.
    end->Close();
    end->Reserve(*m_api, m_transportDesc);

    const std::lock_guard<std::mutex> lock(m_acceptLock);
    m_freeSlots.push_back(m_acceptedSlot[at]);
    ++m_recycled;
  }
  m_accepted.resize(live);
  m_acceptedSlot.resize(live);
}

std::uint32_t QuicListener::RecycledCount() const noexcept
{
  return m_recycled;
}

std::span<QuicTransport* const> QuicListener::Accepted() const noexcept
{
  return std::span<QuicTransport* const>(m_accepted.data(), m_accepted.size());
}

const char* QuicListener::Reason() const noexcept
{
  return m_reason;
}

bool QuicListener::OnNewConnection(void* _connection)
{
  const std::lock_guard<std::mutex> lock(m_acceptLock);
  if (m_freeSlots.empty())
    return false; // every slot is carrying a connection; MsQuic turns this into a refusal

  const std::uint32_t slot = m_freeSlots.back();
  if (!m_pool[slot]->Adopt(_connection))
    return false; // the slot stays free: nothing was taken from it

  m_freeSlots.pop_back();
  m_pending.push_back(slot);
  return true;
}

void QuicListener::SetReason(const char* _format, ...) noexcept
{
  std::va_list arguments;
  va_start(arguments, _format);
  (void)std::vsnprintf(m_reason, sizeof(m_reason), _format, arguments);
  va_end(arguments);
}
} // namespace Neuron
