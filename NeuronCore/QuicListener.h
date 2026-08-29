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
// One client is what this design serves (Design/QuicTransport.md 10). Desc::backlog is a number
// rather than a promise, so that the day there are N clients it is a number that changes and not a
// class that gets rewritten: the transports it hands connections to are pre-allocated by Start, on
// the owning thread, because the accept itself arrives on an MsQuic worker and a worker allocates
// nothing.
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

  // Owning thread: moves what the workers accepted into Accepted(). The transports themselves are
  // polled by whoever took them, exactly as a client end is.
  void Poll();

  // Every connection accepted so far, oldest first. The listener owns them; they are valid until
  // Stop.
  [[nodiscard]] std::span<QuicTransport* const> Accepted() const noexcept;

  [[nodiscard]] const char* Reason() const noexcept;

private:
  // As in QuicTransport: the static thunk MsQuic calls lives in the .cpp, where its signature may
  // name MsQuic types.
  friend struct QuicListenerCallbacks;

  // On an MsQuic worker. Takes the next unused transport out of the backlog and gives it the
  // connection; false when the backlog is exhausted, which MsQuic turns into a refusal.
  [[nodiscard]] bool OnNewConnection(void* _connection);

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
  std::uint32_t m_adopted = 0;
  std::vector<QuicTransport*> m_accepted;

  char m_reason[QUIC_REASON_BYTES] = {};
};
} // namespace Neuron
