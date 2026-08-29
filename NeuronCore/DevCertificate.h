#pragma once

#include <cstdint>

#include "QuicApi.h"

namespace Neuron
{
// A self-signed server certificate for a tree that has no PKI (Design/QuicTransport.md 7, ADR 0023).
//
// Schannel will not run a QUIC server without a certificate whose private key it can reach, and this
// repository has no certificate store to read one from, no configuration file to name one in, and no
// installer to put one there. So the certificate is made rather than read: an RSA-2048 key under the
// Microsoft Software Key Storage Provider, persisted under one fixed name and reused on the next
// boot, and a self-signed X.509 for CN=Outpost Development that points at it. Nothing is written to
// a certificate store, and nothing needs an elevated prompt -- because every one of those is a step
// a fresh clone or a CI runner would fail on.
//
// DEVELOPMENT ONLY. The client that talks to a server holding this certificate is configured not to
// validate it (QuicApi::Desc::allowUnvalidatedPeer), so the connection is encrypted and
// integrity-checked by TLS 1.3 and it is not authenticated. The shipped game's trust model is a
// later decision and nothing here decides it.
//
// This class leaves one thing behind on the machine that runs it: the persisted key. To remove it,
//
//     certutil -delkey -user Outpost.Dev.Quic
//
// Every failure is a diagnostic in Reason(), never a throw and never an assert: a locked-down key
// store is the machine's circumstances and not this program's mistake (AGENTS.md 5).
class DevCertificate final
{
public:
  DevCertificate() = default;
  ~DevCertificate();

  DevCertificate(const DevCertificate&) = delete;
  DevCertificate& operator=(const DevCertificate&) = delete;

  [[nodiscard]] bool Acquire();
  void Release() noexcept;

  // PCCERT_CONTEXT, typed void* to keep <wincrypt.h> out of this header and out of the umbrella.
  // Valid until Release; MsQuic is handed it as QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT and
  // Schannel reaches back into it, so this object outlives the configuration built from it.
  [[nodiscard]] const void* Context() const noexcept;

  [[nodiscard]] const char* Reason() const noexcept;

  // Whether the last Acquire generated the key or found the one a previous run left behind. The test
  // writes it to the log so that a run says which path it took.
  [[nodiscard]] bool KeyWasCreated() const noexcept;

private:
  void SetReason(const char* _format, ...) noexcept;

  const void* m_context = nullptr;
  bool m_keyWasCreated = false;
  char m_reason[QUIC_REASON_BYTES] = {};
};
} // namespace Neuron
