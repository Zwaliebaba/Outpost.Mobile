# 0056 — A jump is a despawn and a spawn under one identity, and a fleet crosses whole

Status: accepted
Date: 2026-09-01

## Context

[`Design/Archive/Universe.md`](../Archive/Universe.md) §6 needs a fleet to cross between systems. Three questions
had to be answered before any of it could be built, and each is one a reasonable person proposes
again: **what a crossing does to a ship**, **whether a fleet crosses together**, and **what a client
is told**.

The tree has been holding a door open for the first of these since Hostiles 4.4. `DespawnCause` was
opened "the width of one list and no wider" with jump-out named as a future cause
([ADR 0040](0040-a-departure-carries-a-cause.md)), and identity was made a shard-scoped serial that
outlives the universe that minted it, with `SpawnShipAs` written for "a handoff from another shard"
that did not exist yet ([ADR 0047](0047-identity-is-a-shard-scoped-serial.md)). This record is the
caller those two were waiting for.

## Decision

**A jump is `DespawnShip(JumpedOut)` followed by `SpawnShipAs` under the same `EntityId`.**

No new mechanism. The ship that left is the ship that arrives: same identity, fresh handle, hull
damage carried. Everything else — route, patrol, docking intent, protector duty, mount state — is
left at a fresh row's rest state, because all of it is intent the far side re-derives.

That shape is chosen for what it makes true later. **An intra-shard jump and a cross-shard handoff
are the same two calls with a transport in the middle**, so the handoff design slots in without
reopening this one — and the wire cannot tell them apart, which is what will let systems move
between shards without any client caring.

**A fleet crosses whole, on one tick, or not at all.** `StepJumps` moves a fleet only on the tick
every live member is inside the gate. A fleet is never half in one system and half in another —
which is a sentence the fleet row cannot say, since `Fleet` has one member list and no notion of
where it is.

**A gate is a Structure with a row in a side table**, which is
[ADR 0038](0038-stations-are-ships-with-a-side-table.md)'s pattern re-run rather than cited. The
destination is the far gate's `EntityId`, not an index: an index into *this* universe means nothing
the day the far side is elsewhere, and the field would have to change shape exactly when that is
hardest.

**A departure that is a jump gets its own run on the wire.** `Publisher::SplitTheLost` routes every
cause it does not recognise into *destroyed*, so before this run existed a fleet crossing a gate
detonated on every screen that watched it leave. The ALPN bumps to `outpost-5`.

## Alternatives considered

- **Teleport the ships: move `posUniverse` and keep the handles.** Far less machinery, and it is
  wrong on the seam that matters. Every subscriber holding that handle would see a ship travel
  60 km in one tick — the interpolator would draw a streak across the map — and the day the far
  side is another process there is no shared handle to move. Despawn-and-spawn is what the wire
  already knows how to say.
- **A fourth `DespawnCause` is unnecessary: reuse `Docked`.** It costs nothing on the wire and it
  lies. A docking is a silent removal; a jump is a thing a player watched happen and should see, and
  the two want different effects. Reusing the cause would also strand the day a client wants to
  count what left through a gate.
- **The fleet pours through one hull at a time**, on the launch metronome's cadence. It reads
  dramatic. It requires a fleet to exist in two systems at once, which the fleet row cannot express
  — and the fleet's own order, defense and cruise speed all assume one place. This game's fleets
  already cruise at their slowest member's speed *so that* arrival is a fleet-shaped event; the door
  keeps that promise rather than breaking it.
- **A transit state: ships leave space and spend time in flight between systems.** The most
  fiction-friendly, and the widest change of the three: a not-in-any-system state in the simulation,
  in the save format and on the wire, for a story this game has not asked to tell.
- **Gate standings: a gate that refuses an aggressor.** Turned down for the reason the stations
  design turned down forgiveness: half a standings design invented in a slice that does not need it
  is worse than none. The row carries an owner so genesis can say what it means, and nothing reads
  it.

## Consequences

- **The fleet's member handles are rebuilt on arrival**, and that is not bookkeeping. Every member's
  handle dies in the crossing; a fleet left holding the dead ones is pruned to nothing by
  `StepFleets` at the end of the very tick it arrived, so the ships would be there and the fleet
  would not. `JumpTests::AJumpClearsIntentAndTheAlert` is the row that holds it, and it went red
  before the code existed.
- **The jump clears the fleet's threat and alert.** Fleeing through a gate is escape; a leash
  anchored a system away would never release, because the pursuit that anchored it cannot follow.
- **A gate that leads nowhere strands nobody.** If the destination does not resolve to a live gate,
  the pass moves no one and the order stands — the fleet waits at the door. Losing a fleet into a
  broken gate is the one failure this pass must not have, and it is the fail-closed direction.
- **`GATE_CAPTURE_METRES` is measured to the skins, per pair.** `Design/Archive/Universe.md` §10 specified a
  flat 120 m centre to centre; a Structure's centre is 251 m from its own skin, so that radius is a
  circle *inside the building* which the blocking pass keeps empty. A fleet ordered through such a
  gate flies at it forever. The range is now `DockApproachRangeMetres`' shape — both hulls' radii
  plus a margin — and the design says so (`Design/Archive/Universe-slice-2.md` §7).
- **Nothing spawns a gate yet.** Slice 2 ships the mechanism and its suite; the composition root is
  untouched and the game boots exactly as it did. Genesis is slice 3's.
- **The despawn log's cause enum is now three wide and the wire's departure runs four.** Any future
  cause — wreck-and-salvage, capture — adds a run beside them rather than overloading one, and
  `SplitTheLost` switches rather than tests, so a cause nobody routed lands in *destroyed* loudly
  instead of quietly.
