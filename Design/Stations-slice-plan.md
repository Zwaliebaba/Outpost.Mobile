# Stations — slice implementation plan

An implementation plan for the six slices [`Stations.md`](Stations.md) §16 lists, grounded against
the tree at `66ac880`. It is not one of Design/README.md's five document kinds and it does not
replace one: the design still argues the shape, and each slice still gets its own work order when
it is picked up. What this adds is the part a work order otherwise has to rediscover — which files
each slice touches, in what order, what the tree has moved under the design since it was written,
and which sequencing decisions are still open.

It is a planning aid with a lifetime. Each section is superseded by that slice's work order the day
one is written, and the whole file goes when the last slice lands.

---

## 1. Where the tree stands

Verified, not assumed. Every claim below was read at `766b4d9` and re-checked at `66ac880`, the
merge that brought NMO slices 2-5 in. `GameLogic/` is byte-identical across that merge, so
everything slices 1-4 rest on is unchanged; what moved is slice 5's, and §2.3 says what.

**Nothing from Stations has landed.** No `Standing`, no `Station`, no `MakeStation`, no
`hostileMask`, no `DespawnCause`, no `UniverseLayout`, no `DockOrder`, no `FACTION_VANDAL`.

**One piece arrived early, from somebody else's slice.** `FACTION_VANGUARD = 2` is already in
`GameLogic/ShipState.h:27`, put there by the NMO livery work because the client's colour table has
to be able to name all three factions. Its comment says in as many words that the
`FACTION_HOSTILE` → `FACTION_VANDAL` rename is Stations' to make and that this line does not
pre-empt it. So slice 2 does not add the constant; it does the rename and rewrites that comment,
which is currently a note about work not yet done.

**What the design builds on is all there and matches its arithmetic:**

| The design says | The tree at `66ac880` |
|---|---|
| `SHIP_RECORD_BYTES` 82 → 83 | 82 today (`WorldSnapshot.cpp:35`) ✓ |
| A `Structure`'s skin is 251 m from its centre | `251.180f` (`HullSpec.h:114`) ✓, so §7.3's 315 m and 419 m dock ranges are right |
| Worst-case static span under the 512-cell ceiling | 13 000 m + 2 × `PATH_GRID_MARGIN_METRES` (512) = 14 024 m; ÷ `PATH_CELL_SIZE_METRES` (32) = 439 cells against `PATH_GRID_MAX_CELLS_PER_AXIS` = 512 ✓ |
| Stations are outside interest at boot | `INTEREST_RADIUS_METRES` = 2 000, nearest station 2 500 m ✓ |
| The order shape to copy | `IssueMoveOrder` + `KIND_MOVE_ORDER` + the adapter's resolve loop (`Publisher.cpp:157-165`) ✓ |
| The intent-table shape to copy | `m_routes` and `m_patrols`, both parallel to `m_ships` (`World.h`) ✓ |
| The despawn-cause door | `m_despawnLog`, `DespawnsSince`, `SplitTheLost` (`Publisher.cpp:204`) ✓ |
| Two call sites for the rename | 15 occurrences of `FACTION_HOSTILE` across 5 files, 2 of them in `Outpost` ✓ |

---

## 2. Where the tree has moved under the design

Five findings. Each changes what a slice contains, and the first is the one that matters.

### 2.1 The docked list rides the reliable lane, not the snapshot header

§7.4 says the docked handles travel "as a fourth span beside the destroyed, with a `dockedCount` in
the update header beside `destroyedCount`; the header grows and `ShipsPerSnapshotFragment` follows
it."

Half of that is now wrong, and it is the half that would send an implementer to the wrong file.
`destroyedCount` is no longer in the snapshot header. Deaths and leaves moved onto the reliable
lane as their own `KIND_LEAVE` message when ADR 0029 landed, for the reason
`WorldSnapshot.cpp:20-26` states: a snapshot is superseded by the next one and heals itself, a
leave is stated once and a lost one is a ghost ship for the rest of the match. That argument covers
a docked handle exactly — a lost *docked* is the same ghost — so docked belongs on the same lane,
in the same message, and the design's conclusion survives even though its byte layout does not.

What that changes, concretely:

- `LEAVE_HEADER_BYTES` goes 17 → 21 (`kind, tick, leaveCount, destroyedCount, dockedCount`), and
  the docked handles are a third run after the destroyed ones.
- `WriteLeaves` gains the span and its `MAX_RELIABLE_BYTES` capacity check covers all three runs.
  The bound is unchanged in practice: (8192 − 17) / 8 and (8192 − 21) / 8 are both 1 021 handles.
- **`ShipsPerSnapshotFragment` does not follow it.** That number derives from
  `SNAPSHOT_HEADER_BYTES`, which the docked list never touches.

The work order should say this, and the design should be corrected — §7.4 is one sentence out of
date rather than wrong in its argument, so an amendment note beats a rewrite.

### 2.2 The `hostileMask` byte *does* land in the snapshot header — in both writers

§4.3 is right about its own lane: the mask must ride the datagram that goes out every update
period, because that is what makes it idempotent against loss. `SNAPSHOT_HEADER_BYTES` goes
26 → 27.

Two things follow that the design does not mention.

**Both snapshot writers must emit it.** `SnapshotWriter::Write` (the whole world) and
`WriteInterest` (one subscriber) both stamp `KIND_SNAPSHOT`, and `SnapshotReceiver::Accept` parses
one header shape for both. A byte written by one and not the other desynchronises the reader on the
first full snapshot.

**Neither writer currently knows whose view it is writing.** `Write` and `WriteInterest` both take
`(const World&, …, Transport&)` and no faction; the subscriber's faction lives on
`Publisher::Subscriber`. Recommendation: `World::HostileMaskFor(FactionId) -> std::uint8_t` beside
`StandingOf`, and both writers take a `FactionId _viewer` — defaulted to `FACTION_PLAYER` on
`Write`, so every existing caller and test compiles unchanged, and passed explicitly by
`PublishOne`. Append the byte after `recordCount` so the existing field order is untouched.

For the record, because a reviewer will expect a change and there is none:
`ShipsPerSnapshotFragment` is 13 before and 13 after — (1152 − 26) / 82 = 13.7 and
(1152 − 27) / 83 = 13.5, both floor to 13.

### 2.3 The colour table Stations §9.3 describes has landed — slice 5 shrinks

**Resolved at `66ac880`.** This was the plan's one open scheduling question and the merge answered
it: NMO slices 2-5 are in, `Design/NmoFormat.md` and all four work orders have moved to
`Design/Archive/`, and the table exists.

`WorldView::LiveryOf(FactionId, bool _own, bool _hostileToMe)` (`WorldView.cpp:558`) is already
§9.3's table, precedence and all — the hostile row outranks the faction rows, `LIVERY_VANGUARD`
(`ViewTuning.h:240`) is the CVC azure, `LIVERY_VANDAL` is the red, `HUD_ALERT_RED` derives from the
latter, and its comment says the Vanguard row "is written now so that the day Stations lands, no
client code changes."

Take that at its word and slice 5's colour work is one wire: `_hostileToMe` is a parameter with
nothing feeding it, and slice 2's `hostileMask` is what feeds it. What remains for slice 5 is
`HUD_VANGUARD_BLUE` for the overview column — the minimap answers friend-or-enemy and deliberately
does not follow a livery — and the mask reaching `LiveryOf` and `CONTACTS`.

### 2.4 `DespawnsSince` changing its element type is a cross-file edit

`DespawnShip` gaining a cause turns `m_despawnLog` from `vector<ShipHandle>` into
`vector<DespawnRecord>`, and `DespawnsSince` returns a span of those. That is mechanical but it is
not local: `Publisher::SplitTheLost` reads it (`Publisher.cpp:212`) and `WorldTests.cpp:117-197`
reads it about a dozen times. Slice 3's work order should name those sites so the diff's size is
expected rather than alarming.

`DespawnShip`'s cause parameter defaults to `Destroyed`, so the F4 debug key
(`OutpostApp.cpp:540`) and every test that despawns keep their meaning with no edit at all.

### 2.5 The minimap clips at the edge; marks have to clamp

`Hud.cpp:381-385` skips a dot whose square would hang over the map edge. §9.3's marks do the
opposite — clamped to the edge at reduced alpha, direction honest and distance saturated — so they
are a second draw path beside the dot loop, not a parameter on it. Slice 5's work order should
say so, because "draw the marks like the dots" is the natural misreading.

---

## 3. The order of work

```
NMO slice 5 (liveries) ──────────────┐   (recommended: land before Stations 5, §2.3)
                                     │
Stations 1 ─┐                        │
  layout    │                        │
            ├─→ Stations 5 ──→ Stations 6
Stations 2 ─┤     scene         on screen
  who is who│
     │      │
     ↓      │
Stations 3 ─┤
  docking   │
     │      │
     ↓      │
Stations 4 ─┘
  response
```

1 and 2 are independent of each other and both are `GameLogic`, so they are serial by the
one-slice-per-layer rule rather than by dependency — either order works, and 2 first is the better
one because 3 and 4 both wait on it while nothing waits on 1 until slice 5. 3 depends on 2 for the
station table; 4 depends on 3 for the docking machinery it stands down through. 5 needs 1 and 2;
6 needs 3, 4 and 5.

**Slice 2 is the largest and should not be split.** It carries a rename, a standings table, a
station table and two wire fields, which looks like two slices — but the rename is 15 lines across
5 files, and the two wire fields are the two halves of one claim (the server states identity, the
client maps it). Splitting it would put a format change in each of two consecutive pull requests
against the same header, which is the conflict the serial rule exists to avoid.

---

## 4. Slice 1 — the layout

**Written, in review** — work order [`Stations-slice-1.md`](Stations-slice-1.md), decision record
[0037](Decisions/0037-the-universe-layout-is-static-content-in-gamelogic.md). What follows is what
was planned; the work order is what was built, and the two agree except that the draw order gained
a fourth item (the bearing jitter, which design §5.2 asks for in prose and leaves off its list).

**Layer:** `GameLogic`, `GameLogicTests`. **Depends on:** nothing. **Blocks:** slice 5.

| File | What happens to it |
|---|---|
| `GameLogic/UniverseLayout.h` | New. `PlanetSite`, `SystemLayout`, `SystemDesc`, `LayOutSystem` per design §5.2 |
| `GameLogic/UniverseLayout.cpp` | New. The draw loop |
| `GameLogic/GameLogic.vcxproj` + `.filters` | Both, both files, or `CheckProjectFiles.py` fails |
| `GameLogic/GameLogic.h` | Include after `Patrol.h`; and the umbrella's randomness sentence (below) |
| `Tests/GameLogicTests/UniverseLayoutTests.cpp` | New, plus both project files |
| `AGENTS.md` §2 | The `GameLogic/` row names its headers; `UniverseLayout` joins them |
| `Design/Decisions/0037-*.md` + `README.md` | The record below |
| `Design/Stations.md` §16 | Marks slice 1 landed |

**The work, in order.** The struct trio first, then `LayOutSystem` as one `Neuron::Pcg32(_seed)`
drawing per planet in the fixed order the design fixes — orbit, radius, body seed — with bearing
computed rather than drawn: `i · 2π / planetCount` plus a jittered fraction of half a slot. Every
position is built through `Translate` from `_starPos`; nothing writes `localX` directly, or a
system laid out near a sector boundary leaves `WorldPos`'s invariant behind.

`.cpp` rather than header-only, unlike `Patrol.h`: that header is two closed-form expressions and
this is a loop with a generator in it.

**One sentence has to change.** `GameLogic.h`'s umbrella comment promises "randomness, when it
arrives, is one seeded generator **held by World**". `LayOutSystem` is the first randomness in the
library and it is not held by World — it is a pure function of a caller-supplied seed, called at
boot, whose output is then ordinary spawn input. That is a better property than the one being
replaced and the design argues it at §10; the comment should say so rather than quietly becoming
false. AGENTS.md §5's own wording ("one seeded PCG32 when randomness arrives") stays true and needs
no edit.

**Acceptance.** `TheLayoutIsAFunctionOfItsSeed`, `TheLayoutRespectsTheGridCeiling`,
`PlanetsKeepTheirDistance` (design §11); every existing suite green and untouched; both build
checks; Debug|x64 builds and the game is visibly unchanged, because nothing calls the new function
yet.

**One thing to decide in the work order.** `TheLayoutRespectsTheGridCeiling` is supposed to pin
"the shipped `SystemDesc` bounds", and the shipped desc will not exist until slice 5 puts it in
`ViewTuning.h`. The clean answer is to make `SystemDesc`'s *defaults* the shipped bounds — three
planets, 2 500–6 500 m, 400–1 200 m radii, exactly as design §5.2 declares them — so the test
asserts against the defaults and slice 5 sets only `pinFirstPlanet` and the two pinned fields.
That removes the duplicated-number problem rather than managing it; Hostiles slice 2 had to manage
it (its §5 says the scene numbers become `ViewTuning.h` constants in slice 3 and "the two must
agree"), and this design does not have to.

**Decision record due:** the universe layout is static content in `GameLogic` — ADR 0008's
three-way elimination re-run for content both binaries need, with `BodyCatalogue` staying client-
side because what a planet *wears* is nobody's business but the client's. Next free number is
**0037**.

---

## 5. Slice 2 — who is who

**Layer:** `GameLogic`, `GameLogicTests`, plus the rename's two `Outpost` call sites.
**Depends on:** nothing. **Blocks:** 3, 4, 5.

| File | What happens to it |
|---|---|
| `GameLogic/ShipState.h` | `FACTION_VANDAL` (rename of `FACTION_HOSTILE`, same value 1); the `FACTION_VANGUARD` comment rewritten from a promise into a fact; `enum class Standing`; `FACTION_LIMIT`; `DEFAULT_STANDINGS` |
| `GameLogic/World.h` / `.cpp` | The standings table + `StandingOf` + `HostileMaskFor`; the standing half of `RecordAggression`; `Station`, `DockedShip`, `StationDesc`, `StationId`, `MakeStation`, `StationOf`, `StationFlagsOf(ShipId)` |
| `GameLogic/WorldSnapshot.h` / `.cpp` | `ShipSnapshot::flags`; `SHIP_RECORD_BYTES` 82 → 83; `SNAPSHOT_HEADER_BYTES` 26 → 27; the mask written and read; `FactionId _viewer` on both writers |
| `GameLogic/Publisher.cpp` | `PublishOne` passes `_subscriber.faction` to `WriteInterest` |
| `Outpost/OutpostApp.cpp:472,485` | The rename, mechanically |
| `Tests/GameLogicTests/StationTests.cpp` | New: the station table and the standings table |
| `Tests/GameLogicTests/SnapshotTests.cpp` | The two new wire fields; plus the rename at its `FACTION_HOSTILE` sites |
| `Tests/GameLogicTests/PatrolTests.cpp`, `OrderTests.cpp` | The rename only |
| `Design/Decisions/0038-*.md`, `0039-*.md` + `README.md` | The two records below |

**The work, in order.** Rename first, as its own commit inside the branch — 15 sites, no behaviour
— so the rest of the diff is readable. Then the standings table: a `FACTION_LIMIT × FACTION_LIMIT`
array of `Standing`, initialised from `DEFAULT_STANDINGS`, read pointwise as `StandingOf(_owner,
_other)`, written only by `RecordAggression`. Then the station table and `MakeStation`, with every
read of `Station::structure` going through `Resolve` so a row whose ship died deactivates rather
than dangles. Then the two wire fields.

**Watch the record writer's cost.** `WriteShipRecord` will ask "is this a station" per record, and
the answer is a linear scan of `m_stations`. At single-digit rows and 13 records a fragment that is
free, and design §6.1 already prices the replacement (an index, the day there are hundreds) — but
the work order should say the scan is deliberate, or a reviewer will read it as an oversight.

**Watch the flag byte's position.** It is written after `factionId`, and the reader takes fields in
write order; the mask byte is appended after `recordCount` in the header for the same reason. Both
are stated in design §6.2 and §4.3 respectively and neither is free to move.

**The "bit-identical" claim needs its exact wording.** Design §10 and §16 say slice 2's proof is
`GameLogicTests` passing unchanged. Three test files change in this slice — for the rename, and
nothing else. The claim to make in the pull request is the honest one: **no assertion changed, no
scene changed, no test's behaviour changed**; a world with no stations and no standings mutation
ticks bit-identically to `66ac880`.

**Acceptance.** `TheStandingTableStartsAsAuthored`, `StandingSurvivesTheWire`,
`TheStationFlagSurvivesTheWire`, `AStationIsItsRow` (design §11); the whole suite green;
Debug|x64 builds; the game plays exactly as before, because nothing calls `MakeStation` yet.

**Decision records due — two.** Next free numbers **0038** and **0039**:

- *Stations are ships with a side table* — against a hull property (which cannot say "this
  `Structure` is scenery") and against a second entity array (which forks snapshots, interest,
  picking and the explosion for a thing that is 95 % a ship).
- *Standings are simulation state, stated per subscriber* — directional, faction-granular, one
  mask byte on every update rather than an event message, and no client-writable path, ever.

---

## 6. Slice 3 — docking

**Layer:** `GameLogic`, `GameLogicTests`. **Depends on:** 2. **Blocks:** 4, 6.

| File | What happens to it |
|---|---|
| `GameLogic/SimTuning.h` | `DOCK_CAPTURE_METRES = 60.0f`, in the contract with its reason |
| `GameLogic/HullSpec.h` | `DockRangeMetres(station, ship)` beside `ArrivalRadiusMetres` |
| `GameLogic/World.h` / `.cpp` | `DespawnCause`, `DespawnRecord`, `DespawnShip(handle, cause = Destroyed)`, `DespawnsSince` → `span<const DespawnRecord>`; `Docking` + `m_dockings`; `StepDockings()` first in the standing-intent slot; `IssueDockOrder` + its three gates; `IssueMoveOrder` clears docking intent; the despawn repair widened to a third table |
| `GameLogic/WorldSnapshot.h` / `.cpp` | `DockOrder` + `WriteDockOrder`/`ReadDockOrder` + `KIND_DOCK_ORDER = 4`; `LEAVE_HEADER_BYTES` 17 → 21; the docked run in `WriteLeaves` and the fourth span on `WriteInterest`; `SnapshotReceiver::Docked()` / `ClearDocked()` |
| `GameLogic/Publisher.cpp` | `SplitTheLost` splits three ways; `m_dockedScratch`; `ApplyOrders` reads `KIND_DOCK_ORDER` and calls `IssueDockOrder` under the same per-tick budget |
| `Tests/GameLogicTests/DockingTests.cpp` | New |
| `Tests/GameLogicTests/WorldTests.cpp` | `DespawnsSince(...)[n]` becomes `...[n].handle`, about a dozen sites |
| `Tests/GameLogicTests/SnapshotTests.cpp`, `PublisherTests.cpp` | The docked list end to end |
| `Design/Decisions/0040-*.md` + `README.md` | The record below |

**The work, in order.** `DespawnCause` first, because it is the mechanical change everything else
sits on and it should be green before any docking logic exists. Then the order kind and its gates.
Then the intent table and the pass.

**`SplitTheLost`, precisely.** Today it walks `DespawnsSince(cursor)` and keeps the handles that
are in this subscriber's `Left()` set, then `set_difference`s them out of `Left()` to get the
merely-left. With a cause it walks the same log, pushes each hit into `m_destroyedScratch` or
`m_dockedScratch` by cause, and the `set_difference` subtracts both. The early-out in `PublishOne`
(`m_sendScratch.empty() && Left().empty()`) stays correct, because a docked handle is always in
`Left()` — a despawned ship leaves every interest set that held it.

**The capture step's ordering is the determinism argument.** Captures are collected during the walk
and applied after it, because `DespawnShip` swap-and-pops and mutating mid-iteration makes visit
order depend on who docked. Collection order is array order, which is deterministic. Design §7.3
and §10 both say this; the work order should carry the sentence, because it is the kind of thing a
later refactor "simplifies" away.

**Keep one order cap.** By its own arithmetic a dock order's header is 17 bytes and would admit 141
handles against a move order's 139 — it carries a station handle instead of a destination and a
facing. Reuse `MaxShipsPerOrder()` verbatim rather than deriving a second number: the client's
selection logic already agrees on one cap, and two caps differing by two is a fact nobody will
remember and a test nobody will write.

**Acceptance.** `AShipDocksAndLeavesTheWorld`, `ADockAndADeathDifferOnTheWire`,
`AMoveOrderCancelsDocking`, `TheStandingGateRefusesADock`, `AggressionAbortsAnApproach`,
`AggressionIsImperialAndPermanent`, `ADespawnRepairsEveryTable` (design §11); the whole suite
green; Debug|x64 builds; the game plays as before, because no client can send a dock order yet.

**Decision record due:** *a departure carries a cause on the wire* — the second cause through
Hostiles §4.4's one-list door, and why docking reuses despawn rather than inventing a `Docked`
order-state that would leave ghost entries in every pass and every index. Next free number
**0040**.

---

## 7. Slice 4 — the response

**Layer:** `GameLogic`, `GameLogicTests`. **Depends on:** 2, 3. **Blocks:** 6.

| File | What happens to it |
|---|---|
| `GameLogic/SimTuning.h` | `PURSUIT_REPLAN_METRES = 64.0f` |
| `GameLogic/World.h` / `.cpp` | Target lists and the launch metronome on `Station`; `ProtectorDuty` + `m_protectors`; `StepProtectors()` last in the slot; the full `RecordAggression`; stand-down-and-dock-home; the despawn repair widened to a fourth table |
| `Tests/GameLogicTests/ProtectorTests.cpp` | New, including the whole-scene replay test |
| `Design/Decisions/0041-*.md` + `README.md` | The record below |

**The work, in order.** `RecordAggression`'s second half (the target list) first, then the
metronome, then the pursuit, then the stand-down. Launches are collected during the pass and
applied after it, for the same reason captures are — they append to the tables the pass is walking.

**Two implementation decisions the design leaves open.**

**`launchedCount` should be derived, not repaired.** Design §8.2 has it counting protectors
currently in space and §8.3 has it decrementing when one docks home — but a protector that *dies*
has to decrement it too, or "losses are replaced by the same metronome" never fires, and
`DespawnShip` has no business knowing what a protector is. The cheap answer is to stop storing it:
count the active duties whose `home` is this station at the top of the protector pass. Single
digits, array order, no repair path, and one fewer counter inside the replay contract. Recommend
that; the `Station` field becomes a readout rather than state.

**A protector's home dock must not become a ledger row.** The capture step in slice 3's pass is
what runs for a returning protector too, so it needs the branch: a docking ship with an active
`ProtectorDuty` whose `home` is this station returns to the garrison instead of into `docked`.
Design §8.3 says so in a sentence; the work order should say which of the two passes owns the test,
because it reads like the protector pass's job and it is the dock pass's.

**One tick of latency is expected and fine.** The passes run dockings, patrols, protectors, so a
stand-down that sets docking intent in the protector pass is flown by the dock pass on the *next*
tick. Deterministic, 16 ms, and worth stating so it is not read as a bug.

**Acceptance.** `TheStationScramblesItsComplement`, `AProtectorPursuesItsTarget`,
`AProtectorStandsDownWhenItsTargetDies`, `ALossIsReplaced`, and
`TheSameResponseProducesTheSameRun` — the last is the replay gate over everything slices 1–4 added
and is the slice's real acceptance (design §11). Debug|x64 builds; the game plays as before,
because nothing calls `RecordAggression` until slice 6's F6.

**Decision record due:** *the protector response reacts to stated acts, not senses* — no aggro
radius, no threat scan, no proximity trigger; the NPC reads exactly two things it did not write
(its target's position and liveness), and the combat design owns the senses. Next free number
**0041**.

---

## 8. Slice 5 — the Vanguard scene

**Layer:** `Outpost`. **Depends on:** 1, 2. **Blocks:** 6.

| File | What happens to it |
|---|---|
| `Outpost/ViewTuning.h` | The starting-system block: layout seed beside `BODY_START_SEED`, the `SystemDesc` overrides (`pinFirstPlanet`, `BODY_START_PLANET_BEARING_DEG`, `BODY_START_PLANET_DISTANCE_METRES`); the Vanguard garrison content (Corvette, complement 3, cadence 90, target cap 4); `HUD_VANGUARD_BLUE`, derived from `LIVERY_VANGUARD` the way `HUD_ALERT_RED` is derived from `LIVERY_VANDAL` (§2.3: the scene colours already exist) |
| `Outpost/OutpostApp.h` / `.cpp` | `LayOutSystem` once at boot; `SpawnVanguardStations()`; the Vandal base registered through `MakeStation` with complement 0; `SpawnStartingBodies` placing worlds at the layout's sites; `FACTION_NAMES` beside `HULL_NAMES`; `CONTACTS` by mask (`OutpostApp.cpp:613`); the `STATIONS ONLINE` boot line; F5 rerolls looks only |
| `Outpost/WorldView.h` / `.cpp` | `LiveryOf`'s `_hostileToMe` fed from the received mask — the table itself already exists (§2.3); the mark list handed in at boot |
| `Outpost/Hud.cpp` | Station dots at `HUD_MINIMAP_STRUCTURE_DOT_PX` by the record's flag rather than by `HullSpecOf(...).immovable` (`Hud.cpp:379`); the hollow marks as a second draw path, clamped rather than clipped (§2.5) |
| `AGENTS.md` §2 | The what-is-here sentences |
| `Design/Stations.md` §16 | Marks slices 1, 2 and 5 landed as they go |

**The work, in order.** The layout call and the station spawns first — they are testable by the
`STATIONS ONLINE` count before anything is coloured. Then the mask into `LiveryOf` and `CONTACTS`.
Then the minimap, dots before marks.

**F5 must stop short of the layout.** `ReseedBodies` currently reseeds from
`BODY_START_SEED + m_bodyRerollCount` (`OutpostApp.cpp:450`). After this slice the *sites* come
from the layout seed and only the *looks* reroll: body shapes and the sky change, positions and
therefore stations hold still. A debug reroll that moved simulation content would be a debug key
changing the world.

**`CONTACTS` changes meaning, not its boot number.** From "not mine" to "hostile to me", by the
mask. At boot that is the Vandal four either way — same number, honest definition — and the
Vanguard stations do not inflate it. Worth saying in the pull request, because a reviewer checking
that the HUD is unchanged is checking the right thing for the wrong reason.

**Acceptance is screenshots** (Design/README.md: a screenshot where only a screen can decide it):
the azure station and its mark at two window sizes; the mark clamped to the map edge at boot; the
`STATIONS ONLINE` line; all four suites green; **no `GameLogic` file touched**, which is the
slice's own out-of-scope rule and the thing to check first in review.

**No decision record due.**

---

## 9. Slice 6 — docking and the response, on screen

**Layer:** `Outpost`. **Depends on:** 3, 4, 5. **Blocks:** nothing; it closes the phase.

| File | What happens to it |
|---|---|
| `Outpost/WorldView.h` / `.cpp` | `PickStation` beside `PickShip` (`WorldView.cpp:541`), ray-testing records whose flag says station, any faction; `OnTap` (`WorldView.cpp:729`) becomes own hull → select, station hull with a selection → dock, ground → move; the refusal check against the received mask before sending; the marker flash in the station's faction colour |
| `Outpost/OutpostApp.cpp` | The docked list consumed beside the destroyed one — removed silently, no explosion, no shake, no `SHIP LOST`; F6 beside F4/F5 calling `RecordAggression` on the nearest Vanguard station; the log lines |
| `Outpost/EventLog.h` callers | `DOCKING \| %d SHIPS`, `DOCKING REFUSED \| %s HOSTILE`, `DOCKED \| %d SHIPS`, `VANGUARD PROVOKED` |
| `Design/Stations.md` §16 | Marks 3, 4 and 6 landed; then the design and every work order move to `Design/Archive/`, citations retargeted in the same commit |

**The one thing this slice must get right.** The docked handles go through a path that is *not*
`ExplodeTheLost`. A docked hull is removed with no ceremony; a destroyed one detonates. They arrive
in the same message and the difference is a list, which is exactly the shape Hostiles §4.4 built —
the failure mode is a single consumer loop that treats both spans alike, and it looks like a bug in
the explosion rather than in the drain.

**`PickShip` stays own-faction-only.** Stations are not selectable, hoverable or box-selectable —
what you cannot command you must not appear to hold. `PickStation` is consulted from exactly one
place: a tap with a non-empty selection. With nothing selected, a tap on a station does nothing,
and no long-press is added: `PointerTracker` learns long-press when there is a menu to open
(design §14).

**Acceptance is screenshots** at two window sizes: a dock order flying and the hulls winking out
with a `DOCKED` line and no explosion; F6 and the garrison launching with the map turning red; the
refusal line at the Vandal base. All four suites green; no `GameLogic` file touched.

**No decision record due.**

---

## 10. The decision-record ledger

Next free number is **0037** — the merge at `66ac880` took 0035 (hulls are authored in GLB) and
0036 (a liveried surface is declared and the combine is a multiply).

| # | Record | Slice |
|---|---|---|
| 0037 | The universe layout is static content in `GameLogic` | 1 |
| 0038 | Stations are ships with a side table | 2 |
| 0039 | Standings are simulation state, stated per subscriber | 2 |
| 0040 | A departure carries a cause on the wire | 3 |
| 0041 | The protector response reacts to stated acts, not senses | 4 |

Numbers are claimed in order of writing, so a slice that lands out of order takes the next free one
and this table is the thing that goes stale, not the records. Each goes in the same pull request as
the change it explains, and the index in `Design/Decisions/README.md` lists it.

---

## 11. What is not decided

The first is answered; the rest are the implementer's and are recommended above.

1. ~~**NMO slice 5 before Stations slice 5?**~~ Resolved at `66ac880`: NMO slice 5 landed first,
   which is what was recommended, and slice 5 is smaller for it (§2.3).
2. **§7.4 is one sentence out of date** (§2.1). The docked list belongs on the reliable lane beside
   the destroyed one, and `ShipsPerSnapshotFragment` does not follow. Recommend an amendment note
   in the design rather than a rewrite — the argument is intact, the byte layout moved underneath
   it, and Design/README.md is explicit that a design is never rewritten to match what was built.
3. **`launchedCount` derived rather than stored** (§7). Recommended: it removes a repair path and a
   counter from the replay contract's shadow.
4. **One order cap, not two** (§6). Recommended: reuse `MaxShipsPerOrder()` for dock orders.
