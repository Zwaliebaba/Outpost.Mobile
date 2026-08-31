# Work order — Fleets slice 2: compose and launch

Implements slice 2 of [`Fleets.md`](Fleets.md) §16: `ComposeFleet` and its gates, the launch
manifest, the metronome that empties it, the rally the launched ships form up on, and the two ends
of a fleet's life — dismantled by docking, retired by loss — falling out of slice 1's prune
(design §5.2, §5.3, §4.3).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** slice 1 (the table), merged.
**Blocks:** slice 3, which lowers fleet orders onto members this slice is the only way to have.

---

## 1. Why this is a slice

Slice 1 built a table and a constructor for ships already in space. This is the one that makes a
fleet out of a station, and with it the tree's **first undock**: `Stations.md` §14 closed that door
on purpose and said the ledger row was designed to be spawned back out. Everything about that spawn
— where a hull appears, how far apart two of them may be born, where they form up — is decided here
and nowhere else, so it is worth landing against nothing else in flight.

It is also where the fleet's life closes. Docking already despawns a ship into a ledger and slice 1
already prunes a member that stopped resolving, so "docking dismantles the fleet" needs no code at
all — but it needs a test, because a property that falls out of two other slices is exactly the kind
that quietly stops being true.

---

## 2. Scope

### 2.1 `GameLogic/SimTuning.h` — the cadence

```cpp
inline constexpr std::uint32_t FLEET_LAUNCH_EVERY_TICKS = 45; // 0.75 s
```

In the replay contract, and near the top of it: it decides on which tick a ship *starts* existing,
which is `DOCK_CAPTURE_METRES`'s own sentence read backwards. Eight hulls are out in 5.25 s.

### 2.2 `GameLogic/World.h` — the manifest half of the row

`Fleet` gains the four fields design §4.1 gives it for launching, and nothing else:

```cpp
ShipHandle launchStructure;              // the station the manifest is inside
std::uint32_t manifest[MAX_FLEET_SHIPS]; // hull ids still to launch, in launch order
std::uint32_t manifestCount = 0;
std::uint32_t launchCooldownTicks = 0;
```

`memberCount + manifestCount <= MAX_FLEET_SHIPS` is the invariant compose establishes and every
launch preserves: a launch moves one hull from the second to the first, and a loss only lowers the
first.

### 2.3 `GameLogic/World.cpp` — `ComposeFleet` and its gates

```cpp
enum class ComposeResult : std::uint8_t { Composed, NotAStation, RefusedStanding, SlotTaken, TooMany, NotDocked };

ComposeResult ComposeFleet(StationId _station, std::uint8_t _slot,
                           std::span<const std::uint32_t> _hullCounts, FactionId _issuerFaction);
```

Design §5.2's five gates, in its order, each refusing the whole call and changing nothing:

| Gate | Result | Reading |
|---|---|---|
| the row is not a live station | `NotAStation` | its structure no longer resolves, or the id is past the table |
| the owner holds the issuer hostile | `RefusedStanding` | the dock gate's mirror — you do not assemble a battle group in a hostile port |
| the slot is not one of `FLEET_SLOTS`, or this faction already holds it | `SlotTaken` | one gate and one result, as design §5.2 words it: a slot that does not exist is not available either |
| the total is 0, or more than `MAX_FLEET_SHIPS` | `TooMany` | one result for one gate, so an empty compose reads as `TooMany` too |
| the issuer's own rows in this ledger do not cover the request | `NotDocked` | including a count against a hull id past `HULL_COUNT`, which no ledger can hold |

`_hullCounts` is indexed by hull id. A per-hull count is bounded before it is summed, so no
arithmetic in the gate can overflow whatever a caller passes.

**Only the issuer's own rows count**, on both the check and the draw: who else is docked in a
station is nobody's business, which is the line `Stations.md` §6.2 drew around the ledger.

An accepted compose fills the manifest **in ascending hull id**, moves those rows out of
`station.docked`, and appends the row. Ascending hull id is stated rather than incidental: it is the
launch order, and the launch order becomes the order members join the fleet in, which slice 3's
formation reads.

**The rows leave the ledger now, not one per launch**, for design §5.2's two reasons — a second
compose cannot claim them, and the screen that offered them cannot disagree with what the launch
finds.

`ComposeFleet` and `FormFleet` share the slot gate through one private `CanTakeSlot`, which is what
slice 1's "composing is written in terms of forming" comes to in practice: a composed fleet starts
with no members at all, so it cannot go through `FormFleet` itself, and the sentence in
`FormFleet`'s comment is corrected in this slice rather than left to read as a promise nobody kept.

### 2.4 `GameLogic/World.cpp` — the launch step in `StepFleets`

Third, after prune and retire, in design §4.4's order. Per fleet with a non-empty manifest:

- **A stranded manifest is dropped.** If `launchStructure` no longer resolves, or its row is no
  longer a station, the manifest can never launch — launching is the only way out of a ledger — so
  it is cleared and the retire frees the slot on the next tick. Design §5.2 says only that the ships
  are lost; this says what happens to the slot, because a fleet nothing can ever fill holding one of
  five for ever is the worse half of that bargain.
- **The metronome** is `StepProtectors`' exactly: a cooldown counted down per tick, a launch when it
  reaches zero, reset to `FLEET_LAUNCH_EVERY_TICKS` after one. A freshly composed fleet launches on
  its first tick rather than 45 later.
- **The geometry.** All of it is derived from the widest hull of the whole composed set — what is
  still inside and what is already out — so it does not move as the manifest empties:

  ```
  standoff     = station bounding radius + largest bounding radius + AVOID_MARGIN_METRES
  spacing      = SlotSpacingMetres(largest bounding radius)
  laneStepRad  = spacing / standoff
  bearing      = the station's own heading + (manifestCount - 1) * laneStepRad
  spawn        = the station's position, standoff along that bearing, heading outward
  rally        = the station's position, DockApproachRangeMetres(station, largest) + spacing
                 along the station's heading
  ```

  **Two launches of one fleet can never be born touching**, and that is a construction rather than a
  hope: consecutive spawn points sit one slot spacing apart on the circle they appear on, and the
  lane is the manifest's own remaining count, which strictly decreases — so no two launches share a
  bearing even across a loss. The design's "the first is well clear before the second exists" is not
  what makes this safe; 0.75 s buys a Corvette about 5.6 m, well inside its own hull.
- **The rally order is one order per ship, to that ship's own slot**, facing pinned to the station's
  outward bearing. The composed size is `memberCount + manifestCount` read back, so a hull can be
  given its final slot of the finished formation the moment it launches — and the slot it flies to
  is the slot its spawn bearing was taken from, so the fan and the formation run the same way round
  and nothing has to cross.

  The other shape — one `IssueMoveOrder` over every member, re-issued after each launch, letting the
  order machinery hand out slots by where the ships lie — was written first and is worse twice over:
  it re-plans the whole fleet on every launch, and it reshuffles who is where, so ships already on
  station cross each other every time. Measured: 1.0 cm of capsule overlap during a Corvette launch,
  against none with a slot per ship.
- **The cooldown is set to `FLEET_LAUNCH_EVERY_TICKS - 1`**, because the launch tick is one of the N.
  Waiting the full count puts consecutive launches N + 1 ticks apart and makes the constant's name
  off by a tick — which is what `StepProtectors`' metronome does today, and is the one place this
  pass deliberately does not copy it.

### 2.5 Two departures from the design, and why

- **The outward bearing is the station's own heading**, where design §5.3 says "the system's star
  anchor". `World` has no star and must not learn about one: the layout is content the composition
  root reads (ADR 0037), and a simulation that reached for the universe origin would be baking a
  content assumption into the tick. A station's heading is already simulation state, already
  authored by whoever spawns it, and is the natural place for "this way is out". An amendment block
  in `Fleets.md` §5.3 records it.
- **Standing is checked at compose and not again at launch.** The dock pass re-checks at capture
  because the door is guarded and not just the doorbell; a launch is the other direction, and by
  then the ships have already left the ledger. Nothing in this phase can change a standing mid-launch
  but `RecordAggression`, and turning a fleet's ships into nothing because its port soured while it
  was undocking is a rule the design does not ask for.

### 2.6 `GameLogic/WorldSnapshot.cpp` — the codec

`WORLD_STATE_FORMAT` goes 2 → 3, and each fleet row grows by the manifest half: `launchStructure`,
`manifestCount` with that many hull ids, and `launchCooldownTicks`. `ReadWorldState` validates
`manifestCount <= MAX_FLEET_SHIPS` and `memberCount + manifestCount <= MAX_FLEET_SHIPS` — the second
is the invariant §2.2 states, and a file that broke it would launch a ninth ship into a row with
nowhere to put it.

### 2.7 What this slice does not touch

- **The wire.** `ComposeOrder` — the client→server message design §5.2 sketches — is not in any
  slice's list in §16, and it belongs with the rest of the fleet wire in slice 5, beside
  `LedgerRequest`/`LedgerReply` which it has no use without. §16's slice 5 row gains it in this
  commit. Nothing here is visible to a client.
- **Fleet orders.** `FleetOrderKind`, `IssueFleetOrder` and the standing order are slice 3's. The
  rally is issued through `IssueMoveOrder` directly, which is what slice 3 will lower onto.
- **The defense**, `HullSpec::combatant`, and the alert: slice 4's.
- **`AGENTS.md` and `README.md`.** Nothing they say becomes false: no `Outpost` file changes and
  nothing calls `ComposeFleet` outside the tests, so every running build still has an empty fleet
  table.

---

## 3. What to build on

- **`World::StepProtectors`** (`World.cpp`) — the launch metronome, the standoff at the skin, and
  the spawn-then-assign shape are all its, one level up.
- **`World::IssueDockOrder` / `StepDockings`** — the ledger this slice draws from, and the pass that
  fills it. Dismantle-on-dock is entirely theirs plus slice 1's prune; this slice adds a test, not
  code.
- **`HullSpec.h`** — `SlotSpacingMetres`, `DockApproachRangeMetres`, `BoundingRadiusMetres`. The
  rally range goes through `DockApproachRangeMetres` rather than restating its sum, so the launch
  and the dock cannot disagree about where a station's door is.
- **`World::IssueMoveOrder`** — the rally order. It sorts slots by where the ships already lie across
  the formation, which is why the launch fan and the slot assignment need not agree: a ship spawned
  on lane *i*'s bearing tends to be given lane *i*'s slot, and nothing breaks when it is not.
- **`Tests/GameLogicTests/SeparationTests.cpp`** — `OverlapBetween`, the capsule overlap as the
  simulation itself computes it, which is what `ALaunchNeverJams` must measure rather than a
  centre-distance approximation.

---

## 4. Acceptance

**`Tests/GameLogicTests/FleetTests.cpp`** — extended; no new file.

| Test | Decides |
|---|---|
| `AFleetIsComposedFromTheLedger` | the manifest holds what was asked for in ascending hull id, those rows are gone from the ledger, the rest of the ledger is untouched, and the slot is held from that tick |
| `ComposeRefusesWhatIsNotThere` | `NotDocked` for more than is docked, for another faction's rows, and for a count against a hull id past the table |
| `AHostilePortRefusesComposition` | `RefusedStanding`, and the ledger is untouched |
| `ComposeRefusesABadSlotOrAnOverfullFleet` | `SlotTaken` for a held slot and for a slot past the fifth; `TooMany` for nine and for none; `NotAStation` for a row that is not one |
| `TheManifestEmptiesOnTheMetronome` | one hull per `FLEET_LAUNCH_EVERY_TICKS`, in ascending hull id, each entering `members` as it spawns, `memberCount + manifestCount` constant across the whole launch |
| `ALaunchNeverJams` | eight hulls launched from one station: no pair of them overlaps, by `CapsuleContact`, on any tick of the launch and the forming-up after it |
| `TheWidestLaunchFanStaysUnderATurn` | over the hull table, `(MAX_FLEET_SHIPS - 1) * spacing / standoff < 2π` — so a fan can never wrap onto itself, whatever the composition |
| `AStrandedManifestFreesItsSlot` | the station's structure despawned mid-launch: the manifest is dropped, the fleet retires, the slot is free |
| `ADockDismantlesTheFleet` | compose, launch, dock the members back: the ledger regains their rows, the slot frees, and the same slot composes again |
| `TheManifestSurvivesTheRoundTrip` | a fleet saved mid-launch reloads with its manifest, its cooldown and its structure handle, and resumes the same launch |
| `TheSameComposeProducesTheSameRun` | compose and a full launch in two worlds, compared state-for-state every tick |

`TheManifestSurvivesTheRoundTrip` earns its place beyond the codec: it is the only test in the suite
that plans a route in a world that came out of a file, and a world out of a file has exactly as much
vector capacity as it has ships. That is what caught the launch reading the station's pose through a
reference into `m_ships` *after* `SpawnShip` had appended to it — correct for as long as the vector
happens to have spare capacity, and garbage on the first reload. The station's pose is taken by value
now, and the test is the reason the bug has a name rather than a bug report six months from here.

Slice 1's eleven tests stay as they are and stay green.

**The existing suites**

- Every existing `GameLogicTests` test passes without edits.
- `WorldStateTests::ASavedWorldReplaysToTheSameRun` untouched and green: its scene composes no fleet,
  so the grown row is still written as a zero count.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass; clang-tidy clean over
  the files this slice touches.
- Debug|x64 builds and all four suites run; the game runs exactly as before.
- No screenshot: nothing visual until slice 6.
- No decision record is due. The table's shape was settled by ADR 0048 and nothing here reverses it;
  the two departures at §2.5 are recorded as an amendment in `Fleets.md` §5.3 and in this file.
- `Fleets.md` §16 marks slice 2 *in review*, and this file moves to `Design/Archive/` in the merge
  commit.

---

## 5. Assumptions the implementer may make

- **Nothing outside the tests composes a fleet.** The composition root calls neither `FormFleet` nor
  `ComposeFleet` until slice 6.
- **A station cannot die**, so the stranded-manifest path is reachable only by a test's own despawn.
  It is implemented anyway, because the alternative is a slot held for ever by a fleet that can never
  exist, and because the user-station design inherits whatever this one leaves.
- **The rally is not a standing order.** It is one `IssueMoveOrder` per launch; nothing re-issues it
  if a member is shoved off station afterwards. Slice 3's standing order and its patience are what
  make a fleet hold formation, and until then a rallied fleet simply arrives and stops.
- **A launched fleet may be ordered by the existing ship-list `IssueMoveOrder`**, which is how the
  tests fly one. Slice 3 retires that path for players; it stays the mechanism underneath.
- **A fleet may be composed at a station of another faction** if that faction does not hold the
  issuer hostile — the Vanguard admits anyone who has not shot at it, and composing is a docking
  right, not an ownership one.
- **`HULL_COUNT` bounds the hull-count span.** A caller passing a longer span is telling the
  simulation about hulls that do not exist; any non-zero count there is `NotDocked` rather than a
  silent truncation.
