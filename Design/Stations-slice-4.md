# Work order — Stations slice 4: the response

Implements slice 4 of [`Stations.md`](Stations.md) §16: the first NPC behavior in the tree that
*reacts*. An attacked station scrambles its garrison; protectors pursue the ship that attacked it
wherever it goes, are replaced if they die, and come home and dock when their target is gone
(design §8).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** slice 2 for the station table and `RecordAggression`'s standing half; slice 3 for the
docking machinery a protector stands down through.
**Blocks:** slice 6, which gives it a debug key and a screenshot.

---

## 1. Why this is a slice

`Design/Archive/Hostiles.md` §5.5 said the first behavior that responds to what it sees would be "a
different design with senses and thresholds", and deferred it. This is that behavior, and landing it
alone is what makes its determinism claim checkable: it is the first pass that both **spawns** and
**despawns** inside a tick, and `TheSameResponseProducesTheSameRun` — the whole scene, twice,
compared field for field — is the acceptance for everything slices 1 to 4 added.

It is also the last `GameLogic` slice. What follows is two `Outpost` slices whose acceptance is
screenshots, so this is the last point at which the simulation half can be judged entirely by tests.

---

## 2. Scope

### 2.1 `GameLogic/SimTuning.h` — the re-aim threshold

```cpp
inline constexpr float PURSUIT_REPLAN_METRES = 64.0f;
```

Its own constant with `PATH_REPLAN_DEVIATION_METRES`'s value and reasoning: re-planning every tick
would cost everything and change nothing, and a threshold measured in path cells is what makes the
chase a sequence of ordinary orders rather than a special movement mode. Separate rather than shared
because one is about a *follower drifting off its leg* and the other about *a target moving*, and
the day either is retuned the other should not move with it.

### 2.2 `GameLogic/World.h` / `.cpp` — the target list and the full `RecordAggression`

`Station` gains `std::vector<ShipHandle> targets` and `std::uint32_t launchCooldownTicks`.

`RecordAggression` gains its second half: the attacker's handle joins the attacked station's target
list if there is room, capped at the station's `targetCap`. A full list **drops the newest**,
deterministically — the standing flip has already happened, which is the part that matters. An
attacker already on the list is not added twice.

Standing is imperial and the response is local: a second aggression against a second station
scrambles *that* station too, and both sentences come straight from the owner's brief.

### 2.3 `launchedCount` is derived, not stored — a departure from design §8.2

Design §8.2 gives `Station` a `launchedCount` field: protectors currently in space, incremented on
launch and decremented when one docks home. **This slice does not store it.** It counts, at the top
of the launch step, the active protector duties whose home is this station.

The reason is a repair path that would otherwise have to exist and would be easy to get wrong. A
protector that *dies* has to decrement the count too, or "losses are replaced by the same metronome"
never fires — and `DespawnShip` has no business knowing what a protector is. Deriving it removes
that path entirely, removes a counter from the replay contract's shadow, and cannot drift from the
truth because it *is* the truth. The cost is a walk of the ship array per station per tick, at
single digits of stations and a fleet that fits in a datagram.

`LaunchedProtectorCount(StationId)` exposes it for tests and for a debug overlay.

### 2.4 `GameLogic/World.h` / `.cpp` — the duty table

```cpp
struct ProtectorDuty
{
  StationId home = INVALID_STATION_ID;
  ShipHandle target;
  bool active = false;
};
std::vector<ProtectorDuty> m_protectors; // parallel to m_ships, repaired with them
```

**`active` means "this ship is a garrison ship of `home`, and it is in space" — not "it is
hunting".** That distinction is load-bearing and design §8.3's wording ("the duty ends and the ship
goes home") would lose it: a protector flying home is still out, still counts against the
complement, and still has to be told apart from a visitor when it reaches the door. A null or stale
`target` with nothing to replace it is what "standing down" means; the duty ends when the ship
docks, and it ends by being swap-and-popped away with it.

That also buys a case the design does not mention and should have: a protector already flying home
when a *new* aggression arrives picks the new target up on the next tick and turns round, because
the duty pass reads the home station's list before it concludes there is nothing to do.

### 2.5 `GameLogic/World.cpp` — `StepProtectors`, last in the standing-intent slot

Three steps, in this order:

1. **The duty pass**, per active duty in array order. Resolve the target; a dead or docked one is a
   stale handle either way. Stale, and the home station's list has a live handle — pruned of stale
   entries as it is read, dense and in order — take it, and clear any docking intent, because this
   ship has somewhere to be. Stale and nothing left: set the docking intent to home and leave the
   duty active. Live: pursue.
2. **The launch pass**, per station in index order. Prune the target list. Empty: reset the
   cooldown and move on — a station with nobody to hunt does not tick a metronome. Otherwise count
   the launches out, tick the cooldown down, and when it fires below the complement, collect one
   launch.
3. **Apply the launches**, in collection order.

**Pursue** means: if the ship is `Idle`, or its target has moved more than `PURSUIT_REPLAN_METRES`
from the point last aimed at, issue a move order at the target's current position — full speed, no
facing, `PlanRoute` as any order does. The point last aimed at is the route's own `destination`, so
the chase needs no state of its own.

That is a chase built entirely out of standing parts: the reaction is *choosing the point*, and
everything after the choice is the same code a player's click exercises. It never gives up — not by
range, not by time — and the server simulates beyond every interest radius, so flight is
postponement.

**A launch** is a `SpawnShip` at the station's skin: on the bearing toward the first live target, at
station bounding + own bounding + `AVOID_MARGIN_METRES`, heading outward, in the station owner's
faction, with a duty entered against the station. Spawns are collected and applied after the pass
for the reason captures are: they append to the tables the pass is walking.

### 2.6 `GameLogic/World.cpp` — a protector docking home is not a ledger row

The dock pass's capture step appends to the ledger unconditionally today. It gains one branch: a
ship whose active duty names the station it is docking at returns to the garrison instead — no
ledger row, and the hull is back in the complement because the derived count no longer sees it.

A garrison is not a guest. Slice 3's work order said this branch would land here and named the pass
that owns it, so this is the promised change and not a surprise.

---

## 3. What to build on

- **`StepDockings`** (slice 3) — the standing-intent slot, the collect-then-apply discipline, and
  the docking intent a stand-down writes.
- **`IssueMoveOrder`** — what a pursuit leg is. The protector pass chooses a point; this does
  everything after.
- **`Route::destination`** (`World.cpp`) — the point last aimed at, already stored, so the re-aim
  threshold needs no new field.
- **`AVOID_MARGIN_METRES`** — the clearance a launch spawns at, so a protector does not appear
  inside its own station's separation band.
- **`StepPatrols`** — the precedent for a pass that issues orders and must `RebuildStaticIfDirty`
  first.

---

## 4. Acceptance

**`Tests/GameLogicTests/ProtectorTests.cpp`** — new file, in both project files under `Tests`.

- **`TheStationScramblesItsComplement`** — aggression → launches on the cadence, capped at the
  complement, all in the owner's faction and on the owner's hull; a station with complement 0 (the
  Vandal base) launches nothing ever.
- **`AProtectorPursuesItsTarget`** — the distance to a fleeing target decreases; the aim point
  refreshes once the target moves past the replan threshold and not before.
- **`AProtectorStandsDownWhenItsTargetDies`** — target despawned → the protector flies home, docks,
  the launched count returns to zero, and **no ledger row is written**.
- **`ALossIsReplaced`** — despawn a protector mid-response: the metronome launches a replacement,
  and the complement is never exceeded while it does.
- **`AggressionIsImperialAndTheResponseIsLocal`** — two stations, one attacked: both hold the
  attacker hostile, only the attacked one launches.
- **`AFullTargetListDropsTheNewest`** — `targetCap + 1` aggressors: the list holds the cap, in
  arrival order, and every one of them is still criminal.
- **`AHomewardProtectorTurnsRound`** — a new aggression while a protector is flying home retargets
  it rather than letting it dock.
- **`ADespawnRepairsEveryTable`** *(extended, in `DockingTests`)* — swap-and-pop moves a protector
  duty with the ship it belongs to.
- **`TheSameResponseProducesTheSameRun`** — the full scene twice, compared field for field every
  tick: the layout's spawns, a dock, an aggression, the scramble, the pursuit. The replay gate over
  everything slices 1 to 4 added, and the slice's real acceptance.

**The existing suites**

- Every existing test passes with no assertion changed. A world in which nothing calls
  `RecordAggression` has empty target lists, so the pass visits nothing.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; the game plays exactly as before — nothing calls `RecordAggression` until
  slice 6's F6.
- One decision record, **0041**: *the protector response reacts to stated acts, not senses*.
- `Design/Stations.md` §16 marks slice 4 landed and this file moves to `Design/Archive/`, both in
  the merge commit. §8.2 gains an amendment note for the derived count (§2.3).

---

## 5. Assumptions the implementer may make

- **Nothing calls `RecordAggression` in the running game.** Tests and, from slice 6, one debug key.
  The combat design owes the real trigger; this slice owes it a socket that already works.
- **A protector has no weapon.** What it does on arrival is *shadow*: avoidance and separation hold
  it off the target's hull and it re-aims as the target moves. The teeth are the combat design's,
  and the socket it needs — a ship that is always in weapons range of its target — is exactly what
  shadowing is.
- **The reserve is bottomless.** Losses are replaced for as long as a target lives, which is safe
  precisely because a protector drops nothing when destroyed (design §8.6) — there is nothing to
  farm. That rule is recorded for the loot design and is not code here.
- **No senses.** The response starts from a *stated* act. No protector or station scans for enemies,
  no radius makes anyone a criminal, and a Vandal flying past a Vanguard station is unmolested
  however hostile the standings table says it is.
- **One tick of latency between passes is expected.** The dock pass runs before the protector pass,
  so a stand-down that writes a docking intent is flown from the next tick. Deterministic, 16 ms.
- **The Vandal base is untouched.** Complement 0 means the launch pass never collects anything for
  it, and its patrol is not a garrison.
