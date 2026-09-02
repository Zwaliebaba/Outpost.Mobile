# Work order — Cross-shard slice 2: the outbox, the inbox, and two universes handed off between

Implements slice 2 of [`CrossShard.md`](CrossShard.md) §8 — the mechanism §4 names, proved between
two `Universe`s in one process.

**Layer:** `GameLogic`.
**Depends on:** slice 1, which landed 2026-09-02.
**Blocks:** slices 3, 4 and 5. Nothing on a wire can be built until this is true in memory.

---

## 1. Why this is not on a wire, and why that is the point

**Two universes in one process, handing off through the same calls a transport will later carry**, is
this tree's own pattern rather than a shortcut. QUIC ran across `127.0.0.1` before there was a second
machine, precisely so nothing would need rewriting the day there was (ADR 0021, 0028). `ServerHost`
and `UniverseView` talk through a `LoopbackTransport` today for the same reason.

It also puts the whole mechanism inside `GameLogicTests`, which is the only place in this tree where
a thing can be *proved* rather than argued — and §9 of the design names three ways this could be
wrong, all three of which are questions a suite can answer and a wire cannot.

## 2. Scope

1. **`StepJumps` learns the outbound branch.** Today a fleet whose gate's destination does not
   resolve locally simply holds — the pass is patient, and that is right while every gate is local
   (`Universe.cpp`, the `farGate == INVALID_SHIP_ID` continue). The branch is
   `EntityShardOf(destination) != Shard()` (§3), and it is taken *before* the resolve, because a
   cross-shard destination is not a failure to resolve — it is a different answer.

   Everything else about the pass is unchanged: whole or not at all, over live members, at the same
   gate range, with the same order-spent-by-being-obeyed at the end.

2. **The outbox.** One entry per crossing ship, written in the same tick as the despawn:

   - the entity, hull, faction and hull points — what ADR 0056 already says rides across;
   - the owner, owner faction and slot — what §5 says the far side re-forms the fleet from;
   - **the far gate's `EntityId`, not an arrival position.** The departing shard cannot compute one:
     the arrival pose is read off the far gate's heading and position, and that gate is in another
     universe. So the handoff names the gate and the arriving shard derives the pose from its own
     copy — which is ADR 0056's "every intent re-derived on the far side" applied to the one thing
     slice 1's local path could take for granted.
   - **the member index and the crossing count**, so the arrival spread is computable per entry
     rather than per batch. An entry that can be applied alone is what makes a partial delivery
     harmless, and partial delivery is exactly what a wire adds in slice 4.
   - **a sequence number**, which slice 4's ack clears and this slice only writes.

3. **The inbox, drained at a tick boundary in entity order.** Not inside `Step`, for the reason an
   order is not: a handoff has to arrive on a *stated* tick or the replay gates stop meaning
   anything. Entity order and not arrival order, so two shards that delivered the same batch in two
   orders produce the same universe.

4. **The apply, and its idempotence.** `SpawnShipAs` already refuses an entity the universe holds —
   the header says so and slice 1 of the identity work put it there for this. This slice is the
   first caller that depends on it, so it gains the test that proves it: **a handoff applied twice
   produces one ship and one fleet.**

   The fleet is re-formed from owner and slot (§5). A slot that is free takes a new row; a slot
   already held **by the same owner** takes the arrivals as additional members; a slot held by
   somebody else refuses the arrival's fleet and leaves the ships in space, which is the honest
   answer and not a silent overwrite.

5. **`Universe` gains no clock, no randomness and no pointer key.** The outbox and inbox are plain
   vectors of a POD, ordered by entity, and everything about them is a pure function of what was
   handed in.

6. **Prose in the same commit**: `CrossShard.md` §4 becomes what was built rather than what was
   proposed, and §8's slice 2 row records it.

7. **A decision record**: a handoff is at-least-once delivery onto an idempotent apply — and why not
   a distributed transaction, not a lease, and not a two-phase commit.

## 3. Out of scope

- **Anything in the state codec.** Slice 3. The outbox is in memory this slice, which means a shard
  that dies mid-handoff loses it — the exact hole slice 3 fills, and naming it here is what stops
  this slice pretending to be that one.
- **Anything on a wire.** Slice 4.
- **A second process.** Slice 5.
- **The ack.** Written as a field and cleared by nothing, because there is no transport to lose a
  message yet. The re-send loop is slice 4's.
- **A refusal when the far shard is down.** §6's `FleetOrderResult` answer needs a transport that can
  be down; in one process the far universe is always there.

## 4. How it must behave

1. A fleet crossing a **local** gate behaves exactly as it does today, to byte equality on a replay.
   That is the claim that this slice added a branch rather than changed a pass.
2. A fleet crossing an **outbound** gate leaves the departing universe on the tick it crosses and is
   in the outbox and nowhere else until it is applied. §4 of the design states that plainly and the
   test states it as a census: ships in A plus ships in B plus the outbox is invariant.
3. **Applying a handoff twice is applying it once.** Every entry, in any order, any number of times.
4. **Two shards stepped in either order produce the same two universes.** The handoff is deterministic
   or this design is not compatible with the tree's central property (§9).
5. The arriving fleet holds the owner and slot it left with, its damage, and none of its intent.

## 5. Acceptance

`GameLogicTests`, and this slice is nearly all test:

- A two-shard galaxy from `BuildStartingGalaxy`, a fleet ordered through a gate that leads out, and
  the census invariant across the crossing.
- The fleet arrives whole, in its own slot, under the same entities, with hull points carried and
  order, threat and alert cleared.
- The same batch applied twice, and applied in reverse entity order, giving the same universe by
  byte equality on `WriteUniverseState`.
- A local jump still byte-identical to before this slice, which is the no-regression claim.
- A slot held by another owner refuses, and the ships arrive without a fleet rather than joining
  somebody else's.
- Both replay gates green, `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy over `GameLogic`.
- The decision record written and indexed.

## 6. Assumptions the implementer may make

- **`SpawnShipAs` already refuses a live entity.** It is written and commented for this; what is
  missing is a caller that needs it and a test that proves it.
- **A fleet crosses whole**, so every member of one fleet enters the outbox on one tick. Partial
  arrival is still handled per entry, because slice 4 makes it possible and an apply that only works
  in batches would have to be rewritten then.
- **The arriving shard owns the arrival pose.** It reads its own gate. The departing shard states
  which gate and nothing about where.
