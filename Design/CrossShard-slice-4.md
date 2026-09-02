# Work order — Cross-shard slice 4: the handoff on the wire

Implements slice 4 of [`CrossShard.md`](CrossShard.md) §8 — a message on the reliable lane, the ack,
and the re-send.

**Layer:** `GameLogic` (+ `NeuronCore` only if the transport needs it; §3 says it does not).
**Depends on:** slice 3, which landed 2026-09-02.
**Blocks:** slice 5.

---

## 1. What is already true, and what is missing

Slices 2 and 3 built the mechanism and made it durable. What has never existed is the thing that
moves a `Handoff` from one universe to another: every test so far has copied the outbox by hand.

- `Handoff` is a flat POD of values — no handle, no pointer — written field by field into the save
  since slice 3. **It is already a wire format**; this slice gives it a kind byte and a lane.
- `Universe::Outbox`, `DeliverHandoff`, `DrainInbox` and `AcknowledgeHandoffs` are the four calls a
  link needs, and the last one is called by nothing (slice 2 §3, slice 3 §8).
- `Transport` has a reliable lane with a refusal — `SendReliable` returns false when the queue is
  full — and `LoopbackTransport` can be given a shallow one on purpose. **That is the loss model**,
  and it is a real one rather than an injected fault.

## 2. The decision this slice has to take, and the design does not answer

**When may the departing shard forget an outbox entry?**

`CrossShard.md` §4 says "an acknowledgement clears the outbox entry" and stops there. Taken
literally — ack the moment the entry lands in the arriving shard's inbox — the scheme has a hole
that at-least-once delivery does not cover:

1. A delivers, B queues it in memory, B acks.
2. A forgets the entry. The ship is now only in B's inbox.
3. B dies before its next save. **The ship is gone from both shards and from both files.**

Idempotence does not help: there is nothing left to replay. This is not a transport problem and no
retry policy fixes it; it is an ack sent for state that was not yet durable.

**Take: a shard acknowledges only what it has saved.** The ack says *"this is in my file"*, not
*"this reached my process"*, and that is the only reading under which ADR 0065's recoverability
survives a crash on the receiving side. Concretely: the link is told the tick its owner last wrote a
save for, and acks entries that were queued at or before it.

The cost is honest and small: an entry lingers in the departing outbox for at most one save period
(`Server.cfg`'s `saveEveryTicks`), and is re-sent in the meantime — which is at-least-once doing
exactly what it is for, and lands on an apply that is already idempotent.

The alternatives, so the choice is not implicit:

- **Ack on deliver.** One line, and it is what "an acknowledgement clears the entry" sounds like.
  Rejected: it is the hole above, and it converts a crash on the arriving shard from "the fleet
  arrives late" into "the fleet is gone".
- **Save before acking, synchronously.** Correct and simple to state. Rejected because it puts a
  file write on the path of every crossing, at a cadence a crossing does not control.
- **Two-phase.** The thing ADR 0065 already refused, for reasons that have not changed.

**A decision record is due**, because a future reader will otherwise ask why an ack waits.

## 3. Scope

1. **Two kind bytes on the existing seam**: `KIND_HANDOFF = 11` and `KIND_HANDOFF_ACK = 12`. New
   numbers, never reused ones — the rule `KIND_SNAPSHOT`'s own comment states about the retired 2
   and 4. The ALPN does **not** move: a client neither sends nor understands these, and an unknown
   kind is already refused rather than misread.

2. **`WriteHandoffMessage` / `ReadHandoffMessage`, and the ack pair**, beside the other codecs in
   `UniverseSnapshot.cpp` and sharing `WriteHandoffQueue`'s field-by-field discipline. A message
   carries as many entries as fit `MAX_RELIABLE_BYTES` and no more; the rest wait for the next send,
   which is what a re-send loop is for anyway.

3. **`ShardLink` in `GameLogic`** — the loop, and the only new type. It owns no `Universe` and no
   `Transport`; it is handed both, exactly as `SnapshotPublisher` is. Per pump it:
   - re-sends unacked outbox entries, **at a cadence and not every tick** (`HANDOFF_RESEND_TICKS`);
     a per-tick re-send would put a fleet's worth of messages on the lane sixty times a second;
   - drains the reliable lane, delivering handoffs into the inbox and applying acks to the outbox;
   - acks what is **durable**, per §2 — never what has merely arrived.

4. **`Universe::DrainInbox` stays where it is** and is still called by whoever owns the tick
   boundary. The link queues; it does not step.

5. **Prose in the same commit**: `CrossShard.md` §4's acknowledgement bullet gains what §2 decided,
   and §8's slice 4 row records what landed.

6. **A decision record**: an acknowledgement means "durable", not "delivered".

## 4. Out of scope

- **A second process.** Slice 5. This is proved over `LoopbackTransport`, for the reason slice 2 was
  proved in one process: the tree's own pattern, and the only place a thing can be proved.
- **Which shard a client talks to.** §7, and slice 5's.
- **A refusal when the far shard is down.** §6 wants `FleetOrderResult` to refuse the jump at the
  order. That needs the link to have an opinion about reachability, which needs a real connection —
  slice 5. Until then a fleet crossing to an unreachable shard sits in the outbox, which is what §6
  says happens and is not yet what a player is told.
- **Bounding the outbox.** An entry leaves when acked. If nothing acks, it grows — and that is now
  the *point*, since it is what makes the ship recoverable.

## 5. How it must behave

1. A handoff crosses a `LoopbackTransport` and the fleet arrives whole, in its slot, without any
   test copying the outbox by hand.
2. **Under a lane too shallow to take the message**, the send is refused, the entry stays, and the
   next re-send delivers it. Nothing is lost and nothing is duplicated.
3. **A duplicate delivery changes nothing** — already true at the apply, and now reached through the
   wire that produces duplicates.
4. **An ack for state that is not durable is never sent**, and an entry acked is gone from the
   outbox and from the next save.
5. Two shards pumped in either order produce the same two universes. The link is not allowed to
   reintroduce the ordering dependence the inbox drain removed.

## 6. Acceptance

- `HandoffTests`, extended: a crossing over a loopback pair; the same under a lane so shallow the
  first send is refused; a delivery duplicated on the wire; an ack withheld until a save is noted,
  and the entry surviving until then.
- `GameLogicTests` otherwise unchanged and green.
- `CheckProjectFiles.py`, `CheckFormat.py`, `CheckViewAccess.py`, clang-tidy over `GameLogic`.
- The decision record written and indexed.

## 7. Assumptions the implementer may make

- **A `Handoff` needs no versioning of its own.** It rides `UNIVERSE_STATE_FORMAT`'s discipline in
  the save and the ALPN's on the wire, and both ends of a shard link are the same binary until
  somebody says otherwise — which is a thing to state, not to design for now.
- **The reliable lane preserves order and does not duplicate.** QUIC's does; the loopback's does.
  The apply is idempotent anyway, which is what makes that assumption cheap to be wrong about.

---

## 8. What changed on contact

- **`NeuronCore` was not touched at all**, which §3 predicted and is worth recording: the reliable
  lane, its refusal, and a loopback with a configurable depth were all already there. The layer line
  at the top of this order says `NeuronCore` "only if the transport needs it", and it did not.
- **One field codec serves the save and the wire.** `WriteHandoffQueue` was split into
  `WriteHandoffFields` plus a count, and the wire message calls the same function. That the two
  spellings of a `Handoff` are the same bytes is now structural rather than a coincidence somebody
  has to maintain — which is what let slice 3 claim the struct was already a wire format.
- **`ShardLink` receives, then acks, then re-sends, in that order.** A message received this pump can
  be acked in the same one if the save already covers it, and re-sending last means a send never
  races an ack that had just made it unnecessary.
- **An entry owes at most one ack.** A re-send of something already queued must not add a second
  entry to the owed list, or that list grows for exactly as long as the far side keeps re-sending —
  which is exactly as long as it is not acked.

## 9. What was verified, and how — and the honest gap

Twelve rows **run**, not parsed — the eight from slices 2 and 3 plus four new ones, all executed
against the real `GameLogic` and the real `LoopbackTransport`:

```
AFleetCrossesAWireWithNobodyCopyingTheOutbox             pass
ALaneTooShallowToTakeTheMessageLosesNothing              pass
NothingIsAcknowledgedUntilTheArrivingShardHasSavedIt     pass
AHandoffMessageRoundTripsThroughItsOwnCodec              pass
```

The loss row uses a real failure rather than an injected one: a reliable lane with
`capacityReliableMessages = 0` refuses every send, the link reports `laneRefused`, the outbox keeps
every entry, and the census holds with the ships counted **in the queue** — which is where a ship is
while a send is being refused. Give it a lane with room and the same entries cross, once.

The ADR 0066 row is the one worth reading: with no save noted, four hundred pumps produce **zero**
acknowledgements and the departing outbox never shrinks, while the far side has the ships. Note a
save and the same pump clears it.

**One row failed first, and again the test was wrong rather than the code** — the same mistake as
slice 2's, which is itself worth recording. `ALaneTooShallowToTakeTheMessageLosesNothing` asserted
that the player's ship count was unchanged while three ships were sitting in the outbox. The first
row of this suite already counts the queue for exactly that reason; the new row did not. Twice now,
the invariant that reads naturally ("no ships were lost") is not the one that is true mid-handoff
("no ships were lost, and the queue is where they are").

**The gaps.** No second process — slice 5, and it is the only thing left. And `CrossShard.md` §6's
refusal at the *order* when a shard is unreachable is still not built: a link has no opinion about
reachability until there is a real connection to lose, so a fleet crossing to a shard that is not
answering sits in the outbox rather than being refused with the fleet where it was. That is what §6
says happens; it is not yet what a player is told, and slice 5 is where it becomes possible to tell
them.
