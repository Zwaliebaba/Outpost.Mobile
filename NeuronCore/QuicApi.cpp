#include "pch.h"

// MsQuic after pch.h and before anything else: pch.h reaches NeuronCore.h, which puts <WinSock2.h>
// ahead of <windows.h> in the one order that works, and QUIC_ADDR is a SOCKADDR_INET that needs it.
#include <msquic.h>
#include <msquic.hpp>

#include "QuicApi.h"
#include "DevCertificate.h"

#include <cstdarg>
#include <cstdio>

namespace Neuron
{
namespace
{
// The application-layer protocol name both ends offer. It bumps the day the wire format's KIND_*
// bytes change meaning, so that two builds which cannot talk refuse at the handshake rather than at
// the parser (Design/QuicTransport.md 4.3).
constexpr const char* QUIC_ALPN = "outpost-1";

// One bidirectional stream is negotiated and none is opened. The reliable lane that will use it is
// Design/QuicTransport.md 8, slice 3a; reserving the count here means that slice does not have to
// change a configuration both ends already agreed on.
constexpr std::uint16_t QUIC_PEER_BIDI_STREAMS = 1;
} // namespace

QuicApi::~QuicApi()
{
  Close();
}

bool QuicApi::Open(const Desc& _desc)
{
  Close();

  // Refused rather than silently built. A configuration that validates certificates is a trust model
  // and this tree has not chosen one (ADR 0023); a caller that has not said it knows that is a
  // caller who thinks the connection is authenticated.
  if (!_desc.allowUnvalidatedPeer)
  {
    SetReason("there is no certificate validation path yet, so Desc::allowUnvalidatedPeer must be set (ADR 0023)");
    return false;
  }

  const QUIC_STATUS opened = MsQuicOpen2(&m_table);
  if (QUIC_FAILED(opened))
  {
    m_table = nullptr;
    SetReason("MsQuicOpen2 failed (0x%08x) -- msquic.dll is missing or the wrong version", static_cast<unsigned>(opened));
    return false;
  }

  const QUIC_REGISTRATION_CONFIG registrationConfig{"Outpost", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
  HQUIC registration = nullptr;
  const QUIC_STATUS registered = m_table->RegistrationOpen(&registrationConfig, &registration);
  if (QUIC_FAILED(registered))
  {
    SetReason("RegistrationOpen failed (0x%08x)", static_cast<unsigned>(registered));
    Close();
    return false;
  }
  m_registration = registration;
  m_idleTimeoutMs = _desc.idleTimeoutMs;

  MsQuicSettings settings;
  settings.SetDatagramReceiveEnabled(true)
    .SetIdleTimeoutMs(_desc.idleTimeoutMs)
    .SetKeepAlive(_desc.keepAliveMs)
    .SetHandshakeIdleTimeoutMs(_desc.handshakeTimeoutMs)
    .SetPeerBidiStreamCount(QUIC_PEER_BIDI_STREAMS);

  const MsQuicAlpn alpn(QUIC_ALPN);
  const QUIC_SETTINGS* const settingsBlock = &settings;

  HQUIC clientConfig = nullptr;
  QUIC_STATUS status =
    m_table->ConfigurationOpen(registration, alpn, alpn.Length(), settingsBlock, sizeof(QUIC_SETTINGS), nullptr, &clientConfig);
  if (QUIC_FAILED(status))
  {
    SetReason("ConfigurationOpen for the client failed (0x%08x)", static_cast<unsigned>(status));
    Close();
    return false;
  }
  m_clientConfig = clientConfig;

  // The placeholder, declared as one. TLS 1.3 still encrypts and integrity-checks the connection;
  // what it does not do is tell this client who it is talking to (Design/QuicTransport.md 7).
  const MsQuicCredentialConfig clientCredential(QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
  status = m_table->ConfigurationLoadCredential(clientConfig, &clientCredential);
  if (QUIC_FAILED(status))
  {
    SetReason("ConfigurationLoadCredential for the client failed (0x%08x)", static_cast<unsigned>(status));
    Close();
    return false;
  }

  m_certificate = std::make_unique<DevCertificate>();
  if (!m_certificate->Acquire())
  {
    SetReason("%s", m_certificate->Reason());
    Close();
    return false;
  }

  HQUIC serverConfig = nullptr;
  status = m_table->ConfigurationOpen(registration, alpn, alpn.Length(), settingsBlock, sizeof(QUIC_SETTINGS), nullptr, &serverConfig);
  if (QUIC_FAILED(status))
  {
    SetReason("ConfigurationOpen for the server failed (0x%08x)", static_cast<unsigned>(status));
    Close();
    return false;
  }
  m_serverConfig = serverConfig;

  QUIC_CREDENTIAL_CONFIG serverCredential{};
  serverCredential.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
  serverCredential.Flags = QUIC_CREDENTIAL_FLAG_NONE;
  serverCredential.CertificateContext = const_cast<void*>(m_certificate->Context());
  status = m_table->ConfigurationLoadCredential(serverConfig, &serverCredential);
  if (QUIC_FAILED(status))
  {
    SetReason("Schannel refused the development certificate (0x%08x) -- see DevCertificate.h", static_cast<unsigned>(status));
    Close();
    return false;
  }

  m_reason[0] = '\0';
  return true;
}

void QuicApi::Close() noexcept
{
  // A registration cannot close while a connection on it lives, and MsQuic's answer to being asked
  // is to wait forever rather than to fail. Whoever got the order wrong wants to know here, at the
  // call that would have hung, rather than in a shipped build's shutdown -- which must not throw
  // (AGENTS.md 5), so this is the debug-only form of the tree's macro.
  DEBUG_ASSERT_TEXT(m_outstanding == 0, L"QuicApi::Close ran with transports or listeners still open on it");

  if (m_table != nullptr)
  {
    if (m_clientConfig != nullptr)
      m_table->ConfigurationClose(static_cast<HQUIC>(m_clientConfig));
    if (m_serverConfig != nullptr)
      m_table->ConfigurationClose(static_cast<HQUIC>(m_serverConfig));
    if (m_registration != nullptr)
      m_table->RegistrationClose(static_cast<HQUIC>(m_registration));
    MsQuicClose(m_table);
  }

  m_clientConfig = nullptr;
  m_serverConfig = nullptr;
  m_registration = nullptr;
  m_table = nullptr;

  // After the configurations, never before: Schannel holds the credential the certificate carries
  // for as long as a configuration is loaded with it.
  m_certificate.reset();
}

const char* QuicApi::Reason() const noexcept
{
  return m_reason;
}

bool QuicApi::IsOpen() const noexcept
{
  return m_table != nullptr && m_clientConfig != nullptr && m_serverConfig != nullptr;
}

const QUIC_API_TABLE* QuicApi::Table() const noexcept
{
  return m_table;
}

void* QuicApi::Registration() const noexcept
{
  return m_registration;
}

void* QuicApi::ClientConfiguration() const noexcept
{
  return m_clientConfig;
}

void* QuicApi::ServerConfiguration() const noexcept
{
  return m_serverConfig;
}

std::uint32_t QuicApi::IdleTimeoutMs() const noexcept
{
  return m_idleTimeoutMs;
}

const char* QuicApi::Alpn() const noexcept
{
  return QUIC_ALPN;
}

void QuicApi::AcquireHandle() noexcept
{
  ++m_outstanding;
}

void QuicApi::ReleaseHandle() noexcept
{
  DEBUG_ASSERT_TEXT(m_outstanding > 0, L"a QUIC handle was released twice");
  if (m_outstanding > 0)
    --m_outstanding;
}

void QuicApi::SetReason(const char* _format, ...) noexcept
{
  std::va_list arguments;
  va_start(arguments, _format);
  (void)std::vsnprintf(m_reason, sizeof(m_reason), _format, arguments);
  va_end(arguments);
}
} // namespace Neuron
