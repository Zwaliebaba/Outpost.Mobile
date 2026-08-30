# 0023 — The development credential is self-signed at boot, and the client does not validate

Status: accepted
Date: 2026-08-29

## Context

Schannel will not run a QUIC server without a certificate whose private key it can reach. This
repository has no certificate store to read one from, no configuration file to name one in, no
installer to put one there, and no argv or environment to point at one (AGENTS.md §5). CI runs on a
fresh runner and a contributor runs on a fresh clone, and a step either of them has to perform by
hand before the game will start is a step that will be skipped, forgotten, or performed differently
on two machines.

The client half has the mirror-image problem. A self-signed certificate is not in anybody's trust
store, so a client that validates will refuse it — which means the choice about the server's
credential is also a choice about the client's validation, and they have to be made together.

## Decision

**The server's certificate is generated at boot, and the client is told not to validate it.**

`Neuron::DevCertificate` opens an RSA-2048 key under the Microsoft Software Key Storage Provider,
persisted under the fixed name `Outpost.Dev.Quic` and reused on the next boot if it is already
there, and builds a self-signed X.509 for `CN=Outpost Development`, valid one year, carrying a
server-authentication EKU and a digital-signature key usage. The certificate is handed to MsQuic as
`QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT` and is written to no store. Removing what it leaves
behind is one command, and `DevCertificate.h` says so: `certutil -delkey -user Outpost.Dev.Quic`.

The client configuration sets `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`. **This is a
placeholder and it is declared as one at three sites**: the flag itself, `QuicApi::Desc`, and this
record. `QuicApi::Open` refuses outright unless the caller has set `allowUnvalidatedPeer`, so no
caller gets an unauthenticated connection without having said in one word that it knows.

What this buys and what it does not: the connection is encrypted and integrity-checked by TLS 1.3,
and it is **not authenticated**. Nothing about the shipped game's trust model is decided here.

## Alternatives considered

- **A certificate the owner provisions by hand**, with its thumbprint as a constant in the
  composition root and `QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE` to find it. This is what a
  shipping build would do and it is the honest long-run answer. Rejected for now: it makes a fresh
  clone and a CI runner fail at boot until somebody performs an install step, and it puts a machine-
  specific value in a file the whole team shares.
- **Adding the generated certificate to `CurrentUser\My`** and loading it by hash. Rejected as
  unnecessary: `CERTIFICATE_CONTEXT` takes the context directly, so nothing has to be written to a
  store at all, and a store entry is state the game would then have to clean up. It stays the stated
  fallback in the work order if Schannel ever refuses the context form.
- **A key held only in memory, never persisted.** Cleaner — nothing left behind — and rejected
  because Schannel is known to refuse a key it cannot reach through a provider, and because a new
  key every boot means a new certificate every boot for no gain.
- **Turning validation on and installing the certificate into the trust root.** Rejected: writing to
  a trust root needs elevation, and a development certificate in a machine's trust root is a real
  security problem on a real developer's machine, in exchange for authenticating a connection to
  `127.0.0.1` against a certificate the same process just generated.
- **Skipping TLS.** Not available: QUIC has no unencrypted mode, and this is a feature.

## Consequences

- **The tree ships an unauthenticated connection, deliberately and visibly.** Anything that can
  reach the port can be the server. The port is `127.0.0.1` only, which bounds it to the machine
  (`Design/Archive/QuicTransport.md` §11), and that is the whole of the mitigation.
- **A trust model is now owed** before this is anything but a development transport. It is not
  scheduled, and this record is what a later one supersedes.
- **`NeuronCore` gains `crypt32` and `ncrypt`**, by `#pragma comment(lib, ...)` in
  `DevCertificate.cpp` beside the existing `ws2_32` precedent. Both are Windows SDK, so no new
  package.
- **The first run of the game leaves a key on the machine**, and the second run reuses it. That is
  state the tree did not have before; the removal command is in the header for the day somebody
  wants it gone, and `ACredentialIsAcquiredWithoutAStore` is what pins reuse down so the count of
  keys left behind stays at one.
- **A locked-down key store makes the game fall back to the loopback rather than fail to boot.**
  `DevCertificate` never throws and never asserts: it reports the call that failed and its status,
  `QuicApi::Open` returns false with that reason, and `OutpostApp` logs it — which is AGENTS.md §5's
  "errors that are the user's fault are diagnostics, not crashes".
