# Work order — Hostiles slice 2: the patrol

Implements slice 2 of [`Hostiles.md`](Hostiles.md) §14: a deterministic patrol pass inside
`World::Step` that walks an assigned ship around a ring of waypoints centred on an anchor ship at
a capped cruise speed, through the order machinery a player's click already uses (design §5).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** slice 1 (`FactionId` on `ShipState`; the tests spawn hostiles).
**Blocks:** slice 3, which spawns the base and assigns the patrol at boot.

---

## 1. Why this is a slice

The patrol is the first intent in the tree that does not come from a client, and it lands in the
one layer with a replay gate so that it is *proved* to reproduce before anything is drawn around
it. It has to be its own slice because it is the only part of the feature that changes what a
tick does, and the claim that an unassigned world ticks bit-identically to today is only checkable
when nothing else in the pull request could have changed a tick.

---

## 2. Scope

### 2.1 `GameLogic/SimTuning.h` — the contract value

```cpp
// --- patrol ------------------------------------------------------------------------------------
// In the contract: it changes which points a patrolling ship steers at (Design/Hostiles.md 5.2).
inline constexpr std::uint32_t PATROL_RING_WAYPOINTS = 12;
```

Ring radius and cruise speed are *not* constants here. They are inputs to `AssignPatrol`, passed
by whoever assigns, the way a spawn position is passed to `SpawnShip` (design §8).

### 2.2 `GameLogic/Patrol.h` — the ring, as a function

A new header, registered in `GameLogic.vcxproj`, its `.filters`, and the umbrella `GameLogic.h`
(after `Formation.h`). Header-only, `namespace Game`:

```cpp
// Waypoint _index of a ring of PATROL_RING_WAYPOINTS around _anchor. 0 is due north (+Z) and the
// index walks north -> east -> south: clockwise on a north-up map, matching the headingRad
// convention. The composition root spawns onto these points and the world steers between them,
// so one function owns the geometry.
[[nodiscard]] inline WorldPos PatrolRingPoint(const WorldPos& _anchor, std::uint32_t _index, float _ringRadiusMetres) noexcept;

// The direction of travel at that waypoint: the tangent, theta + pi/2.
[[nodiscard]] inline float PatrolRingHeadingRad(std::uint32_t _index) noexcept;
```

`theta = (_index % PATROL_RING_WAYPOINTS) * 2 * pi / PATROL_RING_WAYPOINTS`; the offset is
`(sin theta, cos theta) * radius` applied through `Translate`, never by writing `localX`.

### 2.3 `GameLogic/ShipState.h` — the cruise cap

```cpp
// A ceiling on the speed an order asks for; 0 is uncapped. It is a property of the order, not the
// hull -- the same Interceptor cruises at 10 m/s on patrol and burns at 34 m/s under a player --
// and it is intent, so it stays off the wire with steerTargetPos (Design/Hostiles.md 5.4).
float orderSpeedCapMetresPerSec = 0.0f;
```

`WorldSnapshot.h`'s absent-on-purpose list already names it (slice 1 wrote the sentence); check
that it does, and that no writer touches it.

### 2.4 `GameLogic/Movement.cpp` — the clamp

In `SolveOrder`, where `desiredSpeedMetresPerSec` is set from the hull's maximum and the braking
curve: if `_ship.orderSpeedCapMetresPerSec > 0`, `std::min` it in. Nowhere else. `AvoidNeighbours`
only sheds speed and `IntegrateShip` only chases the intent, so the cap holds through both without
either learning of it. A zero cap is arithmetic the current code already performs, which is what
keeps the existing suites bit-identical.

### 2.5 `GameLogic/World.h/.cpp` — the table, the assignment, the pass

```cpp
struct Patrol
{
  ShipHandle anchor;               // a handle, so the station's death ends the patrol (ADR 0005)
  float ringRadiusMetres = 0.0f;
  float cruiseSpeedMetresPerSec = 0.0f;
  std::uint32_t waypointIndex = 0; // the ring point last issued
  bool active = false;
};
std::vector<Patrol> m_patrols;     // parallel to m_ships, swap-and-popped with them like m_routes
```

- `SpawnShip` pushes an inactive entry; `DespawnShip` moves `m_patrols[last]` into the freed
  index beside `m_routes` and pops. The despawned ship's entry is gone with it, and no entry is
  ever looked up by anything but the ship's own index.
- ```cpp
  // Assigns _ship to walk the ring around _anchorStation. The first leg goes to the ring point
  // nearest the ship's current bearing from the anchor, so assignment never teleports intent.
  void AssignPatrol(ShipId _ship, ShipId _anchorStation, float _ringRadiusMetres, float _cruiseSpeedMetresPerSec);
  ```
  Stores `HandleOf(_anchorStation)`, sets `waypointIndex` to the nearest ring index **minus one**
  modulo the ring, so the pass's `+ 1` issues the nearest point first; `active = true`. An
  `_anchorStation` that is not a live ship, or `_ship == _anchorStation`, makes the call a no-op —
  a station does not patrol itself.
- `IssueMoveOrder` sets `orderSpeedCapMetresPerSec = 0` and `active = false` for every ship it
  steers. An explicit order outranks a standing behaviour (design §5.1).
- **The pass.** `void StepPatrols();` called first in `Step`, before `SnapshotPreviousTick` — the
  position an adapter's `ApplyIncomingOrders` occupies from outside, so an NPC order and a player
  order entering the same tick are indistinguishable to every later pass. It calls
  `RebuildStaticIfDirty()` first, as `IssueMoveOrder` does, so a route planned on the tick after a
  spawn sees the grid. Then, in array order:

  ```
  skip if !active
  skip if order != Idle                      -- still flying the last leg
  anchor = Resolve(patrol.anchor)
  if none: active = false; continue          -- the station died; stand down where it is
  waypointIndex = (waypointIndex + 1) % PATROL_RING_WAYPOINTS
  order = Moving; orderHasFacing = false
  orderSpeedCapMetresPerSec = cruiseSpeedMetresPerSec
  PlanRoute(id, PatrolRingPoint(anchorPos, waypointIndex, ringRadius), ownBoundingRadius + PATH_CLEARANCE_MARGIN_METRES)
  ```

  It reads its own ship and the anchor's `posWorld`, both end-of-last-tick state, and writes only
  its own ship — the order-independence argument of design §8, and the comment on the function
  says so. No randomness, no reading of any other ship, no reaction to anything (design §5.5).
- `Step`'s pass list in `World.h`'s comment gains the new first line.

### 2.6 What this slice deliberately does **not** do

- Nothing in `Outpost`: no spawn, no constants in `ViewTuning.h`, no HUD. The scene of design §6
  exists only inside `PatrolTests`.
- No formation on the ring, no reaction to player ships, no aggro, no station lifecycle.
- No change to `PlanRoute`, `AdvanceRoute`, `AvoidNeighbours` or `ApplySeparation`. If the ring
  geometry needs one of them to change, the geometry is wrong, not the machinery — design §5.2's
  table says the legs clear the station by ~120 m.

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| `GameLogic/World.h/.cpp` | `m_routes` — the parallel-array pattern, its spawn push and despawn repair; `PlanRoute`; `IssueMoveOrder`'s order writes |
| `GameLogic/Movement.cpp` `SolveOrder` | the one place desired speed is decided |
| `GameLogic/HullSpec.h` | `HullSpecOf`, `BoundingRadiusMetres()`, `ArrivalRadiusMetres()`; the Interceptor and Structure rows |
| `GameLogic/WorldPos.h` | `Translate`, `OffsetX/Z`, `Distance` — the only way to build a ring point |
| `GameLogic/SimTuning.h` | `PATH_CLEARANCE_MARGIN_METRES`, `PATH_REPLAN_DEVIATION_METRES`, `TICK_HZ` |
| `Tests/GameLogicTests/MovementTests.cpp` `TheSameOrderProducesTheSameRun` | the field-for-field two-world comparison the new determinism test copies |
| `Tests/GameLogicTests/SeparationTests.cpp` | spawning a Structure in a test |
| `Design/Hostiles.md` §5, §8, §9, §11 | the pass, the geometry, the determinism argument, the tests |
| ADR 0005, 0008 | why the anchor is a handle; why behaviour lives here and not in an executable |

---

## 4. Acceptance

**`Tests/GameLogicTests/PatrolTests.cpp`** — new file, in both project files under the `Tests`
filter. A shared helper builds the design §6 scene: a `Structure` at `LocalPos(850, 850)`,
`FACTION_HOSTILE`; three `Interceptor`s at ring points 0, 4, 8 of a 400 m ring, headed by
`PatrolRingHeadingRad`, each `AssignPatrol`'d at 10 m/s.

- **`TheSamePatrolProducesTheSameRun`** — two scenes, stepped 60 s (3 600 ticks), compared
  field-for-field every tick, including `orderSpeedCapMetresPerSec` and `waypointIndex`. The
  replay gate extended over the new pass.
- **`APatrolWalksItsRingInOrder`** — one ship from ring point 0; over one lap it comes within
  `ArrivalRadiusMetres` of points 1, 2, … 11, 0 in that order and no other order. Clockwise: the
  ship's bearing from the anchor increases.
- **`TheCruiseCapHolds`** — over a lap, `speed <= 10 + 0.01` on every tick for a patrolling
  Interceptor; the same hull given a 2 km `IssueMoveOrder` in a second world reaches
  `maxSpeedMetresPerSec` within 1 %.
- **`APatrolNeverEntersItsStation`** — three ships, two laps: on every tick, distance from each
  ship to the station is above the two hulls' summed `BoundingRadiusMetres()`.
- **`APatrolStandsDownWhenItsAnchorDies`** — despawn the station mid-leg; the ship finishes its
  leg (`order` returns to `Idle`) and then stays `Idle` for 600 further ticks.
- **`ADespawnRepairsThePatrolTable`** — three patrolling ships; despawn the first; the ship that
  moved into index 0 keeps its ring: it still reaches its next ring point.
- **`AnOrderOutranksThePatrol`** — a patrolling ship given `IssueMoveOrder` with
  `FACTION_HOSTILE` as issuer flies to the point, arrives, and stays `Idle` — no further leg.
- **`AssignPatrolStartsAtTheNearestPoint`** — a ship placed at bearing 100° from the anchor is
  first sent to ring point 3 (90°), not 0.

**The existing suites**

- Every existing `GameLogicTests` test passes **without edits**. This is the design §8 claim that
  an unassigned world ticks bit-identically; it is the acceptance, and the pull request says the
  suite was run unchanged.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; the game runs exactly as before (it assigns no patrol).
- No screenshot: nothing visual until slice 3.
- One decision record, next number after slice 1's two, in the index: **NPC behaviour lives in
  `GameLogic`, inside the tick** — turning down the adapter-side driver and the bot client
  (design §13 decision 1).
- `Design/Hostiles.md` §14 marks slice 2 `landed`; this file moves to `Design/Archive/`.

---

## 5. Assumptions the implementer may make

- **The anchor may move.** Nothing in the pass depends on the station being immovable; the ring
  is recomputed from the anchor's position each leg. Today it never moves; the test does not need
  to prove the moving case.
- **One tick of `Idle` between legs is fine.** 16 ms at 60 Hz; no arrival logic goes in the pass.
- **`sin`/`cos` under `/fp:precise` are same-binary-same-answer**, which is the only determinism
  this tree promises (Design/Collision.md 2). No lookup table, no fixed point.
- **The ring point count is fixed at compile time.** A per-patrol count is not needed and not
  added; the `Patrol` struct has no field for it.
- **The scene numbers in the tests are the design's** (850, 850, 400, 10, three ships). They are
  test inputs here and become `ViewTuning.h` constants in slice 3; the two must agree, and slice
  3's work order says so.
