#pragma once

#include <cstdint>
#include <memory>

// MsQuic's function table, forward-declared at the global scope where MsQuic itself declares it.
// No header in this tree names an MsQuic type (Design/Archive/QuicTransport.md 3): only QuicApi.cpp,
// QuicTransport.cpp and QuicListener.cpp include <msquic.h>, so the day the package version changes
// the blast radius is one directory rather than every translation unit that reaches the umbrella.
struct QUIC_API_TABLE;

namespace Neuron
{
class DevCertificate;

// Past this much silence MsQuic drains the connection. The snapshot rate is 10 Hz, so a peer that
// has said nothing for ten seconds is gone rather than quiet.
inline constexpr std::uint32_t QUIC_IDLE_TIMEOUT_MS = 10000;

// A client with nothing selected sends no orders at all. The keep-alive is what stops the server
// idling out a player who is thinking.
inline constexpr std::uint32_t QUIC_KEEP_ALIVE_MS = 2000;

// How long boot waits for the handshake before it gives up and runs on the loopback instead
// (Design/Archive/QuicTransport.md 6). It is also the bound on every wait in the tests, so a broken build
// fails in seconds rather than in CI's timeout.
inline constexpr std::uint32_t QUIC_HANDSHAKE_TIMEOUT_MS = 3000;

// Nothing in this directory throws: a port already taken and a key store locked down are the user's
// circumstances, not the program's mistake, and AGENTS.md 5 makes those diagnostics. A diagnostic is
// a sentence, and it lives in a fixed-width buffer so that reporting one allocates nothing and
// truncates rather than overruns -- the same shape as EventLog::Entry.
inline constexpr std::uint32_t QUIC_REASON_BYTES = 192;

// The MsQuic library, opened once: one registration, and the two configurations every connection in
// the process is built from. One instance serves every QuicTransport and QuicListener made from it,
// and it must outlive all of them.
//
// It is not a singleton and there is no global. The composition root constructs it as a value and
// hands it out by reference, the way ServerHost::Desc and Camera::Desc are handed out (AGENTS.md 5:
// no argv, no environment variables). A test constructs its own and nothing is shared between them.
class QuicApi final
{
public:
  struct Desc
  {
    std::uint32_t idleTimeoutMs = QUIC_IDLE_TIMEOUT_MS;
    std::uint32_t keepAliveMs = QUIC_KEEP_ALIVE_MS;
    std::uint32_t handshakeTimeoutMs = QUIC_HANDSHAKE_TIMEOUT_MS;

    // The development root sets this and nothing else may. There is no certificate validation path
    // in this tree yet -- the client is told to accept whatever the server presents -- so Open
    // refuses rather than quietly building a configuration nobody meant to ask for (ADR 0023).
    bool allowUnvalidatedPeer = false;
  };

  // Both of these are out of line, and neither may be moved back into this header. m_certificate is
  // a unique_ptr to a type this header only forward-declares, and the constructor needs the
  // deleter for its own rollback path just as the destructor needs it for the ordinary one -- so a
  // `= default` here compiles everywhere DevCertificate.h happens to have been included already and
  // fails, in <memory>, in the one translation unit that constructs a QuicApi without it.
  QuicApi();
  ~QuicApi();

  QuicApi(const QuicApi&) = delete;
  QuicApi& operator=(const QuicApi&) = delete;

  // False and Reason() on failure; never throws. A tree with no MsQuic, or a key store that will not
  // give up a key, is a boot that falls back to the loopback and says why.
  [[nodiscard]] bool Open(const Desc& _desc);

  // Closes the configurations, the registration and then the library, in that order. Every transport
  // and listener made from this object must already be closed: a registration cannot close over a
  // live connection.
  void Close() noexcept;

  [[nodiscard]] const char* Reason() const noexcept;
  [[nodiscard]] bool IsOpen() const noexcept;

private:
  friend class QuicListener;
  friend class QuicTransport;

  // What the two classes above need and nobody else does. Handles are void* rather than HQUIC for
  // the reason at the top of this file; the .cpp files cast them back.
  [[nodiscard]] const QUIC_API_TABLE* Table() const noexcept;
  [[nodiscard]] void* Registration() const noexcept;
  [[nodiscard]] void* ClientConfiguration() const noexcept;
  [[nodiscard]] void* ServerConfiguration() const noexcept;
  [[nodiscard]] std::uint32_t IdleTimeoutMs() const noexcept;

  // The ALPN both configurations were opened on. QuicListener needs the same string to start on, and
  // reaches it here rather than through a second copy of the constant, which is how two copies of a
  // protocol name end up disagreeing.
  [[nodiscard]] const char* Alpn() const noexcept;

  // How many MsQuic handles are standing on this registration, so that Close can say so rather than
  // hang inside RegistrationClose. A plain counter and not an atomic, because it is only ever
  // touched on the owning thread: a client transport takes one in Connect, and a listener takes one
  // in Start that covers every connection it accepts -- QuicListener::Stop closes its whole pool
  // before it gives that one back, so the count is non-zero for as long as any accepted connection
  // is open. An accepted connection deliberately does not take its own: QuicTransport::Adopt runs on
  // an MsQuic worker, and a worker touching this counter is the data race the whole design exists to
  // avoid (ADR 0022).
  void AcquireHandle() noexcept;
  void ReleaseHandle() noexcept;

  void SetReason(const char* _format, ...) noexcept;

  const QUIC_API_TABLE* m_table = nullptr;
  void* m_registration = nullptr;
  void* m_clientConfig = nullptr;
  void* m_serverConfig = nullptr;

  // Held for the life of the server configuration: MsQuic is handed the certificate context and
  // Schannel reaches back into it, so releasing it early would pull the credential out from under a
  // live listener. Behind a unique_ptr so that DevCertificate.h is included by QuicApi.cpp alone.
  std::unique_ptr<DevCertificate> m_certificate;

  std::uint32_t m_outstanding = 0;
  std::uint32_t m_idleTimeoutMs = QUIC_IDLE_TIMEOUT_MS;
  char m_reason[QUIC_REASON_BYTES] = {};
};
} // namespace Neuron
