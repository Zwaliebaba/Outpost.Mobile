# 0031 — Listener slots are recycled, so backlog means concurrency

Status: accepted
Date: 2026-08-30

## Context

`QuicListener` pre-allocates `backlog` transports in `Start`, on the owning thread, because an accept
arrives on an MsQuic worker and a worker must allocate nothing (ADR 0022). It then handed each new
connection the next one, tracked by a counter that only ever rose.

`Design/MmoScalabilityReview.md` finding E3 is what that counter means: `backlog` was a budget for
the life of the process, not a limit on how many connections could be carried at once. A listener
with 1,000 slots that had seen 1,000 clients connect and disconnect refused everybody afterwards,
until restart. There was no diagnostic — a refusal is a refusal, whether the server is busy or
finished — so the symptom is a server that stops accepting logins for no visible reason, some hours
in, having worked perfectly all day.

The class's own header claimed the opposite: "the day there are N clients it is a number that
changes and not a class that gets rewritten". It was not true as written, and this record is what
makes it true.

## Decision

The listener keeps a free list of slots rather than a high-water mark. `OnNewConnection` takes a
slot from it and refuses when it is empty; `Poll`, on the owning thread, finds the accepted
transports whose connections have reached `Closed`, closes and re-`Reserve`s them, drops them from
`Accepted()`, and returns their slots to the free list.

Recycling happens in `Poll` and not in the callback that saw the shutdown, because a connection that
has reached `Closed` has already had its `SHUTDOWN_COMPLETE`: no worker is still inside a callback on
it, so the owning thread can take it apart. That is ADR 0022's rule holding without an exception.

`QuicTransport::Reserve` now also resets the connection state to `Disconnected`. It was already
right on the first call; on the second it is what stops a pooled transport reporting `Closed` while
being ready to serve.

## Alternatives considered

- **Grow the pool when it is exhausted.** Rejected: growing means allocating, and the only thread
  that knows the pool is exhausted is an MsQuic worker. Allocating there is exactly what the
  pre-allocation exists to avoid, and moving the growth to `Poll` would mean refusing the connection
  that triggered it anyway.
- **Let the caller return a slot explicitly** — an `Release(QuicTransport*)` the session layer
  calls. Rejected: it makes correct behavior depend on every host remembering to call it, and a
  missed call is the same silent attrition with a different cause. The listener already polls; the
  state it needs is already on the transport.
- **Recycle on `SHUTDOWN_COMPLETE`, inside the callback.** Rejected on ADR 0022: it would put a
  `Reserve` — which allocates the in-flight flags — on an MsQuic worker, and it would race a
  concurrent `Accepted()` walk on the owning thread.
- **Give `Accepted()` stable indices** so a caller can hold a position across polls. Rejected: it
  would mean never compacting, which means the span grows forever with holes in it, and it makes the
  natural loop over `Accepted()` wrong. The compacting span with a documented rule — a pointer is
  valid until the Poll that finds its connection closed — is the smaller surprise, and a session
  layer holds its own handle anyway (ADR 0030).

## Consequences

- `backlog` now means what its name suggests. A dedicated server sizes it for concurrent players
  rather than for total logins, and `RecycledCount()` says how many clients have come and gone.
- Memory is unchanged and now worth stating: every slot holds its rings for the life of the
  listener, so a listener costs `backlog` × about 800 KB before anybody connects. Sizing is the
  caller's through `Desc::transport`, which is why the review's 562 MB figure for a 1,000-slot
  server is a default to change rather than a fact to live with.
- `Accepted()` returns live connections only, and a pointer taken from it stops being listed after
  the Poll that finds it closed. The transport itself stays alive — the pool owns it — so a stale
  pointer is not a dangling one, but a caller that keeps one is looking at a transport that now
  belongs to somebody else. Nothing in the tree does this today; the session table (ADR 0030) holds
  its own handle.
- **A departure is no longer observable as a state on the accepted end**, and that cost was found by
  a test rather than foreseen here: `AClosedPeerDrainsThenCloses` waited for `Closed` on the pointer
  the listener owns, and the recycle takes that pointer back and resets it to `Disconnected` in the
  same `Poll`. Reading the state there is a race against the pool. The report is now the transport
  leaving `Accepted()` and `RecycledCount()` rising, which is what a session layer wants anyway —
  it needs to know *that* a subscriber went, not which of five states it passed through. The test
  was rewritten to assert that, and keeps the state assertion on the client half, which no pool
  owns.
- Recycling allocates once per departed client, on the owning thread: `Reserve` reuses every vector
  it already sized except the in-flight flags, which are atomics and cannot be assigned. That is not
  on any per-datagram path, and making it zero would mean a `Reset` that duplicates `Reserve`'s
  bookkeeping — worth doing the day a profile asks for it, not before.
