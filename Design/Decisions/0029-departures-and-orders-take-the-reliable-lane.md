# 0029 — Departures and orders take the reliable lane; positions stay datagrams

Status: accepted
Date: 2026-08-30

## Context

Since the interest set landed, a snapshot has been incremental: upserts for what the subscriber can
see, and a list of handles for what it may no longer. The upsert half heals itself — the next update
carries the same ship again — and the leave half does not. A leave is stated once and never
repeated, so a lost one is a ship that stays on a client's screen for the rest of the match, moving
nowhere, alive to a player and dead to the server.

`Design/Archive/QuicTransport.md` §8 recorded the defect when the migration landed and left it for a later
slice. `Design/MmoScalabilityReview.md` finding E1 priced it: an update is dropped whole if any
fragment is missing, so completeness is (1 − p)^F, and at 2% loss a 13-fragment fleet-battle update
completes 77% of the time. On `127.0.0.1` none of this bites. On a real path all of it does, and the
seam exists precisely so that the day the two halves are on different machines is not the day the
defects are discovered.

A move order has the same shape and a worse symptom: it is stated once, and losing it is a click the
player made and the game ignored.

## Decision

The wire has two lanes and the format chooses between them by asking one question: *if this message
is lost, does a later one make it right?*

- **Datagrams** carry ship records. A position is superseded by the next update, so a late one is
  worse than a lost one, and reliability would buy head-of-line blocking for something that heals
  itself in 100 ms.
- **The reliable lane** carries `KIND_LEAVE` — the leave and destroyed lists, together, as one
  message — and move orders. Nothing supersedes either.

The leave lists move out of the snapshot header rather than being duplicated onto both lanes. The
snapshot header loses `leaveCount` and `destroyedCount` and shrinks from 34 bytes to 26.

Departures are applied the moment they arrive, not held for a snapshot to complete: that is the
point of the split. Because the two lanes have no ordering between them, the receiver is written so
that either order is safe — removing a handle it does not hold is a no-op — and the writer sends the
departure message first, since the dangerous interleaving is an upsert overtaking its own leave.

## Alternatives considered

- **Duplicate the lists onto both lanes**, so an unreliable copy arrives sooner and a reliable one
  guarantees it. Rejected: two paths carrying the same fact is two paths to reason about, the
  unreliable copy is the one that arrives first and therefore the one whose behavior is observed,
  and the receiver would need to dedupe by handle for no benefit a 100 ms lane cannot already give.
- **Acknowledge and retransmit leaves over the datagram lane**, leaving the wire single-laned.
  Rejected: it is a reimplementation of what QUIC's stream already is, in `GameLogic`, where the
  determinism rules make timers and retransmission timers especially unwelcome. The lane was
  reserved at the handshake for exactly this (`QuicApi.cpp`, `QUIC_PEER_BIDI_STREAMS`).
- **Send periodic full snapshots** so a ghost heals within a refresh period. This was slice 1 of
  `Design/MmoScalabilityPlan.md` and is dropped by this record: it blunts the symptom at the cost of
  sending the whole visible world on a timer, and once the lane exists it is a knob to explain and
  then remove. It remains the right answer for a client that has fallen too far behind to resync,
  which is a different problem.
- **Put everything on the reliable lane** and delete the datagram path. Rejected on head-of-line
  blocking: one lost position update would stall every later one behind it, which is precisely the
  failure the datagram lane exists to avoid, and it would make a 10 Hz stream of superseded state
  the thing that must never be dropped.

## Consequences

- A lost datagram no longer costs a subscriber anything permanent. The client's view of *who exists*
  is now exactly as reliable as the connection, and only *where they are* degrades with loss — which
  is the tradeoff every shipped netcode makes and the one this design has been describing since §8.
- `ShipsPerSnapshotFragment()` is unchanged at 13, which was not the expectation: the header shrank
  by 8 bytes and `(1152 − 26) / 82` still floors to 13. What the slice actually buys is that every
  fragment now carries 13, where the first used to carry fewer whenever there were departures.
- `SnapshotReceiver::Destroyed()` accumulates across a drain and is cleared by the consumer
  (`ClearDestroyed`), because several departure messages can arrive in one pump and each death owes
  the client an explosion. Before this, one update meant one list and assignment was enough.
- `SnapshotWriter::RefusedLeaveCount()` exists because nothing repeats a refused departure. A full
  lane or one that is not yet up is the only way a leave can still be lost, and the count is what
  makes that visible rather than silent. It should be zero.
- The ALPN bumps to `outpost-2`: two builds that disagree about the `KIND_*` bytes now refuse at the
  handshake rather than misparse.
- The server drains orders from both lanes. A client whose stream is not up yet still has its clicks
  read, which matters because the lane needs one round trip more than the connection does.
