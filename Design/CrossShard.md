# Cross-shard — the same door, with a wire in the middle

Status: drafted 2026-09-01, not yet agreed with the owner.

[ADR 0056](Decisions/0056-a-jump-is-a-despawn-and-a-spawn-under-one-identity.md) chose the jump's
shape for what it would make true later, and said so:

> An intra-shard jump and a cross-shard handoff are the same two calls with a transport in the
> middle, so the handoff design slots in without reopening this one — and the wire cannot tell them
> apart, which is what will let systems move between shards without any client caring.

This is that design, arriving to collect. If it needs to reopen ADR 0056, that record was wrong and
this one has to say so rather than quietly working around it.

The player-facing sentence: **the frontier stops fitting in one process.**

---

## 1. What is already there, and it is more than it looks

- **`EntityId` is `{shard:16, serial:48}`** (ADR 0047). Identities are already globally unique and
  already carry their origin. This is the load-bearing one — see §3.
- **A jump is `DespawnShip(JumpedOut)` + `SpawnShipAs`** under one identity, with hull damage carried
  and every intent re-derived on the far side (ADR 0056).
- **`StepJumps` already builds the payload.** Its `Jumper` — entity, hull, faction, hull points,
  arrival position, heading, and the fleet owner, slot and member index — is exactly what a handoff
  has to carry. It was written to survive a despawn within one universe, and that is the same
  problem.
- **QUIC with a reliable lane**, an ALPN that has been bumped five times, and a boot that fails
  rather than falling back (ADR 0028).
- **`ConfigureShard`**, a save file that records its shard, and a reader that refuses a file whose
  header and body disagree about it (ADR 0057).
- **`UniverseGen`**, which writes a universe for a shard — and today always writes shard 0
  (ADR 0058).

## 2. The partition

**Which systems a shard holds is a function of the galaxy layout, not a table somebody maintains.**

Every participant — each shard, and every client — must agree on which shard owns a system, and must
agree *without being told*, for the same reason the galaxy itself is a seed rather than a file: two
copies of an authored partition is two chances to disagree, and the disagreement is a ship that
arrives nowhere.

`SystemSite` already carries `cellQ`/`cellR`, which are world-fixed. `ShardOfSystem(site, count)` is
a pure function of those and the shard count. **Contiguity matters and cheapness does not**: a
partition that scatters neighbours across shards turns most gates into wire crossings, which is the
one cost this whole design is trying to bound. A ring-based or block-based split keeps neighbours
together; a hash does not, and is rejected for exactly that.

The shard count is deployment configuration (`Server.cfg`, ADR 0043) and is therefore in the save
header, because a universe generated for four shards is not a universe four *other* shards can run.

## 3. A gate that leads out

**No new field.** `GateDesc::destination` is an `EntityId`, and an `EntityId` carries its shard, so

```
EntityShardOf(gate.destination) != universe.Shard()
```

*is* the test for "this gate leads out of this shard". ADR 0047 chose that layout for identity
across a despawn; it turns out to also answer routing across a process, which is the second time that
record has paid for itself.

What changes in `StepJumps` is one branch: a fleet whose gate leads out is captured into `Jumper`s
exactly as now, despawned exactly as now, and then **handed to an outbox instead of respawned**.

## 4. The hard part, named

Everything above is small. This is the design:

**A handoff must not lose a ship and must not duplicate one, and both shards must stay
deterministic.** Those pull against each other.

- **Losing** is what a fire-and-forget send risks: shard A despawns, the datagram is dropped, and the
  fleet is gone. The reliable lane makes that unlikely and not impossible — a shard can die between
  the despawn and the send.
- **Duplicating** is what a naive retry risks: A resends, B spawns twice, and one entity exists in two
  places. `SpawnShipAs` under an entity B already holds must be a refusal, not a second ship — and
  that refusal is the idempotence the whole scheme rests on.
- **Determinism** is the constraint neither of those usually has. A shard's replay is only worth
  having if a handoff arrives on a *stated* tick. So a handoff is applied the way an order is: taken
  at a tick boundary, in a fixed order, never inside a `Step`.

The shape this design proposes:

1. **An outbox on the departing shard**, written in the same tick as the despawn, and part of the
   saved state. A shard that dies after the despawn and before the send still has the ship, in its
   file, in the outbox — which is the property that makes "lost" recoverable rather than final.
2. **An inbox on the arriving shard**, drained at a tick boundary in entity order.
3. **`SpawnShipAs` refuses an entity that is already live**, so a replayed handoff is a no-op rather
   than a duplicate.
4. **An acknowledgement clears the outbox entry.** Until then it is re-sent. At-least-once delivery
   plus idempotent apply is exactly-once in effect, and it is the only combination of the three that
   does not require a distributed transaction.

**The ship is in the outbox and nowhere else for the duration.** That is the honest statement of what
a player would see: a fleet mid-handoff is not in either universe. It is one tick under any healthy
condition and it is unbounded if the far shard is down, which is why §6 says what happens then.

## 5. Fleets

A fleet crosses whole or not at all (ADR 0056), and `Fleet` is a row in *one* universe with one member
list. Cross-shard, the members arrive in a universe that has no such row.

The fleet is therefore re-formed on the far side, from the owner and slot each `Jumper` already
carries — which is precisely why slice 2 of the universe design put them there, and it did that to
survive a *local* despawn. The same three fields do both jobs.

What does not survive: anything the fleet row holds that is not owner, slot and membership — its
order, its threat, its alert. All of it is intent, and all of it is re-derived, which is the rule
ADR 0056 already set for a ship.

## 6. What happens when a shard is down

The question this design exists to answer badly if it does not answer it deliberately.

- **A gate to a shard that is not answering refuses the jump**, at the order, with the fleet where it
  was. That is `FleetOrderResult`'s existing shape — a refusal a player reads, not a fleet that
  vanishes.
- **A fleet already in the outbox waits.** It is not lost: it is in the file. When the far shard
  returns, it arrives.
- **A shard never invents the far side's state to keep playing.** The alternative is two universes
  that disagree and later have to be reconciled, which is a problem this design will not create for
  itself.

## 7. Which shard a client talks to

Deliberately last, because it is separable and because getting it wrong is expensive.

One client, one connection, to the shard its camera is in — and a camera that crosses a shard
boundary reconnects. That is the smallest thing that works, it matches what slice 4b already made
true (the client follows the camera, not an event), and it is honest about the seam rather than
hiding it behind a gateway this design would then have to specify.

A gateway that multiplexes several shards behind one connection is the answer the day a player wants
to watch two at once. Nothing here forecloses it.

## 8. Slices

| # | Slice | Layer | Size | Depends on | ADR |
|---|---|---|---|---|---|
| 1 | `ShardOfSystem`, the shard count in `GalaxyDesc` and the save header, and `UniverseGen` writing one file per shard | `GameLogic`+`Tools` | M | — | ADR: the partition is a function of the layout |
| 2 | The outbox, the inbox, `SpawnShipAs` refusing a live entity, and **two `Universe`s handed off between in one process** | `GameLogic` | L | 1 | ADR: a handoff is at-least-once delivery onto an idempotent apply |
| 3 | Both in the state codec, so a shard that dies mid-handoff still holds the ship | `GameLogic` | M | 2 | — |
| 4 | The handoff on the wire: a message on the reliable lane, the ack, the re-send | `NeuronCore`+`GameLogic` | L | 3 | — |
| 5 | Two shard processes, and a client that follows its camera across | `Outpost` | L | 4 | — |

**Slice 2 is where the design is proved, and it is deliberately not on a wire.** Two universes in one
process, handing off through the same calls a wire will later carry, is the tree's own pattern: QUIC
ran across `127.0.0.1` before there was a second machine, precisely so nothing would need rewriting
on the day there was (ADR 0021, 0028). It also puts the whole mechanism inside `GameLogicTests`,
which is the only place in this tree where a thing can be proved rather than argued.

## 9. What would make this design wrong

Stated so it can be checked rather than discovered:

- **If a handoff cannot be made deterministic**, the replay gates fail and this design is not
  compatible with the tree's central property. The proposed answer is that an inbox drained at a tick
  boundary in entity order is exactly as deterministic as the order queue already is — if that turns
  out to be false, everything above is unsafe.
- **If `SpawnShipAs` cannot refuse a live entity** without disturbing the local jump path, the
  idempotence has no home and at-least-once delivery becomes at-least-once *spawning*.
- **If the partition cannot keep neighbours together**, most gates become wire crossings and the cost
  this design bounds is not bounded.
