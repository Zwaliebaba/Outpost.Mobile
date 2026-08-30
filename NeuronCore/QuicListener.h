#pragma once

#include "QuicApi.h"
#include "QuicTransport.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace Neuron
{
// The server end of the seam, and the half a loopback never needed: it opens a port, accepts a
// connection, and yields a QuicTransport for it.
//
// It binds 127.0.0.1 and nothing else, by design (Design/QuicTransport.md 5 and 11): there is no
// NAT traversal here, no public port and no firewall prompt, so the address is not a parameter. A
// port of 0 takes an ephemeral one, which is how the tests avoid fighting over a number, and Port()
// reports what was bound.
//
// Desc::backlog is how many connections it can carry AT ONCE, not how many it will ever accept. A
// slot whose connection has closed is recycled in Poll and serves the next client, so a server does
// not die by attrition after backlog logins (ADR 0030, review finding E3). The transports are
// pre-allocated by Start on the owning thread, because the accept itself arrives on an MsQuic worker
// and a worker allocates nothing.
//
// Sizing is the caller's, through Desc::transport, and worth doing deliberately at scale: every slot
// holds its rings for the life of the listener, so backlog x (two datagram rings + two reliable
// rings) is the memory a listener costs before anybody connects. At the defaults that is about
// 800 KB a slot.
class QuicListener final
{
public:
  struct Desc
  {
    std::uint32_t backlog = 1;     // transports pre-allocated to hand accepted connections to
    QuicTransport::Desc transport; // what each of them is built with
  };

  QuicListener() = default;
  ~QuicListener();

  QuicListener(const QuicListener&) = delete;
  QuicListener& operator=(const QuicListener&) = delete;

  // False and Reason() when the port is taken or MsQuic refuses -- never an assert, because the
  // composition root's fallback to the loopback reads it and carries on (Design/QuicTransport.md 6).
  [[nodiscard]] bool Start(QuicApi& _api, std::uint16_t _port, const Desc& _desc);

  // Closes the listener, which waits for its own callbacks, and then every connection it accepted.
  // QuicApi::Close must come after this: a registration cannot close over a live connection.
  void Stop() noexcept;

  // The bound port, which is the only way to learn it after a Start on port 0.
  [[nodiscard]] std::uint16_t Port() const noexcept;

  // Owning thread: moves what the workers accepted into Accepted(), and recycles the slots whose
  // connections have closed. The transports themselves are polled by whoever took them, exactly as a
  // client end is.
  void Poll();

  // Every LIVE connection, oldest first. The listener owns them, and a pointer stays valid until the
  // Poll that finds its connection closed -- after which the transport is still alive but belongs to
  // the pool again, so a caller that holds one across a Poll must be prepared to find it gone from
  // this span (ADR 0030).
  [[nodiscard]] std::span<QuicTransport* const> Accepted() const noexcept;

  // How many slots have been recycled since Start. Diagnostics: it is the number of clients that
  // have come and gone, and it is what says the pool is being reused rather than exhausted.
  [[nodiscard]] std::uint32_t RecycledCount() const noexcept;

  [[nodiscard]] const char* Reason() const noexcept;

private:
  // As in QuicTransport: the static thunk MsQuic calls lives in the .cpp, where its signature may
  // name MsQuic types.
  friend struct QuicListenerCallbacks;

  // On an MsQuic worker. Takes a free transport out of the pool and gives it the connection; false
  // when every slot is carrying one, which MsQuic turns into a refusal.
  [[nodiscard]] bool OnNewConnection(void* _connection);

  // Owning thread, from Poll: returns the slots whose connections have closed to the free list, and
  // resets their transports so the next accept finds them as Start left them.
  void RecycleClosed();

  void SetReason(const char* _format, ...) noexcept;

  QuicApi* m_api = nullptr;
  void* m_listener = nullptr; // HQUIC, void* for the reason at the top of QuicApi.h
  std::uint16_t m_port = 0;

  // Pre-allocated in Start and never resized after it, so the accept path allocates nothing. Behind
  // unique_ptr because a QuicTransport holds a mutex and a condition variable and so cannot be moved,
  // which is what a vector of them would need.
  std::vector<std::unique_ptr<QuicTransport>> m_pool;

  // m_pending is the workers' side and m_accepted the owner's; Poll moves one into the other under
  // the lock. Both are reserved to the backlog in Start, so neither grows on a worker.
  std::mutex m_acceptLock;
  std::vector<std::uint32_t> m_pending;

  // Free slots, not a high-water mark. A counter that only rose is what made backlog a lifetime
  // budget instead of a concurrency one (ADR 0030). Taken from the back, so a slot that has just
  // been recycled is the next one used and the pool stays warm.
  std::vector<std::uint32_t> m_freeSlots;

  // Parallel: m_accepted is what Accepted() spans, m_acceptedSlot is which pool slot each came from,
  // so a closed connection can be given back without searching the pool for its pointer.
  std::vector<QuicTransport*> m_accepted;
  std::vector<std::uint32_t> m_acceptedSlot;

  // What each pooled transport is rebuilt with when its slot is recycled.
  QuicTransport::Desc m_transportDesc;
  std::uint32_t m_recycled = 0;

  char m_reason[QUIC_REASON_BYTES] = {};
};
} // namespace Neuron
