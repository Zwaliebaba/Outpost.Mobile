# The QUIC transport — moving the seam onto MsQuic

**Status: ready to implement.** §13 lists the slices; §14 is the implementation plan. Every open
question was put to the owner on 2026-08-29 and settled (§12); the work orders for slices 1 and 2
are written and linked from §13.

This document proposes how the client/server seam stops being a loopback and becomes a network:
a `QuicTransport` in `NeuronCore` over MsQuic 2.6.1, the package the tree restored on 2026-08-29
(`Microsoft.Native.Quic.MsQuic.Schannel`, imported by `NeuronCore`, `Outpost` and
`NeuronCoreTests`, referenced by no code yet — AGENTS.md §5). It says what the transport looks
like *in this tree*: which layer holds what, what already exists to build on, how MsQuic's worker
threads meet a single-threaded engine, what the numbers are, and what is deliberately left out.

The short version: the `Transport` seam was built so that this change is a swap plus a
composition root, and it is. `WorldView.h` says of itself that "the day the transport becomes a
socket, nothing here changes at all", and this design holds it to that. What is new is one
implementation of an existing interface, one listener beside it, the first synchronisation
primitives in the tree, and a credential. The wire format does not change until a later slice
that has its own reason to (§8).

---

## 1. What is being built

1. **`Neuron::QuicTransport`** — a `Transport` whose datagrams are QUIC DATAGRAM frames on one
   MsQuic connection. One instance is one connection to one peer. `Send` queues a datagram for
   MsQuic, `Poll` moves what MsQuic's workers have delivered onto the owning thread, `Receive`
   hands it out, `State` follows the connection's life. Same contract as `LoopbackTransport`,
   including "a full queue drops, it does not block".
2. **`Neuron::QuicListener`** — the server end that does not exist for a loopback: opens a port,
   accepts a connection, and yields a `QuicTransport` for it. One client in this design (§10);
   the shape does not assume one.
3. **`Neuron::QuicApi`** — the process-wide MsQuic handle: `MsQuicOpen2`, one registration, the
   client and server configurations. Owned by the composition root as a value and passed by
   reference, never a singleton, in the way `ServerHost::Desc` and `Camera::Desc` are passed
   (AGENTS.md §5, "no argv, no environment variables").
4. **A development credential.** Schannel will not run a QUIC server without a certificate, and
   there is no PKI here. A self-signed certificate made at boot, and a client that does not
   validate — declared as the placeholder it is (§7).
5. **The composition root wired both ways.** `Outpost.exe` stays one process
   (Design/Collision.md §18, decision 8) and talks to itself over `127.0.0.1`: the in-process
   server listens, the in-process client connects, and every frame of the game crosses the real
   stack. If QUIC cannot open — no port, no credential, no MsQuic — the root logs why and falls
   back to the loopback, so a missing network is a diagnostic and not a failed boot (AGENTS.md
   §5: "a missing hull logs and is skipped — it does not fail boot").

Not built here, named so nobody looks for it: a second executable, a reliable lane on the wire,
a real certificate chain, more than one client, NAT traversal (§11).

---

## 2. What the tree already guarantees

- **The seam is structural.** `NeuronCore/Transport.h` declares four pure virtuals —
  `Send`, `Receive`, `Poll`, `State` — with the contract that a false `Send` is normal, a
  `Receive` returns 0 when nothing waits, and "delivery happens here, on the owning thread.
  Foreign threads may only enqueue" (`Transport.h:46`). `WorldView` and `WorldSimulation` hold a
  `Neuron::Transport*` and nothing more specific. `MAX_DATAGRAM_BYTES = 1152`.
- **The reference implementation is a fixed-stride ring.** `LoopbackTransport::Connect`
  allocates `capacityDatagrams` slots of `MAX_DATAGRAM_BYTES` once and never again; a full
  queue drops the newest; an oversized datagram is refused, never truncated. `QuicTransport`
  copies this shape so the two behave alike under pressure (§4).
- **The format already survives a network.** `GameLogic/WorldSnapshot` fragments a snapshot into
  datagrams under `MAX_DATAGRAM_BYTES`, reassembles by `{snapshotId, fragmentIndex,
  fragmentCount}`, drops a snapshot missing a fragment whole, ignores a stale tick, and writes
  explicit little-endian (ADR 0008, Collision-slice-2b §2.4). Nothing in it assumes ordering
  beyond "at most one snapshot in progress", which QUIC datagrams can violate only by
  reordering across a snapshot boundary — the receiver's stated response is "drop the older".
- **The threading rule exists before the threads do.** AGENTS.md §5, single-writer state:
  "when a transport's workers or an audio callback arrive, they enqueue to a ring and touch
  nothing else." This design is the first thing that rule was written for.
- **The package is restored and its targets imported** in `NeuronCore.vcxproj:196`,
  `Outpost.vcxproj:417`, `NeuronCoreTests.vcxproj:202`: include path, `msquic.lib`, and a
  post-build copy of `msquic.dll` beside each executable that imports it. Nothing in the tree
  includes `<msquic.h>`.
- **`NeuronCore` is headless** (ADR 0001) and `msquic.h` is not a graphics header; `Build/
  CheckProjectFiles.py`'s `check_headless` will pass a `QuicTransport` in Core, and its
  `check_dependency_rules` substring test does not trip on the package path.
- **Winsock is already there.** `NeuronCore.h:51–52` includes `<WinSock2.h>` and `<ws2tcpip.h>`
  and pragmas `ws2_32.lib`; `QUIC_ADDR` is a `SOCKADDR_INET` and needs nothing more.

---

## 3. Where each piece lives

| Piece | Layer | File | Why there |
|---|---|---|---|
| `QuicApi`, `QuicTransport`, `QuicListener` | NeuronCore | `NeuronCore/QuicApi.h/.cpp`, `QuicTransport.h/.cpp`, `QuicListener.h/.cpp` | Beside `Transport.h` and `LoopbackTransport`; the package is imported here; it is engine, with zero game semantics. R2 already reserves the name: "`Transport` and, when one exists, `QuicTransport`" (AGENTS.md §1). |
| `DevCertificate` | NeuronCore | `NeuronCore/DevCertificate.h/.cpp` | Makes the self-signed credential the listener needs; crypt32/ncrypt are Windows SDK. Lives in Core because the server half must have it when it runs alone (ADR 0001). |
| Tests | NeuronCoreTests | `Tests/NeuronCoreTests/QuicTransportTests.cpp` | Beside `LoopbackTransportTests.cpp`; the package is already imported into this project, which is why `msquic.dll` lands beside the test DLL. |
| Wiring, fallback, boot log | Outpost | `Outpost/OutpostApp.h/.cpp` | The composition root is the only place allowed to choose a transport. |
| Nothing | GameLogic, NeuronServer, NeuronClient | — | The format does not change; `ServerHost` ticks and knows no transport; the renderer never sees one. |

The umbrella `NeuronCore.h` gains three includes after `LoopbackTransport.h`. `msquic.h` is
included only by the three `Quic*.cpp` files; no header in the tree names an MsQuic type
(`QuicApi.h` forward-declares the API table and holds handles as `void*`), so the day the package
version changes, the blast radius is one directory.

---

## 4. `QuicTransport` — one connection as a `Transport`

### 4.1 Lifetime and state

A `QuicTransport` is constructed empty and bound to an `HQUIC` connection by one of two paths:
`QuicTransport::Connect(QuicApi&, const Endpoint&)` on the client, or the listener handing it an
accepted connection on the server. Its `ConnectionState` follows MsQuic's events exactly:

| MsQuic event | `State()` |
|---|---|
| construction | `Disconnected` |
| `ConnectionStart` issued / listener accepted | `Connecting` |
| `QUIC_CONNECTION_EVENT_CONNECTED` **and** `DATAGRAM_STATE_CHANGED` with `SendEnabled` and `MaxSendLength >= MAX_DATAGRAM_BYTES` | `Connected` |
| `SHUTDOWN_INITIATED_BY_TRANSPORT` / `_BY_PEER`, or `Close()` called | `Draining` |
| `SHUTDOWN_COMPLETE` | `Closed` |

Two of the five states were unused by the loopback; all five are used here. A connection whose
peer will not take datagrams of `MAX_DATAGRAM_BYTES` never reaches `Connected`: it is shut down
with an application error code and logged, because a transport that silently truncates is the
bug the loopback refuses to have (`LoopbackTransport.cpp:34–55`). With QUIC's 1280-byte MTU
floor and IPv4 on localhost the reported `MaxSendLength` sits comfortably above 1152; the test in
§9 pins that it does.

`Close()` issues `ConnectionShutdown` and returns; the destructor waits for `SHUTDOWN_COMPLETE`
before `ConnectionClose`, because MsQuic may still be calling back into an object that is being
torn down. That wait is bounded by the connection's idle timeout and is the one place the
transport blocks — in a destructor, on the owning thread, at shutdown, which AGENTS.md §5 allows
to do nothing rather than report.

### 4.2 Two rings and one lock

MsQuic delivers on its own worker threads. The engine is single-threaded and the rule is that
foreign threads only enqueue. So:

- **Inbound.** `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` runs on a worker; the handler takes a
  lock, copies the datagram into the next free slot of a fixed-stride ring (`capacityDatagrams ×
  MAX_DATAGRAM_BYTES`, allocated once in `Connect`, exactly `LoopbackTransport`'s arena), bumps
  the write cursor, releases. A full ring drops the newest and counts it. `Poll()` on the owning
  thread takes the lock, copies the write cursor into the owner's ready cursor, releases;
  `Receive` then pops from the ring without the lock, because everything below the ready cursor
  is owned by the reader and nothing above it is touched by anyone but the writer. The lock is
  held for one memcpy of at most 1152 bytes.
- **Outbound.** `Send` runs on the owning thread; MsQuic's `DatagramSend` takes the buffer by
  reference and keeps it until `DATAGRAM_SEND_STATE_CHANGED` reports a final state
  (`QUIC_DATAGRAM_SEND_STATE_IS_FINAL`). So `Send` copies into a slot of a second ring, passes
  the slot's `QUIC_BUFFER` with the slot index as `ClientContext`, and the state-changed callback
  — on a worker — marks the slot free. Slot reclamation is a per-slot `std::atomic<bool>`; no
  lock on the send path. A ring with no free slot makes `Send` return false, which is the
  contract's "queue full", and the callers already drop rather than block.
- **Nothing else crosses.** Connection events other than the two above set an atomic state and
  return. No callback allocates, logs, or calls another MsQuic API except `SetCallbackHandler`
  on accept.

These are the first `std::mutex` and `std::atomic` in the tree. They are confined to the two
`Quic*.cpp` files, and ADR 0019 (§13) records that as the rule rather than a precedent.

### 4.3 Sizes

| Constant | Value | Where | Why |
|---|---|---|---|
| `MAX_DATAGRAM_BYTES` | 1152, unchanged | `Transport.h` | Under QUIC's `MaxSendLength` at the 1280 floor, and the format's fragment sizes derive from it (`ShipsPerSnapshotFragment() = 13`). |
| `QuicTransport::Desc::capacityDatagrams` | 256 | `QuicTransport.h` | The loopback's default; 288 KB per ring, two rings per transport. |
| `QUIC_IDLE_TIMEOUT_MS` | 10 000 | `QuicApi.cpp` | Past this silence MsQuic drains the connection; the publish rate is 10 Hz, so silence means the peer is gone. |
| `QUIC_KEEP_ALIVE_MS` | 2 000 | `QuicApi.cpp` | A client with nothing selected sends no orders; the keep-alive is what stops the server from idling it out. |
| `QUIC_HANDSHAKE_TIMEOUT_MS` | 3 000 | `QuicApi.cpp` | Boot waits at most this long before the fallback (§6). |
| `QUIC_ALPN` | `"outpost-1"` | `QuicApi.cpp` | Bumps when the wire format's `KIND_*` bytes change meaning, so two builds that cannot talk refuse at the handshake rather than at the parser. |

Settings go in through `MsQuicSettings` (`msquic.hpp`, shipped with the package):
`DatagramReceiveEnabled = 1` on both configurations, the three timeouts above, and
`PeerBidiStreamCount = 1` reserved for §8 but unopened.

---

## 5. `QuicListener` and `QuicApi`

`QuicApi` opens the library once (`MsQuicOpen2`), owns one `HQUIC` registration named
`"Outpost"` with `QUIC_EXECUTION_PROFILE_LOW_LATENCY`, and builds two configurations: a client
one with `QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`, and a
server one loaded from a `DevCertificate` (§7). Its `Desc` is the port, the timeouts and an
`allowUnvalidatedPeer` bool that the development root sets and that a shipping root would not.
`QuicApi` is constructed by the composition root and outlives every transport and listener made
from it; it is not a singleton and there is no global.

`QuicListener::Start(QuicApi&, std::uint16_t _port)` binds `127.0.0.1:_port` — loopback only, by
design (§11: no NAT, no exposure). `QUIC_LISTENER_EVENT_NEW_CONNECTION` arrives on a worker; the
handler hands the `HQUIC` to a `QuicTransport` the listener pre-allocated for the purpose and
calls `ConnectionSetConfiguration`. `QuicListener::Poll()` on the owning thread moves accepted
transports into `Accepted()`, a span the root drains. One pre-allocated transport in this design;
`Desc::backlog` names the number so the day there are N clients it is a number and not a
rewrite.

The port is `constexpr std::uint16_t OUTPOST_QUIC_PORT = 30081` in `Outpost/OutpostApp.cpp`, at
the composition root and nowhere lower, for the same reason the loopback's latency knob is one
line there (`OutpostApp.cpp:108–111`): there is no configuration file, and a number the game
needs at boot is a constant the root owns. The value is arbitrary and unregistered; if it is
taken, boot falls back (§6) and the log says so.

---

## 6. What `Outpost` does — the boot, the fallback, the log

`OutpostApp` gains `Neuron::QuicApi m_quic; Neuron::QuicListener m_listener;
Neuron::QuicTransport m_serverQuic, m_clientQuic;` beside the two `LoopbackTransport`s, which
stay. `Init` becomes:

1. Construct `m_quic` from a `QuicApi::Desc`. On failure — MsQuic missing, credential
   unobtainable — log `LINK | QUIC UNAVAILABLE | <reason>` and go to step 5.
2. `m_listener.Start(m_quic, OUTPOST_QUIC_PORT)`; on failure (port taken) log and go to 5.
3. `m_clientQuic.Connect(m_quic, {"127.0.0.1", OUTPOST_QUIC_PORT})`. Pump `m_listener.Poll()`
   and `m_clientQuic.Poll()` until both ends are `Connected`, or `QUIC_HANDSHAKE_TIMEOUT_MS` has
   elapsed on `FrameClock`. Boot is the one place a wait is acceptable; it is bounded and logged.
4. `m_simulation.Connect(serverEnd); m_view.Init(clientEnd, ...)`. Log `LINK | QUIC |
   127.0.0.1:30081 | <handshake ms>`. Done.
5. Fallback: `LoopbackTransport::Connect(m_serverLink, m_clientLink, linkDesc)` exactly as today,
   and `LINK | LOOPBACK | <why>` in the event log the HUD already shows.

`Run` keeps `AdvanceTo` on the loopback pair (harmless when unused) and keeps
`m_view.PumpNetwork()` per tick and once per tickless frame; the QUIC ends need only `Poll`,
which `PumpNetwork` and `ApplyIncomingOrders` already call. Real latency replaces counted
latency: `INTERP_DELAY_TICKS = INTEREST_UPDATE_EVERY_TICKS` (100 ms at 60 Hz) is already the
client's cushion, and on localhost QUIC adds well under a tick.

`Shutdown` closes the client end, then the listener's end, then the listener, then `m_quic`, in
that order, because a registration cannot close while a connection on it lives.

The simulation, the format, the interest set and the renderer do not know which transport they
got. That is the acceptance criterion for the whole design, and §9 tests it by running the same
snapshot round-trip over both.

---

## 7. The credential

Schannel needs a server certificate with a private key it can reach. The tree has no file
format, no configuration file and no installer, so the certificate is made, not read:

`DevCertificate` creates an RSA-2048 key through NCrypt under the Microsoft Software Key Storage
Provider and a self-signed X.509 for `CN=Outpost Development` valid one year, via
`CertCreateSelfSignCertificate`, and hands the `PCCERT_CONTEXT` to MsQuic as
`QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT`. Schannel is known to refuse a key that exists only in
memory, so the key is persisted under a fixed name (`"Outpost.Dev.Quic"`) in the current user's
key store and reused on the next boot if present; the certificate itself is never written to a
store. Removing the key is `certutil -delkey -user Outpost.Dev.Quic`, and the header says so.

The client sets `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`. This is a placeholder and it is
declared as one, at the flag and in ADR 0020: the connection is encrypted and integrity-checked
by TLS 1.3, and it is not authenticated. Nothing about the shipped game's trust model is decided
here; what is decided is that development and CI need no certificate installed, no script run,
and no elevated prompt, because every one of those is a step a fresh clone would fail on.

---

## 8. What changes on the wire — nothing, yet

Every message today is a datagram, and QUIC DATAGRAM frames are datagrams: unreliable,
unordered, bounded. The mapping is one-to-one and the format is untouched, which is what makes
the migration a swap. Two things this does *not* fix, both already on record:

- **A dropped delta is not self-healing** (Collision-slice-6 §3.5). Since slice 6 the snapshot
  is incremental — upserts and leaves — and a lost leave is a ghost ship until the next full
  update. Over the loopback loss is zero unless asked for; over QUIC on localhost it is
  effectively zero; over a real path it is not.
- **A dropped move order is a dropped click.** `WorldView::IssueMoveOrder` drops on a false
  `Send` by contract.

QUIC has the answer built in — a stream is reliable and ordered — and taking it is a change to
the seam's *contract*, not its implementation: `Transport` would gain a reliable lane, the
loopback would implement it as "never dropped", and `GameLogic` would decide which messages use
it (leaves and destroyed lists, orders) and which stay datagrams (position updates, where a late
one is worse than a lost one). That is two layers — Core for the lane, GameLogic for the choice
— and the format is ADR 0008's territory, so it is two further slices with a GameLogic work
order of their own (§13, slices 3a and 3b), not part of the migration. The migration makes them
possible; it does not do them.

---

## 9. Tests

`Tests/NeuronCoreTests/QuicTransportTests.cpp`, class `QuicTransportTests`. Every test opens a
`QuicApi`, a listener on port 0 (ephemeral — the test asks the listener which port it got, which
is why `QuicListener` exposes `Port()`), and a client to it, and pumps `Poll` on both with a
bounded spin on `FrameClock` — never an unbounded wait, so a broken build fails in seconds and
not in CI's timeout.

| Test | Decides |
|---|---|
| `AConnectionReachesConnectedOnBothEnds` | The handshake completes on localhost within `QUIC_HANDSHAKE_TIMEOUT_MS`, and `State()` is `Connected` on both ends. |
| `TheMaxSendLengthCoversTheDatagram` | The reported `MaxSendLength` is `>= MAX_DATAGRAM_BYTES`; the number is logged so a change in MsQuic's MTU floor shows in the run. |
| `AFullSizedDatagramSurvivesAndALargerOneIsRefused` | The loopback's test, over QUIC: 1152 bytes round-trip byte-for-byte; 1153 is refused without a send. |
| `BothDirectionsAreIndependent` | The loopback's test, over QUIC. |
| `AFullRingDropsTheNewestAndCountsIt` | Fill the inbound ring without polling; the 257th is dropped, `DroppedCount()` is 1, the first 256 arrive intact. |
| `AClosedPeerDrainsThenCloses` | Client `Close()`; the server end passes through `Draining` to `Closed` and `Send` on it returns false. |
| `NothingIsDeliveredOutsidePoll` | Send, spin without polling, `Receive` returns 0; poll once, it does not. This is the threading rule as a test. |
| `AFragmentSizedBurstArrivesIntact` | Thirteen 1152-byte datagrams sent back to back — one snapshot's worth of fragments — all arrive, each byte-for-byte. Not a `SnapshotWriter` test: `GameLogicTests` does not import MsQuic and stays that way, so the format's tests stay on the loopback and this one stands in for them here. |
| `ARefusedListenerReportsWhy` | Two listeners on the same port; the second `Start` returns false with a reason string, not an assert — the fallback in §6 depends on this being a diagnostic. |

`LoopbackTransportTests`, `SnapshotTests` and `InterestTests` pass unchanged; they are the
format's tests and they stay on the loopback, because a format test with real latency in it is a
flaky test. The replay gate `TheSameOrderProducesTheSameRun` is untouched: the simulation never
sees the transport.

Acceptance for the `Outpost` slice is a boot log reading `LINK | QUIC | 127.0.0.1:30081 | N ms`
and the game playing as before at two window sizes, plus one boot with the port deliberately
occupied showing the `LOOPBACK` line and the game still playing.

---

## 10. The MMO ledger

What this design does for the persistent-world direction in Design/Collision.md §2, and what it
leaves owed:

- **Paid.** The server half can now be talked to from outside its process; `QuicListener` is
  what a dedicated server would start, and it lives in the headless library. TLS 1.3 comes with
  the connection — encryption was unaddressed in every design so far. Connection migration and
  path validation are QUIC's, not ours.
- **Owed.** One client: `WorldSimulation` holds one `Transport*`, one `InterestSet`, one
  subscriber faction. N clients is a `WorldSimulation` change — a table of `{transport, interest
  set, faction}` — and belongs to a design of its own; the listener's `backlog` is the only
  concession made here. A second process needs a headless composition root, which AGENTS.md §5
  forbids configuring by argv, so it needs a decision on how a server is told what to be. Both
  are named in §11 and neither is scheduled.

---

## 11. Deliberately left out

- **A second executable.** Collision.md §18 decision 8: the process split is taken on an
  operational trigger. After this design it is a composition root away.
- **More than one client** (§10).
- **A reliable lane and the format's use of it** (§8; slices 3a/3b, scheduled after the
  migration, not in it).
- **Certificate validation, a PKI, a trust model** (§7). The flag is the placeholder.
- **Listening on anything but `127.0.0.1`**, NAT traversal, a public port, a firewall prompt.
- **0-RTT, connection migration settings, ECN, MsQuic's XDP datapath.** Defaults, all of them.
- **App-driven execution** (`QUIC_EXECUTION_CONFIG`, MsQuic driving on our thread instead of
  its workers). It would remove the lock and keep the tree single-threaded, and it is the
  alternative ADR 0019 records as turned down: it is the newest surface in the API, Windows
  only, and the ring-and-lock shape is what AGENTS.md §5 already describes for the day an audio
  callback arrives too. Worth revisiting when the API is a release old.
- **A `NeuronNet` project.** Three files beside `Transport.h` are not a library.
- **Tick-counted latency on QUIC.** `AdvanceTo` stays on `LoopbackTransport`; a socket has real
  latency and no tick, as its header says. Reproducible latency tests stay on the loopback.

---

## 12. Decisions taken with the owner

Put to the owner on 2026-08-29, each with the alternative it beat:

1. **The development credential is self-signed at boot and the client does not validate** (§7).
   Over a certificate the owner provisions by hand with its thumbprint as a constant in the root:
   a fresh clone and a CI runner need no install step, and the shipped game's trust model is a
   later decision. Recorded as ADR 0020.
2. **`Outpost.exe` boots over QUIC on localhost, with the loopback as the fallback** (§6). Over
   loopback-by-default with QUIC behind a constant: a path nobody runs is a path nobody notices
   breaking.
3. **Port `30081`, ring capacity 256 datagrams** (§4.3, §5). Both are one constant each.
4. **The reliable lane (slices 3a/3b) waits until the migration has landed and been lived with**
   (§8). Slices 1 and 2 stay a pure swap; the work orders for 3a/3b are written when they are
   scheduled.

---

## 13. Slices

Four, in dependency order. Slices 1 and 3a are both `NeuronCore` and run serially; 2 can start the
moment 1 merges; 3b follows 3a because the lane must exist before the format chooses it.

| # | Slice | Layer | Depends on | Status | Work order |
|---|---|---|---|---|---|
| 1 | `QuicApi`, `QuicTransport`, `QuicListener`, `DevCertificate`, the tests, ADRs 0018–0020 | `NeuronCore` | — | landed | [slice 1](Archive/QuicTransport-slice-1.md) |
| 2 | The composition root: boot over QUIC, fallback, log, AGENTS.md text | `Outpost` | 1 | | [slice 2](QuicTransport-slice-2.md) |
| 3a | A reliable lane on `Transport`, on both implementations | `NeuronCore` | 1 | | to write, after 2 has landed (§12 decision 4) |
| 3b | Leaves, destroyed lists and orders go reliable | `GameLogic` | 3a | | to write, with 3a |

Three decision records are due, all in slice 1: *the network transport is MsQuic* (a dependency
record, overdue since `ac1eb00`), *MsQuic's workers enqueue to a ring and the owning thread
delivers* (the first synchronisation primitives in the tree, and app-driven execution turned
down), and *the development credential is self-signed at boot and the client does not validate*.
They are written as 0018–0020 here; if the numbers are taken while the slice is in flight, the
records take the next free ones and this document is not corrected — records are never
renumbered.

They were taken: 0018, 0019 and 0020 went to the planet renderer's last slices while this design was
being written, so the three records above landed as **0021**, **0022** and **0023**. The numbers in
the row and the paragraph above are the ones this document guessed at, and they stay as written.

---

## 14. Implementation plan

What each slice builds, in the order it is built, with the acceptance that decides it. The work
orders carry the exact signatures, the "what to build on" tables and the assumptions; this is the
plan they are cut from, and it is written to the decisions in §12.

### Slice 1 — the transport (`NeuronCore`, no game, no window)

| Step | File | What |
|---|---|---|
| 1.1 | `QuicApi.h/.cpp` | `Desc { idleTimeoutMs, keepAliveMs, handshakeTimeoutMs, allowUnvalidatedPeer }`, `Open`/`Close`/`Reason`/`IsOpen`. One `MsQuicOpen2`, one registration, two configurations on `QUIC_ALPN = "outpost-1"` with `DatagramReceiveEnabled` and `PeerBidiStreamCount = 1`. The header names no MsQuic type; the `.cpp` includes `<msquic.h>` and `<msquic.hpp>` after `pch.h`. |
| 1.2 | `DevCertificate.h/.cpp` | `Acquire`/`Release`/`Context`/`Reason`. Persisted NCrypt RSA-2048 key `Outpost.Dev.Quic`, `CertCreateSelfSignCertificate` for `CN=Outpost Development`, handed over as `QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT`. Fail-closed diagnostics, never a throw. |
| 1.3 | `QuicTransport.h/.cpp` | `Endpoint`, `Desc { capacityDatagrams = 256 }`, `Connect`, `Close`, the four overrides, `DroppedCount`, `MaxSendLength`. The state table of §4.1; the two rings and one lock of §4.2; the static callback that allocates nothing and calls no MsQuic API; the bounded destructor wait. |
| 1.4 | `QuicListener.h/.cpp` | `Desc { backlog = 1, transport }`, `Start(api, port)` on `127.0.0.1` only (port 0 = ephemeral), `Stop`, `Port`, `Poll`, `Accepted`, `Reason`. `NEW_CONNECTION` adopts a pre-allocated transport; an exhausted backlog refuses. |
| 1.5 | `NeuronCore.h`, `.vcxproj`, `.filters` | Three umbrella includes after `LoopbackTransport.h`; eight files registered; `QuicTransportTests.cpp` registered in `NeuronCoreTests` under `Tests`. |
| 1.6 | `Tests/NeuronCoreTests/QuicTransportTests.cpp` | The ten tests of the work order §4, each over a real localhost connection with a bounded `FrameClock` wait. |
| 1.7 | `Design/Decisions/0018…0020`, `AGENTS.md` §2, §5 | The three records and their index rows; "no code references it yet" and "what remains is a socket" rewritten in the same commit. |

**Accepted when:** the ten tests and every existing suite pass; `Build/CheckProjectFiles.py` and
`CheckFormat.py` pass; `msquic.dll` sits beside `NeuronCoreTests.dll` without a copy step of ours;
the `MaxSendLength` from the test log is stated in the pull request; a code read finds no
allocation after `Connect`/`Start` and no `std::mutex`/`std::atomic`/`std::condition_variable`
outside the four `Quic*` files; the game boots and plays exactly as before on the loopback.

### Slice 2 — the boot (`Outpost`)

| Step | File | What |
|---|---|---|
| 2.1 | `OutpostApp.h` | `m_quic`, `m_listener`, `m_clientQuic`, `m_serverQuic`, `m_linkIsQuic` beside the loopback pair, which stays; the seam comment at L76–79 gains its QUIC sentence. |
| 2.2 | `OutpostApp.cpp` | `OUTPOST_QUIC_PORT = 30081` beside the latency knob; `OpenQuicLink()` doing §6 steps 1–5 with the handshake wait bounded on `m_clock`; `Init` picks the two ends and falls back to the existing loopback block; the `LINK` lines through `PushFormat` — `Friendly` for QUIC, `Alert` for every fallback. |
| 2.3 | `OutpostApp.cpp` `Shutdown` | `m_clientQuic.Close(); m_listener.Stop(); m_quic.Close();` before `m_gpu.Shutdown()`, when `m_linkIsQuic`. `Run` unchanged. |
| 2.4 | `AGENTS.md` | "What is actually here", "Deliberately not here yet", §2 seam paragraph, §5 package sentence — each rewritten to the tree as it now stands. |

**Accepted when:** screenshots at two window sizes show `LINK | QUIC | 127.0.0.1:30081 | N MS` in
the event log and the game playing as before; one boot with the port held shows `LINK | PORT
30081 REFUSED | …` then `LINK | LOOPBACK` and the game still plays; the handshake time is in the
pull request; closing the window exits cleanly; `git diff --stat` shows only `Outpost/`,
`AGENTS.md`, `Design/`; all four suites unchanged and green.

### Slice 3a — the reliable lane (`NeuronCore`)

Written as a work order when scheduled (§12 decision 4). The steps as sketched in §8:

| Step | File | What |
|---|---|---|
| 3a.1 | `Transport.h` | `SendReliable`/`ReceiveReliable` as non-pure virtuals whose defaults refuse (`false` / `0`), so `CaptureTransport` in `GameLogicTests` compiles untouched; `MAX_RELIABLE_BYTES` for one framed message. |
| 3a.2 | `LoopbackTransport` | The lane as never dropped and never reordered; `dropOneInN` does not apply to it. |
| 3a.3 | `QuicTransport` | Open the one bidirectional stream `PeerBidiStreamCount` reserved, on `Connected`; frame each message with a 2-byte length; a third ring for the stream's receive side, filled from `STREAM_EVENT_RECEIVE` on the worker. |
| 3a.4 | tests | Loopback and QUIC rows for the lane: order preserved, nothing lost under `dropOneInN`, a message larger than `MAX_RELIABLE_BYTES` refused. |

**Accepted when:** the existing suites pass unchanged; the lane's tests pass on both
implementations; `ShipsPerSnapshotFragment()` is untouched.

### Slice 3b — the format chooses (`GameLogic`)

| Step | File | What |
|---|---|---|
| 3b.1 | `WorldSnapshot.h/.cpp` | `KIND_LEAVE = 3`: the leave and destroyed lists move out of the snapshot header into a message of their own on the reliable lane; the header shrinks; orders go `SendReliable`. |
| 3b.2 | `WorldSimulation.h`, `WorldView.cpp` | The adapter lines: `WriteInterest` sends the leave message reliably; `IssueMoveOrder` stops treating a false send as a dropped click. |
| 3b.3 | `QuicApi.cpp` | `QUIC_ALPN` bumps to `"outpost-2"`. |
| 3b.4 | tests | `SnapshotTests`/`InterestTests` rows: a leave survives a datagram drop; an order survives one; the replay gate unchanged. |

**Accepted when:** `ADeathAndADepartureDifferOnTheWire` and `AnUpdateUpsertsAndALeaveRemoves` pass
with the lists on the new message; a test drops every datagram (`dropOneInN = 1`) and the client
still sees every leave and every order; `TheSameOrderProducesTheSameRun` unchanged; ADR 0008
cited, not amended.

### What runs in parallel

Slice 2 can be implemented against slice 1's branch and rebased, since it touches only the
executable. Nothing else in this design runs alongside itself: 1 and 3a share `NeuronCore.vcxproj`,
`NeuronCore.h` and `Transport.h`. Against other designs, slice 1 conflicts with nothing currently
in flight — the planet renderer's slices are all `NeuronClient` and `Outpost` — and slice 2
shares `OutpostApp.cpp` with whichever `Outpost` slice is open that week; whoever lands second
rebases.
