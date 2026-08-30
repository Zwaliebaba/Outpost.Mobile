# Work order — Blocked routes: a ship can never be stuck

A ship pressed against a station stopped answering orders: it sat on the skin at full thrust, nose
in, and did not turn. The owner's rule, stated on 2026-08-30 when it was reported: **we can never
have a ship getting stuck.** This order fixes the cause and adds the guarantee.

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** nothing; it is a defect in the router that Stations made easy to reach.
**Blocks:** Stations slice 6 in practice — docking approaches bring every ship to a station's
skin, which is exactly where this bites.

---

## 1. The cause

Reproduced in `TheStartingFleetOrderedOntoAStationObeysEveryLaterOrder`: the three starting hulls
ordered onto a station as a formation, then ordered about, then away. Two of the three never left —
a Corvette at 19 m/s and a Frigate at 7 m/s, each on the skin, each `Moving`, each steering at a
waypoint a few metres *inside* the wall band.

The waypoint was the ship's own path-cell centre. `PathGrid::BlockedAlong` sampled the run from its
start point, and a ship pressed against a wall stands in a cell whose clearance is below its own
requirement — so every run from it was blocked at zero whatever direction it took, and the
string-pull anchored on the start cell's centre: a point 253 m from a 251.8 m structure, which a
13 m hull can never reach within its 4.6 m arrival radius. Behind it came a staircase of every
cell to the goal, because every later run started from that same blocked anchor.

What the owner saw follows from that: the order solver keeps asking for the point, the avoidance
fan scores every reachable heading as equally dangerous and keeps the one it has, the blocking pass
undoes the advance every tick — full thrust, no turn, no progress. Backing off frees it because the
*next* plan starts from a clear cell.

## 2. Scope

### 2.1 `GameLogic/PathGrid.cpp` — the run's own start cell is exempt

`BlockedAlong` skips samples that fall in the cell `_from` is in — the exemption `FindPath`'s
neighbour test already makes ("a ship already pressed against a wall has to be able to leave the
cell it is in"), applied to the run test the string-pull and the direct-line check share. A route
from the skin now begins at the first *clear* cell, and a tangential order from the skin is a
straight line.

### 2.2 The guarantee — a waypoint the wall will not allow is taken as reached

- `GameLogic/SimTuning.h`: `BLOCKED_WAYPOINT_TICKS = 60`, in the pathfinding contract.
- `World::Route` gains `blockedTicks`. `ApplyBlocking` counts consecutive ticks in which the
  blocking correction opposed the ship's own leg (its dot with the offset to `steerTargetPos` is
  negative) while `Moving`; any other tick resets it. A hull shouldered sideways while rounding a
  station does not count — it is still gaining on its point.
- `AdvanceRoute`, before the arrival test: at the threshold, the current waypoint is taken as
  reached. A later waypoint becomes the leg; the last one moves `route.destination` to where the
  ship stands and sets the steer target there, so the order solver's arrival fires on the next tick
  and a route that ran out is not re-planned back at the same unreachable point.

This covers what §2.1 does not: an order tapped into the middle of a station ends at its skin
("as close as the geometry allows", which `PathGrid.cpp` promised in a comment and did not
deliver), and any future geometry that puts a waypoint out of reach.

### 2.3 Not done, on purpose

- **No reverse thrust.** It was the first idea and the tests say it is not needed: with a route that
  starts at a clear cell a hull turns on the spot against the wall and leaves
  (`AShipPressedAgainstAStationBacksOffAndTurns`). A second steering mode would be a second thing
  the replay contract has to carry, for a case the router should not produce.
- Nothing on the wire. `blockedTicks` lives with the route, which is intent.

## 3. What to build on

`PathGrid::FindPath`'s neighbour exemption; `AdvanceRoute`'s arrival-and-advance; `ApplyBlocking`'s
per-ship correction vector, which already exists and is read once more.

## 4. Acceptance

`Tests/GameLogicTests/BackOffTests.cpp` — new file, in both project files:

- `AShipPressedAgainstAStationBacksOffAndTurns` — nose-in on the skin, ordered to the far side:
  arrives.
- `AnOrderIntoAStationThenAwayIsObeyed` — into the centre, then three orders in three directions:
  each arrives.
- `TwoShipsOrderedIntoAStationBothLeaveOnTheNextOrder`, `AShipSandwichedBetweenAFriendAndAStationLeaves`.
- `TheStartingFleetOrderedOntoAStationObeysEveryLaterOrder` — the owner's scene; **failed before
  §2.1, passes after**.
- `AnOrderIntoAStationEndsAtItsSkin` — the order goes `Idle` on the skin, not inside it and not
  short of it; **the guarantee of §2.2**.

All existing suites green, the replay-equality tests included; `CheckProjectFiles.py` and
`CheckFormat.py` pass; Debug|x64 builds. Decision record **0042**.

## 5. Assumptions

- A second of pushing is long enough to be certain and short enough not to be seen: 60 ticks at a
  Corvette's 30 m/s is a hull that would have covered 30 m if it could.
- The station tap that produces the walled-in order goes away with Stations slice 6 (a dock order's
  approach point is outside the skin by construction); the move-order path stays reachable and is
  what §2.2 is for.
