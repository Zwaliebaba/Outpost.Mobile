# Work order — slice 17: the state codec and the replay gate

Cut from [`MmoScalabilityPlan.md`](../MmoScalabilityPlan.md) §6 slice 17, against the tree at `4d77a36`
(slices 15, 16 and 24 landed). It retires the second half of finding U3 of
[`MmoScalabilityReview.md`](../MmoScalabilityReview.md).

Layer: `GameLogic`.

---

## 1. The problem, restated from the tree

There is **no serialization of authoritative state at all**. `WorldSnapshot` is a view: it exists to
*withhold* — `steerTargetPos`, `orderFacingRad`, `orderHasFacing`, `avoidHeadingRad` and the order's
speed cap are deliberately absent, and so are routes, patrols, dockings, protector duties, the
station ledgers and the standings table. So the snapshot path structurally cannot carry a save or a
handoff, and no other writer exists.

The tree has also been promising itself a recorded replay gate and does not have one. What it has is
four **same-process rerun** tests — `TheSameOrderProducesTheSameRun` and its three siblings — which
run the same input twice in one process and compare. That is a real gate and it catches a clock or
an address leaking into the tick. What it cannot catch is state that `Step` depends on and nothing
can reconstruct, because both runs build it the same way from the same start.

Both fixes are additive and cheap now, and expensive once combat stores targets.

## 2. Scope

**`WriteWorldState` and `ReadWorldState`, beside the snapshot functions in `WorldSnapshot.{h,cpp}`,
carrying every field of `World` that `Step` reads.** Full fidelity, not the wire's lattice: this is
a save, and exactness is the entire point.

What is written, and what is rebuilt instead:

| Written | Rebuilt on load |
|---|---|
| `m_ships` — every `ShipState` field, including the five the snapshot withholds | `m_shipSlot`, from `m_slots` |
| `m_slots` — ship, generation, entity | `m_entityRows`, from `m_slots`, then sorted |
| `m_freeSlots`, in order — LIFO reuse is reproducible and the order is the reproduction | `m_index`, which `Step` rebuilds every tick from `prevPos` |
| `m_despawnLog` and `m_despawnBase` | `m_pathIslands`, from the loaded static set |
| `m_tick`, `m_shard`, `m_nextEntitySerial` | `m_extent`, recomputed as the index rebuilds |
| `m_routes`, `m_patrols`, `m_dockings`, `m_protectors` | every scratch vector, and the two candidate counters, which are readouts |
| `m_stations`, with their target lists and ledgers | |
| `m_standings` | |

### 2.1 The route's grid version is written as a *relation*, not a number

`Route::gridVersion` is compared against `PathIslands::Version()`, and a mismatch makes
`AdvanceRoute` re-plan. The version is an epoch counter with no meaning outside the run that
produced it: a loaded world rebuilds its islands and gets whatever number that rebuild produces.

So the codec writes **whether the route was current** — one byte — and the loader sets `gridVersion`
to the rebuilt `Version()` when it was, and to something else when it was not. That reproduces the
*behaviour* the number encodes without pretending the number itself means anything across a save.

For that to work the static rebuild has to happen **during the load**, not on the first `Step`:
otherwise the first tick bumps the version under every route that was just marked current, and every
routed ship re-plans on the tick after a reload and on no other.

### 2.2 A route's dead waypoints are not written

`Route::waypoint` is a fixed array of `MAX_PATH_WAYPOINTS` and `count` says how many are live. The
codec writes `count` of them. Entries past it are whatever the last longer route left behind, they
are never read, and writing them would make two worlds that behave identically compare unequal.

### 2.3 The codec is a friend, not thirty accessors

`World` keeps its state private and the codec needs all of it. Two `friend` declarations are a
targeted, reviewable grant naming exactly two functions; the alternative is thirty getters that
exist for one caller and that widen `World`'s surface for every other caller forever.

### 2.4 The gate compares state, not a field list

The replay test is:

1. run a world with architecture, orders, patrols, a station and a docking to tick T;
2. `WriteWorldState` → bytes A;
3. `ReadWorldState` into a *fresh* `World`;
4. step both worlds N more ticks, comparing every ship's position and heading on every tick;
5. `WriteWorldState` both → bytes B and C, and assert **B == C**.

Step 5 is the part worth having: it is a total comparison with no field list to keep in step, and it
fails the day somebody adds a field to `World` that `Step` reads and forgets the codec. Step 4 is
what says *when* a divergence started rather than only that one happened.

## 3. Out of scope

- **A save-file format for players.** No compression, no file header beyond a magic and a format
  byte, no directory, no naming convention. This is the codec; whatever writes a file is not.
- **Versioning beyond the format byte.** One byte, checked, and a mismatch is a refusal. There is no
  migration path because there is nothing yet to migrate from.
- **Persistence scheduling** — when a server saves, how often, and what it does with the bytes.
- **The handoff protocol.** ADR 0044 built the door; this slice makes the state that would go
  through it expressible, and stops there.
- **`WorldSnapshot`.** The view stays exactly as narrow as it is. Nothing the codec carries becomes
  visible to a client.
- **Presentation.** `Outpost` and `NeuronClient` are untouched.

## 4. What to build on

- `GameLogic/WorldSnapshot.cpp` — `ByteWriter`/`ByteReader` in the anonymous namespace, whose
  `Pos`, `F32`, `U64` and `Entity` are what full fidelity needs.
- `GameLogic/World.h` — every private member, `Slot`, `EntityRow`, `Route`, `Patrol`, `Docking`,
  `ProtectorDuty`, `Station`, `DockedShip`, `DespawnRecord`, and `RebuildStaticIfDirty`.
- `GameLogic/PathIslands.h` — `Version()`.
- `Tests/GameLogicTests/MovementTests.cpp` — `TheSameOrderProducesTheSameRun`, whose scene is the
  right shape to reuse: a fleet spanning the hull table with architecture in it.
- `Tests/GameLogicTests/StationTests.cpp` and `DockingTests.cpp` — for a scene that has a station,
  a ledger and a docking in flight when the save is taken.

## 5. Acceptance

- [ ] **Save → load → replay equality, every tick**: positions and headings match tick for tick
      across a run long enough to exercise avoidance, separation, routing, patrols, docking and the
      protector response.
- [ ] **The two states are byte-identical afterwards**, which is the assertion that no field was
      forgotten.
- [ ] **A save taken mid-order resumes mid-order**: a ship steering a planned route does not
      re-plan on the tick after a reload, which is §2.1's whole subject.
- [ ] **A round trip preserves identity**: handles resolve to the same ships, entity ids are
      unchanged, and the next spawn after a reload does not reissue an id the file used.
- [ ] **A malformed or truncated buffer is refused**, reports nothing, changes nothing, and neither
      throws nor asserts — §5's rule, and the same discipline `SnapshotReceiver` already keeps.
- [ ] **AGENTS.md §8's "replay-equality test" line becomes literally true** and says which test.
- [ ] The four existing rerun tests pass unchanged, and so does the rest of the suite.
- [ ] `python Build/CheckFormat.py` and `python Build/CheckProjectFiles.py` green.

## 6. What is *not* re-trued, and why

`Design/Archive/Collision.md` is an archived design. `Design/README.md` says a design is never
rewritten to match what was built — "where the code diverged, the decision record says so and the
design keeps its argument" — and its §14 replay-contract section is an argument, not a status line.
The plan's acceptance sentence for this slice predates the move to `Archive/`, exactly as slice 15's
`AGENTS.md` R6 sentence did. What does change is `AGENTS.md` §8's checklist line, which is a rule
about what to run and is meant to be kept true.

## 7. Assumptions the implementer may make

- **No decision record is due.** The plan's table asks for none, and nothing here moves a type
  between libraries, changes a dependency rule, or turns down an approach someone will re-propose:
  the friendship, the version-as-a-relation and the rebuilt-not-written list are all argued at the
  code and in this order. If review disagrees, the record is cheap to add.
- **One format, one reader.** Both ends are the same binary; the format byte exists so that the day
  they are not, the refusal is a refusal rather than a misread.
- **No screenshot is owed.** Nothing visual changes.
