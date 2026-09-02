# 0065 — A handoff is at-least-once delivery onto an idempotent apply

Status: accepted
Date: 2026-09-02

## Context

`CrossShard.md` §4 named the hard part and did not hide it: **a handoff must not lose a ship and must
not duplicate one, and both shards must stay deterministic.** The three pull against each other.

- Fire-and-forget loses: shard A despawns, the message is dropped, the fleet is gone.
- A naive retry duplicates: A re-sends, B spawns twice, one entity exists in two places.
- Determinism is the constraint neither of those usually carries. A shard's replay is only worth
  having if a handoff arrives on a *stated* tick, which rules out applying one wherever it lands.

This tree has a strong reason not to reach for the usual answer. There is no distributed transaction
anywhere in it and no coordinator to run one; the simulation's whole contract is that a tick is a
pure function of the state and the orders it was given (ADR 0045, and the replay gates).

## Decision

**At-least-once delivery onto an idempotent apply, which is exactly-once in effect and needs no
agreement between the two shards.**

- **An outbox on the departing shard**, written in the same tick as the despawn. The ship is there
  and in neither universe for the duration — the honest statement of what a player sees, and the
  property that makes "lost" recoverable rather than final.
- **An inbox on the arriving shard, drained at a tick boundary in entity order.** Not inside `Step`,
  for the reason an order is not; entity order and not arrival order, so two shards that delivered
  one batch in two orders produce one universe.
- **`SpawnShipAs` refuses an entity the universe already holds**, so a replayed handoff is a no-op.
  That refusal was written a design ago for a reload and commented for this; slice 2 is the first
  caller that depends on it, and the row that proves it.
- **The handoff names the far GATE, not an arrival position.** The departing shard cannot compute
  one — the pose is read off the far gate's heading and position, and that gate is in another
  universe. ADR 0056's "every intent is re-derived on the far side", applied to the one thing the
  local jump path could take for granted.
- **An acknowledgement clears an outbox entry by sequence**; until then it is re-sent. Nothing
  acknowledges anything yet, because there is no transport that can lose a message (slice 4).

## Alternatives considered

- **A two-phase commit between the shards.** The textbook answer, and it is genuinely correct.
  Rejected because it makes a crossing block on a round trip *inside* a tick: shard A cannot finish
  its tick until B has voted, so one slow shard stalls every shard it borders, and the tick stops
  being a pure function of state and orders. It also needs a coordinator this tree does not have and
  would have to invent a failure model for.
- **A lease: B owns the entity for a window, A may not respawn it.** Weaker than 2PC and still
  requires the two shards to agree about *time*, which is the one thing two independent simulations
  running their own tick loops have no way to do. It would put a wall clock inside the thing whose
  first determinism rule is that there isn't one.
- **Exactly-once delivery on the transport.** Tempting, since the reliable lane is already there.
  Rejected because it is not a property a transport can actually have across a process restart: the
  sender can die between the send and the durable record of it, and then "exactly once" is a claim
  about a message rather than about a ship. Idempotence at the apply is what survives that, and it
  is cheap here because identity is already globally unique (ADR 0047).
- **A batch apply rather than a per-entry one.** Simpler, and it is what one process needs. Rejected
  because a wire can deliver half a batch and an apply that only worked whole would have to be
  rewritten exactly when it is hardest to — so each entry carries its own member index and crossing
  count, and can be applied alone.
- **Dropping a handoff whose gate this shard does not hold.** The easy answer to a message that
  cannot be applied. Rejected outright: it deletes a fleet to tidy up a queue. It stays queued and
  arrives when the gate does.

## Consequences

- **A fleet mid-handoff is in neither universe**, for one tick under any healthy condition and
  unbounded while the far shard is unreachable. `CrossShard.md` §6 answers that at the *order* — a
  gate to a shard that is not answering refuses the jump, with the fleet where it was — rather than
  letting the outbox become where fleets go to wait for ever.
- **The outbox is in memory this slice, so a shard that dies loses it.** That is the hole slice 3
  fills by putting both queues in the state codec, and it is named rather than hidden: until then
  the recoverability this record claims is a property of the design and not yet of the build.
- The determinism claim is measured, not argued: the same batch delivered forwards and reversed
  produces byte-identical state six hundred ticks later.
- **`Universe` gains no clock, no randomness and no pointer key.** Both queues are vectors of a POD
  ordered by entity, and everything about the apply is a pure function of what was handed in — which
  is what lets a handoff sit inside the replay contract rather than beside it.
- The `Handoff` struct is written now and put on a wire in slice 4, so the transport carries a shape
  that already exists rather than designing one. That is the tree's own pattern: QUIC ran across
  `127.0.0.1` before there was a second machine (ADR 0021, 0028).
