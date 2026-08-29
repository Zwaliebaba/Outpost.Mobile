# 0022 — MsQuic's workers enqueue to a ring, and the owning thread delivers

Status: accepted
Date: 2026-08-29

## Context

Until this slice the tree had no threads of its own and no synchronisation primitive anywhere in it.
Everything ran on one thread, and AGENTS.md §5's single-writer rule was easy to keep because there
was nothing to keep it from: "the authoritative world belongs to whichever thread ticks it, render
state to the main thread. Today both are the same thread, which is why this rule is easy to break
without noticing: when a transport's workers or an audio callback arrive, they enqueue to a ring and
touch nothing else."

MsQuic is that transport's workers arriving. It runs its own thread pool and calls back into the
application from it — a datagram received, a send completed, a handshake finished, a connection
gone — and it does so whether or not the game is between frames. `Transport.h` already stated the
rule the other way round: "delivery happens here, on the owning thread. Foreign threads may only
enqueue" (`Transport.h:46`). Something had to make that true rather than aspirational.

## Decision

**MsQuic's workers only ever enqueue, and delivery happens in `Poll` on the owning thread.**

Inbound, the `DATAGRAM_RECEIVED` handler takes one lock, copies into the next slot of a
fixed-stride ring — the same arena `LoopbackTransport::Connect` allocates, taken once and never
again — advances a write cursor, and releases. A full ring drops the newest and counts it.
`Poll()` takes the same lock only to copy the write cursor into a ready cursor, and `Receive` pops
below that cursor with no lock at all, because everything below it belongs to the reader and
nothing above it is touched by anyone but the writer. The lock is held for one `memcpy` of at most
1152 bytes, or for one cursor copy.

Outbound there is no lock. MsQuic keeps a send buffer until the send reaches a final state, so
`Send` copies into a second ring, hands MsQuic the slot, and the completion callback clears a
per-slot `std::atomic<bool>`. A ring with no free slot makes `Send` return false, which is the
contract's "the queue is full" and which every caller already handles by dropping.

Every other connection event sets an atomic and returns. No callback allocates, and none logs.

**These are the first `std::mutex`, `std::atomic` and `std::condition_variable` in the tree, and
they are confined to `QuicTransport.h/.cpp` and `QuicListener.h/.cpp`.** That confinement is the
rule, not an accident of this slice: a synchronisation primitive appearing in a fifth file is a
change that needs a record of its own.

## Alternatives considered

- **App-driven execution** (`QUIC_EXECUTION_CONFIG`, `ExecutionCreate`/`ExecutionPoll`), which lets
  MsQuic run on our thread instead of its workers. It would remove the lock outright and keep the
  tree single-threaded, which is genuinely attractive, and it is the alternative most likely to be
  proposed again. Rejected for now on three counts: it is the newest surface in the API, added in
  2.5 and still behind `QUIC_API_ENABLE_PREVIEW_FEATURES`; it is not available on every platform
  MsQuic supports, which matters the day the headless server is not Windows; and the ring-and-lock
  shape is what AGENTS.md §5 already describes for the day an audio callback arrives too, so
  building it once serves both. Worth revisiting when the API is a release old.
- **A lock-free single-producer/single-consumer ring**, with no mutex at all. Rejected as unearned:
  MsQuic serialises callbacks per connection today, but that is a guarantee of its implementation
  rather than of its contract, and a lock held for one `memcpy` at 10 Hz costs nothing measurable.
  The atomics on the cursors are what make the unlocked `Receive` path well-defined; the mutex is
  what makes the writer side correct without depending on how many workers MsQuic chooses to use.
- **Delivering straight from the callback into `WorldView`**, with a lock around the world.
  Rejected outright: it would put a foreign thread inside the simulation, which is the single thing
  AGENTS.md §5 exists to prevent, and it would make a frame's cost depend on when a packet arrived.
- **A queue that grows**, so nothing is ever dropped. Rejected: `LoopbackTransport` drops the newest
  on a full ring and the callers are written to that contract, so a QUIC transport that blocked or
  allocated instead would be a different contract wearing the same interface.

## Consequences

- **`Receive` is correct only because `Poll` ran.** A caller that skips `Poll` sees nothing, which is
  exactly what `NothingIsDeliveredOutsidePoll` pins down, and is the behaviour `WorldView` and
  `WorldSimulation` already rely on.
- **Nothing allocates after `Connect` or `Start`.** Both rings, the send descriptors, the listener's
  backlog and both of its accept vectors are taken on the owning thread up front, because a worker
  that allocated would be a worker doing more than enqueueing.
- **A dropped inbound datagram is now possible without the sender knowing**, where the loopback
  refused the send instead. `DroppedCount()` is what makes it visible; a snapshot missing a fragment
  is already dropped whole by the format.
- **There is one blocking wait in the transport**, in `Close`, bounded by the idle timeout, waiting
  for `SHUTDOWN_COMPLETE` before the connection handle is closed — because MsQuic may still be
  inside a callback on an object that is being torn down. If it never comes, the leak of one handle
  is preferred to a deadlock at shutdown.
- **Thread-safety is now something reviews have to think about in this tree**, in four files. The
  cost is real and it is the reason the confinement above is written down rather than assumed.
