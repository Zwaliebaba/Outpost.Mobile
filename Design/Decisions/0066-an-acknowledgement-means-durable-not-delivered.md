# 0066 — An acknowledgement means "durable", not "delivered"

Status: accepted
Date: 2026-09-02

## Context

[ADR 0065](0065-a-handoff-is-at-least-once-delivery-onto-an-idempotent-apply.md) chose at-least-once
delivery onto an idempotent apply, and `CrossShard.md` §4 states the loop in one line:

> **An acknowledgement clears the outbox entry.** Until then it is re-sent.

It does not say what an acknowledgement asserts, and the obvious reading — the arriving shard acks
when the entry lands in its inbox — has a hole that neither at-least-once nor idempotence covers:

1. A delivers a handoff; B queues it in memory; B acks.
2. A clears the entry. The ship now exists only in B's inbox.
3. B crashes before its next save. **The ship is in no universe and in no file.**

Idempotence is no help, because idempotence is about applying something twice and there is nothing
left to apply. A retry policy is no help either: A has nothing to retry. This is not a transport
failure — the message arrived — it is an acknowledgement sent for state that was not yet durable.

Slice 3 is what makes the question answerable at all. Before it, neither queue was in the save and
"durable" named nothing.

## Decision

**An acknowledgement asserts that the entry is in the acknowledging shard's save file. Nothing is
acked until it is.**

- `ShardLink` records, per delivered handoff, the tick this end was on when it arrived.
- The composition root — which is the only thing that knows when a save completed — calls
  `NoteDurableThrough(tick)`.
- The link acks an entry only once its arrival tick is covered by that. Everything else stays owed,
  is not acked, and is therefore re-sent by the far side.

The cost is bounded and stated: an entry lingers in the departing outbox for at most one save period
(`Server.cfg`'s `saveEveryTicks`) and is re-sent at `HANDOFF_RESEND_TICKS` in the meantime. Every
re-send lands on an apply that is already idempotent, so the cost is a message and never a ship.

## Alternatives considered

- **Ack on arrival.** One line, and it is what §4's sentence sounds like. Rejected: it is the hole
  above. It converts a crash on the arriving shard from "the fleet arrives late" into "the fleet is
  gone", which is precisely the outcome ADR 0065 was chosen to make impossible.
- **Ack on drain**, once the ships are spawned. Feels safer and is not: a spawned ship is in memory
  exactly as a queued handoff is, and the same crash loses it. It moves the window without closing
  it.
- **Save synchronously before acking.** Correct, and simple to state. Rejected because it puts a file
  write on the path of every crossing, at a cadence the crossing does not control — a fleet jumping
  during a busy tick would force a save the deployment did not ask for.
- **Two-phase commit.** ADR 0065 refused it for reasons that have not changed: it makes a crossing
  block on a round trip inside a tick, and needs a coordinator this tree does not have.
- **Say nothing and let the window stand.** The honest version of doing nothing, and it was the
  status quo until this record. Rejected because the window is not small in the way it sounds: a save
  period is seconds, a crash is not rare over a long-running shard, and a design that loses a fleet
  occasionally is a design that loses fleets.

## Consequences

- **The link needs to be told about saving, and that is a new coupling.** `ShardLink` cannot ask a
  `Universe` when it was last written — a universe does not know it is ever saved, and this record
  does not change that. The composition root tells it. That is one call in one place, and it is the
  same shape as the root already having to own `saveEveryTicks`.
- **A shard that never saves never acks**, and its far side's outbox grows for ever. That is correct
  rather than a leak: nothing has been made safe, so nothing may be forgotten. A deployment with
  saving switched off is a deployment that cannot recover, and the growing outbox is the symptom
  saying so.
- **The re-send is now load-bearing, not a safety net.** It is the normal path for the first save
  period of every crossing, which means it is exercised constantly rather than only after a fault —
  the best thing that can happen to a recovery path.
- The window is not closed, it is moved to a place where it costs nothing: between the arriving
  shard's save and the departing shard's receipt of the ack, both ends hold the entry. Duplication is
  what the apply already handles.
