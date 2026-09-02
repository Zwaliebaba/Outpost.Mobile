# Universe slice 2 — gates, and the jump door

Work order for slice 2 of [`Universe.md`](Universe.md). One branch, one pull request.
Depends on [slice 1](Universe-slice-1.md), which supplies the gate graph this places gates for.

## 1. Scope

The galaxy becomes crossable. `GameLogic` gains a gate, an order that names one, and the pass that
takes a fleet through it.

- **A gate is a Structure with a row in a side table** — `Universe::Gate`, `GateId`, `MakeGate`,
  `GateAt`/`IsGate`/`GateOf`/`GateCount`. ADR 0038's pattern re-run (§5 of the design).
- **`DespawnCause::JumpedOut`** — the third cause through the door `Universe.h` has held open for
  it since Hostiles 4.4.
- **`FleetOrderKind::Jump`** — a fleet order naming a gate, shaped like `Dock` naming a station:
  `FleetCommand::gate`, `Fleet::orderGate`, a `NotAGate` refusal, and the approach leg through the
  move machinery every order already uses.
- **`StepJumps`** — the pass, beside `StepDockings` at the top of the standing-intent slot,
  gather-then-apply. The tick in which **every** live member stands inside the gate's capture range
  moves the whole fleet: `DespawnShip(JumpedOut)` then `SpawnShipAs` at the far gate, same
  identities, fresh handles, damage carried, formed up on the exit bearing.
- **The wire** — a fourth departure run. `SplitTheLost` today routes everything that is not
  `Docked` into *destroyed*, so a jump would detonate the fleet on every client that watched it
  leave. The run, its reader, `Publisher`, and the ALPN bump to `outpost-5`.
- **The state codec** — the gate table and `Fleet::orderGate`, or the replay gate goes red
  (AGENTS.md §8).
- **A decision record** — a jump is a despawn and a spawn under one identity.

## 2. Out of scope

- **Genesis.** Nothing spawns a gate: no composition-root change, so the game boots exactly as it
  does today. Slice 3 places them from slice 1's graph.
- **Any client change.** The fourth run is carried and decoded; no view draws a wink-out yet.
  Slice 4 is the `JUMP` affordance and the camera.
- **Cross-shard handoff.** The door is proven inside one `Universe`. The transport, the
  acknowledgment and the failure modes are a design of their own (design §11).
- Gate ownership, activation, contest or destruction; the save file (slice 5); the island-scoped
  replan (slice 6).

## 3. What to build on

- `Universe::Station`, `MakeStation`, `StationAt` — the side-table pattern, copied in shape.
- `Universe::StepDockings` — the gather-then-apply idiom exactly: captures taken during the walk
  because `DespawnShip` swap-and-pops, applied after it.
- `IssueFleetOrder`, `LowerFleetOrder`, `FleetCommand` — the order gate and the lowering.
- `SpawnShipAs` (ADR 0047), `DespawnShip` (ADR 0040), `DespawnRecord::cause`.
- `Publisher::SplitTheLost`, `SnapshotWriter::WriteInterest`/`WriteLeaves`, `SnapshotReader`'s
  `Docked()`/`ClearDocked()` — the third run is the shape the fourth copies.
- `WriteUniverseState`/`ReadUniverseState` — the station block is the gate block's model.
- `SolveFormation` / `IssueMoveOrder` — arrival in formation at the far side.

## 4. How it must behave

1. **A gate is a live Structure with a row.** Every read of the structure goes through `Resolve`
   (ADR 0005). The destination is the far gate's **`EntityId`**, not an index — the currency that
   already crosses shards.
2. **`Jump` refuses `NotAGate`** for a record that is not a live gate row, and refuses nothing else:
   a gate takes anyone this phase.
3. **Atomic.** The fleet moves on the tick every live member is inside the radius, or not at all. A
   fleet never has members in two systems.
4. **Identity survives; intent does not.** Entity, hull, faction and hull damage cross. Routes,
   patrols, protector duties and mount state are left at their fresh-spawn rest state.
5. **The jump clears the fleet's threat and alert.** Fleeing through a gate is escape, and a leash
   anchored a galaxy away would never release.
6. **A stale destination is a no-op, not a loss.** If the far gate does not resolve, nobody
   despawns — the fleet holds at the near gate. Losing a fleet into a gate that leads nowhere is
   the one failure this pass must not have.

## 5. Acceptance

`GameLogicTests` rows unless stated:

- `AGateIsAStructureWithARow` — `MakeGate` on a live ship, `GateAt`/`IsGate` answer, a dead
  structure stops resolving, a non-ship returns `INVALID_GATE_ID`.
- `AJumpOrderNamesAGate` — `Jump` at a gate is `Ordered`; at a station or a plain ship,
  `NotAGate`; an empty slot is `NoSuchFleet`. A refusal changes no standing order.
- `AFleetJumpsWholeOrNotAtAll` — with one member held outside the radius, nobody moves; when the
  last member arrives, every member is at the far gate on the same tick.
- `AJumpKeepsIdentityAndDamage` — every `EntityId` is the same after the jump, handles are not,
  and a damaged hull arrives damaged.
- `AJumpClearsIntent` — route, patrol, docking, protector duty and mount targets are at rest after
  arrival; the fleet's threat and alert are cleared, the order is `Idle`.
- `AJumpThroughAStaleGateStrandsNobody` — the far gate's structure despawned: the fleet is intact
  at the near gate and nothing was despawned.
- `AJumpedShipIsNotDestroyedOnTheWire` — over paired `LoopbackTransport`s: the subscriber's reader
  reports the ship under `Jumped()` and **not** under `Destroyed()`.
- `TheStateCodecCarriesGates` — a universe with gates and a standing jump order saves, reloads and
  replays to byte equality; a truncated gate block is refused.
- The two standing replay gates stay green.

Plus `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`, and clang-tidy over
GameLogic clean.

## 6. Assumptions the implementer may make

- **Nothing spawns a gate**, so the boot scene is unchanged and no screenshots are owed.
- The gate's capture range is `SimTuning.h`'s (a rule of the simulation); where a gate *sits* in a system
  is layout content and belongs to slice 3's genesis, not here.
- The far-side arrival uses the existing formation solver with the exit bearing pointing away from
  the destination gate's star; the exact stand-off is a tuning constant, stated in the pull request.
- `Debug|x64` is not buildable in this container; the out-of-tree harness and CI stand in, as in
  slice 1.

---

## 7. What changed on contact

Append-only, per `Design/README.md`.

**The gate radius from `Universe.md` §10 could not be satisfied by anything.** It specified a flat
120 m, centre to centre. A Structure's centre sits **251.8 m** from its own skin, so a 120 m circle
is *inside the building* — exactly the space the blocking pass exists to keep empty — and a fleet
ordered through such a gate flies at it forever. Caught by `AJumpedShipIsStatedAsJumpedAndNotAsDestroyed`,
which parked a fleet, ran twenty ticks so the blocking pass could settle it, and then watched the
crossing never happen.

Fixed the way docking already solved the same problem: **`GateRangeMetres(gate, ship)` is both
hulls' bounding radii plus `GATE_CAPTURE_METRES`**, mirroring `DockApproachRangeMetres`. The margin
is 400 m rather than docking's 60 because a jump is atomic where a docking is one ship at a time —
the doorstep has to hold a whole formation simultaneously. `LowerFleetOrder` also stopped aiming the
approach at the gate's centre, which is a point the wall forbids (ADR 0042), and now uses
`DockApproachPoint` — shared, not copied, so two approach points cannot disagree about where a
structure's doorstep is.

**The fleet's member handles had to be rebuilt on arrival**, which §6.2 implied and this order did
not spell. A jump despawns and respawns, so every member's handle dies; a fleet still holding the
dead ones is pruned to nothing by `StepFleets` at the end of the very tick it arrived. The ships
were there and the fleet was not. `AJumpClearsIntentAndTheAlert` went red first, and the `Jumper`
capture now carries the owner, the slot and the member index — owner and slot because that pair is
the name that survives a retirement where a fleet index is not.

**The arrival position had to move from the pass onto the capture.** The first draft held one
destination for the whole pass, which sends two fleets crossing on the same tick through different
gates to whichever destination was resolved last. Found by re-reading the diff, not by a test.

**The wire needed a fourth departure run, not a reused one.** `SplitTheLost` routes every cause it
does not recognise into *destroyed*, so a jump would have detonated the crossing fleet on every
screen that watched it leave. It now `switch`es, so a future cause lands in *destroyed* loudly
rather than quietly. ALPN `outpost-4` → `outpost-5`.

## 8. What was verified, and how

**Not built in `Debug|x64`** — no Windows, no MSVC, no SDK in this container. What was done instead:

- **The five touched translation units compile clean** under `g++ -std=c++20 -Wall -Wextra`:
  `Universe.cpp`, `UniverseSnapshot.cpp`, `Publisher.cpp`, plus slice 1's two.
- **The committed test files were compiled and run** against a stand-in for the VS assertion macros:
  `JumpTests` 7/7, `PublisherTests`' new row, `UniverseStateTests` 7/7 (12 777 assertions,
  **including both standing replay gates**), and slice 1's `GalaxyLayoutTests` 12/12 and
  `UniverseLayoutTests` 6/6 still green.
- **`clang-tidy` clean** over all five sources with the tree's own `.clang-tidy`, under **LLVM
  22.1.8** — the line CI's VS-bundled clang-tidy comes from.
- **`CheckProjectFiles.py` and `CheckFormat.py`** (clang-format 18.1.3) pass.
- **The suite was measured against itself.** Eight deliberate defects, one at a time: the fleet
  handles not rebuilt, the crossing made per-ship, a stale destination despawning anyway, damage
  dropped, the alert surviving, the jump logged as `Destroyed`, the publisher not routing
  `JumpedOut`, and the gate range back to flat centre-to-centre. **Six went red at once. Two
  survived, and both were holes in the tests** — the whole-fleet row asserted on `ShipCount()`,
  which a despawn-and-respawn leaves unchanged, and the damage row computed a wound it never
  applied. Worse, the whole-fleet row's straggler had never been placed outside the gate at all, so
  the row had never tested its own premise. All three closed; all eight mutations now red.

**A reviewer on Windows should still build `Debug|x64` and run all four suites.** No screenshots are
owed: nothing spawns a gate, so the boot scene is unchanged.
