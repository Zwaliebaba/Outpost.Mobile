# 0021 — The network transport is MsQuic, and the seam stays datagram-shaped

Status: accepted
Date: 2026-08-29

## Context

`NeuronCore/Transport.h` has declared the client/server seam since the collision work: four pure
virtuals, a 1152-byte datagram, and one implementation whose far end is in this process. The design
that put it there said what would come next — "a loopback implementation first, a network one after
it, and neither half changing" — and left the network one unwritten.

The dependency for it arrived before the record did. `Microsoft.Native.Quic.MsQuic.Schannel` 2.6.1
was restored in `ac1eb00` and its build targets imported by `NeuronCore`, `Outpost` and
`NeuronCoreTests`, with no code referencing it. AGENTS.md §9 requires a record whenever a
third-party dependency is added, and that commit owed one; this is it, written late and saying so.

What forced the choice now is that the seam is only load-bearing if something crosses it. A
loopback that nobody has ever replaced is a seam nobody has tested, and every assumption it hides —
that delivery is instant, that nothing is dropped, that both ends stand on the same tick — is an
assumption that becomes expensive on the day a second machine exists.

## Decision

The network transport is QUIC over MsQuic 2.6.1, and it enters the tree as one more implementation
of the existing `Transport`: `QuicTransport`, `QuicListener` and `QuicApi` in `NeuronCore`, beside
`LoopbackTransport` and under the same contract.

**The wire format does not change.** Every message the game sends today is already a datagram, and a
QUIC DATAGRAM frame is a datagram: unreliable, unordered, bounded. The mapping is one to one, so
`MAX_DATAGRAM_BYTES` stays 1152, `WorldSnapshot`'s fragmentation is untouched, and the loopback
keeps its tick-counted latency as the instrument it was built to be
(`Design/Archive/Collision-slice-2b.md` §2.1). The loopback is not deleted: it is the fallback when
QUIC cannot open, and it is the only place a reproducible latency measurement can be taken.

## Alternatives considered

- **A hand-written UDP transport on Winsock.** No dependency, no package, and about a hundred lines
  for a datagram socket. Rejected: it is a hundred lines until the first person asks for
  encryption, a handshake, path validation or connection migration, and then it is a protocol
  nobody reviewed. TLS 1.3 arrives with QUIC and encryption was unaddressed in every design so far.
- **TCP, or a WebSocket over it.** Rejected: head-of-line blocking is the wrong trade for position
  updates, where a late datagram is worse than a lost one. The reliable lane this tree will
  eventually want is a QUIC stream *beside* the datagrams, not instead of them.
- **A third-party game-networking library** (ENet, GameNetworkingSockets). Rejected under AGENTS.md
  §5: a new external dependency needs the owner's approval, and MsQuic is already restored,
  already imported, shipped by the platform vendor, and needs no build of its own.
- **Leaving the seam on the loopback until a second process exists.** Rejected, and it is the
  alternative most likely to be proposed again: it is exactly the reasoning that leaves a path
  nobody runs, and a path nobody runs is a path nobody notices breaking. `Outpost.exe` now boots
  across `127.0.0.1` so that every frame of every run crosses the real stack.

## Consequences

- **`NeuronCore` gains a third-party dependency and a directory that knows about it.** `msquic.h` is
  included by three `.cpp` files and named by no header: `QuicApi.h` forward-declares
  `QUIC_API_TABLE` and holds handles as `void*`, so the day the package version changes the blast
  radius is one directory.
- **`NeuronCore` stops being free of threads.** MsQuic delivers on its own workers, which is what
  ADR 0022 is about.
- **A QUIC server needs a certificate**, which is what ADR 0023 is about.
- **`msquic.dll` now has to be beside every executable that links it.** The package's targets
  already do this; nothing in this tree copies it, and nothing should start.
- **Two implementations of one contract now have to stay in step.** The `NeuronCoreTests` rows that
  decide the loopback's behaviour — a full-sized datagram survives, an oversized one is refused,
  the two directions are independent, a full queue drops the newest — are written a second time
  against a real connection, and a change to the contract has two places to make it.
- **What is still owed.** One client (`WorldSimulation` holds one `Transport*`), one process, no
  certificate validation, and no reliable lane. All four are named in `Design/QuicTransport.md` §11
  and none is scheduled by this record.
