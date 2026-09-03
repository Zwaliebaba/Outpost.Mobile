# Work order — Combat slice 1: the fire pass

Implements slice 1 of [`Combat.md`](Combat.md) §16: the device and mount tables, per-mount state,
`StepMounts` and its target priority, the acts a landed shot states, hull points and death through
the despawn door, and the stand-off that makes a pursuit hold its guns on a target instead of
ramming it (design §3, §4, §5, §6, §7, §8).

**Status: landed 2026-09-01 and in review.** §4a records the four things the implementation changed
about this order; the decision record is
[ADR 0052](../Decisions/0052-gunnery-is-deterministic-and-the-fire-pass-states-the-acts.md).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** nothing unmerged. Every socket it calls is on `main` — `RecordHostileAct` and
`RecordAggression` (ADRs 0050, 0041), `PursueTarget`, `HullSpec::combatant`, the despawn door
(ADR 0040), and the fleet posture that already suspends and resumes a standing order.
**Blocks:** slice 2, which puts the hull fraction and the fire block on the wire; slice 4, which
cannot draw a shot nobody has fired; slice 5, whose measurement is of what this slice tunes. Not
slice 3, which turned out to sit beside this one rather than under it, as its own depends-on line
says.

---

## 1. Why this is a slice

This is the one that makes the tree's promises come true. Two decision records end with the
sentence "the combat design… gives the shadowing combatants their guns," and until this slice
lands, the phrase *there is still no combat* is true in five documents. Everything after it is
transport and paint: after slice 1 a ship can die because another ship shot it, which is the
sentence the whole feature exists to make sayable.

It is large for one slice and cannot honestly be smaller. Damage with no fire pass is a number
nothing moves; a fire pass with no death is a number nothing spends; either half alone would land
a table into the replay contract with no test able to reach it, which is exactly what
`Fleets.md` §4.1 was criticised for. What keeps it bounded is that **nothing outside `GameLogic`
changes**: no wire record, no view, no key binding, no art. The client sees a ship vanish and
plays the shatter it already plays, which is a true picture of a world that has become lethal and
has not yet learned to describe itself.

---

## 2. Scope

### 2.1 `GameLogic/DeviceSpec.h` — a new file

The device table: what a mounted thing does when it cycles. Registered in
`GameLogic.vcxproj` **and** `GameLogic.vcxproj.filters` (§8's checklist), included by `HullSpec.h`,
and depending on nothing but `SimTuning.h` and `<cstdint>`.

```cpp
enum class DeviceKind : std::uint8_t
{
  Gun,
  MiningTool
};
```

Declared whole so the byte never renumbers, and `MiningTool` is reserved exactly as
`FleetOrderKind::Mine` is reserved: no device row uses it, and `StepMounts` skips any mount whose
device is not a `Gun` rather than guessing what extraction would mean (design §12).

```cpp
enum class DeviceId : std::uint32_t
{
  LightGun,
  StrikeCannon,
  LightTurret,
  MediumTurret,
  HeavyTurret
};

struct DeviceSpec
{
  DeviceKind kind = DeviceKind::Gun;
  float rangeMetres = 0.0f;             // to the target's skin, not its centre (2.5)
  std::uint32_t cooldownTicks = 0;      // ticks, never seconds (ADR 0045)
  std::uint32_t damage = 0;             // whole hull points; the damage path holds no float (2.4)
  float traverseRadPerSec = 0.0f;       // 0 is a FIXED mount: the hull's own turn is the traverse
};
```

`DEVICE_SPECS` carries design §13's starting table, and `DeviceSpecOf(DeviceId)` resolves out of
range to row 0 for `HullSpecOf`'s reason — content is a diagnostic, never a crash.

**`traverseRadPerSec == 0` means fixed, and that is the whole of the special case.** A fixed mount
has no slew to settle, so its aim state is never read and the firing gate is its arc alone; a
turret's aim slews within its arc and must settle. The convention is physical rather than a
sentinel: a barrel bolted to a hull is aimed by flying the hull, which is why a fighter flies
attack runs (§2.6) without a line written for attack runs.

### 2.2 `GameLogic/HullSpec.h` — mounts on the hull

```cpp
inline constexpr std::uint32_t MAX_MOUNTS = 6;

struct MountSpec
{
  float bearingRad = 0.0f;   // hull frame: 0 is dead ahead, +ve to starboard
  float arcHalfRad = 0.0f;   // half-width of the cone this mount may bear through
  DeviceId device = DeviceId::LightGun;
};
```

`HullSpec` gains `MountSpec mounts[MAX_MOUNTS]`, `std::uint32_t mountCount = 0`, and
`std::uint32_t maxHullPoints = 0`; every one of the ten rows gains values from design §13, because
the rows are positional aggregates and a column added to the header is a column every row states.
`LongestMountRangeMetres()` and `ShortestTurretRangeMetres()` are `constexpr` members derived from
the row rather than restated beside it, for the reason the file's other derived functions give:
adding a hull must not be able to leave a second table stale.

Two `static_assert`s, both cheap and both the kind this file already carries: no row's `mountCount`
exceeds `MAX_MOUNTS`, and no row is both `immovable` and armed — a station that shoots is a design
nobody has written (2.9).

Six is one more than the largest authored loadout (the Battleship's five). It is a capacity rather
than a behavior, so raising it costs a state-size change and no recorded outcome — but the assert
is what makes that a compile error instead of a silent truncation.

### 2.3 `GameLogic/SimTuning.h` — four constants

```cpp
inline constexpr float FIRE_ALIGN_RAD = 0.026f;          // about 1.5 degrees
inline constexpr float ENGAGE_STANDOFF_FRACTION = 0.8f;
inline constexpr float GUNNERY_QUERY_MARGIN_METRES = 4.0f;
```

All three in the replay contract, each with the argued comment this file requires. The first is how
close a turret's aim must be before it fires and is the settle tolerance, not the arc. The second
is where a pursuit holds (2.6). The third is `SEPARATION_QUERY_MARGIN_METRES`' sentence for the
gunnery term of the query radius: slack so a target is in the list on the tick before it is in
range rather than on the tick it is.

### 2.4 `GameLogic/ShipState.h` — one number

`std::uint32_t hullPoints = 0`, spawned at the hull's `maxHullPoints` by both `SpawnShip` and
`SpawnShipAs`, subtracted **saturating** at zero.

Unsigned integer rather than float, and it is worth one sentence because it is a determinism
result and not a taste: every damage number in the table is a whole number, so an integer damage
path is bit-exact by construction on every machine and under every summation order, and the
replay contract gains a field it cannot argue about. A float would have been fine today and a
liability the first afternoon somebody added a fractional resist.

### 2.5 `GameLogic/World.h` / `.cpp` — the mount table and the pass

**The table.** `struct MountState { float aimBearingRad = 0.0f; std::uint32_t cooldownTicks = 0;
ShipHandle target; }` and `struct ShipMounts { MountState mount[MAX_MOUNTS]; }`, held as
`std::vector<ShipMounts> m_mounts` parallel to `m_ships` and swap-and-popped with them. It is the
**fifth** table the despawn repair covers, and `m_protectors`' comment saying "the fourth" changes
in the same commit. Fixed capacity per ship rather than a vector each, for `Route`'s reason.

Aim starts at the mount's authored bearing. It is intent and never reaches a snapshot; the view
will slew its turrets off the fire block and its own clock (design §10.2), and is allowed to differ
by a degree.

**`StepMounts`, last in the standing-intent slot**, after `StepFleets` and before
`SnapshotPreviousTick`, so a mount reads the posture decided this tick — the threat taken, the
chase re-lowered — rather than last tick's.

It reads the neighbour list gathered on the previous tick, which is one tick staler than
`posWorld`. That is sound and must stay sound in the only way that matters: **the neighbour list
is a candidate set, and every geometric gate is recomputed from current `posWorld`**, through
`OffsetX`/`OffsetZ`/`Distance` and never from the record's cached offsets. The staleness is
bounded by one tick of closing at the table's maxima — 68 m/s ÷ 60 = 1.14 m — which is inside
`GUNNERY_QUERY_MARGIN_METRES` by construction.

Gather-then-apply, the dock pass's idiom, in four steps and this order:

1. **Walk.** Per ship in array order, per mount in index order: decrement the cooldown, choose a
   target (2.7), slew the aim toward it within the arc, and test the four gates (2.8). A shot
   appends `{shooter handle, victim handle, mount index, damage}` to `m_shotScratch` and resets
   the cooldown. The walk writes only the visiting ship's own mount state, so it is order-free.
2. **Damage.** Resolve each shot's victim and subtract, saturating. **A hull whose spec is
   `immovable` discards its damage** — Stations §8.5's standing rule, implemented here and
   permanently (design §7.2).
3. **Acts**, off the same list and *before* the deaths in step 4. This ordering is load-bearing
   and is the one thing in this slice most likely to be got backwards: `RecordHostileAct` resolves
   its victim and returns early when the handle is stale, so a fleet member killed by a single
   shot would never rouse its fleet if the deaths were applied first. Every hit calls
   `RecordHostileAct(shooter, victim)`, which self-filters to victims in a fleet; a hit whose
   victim is a station's structure, or a garrison ship with an active `ProtectorDuty`, **also**
   calls `RecordAggression(shooter, thatStation)`.
4. **Deaths.** Every ship at zero leaves through `DespawnShip(handle, DespawnCause::Destroyed)` —
   collected as handles during step 2 and applied here, because each despawn swap-and-pops and an
   id collected earlier would name a stranger. Nothing else is written: the shatter, the shock
   ring, `SHIP LOST`, the fleet prune, the protector stand-down and the departure runs are all
   already on the other side of that door, which is why this slice is smaller than it looks.

### 2.6 `GameLogic/World.cpp` — the stand-off, and the drift test it breaks

`PursueTarget` keeps its chassis and its master count. Its destination becomes the point
`EngageStandoffMetres(hull)` short of the target along the bearing **from the target to the
pursuer**, where that distance is `ENGAGE_STANDOFF_FRACTION` of the shortest range among the
hull's mounts **that can traverse**, and zero for a hull with none.

Zero for fixed-gun hulls is the design working rather than a gap: an Interceptor and a Bomber are
sent at the target itself, must point at it to satisfy the arc gate, and therefore fly attack runs
and overshoot, while a Corvette holds at 144 m and a Frigate at 224 m with every gun bearing. A
hull with no mounts at all — the Q-ship `HullSpec` insists stays expressible — shadows at the old
range, which is the degenerate case of the same arithmetic.

**`Route` gains `WorldPos pursuitAimedAt`**, the target's position when the route was planned, and
`PursueTarget`'s drift test compares the target's position against *it* rather than against
`m_routes[_ship].destination`.

This reverses `Fleets-slice-4.md` §2.3, which refused a stored aim point on the ground that "the
point last aimed at is already `m_routes[member].destination`". That was true and is not any more:
with a stand-off the destination sits up to 224 m from the target, so the old test reads a
constant offset as drift, exceeds `PURSUIT_REPLAN_METRES` on every tick of every pursuit, and
re-plans an A* sixty times a second — the exact cost that constant exists to avoid. It is a
premise that expired rather than a rule that was wrong, which is what the review gate between
slices is for; the record in §4 says so.

### 2.7 Target priority

One fixed order, first that stands:

1. the fleet's **threat**, while the fleet is engaged — the defense outranks everything;
2. the fleet's **ordered attack target**;
3. the ship's **protector duty target**;
4. **opportunistic**: the nearest candidate in the ship's neighbour list whose faction the mount's
   owner holds `Standing::Hostile`, inside the device's envelope — nearest by
   `(proximityMetres, ShipId)`, which is the order the sense pass already sorted.

A **held target** short-circuits the search: the mount keeps last tick's target while it still
resolves, is still valid under 2.8, and still satisfies the priority it was taken at. It is
`avoidHeadingRad`'s argument at gunnery scale — without it a mount flickers between two candidates
scoring within noise of each other and restarts its traverse each time — and it is a tie-break,
never a commitment.

It landed as a **step in the walk rather than a short-circuit around it**, between the protector
duty and the opportunistic sweep, and additionally requiring hostile standing so that an order which
ends is an order the guns stop obeying. Design §5.2 now numbers it 4 and the opportunistic sweep 5,
so *its* priority 4 is *this* order's held target and *its* 5 is this order's 4 (ADR 0054).

Priorities 1–3 fire at the stated handle whatever the standing table says, because an ordered
attack on a neutral is the player spending their own standing. Priority 4 fires only on
`Standing::Hostile`, so nobody drifts into a war by parking near one. **No priority may resolve to
a ship of the mount's own faction**, which is where design §11's structural answer to friendly
fire is actually enforced.

### 2.8 The four gates

A mount fires on the tick where all four hold, and there is no roll anywhere in the sequence:

1. **Range** — `Distance(shooter, target) - targetHull.BoundingRadiusMetres()` is within the
   device's `rangeMetres`;
2. **Arc** — the bearing to the target, in hull frame, is within `arcHalfRad` of the mount's
   authored bearing. Angles wrap through `XMScalarModAngle`, which is the tree's idiom
   (`Movement.cpp`'s `headingError`) and not a new helper;
3. **Aim** — for a turret, the aim is within `FIRE_ALIGN_RAD` of the target's bearing after this
   tick's slew; for a fixed mount, satisfied by gate 2 (2.1);
4. **Cooldown** — spent.

Gate 3 is where design §4's tactical sentence lives, and it is worth stating as arithmetic so a
test can assert it rather than a comment claim it: a target crossing at speed *v* at range *r*
subtends *v/r* rad/s, so a `HeavyTurret` at 18°/s holds a 34 m/s fighter beyond 108 m and loses it
inside that. Closing under the guns of a capital is therefore a real tactic, produced by three
authored numbers and no mechanism built for it.

### 2.9 `GameLogic/HullSpec.h` — the query has to reach as far as the guns

`QueryRadiusMetres` takes the greatest of three terms rather than two, the third being
`_hull.LongestMountRangeMetres() + _extent.largestMobileRadiusMetres + GUNNERY_QUERY_MARGIN_METRES`
— the range is measured to a target's skin, so the query has to carry the widest skin present.

**`PairRelevanceRadiusMetres` gains the same term against `_other`**, and that is not optional
tidiness: the pair filter is what decides whether a candidate becomes a `Neighbour` record at all,
so a query widened without it would find a target and then drop it before the mount could see it.
`GameLogicTests` already asserts the pair radius never exceeds the query radius over every ordered
pair and every subset of the table, which is what catches the half-done version.

The measurement that says this term is needed, since the whole-table maxima hide it: a skirmish of
Interceptors alone narrows the query to **137.1 m**, while an Interceptor's `LightGun` reaches
163.5 m against the widest hull present — so without the third term a fighter's guns out-range its
senses by 26 m. Against the whole-table extent the avoidance term still dominates for every hull
(a Battleship queries 619.9 m and its heaviest gun reaches 527.5 m), which is why nothing showed
before the extent narrowed.

### 2.10 `GameLogic/World.cpp` — one refusal

`FleetOrderResult` gains `RefusedFriendly`, returned by `IssueFleetOrder` for an `Attack` naming a
ship of the issuer's own faction. `NoSuchTarget` would have been a lie, and the gate is in the
simulation rather than the sheet for ADR 0014's reason: the simulation refusing is a property, an
adapter refusing is a convention.

### 2.11 The codec

`WORLD_STATE_FORMAT` goes 5 → 6, carrying `ShipState::hullPoints`, the `m_mounts` table and
`Route::pursuitAimedAt`. The mount block writes `MAX_MOUNTS` entries per ship, entries past the
hull's `mountCount` held at rest, so a reloaded row equals the row that was saved — the `Fleet`
row's argument, for the same reason: this table is compared whole.

`ReadWorldState` bounds `hullPoints` at the hull's maximum and clamps a mount's cooldown to its
device's, both fail-closed: a file claiming an invincible ship or a gun that never reloads is a
diagnostic, not a crash.

### 2.12 What this slice does not touch

- **The wire.** No snapshot record field, no fire block, no new `KIND_*`. A client learns that a
  ship died and nothing about why. That is slice 2.
- **The view, the HUD, the keys.** F4, F6 and F7 all stay exactly as they are; F6 and F7 retire in
  slice 4, when real acts have somewhere to be seen.
- **Art.** No hull carries a `Gun` marker and none needs one: the simulation reads authored
  numbers and never a mesh (ADR 0002). Slice 5 authors them.
- **Stations shooting.** No `immovable` row is armed, and the assert in 2.2 keeps it that way.
- **Station death, wrecks, loot, repair, shields, projectiles, kill attribution, mining.** Design
  §14, each with a design of its own.
- **`AGENTS.md` and `README.md`.** Several sentences in both become false — "there is still no
  combat" among them — and they change in **slice 4's** commit, when the game a player boots is
  the one they describe. A tree that can kill in tests and not on a screen is honestly described
  by neither wording, and the rulebook's own rule is that a sentence changes in the commit that
  falsifies it; the exception is stated here so the reviewer sees a decision rather than an
  oversight, and slice 4's acceptance carries it.

---

## 3. What to build on

- **`World::StepDockings`** — the collect-then-apply idiom, and the argument for why a pass that
  despawns applies after its walk rather than during it.
- **`World::StepProtectors`** — the second half of that idiom for spawns, and the pass whose duty
  target is priority 3.
- **`World::RecordHostileAct` / `RecordAggression`** (ADRs 0050, 0041) — the sockets, and the rule
  that no client message may ever state an act. The fire pass is the caller both records were
  written for.
- **`World::PursueTarget`** — one function with two masters; it gains a stand-off and keeps both.
- **`HullSpec`'s `avoidanceAuthority` and `combatant`** — the precedent for an authored per-hull
  column over a derived one, which is why devices are authored beside the hull and not inferred
  from a mesh.
- **`HullSpec`'s `QueryRadiusMetres` / `PairRelevanceRadiusMetres`** and the extent narrowing they
  read — the pair whose invariant 2.9 must preserve.
- **`Movement.cpp`'s `headingError`** — `XMScalarModAngle` is the wrap; do not write a second one.
- **`WriteWorldState` / `ReadWorldState`** — the parallel-table blocks the mount table joins.

---

## 4. Acceptance

**`Tests/GameLogicTests/CombatTests.cpp`** — new, and registered in both the `.vcxproj` and the
`.filters`.

| Test | Decides |
|---|---|
| `TheSameBattleProducesTheSameRun` | two worlds, one seed, a full engagement to mutual losses, compared state-for-state every tick |
| `AMutualKillTakesBothShips` | two ships each landing a fatal shot on the same tick both die, whichever array order they are visited in |
| `AShotStatesTheActThatRousesTheFleet` | a landed hit calls `RecordHostileAct`: the victim's fleet takes the threat, the anchor and a full alert |
| `AKillingShotStillRousesTheFleet` | a member killed outright still rouses its fleet — acts are stated before deaths (2.5) |
| `AHitOnAStationProvokesTheGarrison` | `RecordAggression` fires: the law flips empire-wide and the station launches protectors |
| `AHitOnAGarrisonShipProvokesItsStation` | the same, through the protector duty's `home` |
| `AStationDiscardsDamageAndStillJudges` | an immovable hull loses no hull points and the act is stated anyway |
| `ATurretLosesACrossingFighter` | a `HeavyTurret` tracks a 34 m/s crosser at 300 m and cannot hold it at 80 m — §2.8's arithmetic as an assertion |
| `AFixedGunFiresOnlyThroughItsArc` | a bow mount fires when pointed and holds fire off-bore, with no settle gate |
| `AMountHoldsItsTargetThroughATie` | two candidates within noise of each other: the mount keeps the first and does not restart its traverse |
| `NobodyShootsAFriend` | no priority resolves to an own-faction ship, and `IssueFleetOrder` returns `RefusedFriendly` for an attack naming one |
| `OpportunisticFireNeedsAHostileStanding` | a neutral inside the envelope is never shot; a hostile at the same range is |
| `AnOrderedAttackShootsANeutral` | a stated target is fired on whatever the standing table says |
| `TheStandOffKeepsTheGunsBearing` | a Corvette pursuing settles near 144 m with its target in arc, rather than closing to contact |
| `AFixedGunHullFliesAttackRuns` | an Interceptor is sent at the target itself, fires through the pass, and overshoots |
| `APursuitReplansOnlyWhenTheTargetMoves` | the route plan count stays flat while a pursued target holds station — the 2.6 regression, which fails loudly against the old drift test |
| `DeathTakesTheShipThroughTheDespawnDoor` | the departure is logged `Destroyed`, the fleet prunes it, and a protector's death frees its slot in the complement |
| `AProtectorThatDiesIsReplaced` | the metronome tops the garrison back up, because the count is derived |
| `TheMountTableSurvivesTheRoundTrip` | a world saved mid-engagement reloads with the same aim, cooldowns, held targets and hull points, and resumes the same run |
| `NoHullOutRangesItsOwnSenses` | for every hull and every subset of the table, the longest mounted range plus the widest skin present is inside the query radius (2.9) |

**`Tests/GameLogicTests/HullSpecTests.cpp`** — the existing pair-versus-query invariant re-run over
the widened radii, and the two new `static_assert`s exercised by a row-shaped test.

**The existing suites**

- Every existing `GameLogicTests` test passes without edits. `ProtectorTests` and `FleetTests` are
  the ones that matter: they say the stand-off and the new pass changed no behavior that was
  already specified — a protector still hunts, a fleet still stands down, an alert still burns for
  exactly `FLEET_ALERT_TICKS`.
- `WorldStateTests::ASavedWorldReplaysToTheSameRun` covers the three new fields by construction.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py` (clang-format 18.1.3),
  clang-tidy clean on the files written.
- Debug|x64 builds and all four suites run, with the configurations stated in the report.
- No screenshot: nothing visual until slice 4.
- One decision record: **gunnery is deterministic, and the fire pass states the acts** — what it
  completes in ADRs 0041 and 0050, why dice and live projectiles lost, and the expired premise in
  `Fleets-slice-4.md` §2.3 that 2.6 reverses. Next free number is **0052**.
- `Combat.md` §16 marks slice 1 *in review*.

---

## 4a. What the implementation changed about this order

Landed 2026-09-01. Four things came back different, and they are recorded here rather than left to
be found in the diff — three of them are the review gate doing its job on an order written before
the code existed.

1. **The pass runs last in the tick, not in the standing-intent slot** (§2.5; design §5.1 was
   amended to say so, ADR 0054). Opportunistic acquisition reads the neighbour list, a `Neighbour`
   names a `ShipId`, and a `ShipId` is an array
   index that every despawn moves (ADR 0005). That list is trustworthy only between the gather that
   built it and the next despawn, and the standing-intent slot is outside that window: it runs before
   the gather, so it would read the previous tick's list with ids that this tick's dock pass has
   already moved. Running last puts the whole pass inside the window, and it also means a mount fires
   on where the ships ended the tick rather than where they began it. The order's stated reason for
   the position — that a mount reads the posture decided this tick — is preserved either way.

2. **`maxHullPoints == 0` means indestructible**, rather than the pass testing `immovable` (§2.5,
   step 2). It is the same set of hulls today and a better-shaped rule: one column decides, it is the
   hull's rather than the faction's, and Stations §8.5 comes out with no station special case in the
   fire pass at all. A `static_assert` keeps the immovable rows on the right side of it.

3. **The stand-off is clamped to the pursuer's current distance** (§2.6). Written as the order
   specified, a Corvette already 30 m from its quarry — inside its own 180 m turrets — would *back
   away* to 144 m to reach a nominal gunnery range it was already well inside, and would oscillate
   against anything that closed. A stand-off may shorten a chase and may never turn one into a
   withdrawal.

4. **Three test rows changed, where the order said none would** (§4, "the existing suites").
   `ProtectorTests::AProtectorPursuesItsTarget` stated the replan invariant against the route's
   destination, which §2.6 knowingly moved away from the target — the row now asks
   `World::PursuitAimedAt`, added for it. Two `FleetTests` rows were written when nothing could die:
   one read "the combatant turns on the attacker" as distance closed, which a combatant already in
   range correctly declines to do, and now reads it as the attacker losing hull points; the other
   isolated the alert as the only bound on an engagement, which stopped being true once a roused
   fleet could kill its threat, and now uses a fleet with nothing armed in it. None of the three
   changed what it means, and each carries the reason in a comment.

## 5. Assumptions the implementer may make

- **`GameLogicTests` is the only observer.** Nothing draws a shot and nothing on the wire mentions
  one; a shot exists in this slice only as a hull point that fell and an act that was stated.
- **Design §13's numbers are a starting table, not a verdict.** They are written to be measured
  against §13's five pacing targets in slice 5, and the Battleship's hull points are already
  flagged there as short of the target it serves. Do not retune them here — land them as written
  so the measurement has a baseline, and report anything the tests show to be badly wrong.
- **No ammunition, no heat, no reload state, no accuracy falloff.** A device is range, cooldown,
  damage and traverse; anything else is a later design's field.
- **One target per mount, and mounts on one ship may hold different targets.** A Battleship's five
  mounts each run 2.7 independently, which is what point defense is and costs nothing to allow.
- **A dead attacker's shot still counts.** `RecordHostileAct` deliberately does not check the
  attacker's liveness (ADR 0050), and a shot resolved in step 1 lands in step 2 even if its
  shooter died in step 4 of the same tick. Being shot by something that then died is still being
  shot.
- **`MiningTool` is reserved and inert.** A mount carrying one is skipped by the pass. Nothing in
  this slice knows what a resource is, and the mining design owns everything behind that byte
  (design §12).
- **The stand-off reads the shortest *turret* range**, which is a reading design §8 states and
  slice 5 may overturn with a measurement: a Battleship holding at its light turrets' 144 m
  spends its heavy guns' reach to bring five mounts to bear, and whether that is right is a
  question for a fight nobody has watched yet.
