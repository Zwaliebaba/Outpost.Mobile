# Work order — Stations slice 3: docking

Implements slice 3 of [`Stations.md`](Stations.md) §16: a second order kind, a departure that carries
a cause, and the pass that flies a ship to a station and takes it out of the world (design §7).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** slice 2 — the station table it docks at, and the standings the gate reads.
**Blocks:** slice 4, which stands its protectors down through this machinery, and slice 6.

---

## 1. Why this is a slice

It is the first thing in the phase that removes a ship from the world for a reason that is not
death, and everything about it is that distinction: a new despawn cause, a second list on the
reliable lane, a silent removal on the client. Landing it alone is what keeps the claim checkable —
**a departure now carries a cause, and every existing caller still means `Destroyed`**.

It also lands the order path a player will use, which the slice after it reuses without a player:
a protector standing down docks at home through this same table, so building it here means slice 4
adds a behavior rather than a mechanism.

---

## 2. Scope

### 2.1 The wire's docked list rides the reliable lane — an amendment to design §7.4

Design §7.4 says the docked handles travel "as a fourth span beside the destroyed, with a
`dockedCount` in the update header beside `destroyedCount`; the header grows and
`ShipsPerSnapshotFragment` follows it."

Half of that is out of date, and it is the half that would send an implementer to the wrong file.
`destroyedCount` is no longer in the snapshot header: deaths and leaves moved to the reliable lane
as their own `KIND_LEAVE` message when [ADR 0029](Decisions/0029-departures-and-orders-take-the-reliable-lane.md)
landed, for the reason `WorldSnapshot.cpp` states — a snapshot is superseded by the next one and
heals itself, a leave is stated once and a lost one is a ghost ship for the rest of the match.

That argument covers a docked handle exactly. A lost *docked* is the same ghost. So the design's
conclusion survives intact and only its byte layout moved underneath it:

- `dockedCount` joins `LEAVE_HEADER_BYTES`, 17 → 21, and the docked handles are a third run after
  the destroyed ones.
- **`ShipsPerSnapshotFragment` does not follow it.** That number derives from
  `SNAPSHOT_HEADER_BYTES`, which the docked list never touches.
- What does follow is `WriteLeaves`'s `MAX_RELIABLE_BYTES` capacity check, which now covers three
  runs. The bound is unchanged in practice: `(8192 − 17) / 8` and `(8192 − 21) / 8` both floor to
  1 021 handles.

Per Design/README.md a design is never rewritten to match what was built, so §7.4 keeps its prose
and gains a dated amendment note pointing here and at the decision record.

### 2.2 `GameLogic/SimTuning.h` and `HullSpec.h` — the capture range

```cpp
// SimTuning.h -- contract: it decides on which tick a ship stops existing in space.
inline constexpr float DOCK_CAPTURE_METRES = 60.0f;

// HullSpec.h, beside ArrivalRadiusMetres
[[nodiscard]] constexpr float DockApproachRangeMetres(const HullSpec& _station, const HullSpec& _ship) noexcept;
[[nodiscard]] constexpr float DockRangeMetres(const HullSpec& _station, const HullSpec& _ship) noexcept;
```

Derived per pair, not flat, and the reason is the hull table's own spread: a flat 300 m is inside a
Carrier's no-go band and a canyon for an Interceptor. The approach range is 60 m clear of the two
skins for every pair in the table — outside the hull-plus-clearance band where separation is shoving
and routes cannot end, and generous enough that the destination sits in unblocked path cells at a
32 m cell size. The 60 m is a *slack* chosen against path-cell quantization, not a feel number.

**Two functions, not one, and design §7.3 asked for one.** This is the slice's only real divergence
and it was found by running the thing. §7.3 sends a ship to a point "at exactly its own dock range"
and captures it "within `DockRangeMetres`" — the same number. But a ship is declared *arrived* when
it is within `ArrivalRadiusMetres` of its destination **in any direction**, so one sent to a point on
the capture boundary can settle just outside, go Idle, be re-aimed at the point it is already
standing on, and stay there for ever. Measured: a Corvette parked at 328.66 m against a 324.88 m
boundary and never docked, for six thousand ticks.

So `DockRangeMetres` is the approach range **plus the ship's own arrival radius**, which makes
arriving and docking the same event by construction. No fixed margin inside the boundary would have
worked: a Carrier's 37 m arrival tolerance is more than half the 60 m slack, and pulling the aim
point far enough in to absorb it would put it inside the station's own hull. Design §7.3 carries an
amendment note saying so; its argument is untouched, because the number it argued for is the
approach range and that is unchanged.

### 2.3 `GameLogic/World.h` / `.cpp` — a departure with a cause

```cpp
enum class DespawnCause : std::uint8_t { Destroyed, Docked };
struct DespawnRecord { ShipHandle handle; DespawnCause cause = DespawnCause::Destroyed; };

bool DespawnShip(ShipHandle _handle, DespawnCause _cause = DespawnCause::Destroyed);
[[nodiscard]] std::span<const DespawnRecord> DespawnsSince(std::uint64_t _cursor) const noexcept;
```

The cause is defaulted, so F4 and every existing caller keep their meaning with no edit. The return
type change is not local: `Publisher::SplitTheLost` reads the log, and `WorldTests.cpp` reads it
about a dozen times. Those are mechanical and the diff's size should be expected rather than
alarming.

This widens Hostiles §4.4's mechanism rather than adding a parallel one — that design called itself
"the door, opened the width of one list", and *docked* is the second cause through the same door.

### 2.4 `GameLogic/World.h` / `.cpp` — the order and its gates

```cpp
enum class DockOrderResult : std::uint8_t { Ordered, NotAStation, RefusedStanding };
DockOrderResult IssueDockOrder(std::span<const ShipId> _ships, ShipId _station, FactionId _issuerFaction);
```

Three gates, in the simulation and not the adapter, for ADR 0014's reason:

1. `_station` must be a live station row — anything else is `NotAStation`, a no-op.
2. Ships not of `_issuerFaction` are dropped exactly as the move gate drops them.
3. `StandingOf(stationOwner, _issuerFaction) == Hostile` refuses the **whole** order. Refused means
   nothing changes: the client's affordance already knew and said so before sending (§9.2), so the
   silent wire costs nothing.

An accepted order, per ship: the patrol is cleared — an explicit order outranks a standing behavior,
the line `IssueMoveOrder` already has — the docking intent is set, and the first approach leg is
issued immediately, so the order feels like an order and not like a next-tick suggestion.

**`IssueMoveOrder` clears docking intent** in return. A later move order is a change of mind, and
before capture a docking ship is just a ship flying somewhere. There is no undock and no
cancel-into-hold: cleared intent leaves the ship doing whatever it was last told.

### 2.5 `GameLogic/World.h` / `.cpp` — the intent table and the pass

```cpp
struct Docking { ShipHandle station; bool active = false; };
std::vector<Docking> m_dockings; // parallel to m_ships, swap-and-popped with them
```

Not a field on `ShipState`, for the sentence the patrol table already cites: an intent is what the
snapshot exists to withhold. The despawn repair extends to a third table the way it covered the
second.

`StepDockings` runs **first** in the standing-intent slot, before `StepPatrols`:

```
for each ship with active docking, in array order:
  resolve the station structure; gone, or no longer a station row -> active = false (stand down where it is)
  else if within DockRangeMetres(station hull, own hull)
       -> hostile now? active = false        (turned criminal mid-approach: aborted at the door)
          else collect for capture
  else if order == Idle
       -> re-issue the approach: the point on the ship's current bearing from the station at exactly
          its own dock range, PlanRoute with the ship's ordinary clearance
after the loop, in collection order:
  append {hullId, factionId} to the ledger; despawn with cause Docked
```

Two things in that shape are load-bearing and a later refactor will want to "simplify" both:

- **Captures are collected during the walk and applied after it.** `DespawnShip` swap-and-pops four
  parallel tables, so mutating mid-iteration would make the visit order depend on who docked.
  Collection order is array order, which is deterministic.
- **The standing is re-checked at capture**, not only at order time. That closes the window between
  an accepted order and an aggression recorded during the flight: the door is guarded, not just the
  doorbell.

The approach is re-issued whenever the ship goes `Idle` short of range — shoved off by traffic,
replanned, blocked — so docking is patient the way patrols are, with no arrival logic of its own.

### 2.6 `GameLogic/WorldSnapshot.h` / `.cpp` — the order and the docked list

`DockOrder` + `WriteDockOrder` / `ReadDockOrder` + `KIND_DOCK_ORDER = 4`, the same shape as the move
order beside it.

**One order cap, not two.** By its own arithmetic a dock order's header is 17 bytes and would admit
141 handles against a move order's 139 — it carries a station handle instead of a destination and a
facing. Reuse `MaxShipsPerOrder()` verbatim rather than deriving a second number: the client's
selection logic already agrees on one cap, and two caps differing by two is a fact nobody will
remember and no test will pin.

`WriteLeaves` and `WriteInterest` gain a docked span; `SnapshotReceiver` gains `Docked()` and
`ClearDocked()` beside `Destroyed()` and `ClearDestroyed()`, appended across a drain for the same
reason the deaths are.

### 2.7 `GameLogic/Publisher.cpp` — the three-way split and the second order kind

`SplitTheLost` walks the same log and sorts each hit into `m_destroyedScratch` or `m_dockedScratch`
by cause; the `set_difference` that produces the merely-left subtracts both. `PublishOne`'s early
out is unaffected: a docked handle is always in `Left()`, because a despawned ship leaves every
interest set that held it.

`ApplyOrders` tries `ReadDockOrder` when `ReadMoveOrder` declines, under the same per-tick budget.

---

## 3. What to build on

- **`IssueMoveOrder`** (`World.cpp`) — the faction gate, the patrol clear, the immediate `PlanRoute`.
  `IssueDockOrder` is the same shape with a different destination rule.
- **`StepPatrols`** (`World.cpp`) — the standing-intent slot, why it is order-independent there, and
  the `RebuildStaticIfDirty` an order-issuing pass needs at its top.
- **`m_patrols`** — the parallel-table pattern: appended in `SpawnShip`, swap-and-popped in
  `DespawnShip`, its own test that the repair works.
- **`SplitTheLost`** (`Publisher.cpp`) — the intersection of the despawn log with this subscriber's
  `Left()` set, and the `set_difference` that follows it.
- **`WriteLeaves` / `AcceptLeaves`** (`WorldSnapshot.cpp`) — the reliable message, its capacity
  check, and the read-it-all-before-touching-the-set discipline.

---

## 4. Acceptance

**`Tests/GameLogicTests/DockingTests.cpp`** — new file, in both project files under `Tests`.

- **`AShipDocksAndLeavesTheWorld`** — order → approach → capture within `DockRangeMetres` → despawn,
  with a ledger row carrying the ship's hull and faction, and a despawn log entry whose cause is
  `Docked`.
- **`EveryHullDocksAtItsOwnRange`** — an Interceptor, a Corvette and a Carrier each dock, none
  captured further out than its own range. The three ends of the hull table, because the range is
  derived per pair and a flat one would strand at least one of them (§2.2).
- **`ADeadStationStandsItsVisitorsDown`** — a station whose structure dies ends the approach where
  the visitor stands, rather than flying it at an index that now means something else.
- **`AMoveOrderCancelsDocking`** — a move order after a dock order clears the intent; the ship
  diverts and never captures, however long it is stepped.
- **`TheStandingGateRefusesADock`** — a hostile issuer gets `RefusedStanding` and no ship's state is
  touched; a non-station target gets `NotAStation`; another faction's ships are dropped from an
  otherwise good order.
- **`AggressionAbortsAnApproach`** — aggression recorded mid-flight: the ships arrive and are turned
  away at capture range rather than admitted, and the ledger stays empty.
- **`ADespawnRepairsEveryTable`** — the patrol test's shape, widened: swap-and-pop moves docking
  intent with the ship that moved.
- **`TheSameDockProducesTheSameRun`** — the whole scene twice, compared field for field every tick.
  The replay gate over the new pass.

**`Tests/GameLogicTests/SnapshotTests.cpp`**

- **`ADockAndADeathDifferOnTheWire`** — a docked handle arrives in the docked list, a destroyed one
  in the destroyed list, and a range-leaver in neither.
- **`ADockOrderRoundTrips`** — write and read, with the station handle and the ship list intact; an
  oversized one is refused.

**`Tests/GameLogicTests/WorldTests.cpp`** — the `DespawnsSince` reads become `.handle`, and one new
assertion that the default cause is `Destroyed`.

**The existing suites**

- Every existing test passes with no assertion changed. `WorldTests` is edited for the element-type
  change and nothing else.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; the game plays exactly as before — no client sends a dock order until slice 6.
- One decision record, **0040**: *a departure carries a cause on the wire*.
- `Design/Stations.md` §7.4 gains its amendment note; §16 marks slice 3 landed and this file moves to
  `Design/Archive/`, both in the merge commit.

---

## 5. Assumptions the implementer may make

- **Nothing sends a dock order over a real wire.** `IssueDockOrder` is exercised by tests and by
  `Publisher::ApplyOrders`; the client's tap is slice 6.
- **A docked ship never comes back.** Undocking is the management menu's, next phase. Nothing reads
  the ledger but tests, and it is designed so the menu can spawn from it.
- **One tick of `Idle` between legs is fine**, as it is for patrols. No arrival logic goes in the
  pass.
- **The ledger row is hull and faction**, because that is the whole of what a ship is today. When
  undocking spawns from it the handle is new, so control groups will have pruned the docked member
  and will not reclaim it — the honest consequence of handles naming lives, and what groups already
  do for any despawn.
- **A protector docking home is not a ledger row.** That distinction belongs to slice 4 and this
  slice does not anticipate it: the capture step appends unconditionally, and slice 4 is what adds
  the branch. Stated here so the reviewer of slice 4 knows where it lands.
