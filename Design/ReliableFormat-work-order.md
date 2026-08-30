# Work order — the format chooses the lane

Slice 3b of [`QuicTransport.md`](QuicTransport.md) §14, and slice 6 of
[`MmoScalabilityPlan.md`](MmoScalabilityPlan.md). Depends on
[slice 3a](ReliableLane-work-order.md): the lane has to exist before the format can choose it.

One slice, `GameLogic` (plus the two adapter lines and the ALPN bump). This is the slice that
actually retires review finding E1 — 3a only made it possible.

## Scope

1. **`WorldSnapshot.h/.cpp`** — `KIND_LEAVE = 3` beside `KIND_SNAPSHOT = 1` and
   `KIND_MOVE_ORDER = 2`. The leave and destroyed lists move out of the snapshot header into a
   message of their own, sent on the reliable lane; the snapshot header loses `leaveCount` and
   `destroyedCount` and the first fragment stops reserving room for them, so
   `ShipsPerSnapshotFragment()` rises and the fragment arithmetic in `WriteInterest` simplifies to
   what `Write` already does.

   The reason the lists move rather than being duplicated: a leave repeated on an unreliable lane is
   still a leave that can be lost, and two paths carrying the same fact is two paths to reason
   about. The upserts stay datagrams, deliberately — a late position is worse than a lost one,
   which is the whole argument for the split (§8).

2. **`SnapshotReceiver`** — accepts `KIND_LEAVE` from the reliable lane and applies it with the
   same upsert/remove semantics it has now. Ordering between the two lanes is not guaranteed and
   must not be assumed: a leave that arrives before the snapshot naming the ship is applied to a
   handle the receiver does not hold, which is already a no-op, and a leave arriving after removes
   it. State the ordering assumption at the definition rather than leaving it to be discovered.

3. **`WorldSimulation.h`** — `WriteInterest` sends the leave message through `SendReliable`.
   **`WorldView.cpp`** — `IssueMoveOrder` sends orders through `SendReliable` and stops treating a
   false send as a dropped click; a false return is now a full queue, which is backpressure, and the
   comment that called it a dropped click goes with it.

4. **`QuicApi.cpp`** — `QUIC_ALPN` bumps to `"outpost-2"`. The format changed; the negotiation says
   so. Read the comment at that constant first: it exists to make a version mismatch a failed
   handshake rather than a misparse.

5. **Tests** — `SnapshotTests` and `InterestTests` rows:
   - a leave survives `dropOneInN = 1`, every datagram dropped;
   - an order survives the same;
   - `ADeathAndADepartureDifferOnTheWire` and `AnUpdateUpsertsAndALeaveRemoves` pass with the lists
     on the new message;
   - a leave arriving before the snapshot that names its ship is harmless.

## Out of scope

- **Acks, retransmission, or a sequence number on the datagram lane.** The lane is reliable because
  QUIC's stream is; nothing in `GameLogic` implements reliability itself.
- **Full-snapshot resynchronisation.** Slice 1 of the MMO plan (the periodic full refresh) is a
  different answer to the same finding and is not needed once this lands; if it landed first, this
  slice removes its knob.
- **Delta compression, quantization, or any other change to what a record contains.** That is slice
  15 of the MMO plan. This slice moves two lists between lanes and changes no field.
- **ADR 0008.** It is cited, not amended: the wire format still lives in `GameLogic`, still writes
  field by field, still explicit little-endian. Adding a message kind is what that record's shape
  was for.

## What to build on

`ByteWriter`/`ByteReader` and the existing `KIND_*` dispatch in `SnapshotReceiver::Accept`. The
leave and destroyed spans `WriteInterest` already takes — the call signature does not need to
change, only where the two lists are written. `WorldSimulation::SplitTheLost`, which already
separates the two lists and now hands them to a different writer.

Slice 3a's `SendReliable`/`ReceiveReliable` and `MAX_RELIABLE_BYTES`. A leave message that would
exceed it is fragmented on the same principle the snapshot is, or refused and retried on the next
update — decide it in the slice and say which, because a subscriber losing 400 ships at once is a
real update at MMO scale even though it is not one today.

## Acceptance

- The five test rows above, green.
- **The one that decides the slice:** a test sets `dropOneInN = 1` on a `LoopbackTransport` pair, so
  no datagram whatsoever arrives, and the client still sees every leave, every destroyed handle and
  every order. That is finding E1 retired, stated as a test rather than as an argument.
- `TheSameOrderProducesTheSameRun` and the permutation test unchanged and green: none of this is
  simulated, all of it is sent.
- `ShipsPerSnapshotFragment()` is expected to *change* in this slice — it rises, because the first
  fragment no longer reserves room for two lists. The pull request states the old and new values.
- Every suite green; Debug|x64 builds; both `Build/` checks pass.
- `AGENTS.md`'s "no reliable lane on the wire — every message is a datagram and a lost one stays
  lost" sentence is false the moment this lands, and changes in the same commit.
