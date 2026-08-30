# Work order — QUIC transport slice 1: the transport, the listener, the credential

Implements slice 1 of [`QuicTransport.md`](QuicTransport.md) §13: `QuicApi`, `QuicTransport`,
`QuicListener` and `DevCertificate` in `NeuronCore`, their tests in `NeuronCoreTests`, and the
three decision records the design owes (design §13).

**Layer:** `NeuronCore` and `NeuronCoreTests`. Plus two sentences in `AGENTS.md` and the ADR index.
**Depends on:** nothing. The package is restored and imported (`NeuronCore.vcxproj:196`,
`NeuronCoreTests.vcxproj:202`).
**Blocks:** slice 2 (the composition root has nothing to wire until this exists).

---

## 1. Why this is a slice

Nothing in the game changes. `Outpost.exe` still boots on the loopback, and no file outside
`NeuronCore/`, `Tests/NeuronCoreTests/`, `Design/` and `AGENTS.md` is touched — that is the
acceptance, not a limitation. What lands is a second implementation of `Transport` that the tests
drive end to end over `127.0.0.1`, the listener the server half will need the day it runs alone,
and the first synchronisation primitives in the tree, confined and recorded. Slice 2 is then a
composition-root change reviewed against a transport that has already proved itself in a suite.

---

## 2. Scope

### 2.1 `NeuronCore/QuicApi.h/.cpp` — the library, once

```cpp
class QuicApi final
{
public:
  struct Desc
  {
    std::uint32_t idleTimeoutMs = QUIC_IDLE_TIMEOUT_MS;           // 10 000
    std::uint32_t keepAliveMs = QUIC_KEEP_ALIVE_MS;               // 2 000
    std::uint32_t handshakeTimeoutMs = QUIC_HANDSHAKE_TIMEOUT_MS; // 3 000
    bool allowUnvalidatedPeer = false; // the development root sets this; nothing else may
  };

  [[nodiscard]] bool Open(const Desc& _desc);   // false and Reason() on failure; never throws
  void Close() noexcept;
  [[nodiscard]] const char* Reason() const noexcept;
  [[nodiscard]] bool IsOpen() const noexcept;
```

- `Open` calls `MsQuicOpen2`, opens one registration (`"Outpost"`,
  `QUIC_EXECUTION_PROFILE_LOW_LATENCY`), and builds two configurations on the ALPN
  `QUIC_ALPN = "outpost-1"`: the client one with `QUIC_CREDENTIAL_FLAG_CLIENT` plus
  `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION` when `allowUnvalidatedPeer` (and `Open` fails
  with a reason when it is false, because there is no validating path yet — say so in the reason),
  and the server one from `DevCertificate::Acquire()` as
  `QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT`. Both set `DatagramReceiveEnabled = 1`,
  `IdleTimeoutMs`, `KeepAliveIntervalMs`, `HandshakeIdleTimeoutMs`, `PeerBidiStreamCount = 1`
  through `MsQuicSettings` from `msquic.hpp`.
- The header includes neither `msquic.h` nor `msquic.hpp`: it forward-declares `struct
  QUIC_API_TABLE` outside `namespace Neuron` and holds the registration and configurations as
  `void*` members named `m_registration`, `m_clientConfig`, `m_serverConfig`. The `.cpp` includes
  `<msquic.h>` and `<msquic.hpp>` before `NeuronCore.h`'s Winsock order can be disturbed — put
  them after `pch.h`, which already has Winsock first. Only `QuicApi.cpp`, `QuicTransport.cpp`
  and `QuicListener.cpp` name an MsQuic type.
- `Close` closes the two configurations, the registration, then `MsQuicClose`. It must be called
  after every transport and listener made from this object is closed; the destructor calls it and
  asserts nothing is outstanding (`ASSERT_TEXT`, the tree's macro).
- Not a singleton. No static. Constructed by whoever owns it and passed by reference.

### 2.2 `NeuronCore/DevCertificate.h/.cpp` — the credential

```cpp
// A self-signed certificate for a server with no PKI (Design/Archive/QuicTransport.md 7, ADR 0020).
// Development only: the client that talks to it validates nothing, and the header says so.
// To remove the key this leaves behind: certutil -delkey -user Outpost.Dev.Quic
class DevCertificate final
{
public:
  [[nodiscard]] bool Acquire();            // false and Reason() on failure; never throws
  void Release() noexcept;
  [[nodiscard]] const void* Context() const noexcept; // PCCERT_CONTEXT, typed void* to keep wincrypt out of the header
  [[nodiscard]] const char* Reason() const noexcept;
```

- `NCryptOpenStorageProvider(MS_KEY_STORAGE_PROVIDER)`; `NCryptOpenKey` on
  `L"Outpost.Dev.Quic"`, and if absent `NCryptCreatePersistedKey` RSA 2048 + `NCryptFinalizeKey`.
  Then `CertCreateSelfSignCertificate` for `CN=Outpost Development`, one year, with a
  `CRYPT_KEY_PROV_INFO` naming that key so Schannel can find the private half. The certificate
  is not written to any store.
- Every failure is a diagnostic with the failing call and its status in `Reason()`. No throw, no
  assert: this is the "user's fault" class of error in AGENTS.md §5 — a locked-down key store
  is a reason, not a crash.
- `crypt32.lib` and `ncrypt.lib` by `#pragma comment(lib, ...)` in the `.cpp`, beside the
  existing `ws2_32` pragma's precedent in `NeuronCore.h:79`.

### 2.3 `NeuronCore/QuicTransport.h/.cpp` — one connection

```cpp
struct Endpoint
{
  const char* host = "127.0.0.1";
  std::uint16_t port = 0;
};

class QuicTransport final : public Transport
{
public:
  struct Desc
  {
    std::uint32_t capacityDatagrams = 256; // per ring, fixed at Connect; nothing allocates after
  };

  [[nodiscard]] bool Connect(QuicApi& _api, const Endpoint& _peer, const Desc& _desc); // client side
  void Close() noexcept;   // ConnectionShutdown; the state goes Draining and later Closed
  ~QuicTransport() override;   // waits for SHUTDOWN_COMPLETE, bounded by the idle timeout, then ConnectionClose

  [[nodiscard]] bool Send(const std::uint8_t* _bytes, std::uint32_t _count) override;
  [[nodiscard]] std::uint32_t Receive(std::uint8_t* _outBytes, std::uint32_t _capacity) override;
  void Poll() override;
  [[nodiscard]] ConnectionState State() const override;

  [[nodiscard]] std::uint32_t DroppedCount() const noexcept;   // inbound ring overflows, counted on the worker
  [[nodiscard]] std::uint32_t MaxSendLength() const noexcept;  // what the peer will take; 0 until DATAGRAM_STATE_CHANGED
```

- **The state table is design §4.1, verbatim.** `Connected` requires both the `CONNECTED` event
  and a `DATAGRAM_STATE_CHANGED` with `SendEnabled` and `MaxSendLength >= MAX_DATAGRAM_BYTES`; a
  peer that reports less is shut down with application error code `1` and the state goes to
  `Draining`. The state is a `std::atomic<ConnectionState>` written by the worker, read by
  `State()`.
- **Inbound ring** (design §4.2): `m_inArena` of `capacityDatagrams × MAX_DATAGRAM_BYTES` bytes and
  `m_inSlots` of `{size}`, allocated in `Connect` (or in `Adopt`, 2.4). The worker's
  `DATAGRAM_RECEIVED` handler locks `m_inLock` (`std::mutex`), copies into the slot at
  `m_inWrite`, advances it, unlocks; when the ring is full it increments `m_dropped`
  (`std::atomic<std::uint32_t>`) and copies nothing. `Poll()` locks, copies `m_inWrite` to
  `m_inReady`, unlocks. `Receive` pops from `m_inRead` up to `m_inReady` with no lock, refusing
  with `ASSERT_TEXT` a `_capacity` smaller than the slot, as the loopback does.
- **Outbound ring**: the same arena shape with `m_outSlots` of `{QUIC_BUFFER buffer;
  std::atomic<bool> inFlight}`. `Send` refuses `_count > MAX_DATAGRAM_BYTES` and a state other than
  `Connected`; finds the next slot whose `inFlight` is false (one cursor, wrapping once, because
  completion is nearly in order); copies; sets `inFlight`; calls `DatagramSend(conn, &buffer, 1,
  QUIC_SEND_FLAG_NONE, slotIndexAsContext)`. A `DatagramSend` failure clears the flag and returns
  false. The worker's `DATAGRAM_SEND_STATE_CHANGED` clears `inFlight` when
  `QUIC_DATAGRAM_SEND_STATE_IS_FINAL(State)`. No lock on this path.
- **The callback** is `static QUIC_STATUS QUIC_API OnConnectionEvent(HQUIC, void* _context,
  QUIC_CONNECTION_EVENT*)` in the `.cpp`, `_context` being `this`. It handles the six events named
  above and `SHUTDOWN_COMPLETE` (state `Closed`, signal the destructor's wait) and returns
  `QUIC_STATUS_SUCCESS` for everything else. It allocates nothing, logs nothing, and calls no
  MsQuic API.
- The destructor's wait is a `std::condition_variable` on the state reaching `Closed`, with a
  timeout of `idleTimeoutMs`; on timeout it closes anyway and this is the one place the comment
  says "and if MsQuic is still holding a reference, the leak is preferred to the deadlock".

### 2.4 `NeuronCore/QuicListener.h/.cpp` — the server end

```cpp
class QuicListener final
{
public:
  struct Desc
  {
    std::uint32_t backlog = 1;                 // transports pre-allocated to hand accepted connections to
    QuicTransport::Desc transport;             // what each of them is built with
  };

  [[nodiscard]] bool Start(QuicApi& _api, std::uint16_t _port, const Desc& _desc); // 0 = ephemeral
  void Stop() noexcept;
  [[nodiscard]] std::uint16_t Port() const noexcept;                // the bound port, for a port-0 Start
  void Poll();                                                       // owning thread: moves accepted into Accepted()
  [[nodiscard]] std::span<QuicTransport* const> Accepted() const noexcept;
  [[nodiscard]] const char* Reason() const noexcept;
```

- `Start` opens the listener on the registration and starts it on `127.0.0.1:_port` — loopback
  only, by design §5 and §11; the address is not a parameter. A failure (port in use, listener
  refused) is `false` with a reason, never an assert: slice 2's fallback reads it.
- `NEW_CONNECTION` on the worker: take the next unused pre-allocated `QuicTransport`, give it the
  connection via `QuicTransport::Adopt(HQUIC, QuicApi&)` (a private method the listener is a
  friend of, which sets the callback with `SetCallbackHandler` and calls
  `ConnectionSetConfiguration` with the server configuration), push its index onto a small
  locked vector. Refuse (`QUIC_STATUS_CONNECTION_REFUSED`) when the backlog is exhausted.
  `Poll()` locks, moves the indices into the owner-side `m_accepted`, unlocks.
- `Stop` closes the listener handle (which waits for its callbacks, as MsQuic documents) and then
  closes each pre-allocated transport. `QuicApi::Close` must come after.

### 2.5 Registration and the umbrella

- `NeuronCore.vcxproj` and `.filters`: `QuicApi.h/.cpp`, `DevCertificate.h/.cpp`,
  `QuicTransport.h/.cpp`, `QuicListener.h/.cpp`, in the same filter as `LoopbackTransport`.
- `NeuronCore.h`: `#include "QuicApi.h"`, `"QuicTransport.h"`, `"QuicListener.h"` after
  `LoopbackTransport.h`. `DevCertificate.h` is included only by `QuicApi.cpp`.
- `Tests/NeuronCoreTests`: `QuicTransportTests.cpp` in the `.vcxproj` and under
  `<Filter>Tests</Filter>` in the `.filters`.

### 2.6 What this slice deliberately does **not** do

- Touch `Outpost/`, `GameLogic/`, `NeuronServer/`, `NeuronClient/` or any test suite but
  `NeuronCoreTests`. The game boots on the loopback until slice 2.
- Change `Transport.h`, `MAX_DATAGRAM_BYTES`, or `LoopbackTransport`. The reliable lane is slice
  3a.
- Open a stream. `PeerBidiStreamCount = 1` is set and unused.
- Listen on anything but `127.0.0.1`; validate a certificate; read a file, the registry or an
  environment variable.

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| `NeuronCore/Transport.h` | the four pure virtuals and their contract, `MAX_DATAGRAM_BYTES`, the five `ConnectionState`s — three of them unused until now |
| `NeuronCore/LoopbackTransport.h/.cpp` | the fixed-stride ring (`m_slots`, `m_arena`, `Accept`, the head/ready cursors), the refuse-do-not-truncate rule at `Send`, the `ASSERT_TEXT` on `Receive` capacity — copy the shape, then add the lock |
| `NeuronCore/NeuronCore.h:51–52, 79` | Winsock before `<windows.h>`, and the `#pragma comment(lib)` precedent |
| `NeuronCore/FrameClock.h` | `Now()` / `ElapsedMs()` for the bounded waits in the tests and the destructor |
| `packages/Microsoft.Native.Quic.MsQuic.Schannel.2.6.1/build/native/include/msquic.hpp` | `MsQuicSettings`, `MsQuicAlpn`, `MsQuicCredentialConfig` — use them for settings and credential structs; do not wrap handles in `MsQuicConnection`, whose callback model does not fit a ring |
| `Tests/NeuronCoreTests/LoopbackTransportTests.cpp` | `Buffer`, `SendByte`, the assertion messages — the tests below reuse its vocabulary |
| `Build/CheckProjectFiles.py` | `check_registration` will fail on an unregistered file; `check_headless` passes `msquic.h` |
| `Design/Archive/QuicTransport.md` §4, §5, §7, §11 | the transport, the listener, the credential, and what was turned down |
| AGENTS.md §5 "Single-writer state", `Transport.h:46` | the threading rule this slice is the first to need |
| ADR 0001, 0008 | Core is headless; the format is not Core's business |

---

## 4. Acceptance

Tests in `Tests/NeuronCoreTests/QuicTransportTests.cpp`, class `QuicTransportTests`, sentence-named,
a why-comment each. A file-scope helper `Pair` opens a `QuicApi{allowUnvalidatedPeer = true}`, a
listener on port 0, a client to `Port()`, and `PumpUntil(predicate, timeoutMs)` polls both ends on
a `FrameClock` — every wait is bounded, and a timeout fails the test with the states it saw.

- **`AConnectionReachesConnectedOnBothEnds`** — within `QUIC_HANDSHAKE_TIMEOUT_MS`, both ends
  report `Connected`.
- **`TheMaxSendLengthCoversTheDatagram`** — `MaxSendLength() >= MAX_DATAGRAM_BYTES` on both ends;
  the value is written to the test log with `Logger::WriteMessage` so the number appears in the
  pull request.
- **`AFullSizedDatagramSurvivesAndALargerOneIsRefused`** — 1152 bytes of a known pattern arrive
  byte-for-byte; 1153 returns false and nothing arrives.
- **`AFragmentSizedBurstArrivesIntact`** — thirteen 1152-byte datagrams (one snapshot's fragments,
  `ShipsPerSnapshotFragment()`'s worth) sent back to back all arrive, each intact, in any order.
- **`BothDirectionsAreIndependent`** — the loopback's test over QUIC.
- **`AFullRingDropsTheNewestAndCountsIt`** — 257 sends with no `Poll` on the receiver; after a
  `Poll`, 256 are readable and `DroppedCount() == 1`. Use a `capacityDatagrams` of 8 and 9 sends
  if 257 is slow; the number is the ring's, not the test's.
- **`NothingIsDeliveredOutsidePoll`** — send, spin 50 ms without polling the receiver, `Receive`
  returns 0; one `Poll`, and it does not.
- **`AClosedPeerDrainsThenCloses`** — client `Close()`; the server end reaches `Closed` within
  the idle timeout and `Send` on it returns false.
- **`ARefusedListenerReportsWhy`** — a second listener on the first's `Port()`: `Start` is
  false, `Reason()` is non-empty, no assert fired.
- **`ACredentialIsAcquiredWithoutAStore`** — `DevCertificate::Acquire()` succeeds on a clean
  machine and again on the second call (the persisted key is reused, not recreated). Test log
  states which path it took.

**The existing suites** — `LoopbackTransportTests` and every other test in the four suites pass
unchanged.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` (clang-format 18.1.3) pass.
- Debug|x64 builds with no new warnings; `msquic.dll` is beside `NeuronCoreTests.dll` (the
  package's target does this — verify, do not add a copy step).
- The game runs exactly as before. No screenshot: nothing visual.
- A code read confirms: no allocation after `Connect`/`Start`; the mutex is held only around a
  memcpy or a cursor copy; `std::mutex`, `std::atomic` and `std::condition_variable` appear in no
  file but `QuicTransport.cpp/.h` and `QuicListener.cpp/.h`.
- Three decision records, numbered next after 0017 and indexed, per design §13: **the network
  transport is MsQuic** (context: the package landed in `ac1eb00` without the record §9 requires
  — say so), **MsQuic's workers enqueue to a ring and the owning thread delivers** (with
  app-driven execution as the alternative turned down), **the development credential is
  self-signed at boot and the client does not validate**.
- `AGENTS.md` §5: the sentence "MsQuic … no code references it yet either" becomes a sentence
  saying `QuicTransport` exists and the game does not yet boot on it; §2 "what remains is a socket
  and a second process" becomes "what remains is a second process". Both in the same commit.
- `Design/Archive/QuicTransport.md` §13 marks slice 1 `landed`; this file moves to `Design/Archive/`.

---

## 5. Assumptions the implementer may make

- **MsQuic's `MaxSendLength` on localhost exceeds 1152.** If the test proves otherwise, stop and
  report; do not lower `MAX_DATAGRAM_BYTES`, which the format's fragment sizes derive from.
- **Schannel accepts a persisted NCrypt key by `CRYPT_KEY_PROV_INFO`.** If it insists on a store,
  the fallback is to add the certificate to `CurrentUser\My` and use
  `QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE` — report it, keep `DevCertificate`'s interface,
  and add the removal command for the store to the header comment.
- **Completion order of datagram sends is close to send order**, so one wrapping cursor over the
  outbound ring finds a free slot in O(1) amortised. A full scan is acceptable if it is not.
- **One client per listener** in every test and in slice 2; `backlog` is a number so it need not
  be a rewrite later, not a promise that N works.
- **The worker thread count is MsQuic's default.** No processor affinity, no execution config.
- **Nothing is reliable.** A datagram may be lost or reordered by QUIC on a real path; the
  tests run on localhost where neither happens, and none of them may depend on that being true
  for correctness — only for timing.
