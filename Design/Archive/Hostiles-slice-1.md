# Work order — Hostiles slice 1: allegiance and the wire

Implements slice 1 of [`Hostiles.md`](Hostiles.md) §14: `FactionId` on the ship and on the record,
the command-authority gate in `World::IssueMoveOrder`, and the *destroyed* list that lets the
interest update state a death rather than leave the client to infer one (design §4).

**Layer:** `GameLogic` and `GameLogicTests`, plus the adapter lines in `Outpost/WorldSimulation.h`
that pass the subscriber's faction and the destroyed list through.
**Depends on:** nothing.
**Blocks:** slice 2 (the patrol needs the field to exist) and slice 3 (the overview needs it on the
wire).

---

## 1. Why this is a slice

Nothing in this slice is visible. Every ship stays the player's, every blip stays green, and the
game plays exactly as before — that is the acceptance, not a limitation. What lands is the three
things that are cheap while there is one faction and a rewrite once there are two: whose a ship
is, who may steer it, and whether a ship that vanished died. Slice 2 builds a patrol on the first,
slice 3 builds the overview on all three, and neither can be reviewed against a wire that has not
yet decided what it carries.

---

## 2. Scope

### 2.1 `GameLogic/ShipState.h` — the field

```cpp
// Beside ShipId and ShipHandle.
using FactionId = std::uint8_t;
inline constexpr FactionId FACTION_PLAYER = 0;
inline constexpr FactionId FACTION_HOSTILE = 1;
```

`ShipState` gains `FactionId factionId = FACTION_PLAYER;` after `hullId`. The comment on it says
what design §4.1 says in one sentence: identity, stated by the server; a relation is the
client's to derive. It is not an NPC flag and must not become one.

### 2.2 `GameLogic/World.h/.cpp` — spawn, the gate, the despawn log

```cpp
ShipId SpawnShip(const WorldPos& _posWorld, float _headingRad, std::uint32_t _hullId, FactionId _factionId = FACTION_PLAYER);

float IssueMoveOrder(std::span<const ShipId> _ships, const WorldPos& _point, bool _hasFacing, float _facingRad,
                     FactionId _issuerFaction = FACTION_PLAYER);
```

- **The gate.** `IssueMoveOrder`'s existing `chosen` filter (today `id < m_ships.size()`) also
  drops a ship whose `factionId != _issuerFaction`. Dropped, not rejected: the rest of the list
  is steered, the return value is the heading the surviving ships were solved onto, and an order
  that loses every ship returns `_facingRad` unchanged exactly as an empty list does today. The
  comment on the parameter records why the check is here and not in the adapter (design §4.3):
  the adapter has no test suite and every future host would have to remember it.
- **The defaults are meaning, not convenience** — every existing caller spawns and orders the
  player's own ships, and the default says so (design §11). No existing call site changes.
- **The despawn log.** `DespawnShip` appends the handle it was given to
  `std::vector<ShipHandle> m_despawnLog` before it retires the slot. Two accessors:

  ```cpp
  // Handles despawned since the last ClearDespawnLog, in despawn order. The publisher drains it
  // once per interest update, so a subscriber hears about every death in the interval and not only
  // the ones on the tick the update happened to fall on.
  [[nodiscard]] std::span<const ShipHandle> DespawnLog() const noexcept;
  void ClearDespawnLog() noexcept;
  ```

  The log is not cleared by `Step`. It is the publisher's, and there is one publisher today; the
  day there are several the log becomes per-subscriber and this comment says so.

### 2.3 `GameLogic/WorldSnapshot.h/.cpp` — the record and the destroyed list

- `ShipSnapshot` gains `FactionId factionId = FACTION_PLAYER;`. On the wire it is one `U8` written
  **after `order` and before `hullId`**; `SHIP_RECORD_BYTES` becomes `8 + 24 + 24 + 20 + 1 + 1 + 4`
  = 82 and its layout comment gains the word. `ShipsPerSnapshotFragment()` follows without being
  touched. Both `Write` and `WriteInterest` go through the same record writer, so one change
  covers both.
- The snapshot header gains a `destroyedCount` after `leaveCount` (`SNAPSHOT_HEADER_BYTES` + 4),
  and the destroyed handles ride in the first fragment directly after the leaves, `HANDLE_BYTES`
  each, counted against the first fragment's room exactly as leaves already are. A full `Write`
  writes zero for both counts, as it writes zero leaves today.
- `WriteInterest` gains a parameter:

  ```cpp
  std::uint32_t WriteInterest(const World& _world, std::span<const ShipHandle> _sent, std::span<const ShipHandle> _left,
                              std::span<const ShipHandle> _destroyed, Neuron::Transport& _transport);
  ```

  A handle belongs in one list, never both; the caller decides which (2.4), the writer does not
  check.
- `SnapshotReceiver` removes a destroyed handle exactly as it removes a leave — same upsert-then-
  remove `Apply`, same "an incomplete update is dropped whole" rule — and additionally keeps it:

  ```cpp
  // The handles the last applied update said were destroyed, as distinct from those that merely
  // left this subscriber's view. Valid until the next update applies; empty for a full snapshot.
  [[nodiscard]] std::span<const ShipHandle> Destroyed() const noexcept;
  ```

  `m_pendingDestroyed` sits beside `m_pendingLeaves` and is abandoned with it.
- The header comment's "reviewable list" of what a record carries gains `factionId`, and its list
  of deliberate absences gains the two from design §4.2: no NPC flag, and no speed cap (slice 2
  adds the cap; this slice writes the sentence so slice 2 does not have to remember to).

### 2.4 `Outpost/WorldSimulation.h` — the adapter's two lines, and the split

- `FactionId m_subscriberFaction = FACTION_PLAYER;` with the same "the day a real player connects,
  this comes from the session" comment `SubscriberCentre` carries. `ApplyIncomingOrders` passes it
  as `_issuerFaction`.
- `PublishInterest` splits `m_interest.Left()` into two sends: a handle that is also in
  `m_world.DespawnLog()` goes in `_destroyed`, the rest in `_left`; then `ClearDespawnLog()`.
  `Left()` is a sorted vector (ADR 0010) and the log is a handful of handles, so a linear walk of
  the log with a binary search into `Left()` is enough; no new allocation per publish beyond two
  scratch vectors sized once. `InterestTests::ADespawnedShipLeavesTheSet` is the guarantee this
  split relies on — a despawned ship the subscriber held always appears in `Left()`.
- The early return "nothing changed and nothing came due" stays as it is: a destroyed handle is
  a leave first, so an update carrying one is never empty.

`SubscriberCentre` is **not** filtered by faction in this slice. Its premise ("every ship is the
subscriber's") is still true until slice 3 spawns a hostile, and slice 3 owns that change.

### 2.5 What this slice deliberately does **not** do

- No hostile is spawned anywhere, in the game or in an existing test. `FACTION_HOSTILE` is used
  by the new tests only.
- `WorldView::ExplodeTheLost` still detonates every leave. With no hostile in the world a leave is
  still only ever F4's despawn, so the inference holds for one more slice; slice 3 switches it to
  `Destroyed()` in the same change that makes the inference wrong.
- No selection filter, no HUD change, no content, no patrol, no speed cap.
- No renaming of `Left()`/leaves. A leave still means "no longer in your view"; destroyed is the
  new word, not a replacement.

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| `GameLogic/ShipState.h` | `ShipId`, `ShipHandle`, `OrderState` — the place for `FactionId` and its two constants |
| `GameLogic/World.cpp` `IssueMoveOrder` | the `chosen` filter the gate extends; `DespawnShip` the log appends from |
| `GameLogic/WorldSnapshot.cpp` | `ByteWriter`/`ByteReader`, `SHIP_RECORD_BYTES`, the first-fragment leave accounting the destroyed list copies |
| `GameLogic/InterestSet.h` | `Left()` — sorted, and already containing despawned subscribed ships |
| `Outpost/WorldSimulation.h` | `PublishInterest` and `ApplyIncomingOrders`, the only two call sites the adapter changes |
| `Tests/GameLogicTests/SnapshotTests.cpp` | `CaptureTransport`, `OneShipRoundTripsFieldForField` — the round-trip pattern |
| `Tests/GameLogicTests/InterestTests.cpp` | `AnUpdateUpsertsAndALeaveRemoves` — how a `WriteInterest` update is driven through a loopback and asserted on |
| `Design/Hostiles.md` §4, §8, §9, §11 | the field, the gate, the destroyed list, and why each is shaped as it is |
| ADR 0005, 0008, 0009, 0010 | handles not ids; the wire lives here; the record is a reviewable list; interest sets are sorted vectors |

---

## 4. Acceptance

Tests in `Tests/GameLogicTests/`, sentence-named, a why-comment each:

**`SnapshotTests.cpp`**

- `OneShipRoundTripsFieldForField` — extended: the spawned faction is `FACTION_HOSTILE` and the
  decoded record says so. (Extend, do not duplicate; the test's job is "every field".)
- **`FactionSurvivesTheWire`** — one player and one hostile ship; after `Write` *and* after
  `WriteInterest`, each decoded record carries the faction it was spawned with.
- **`ADeathAndADepartureDifferOnTheWire`** — two ships the subscriber holds. Despawn one; move
  the other outside `INTEREST_RADIUS_METRES`; drive one interest update through the split in 2.4
  (a test-local copy of the same rule, since `WorldSimulation` is in the executable). The receiver
  no longer holds either; `Destroyed()` contains exactly the despawned handle; the departed one is
  absent from it.
- **`TheDespawnLogHoldsUntilDrained`** (`WorldTests.cpp` if it reads better there) — three
  despawns across three `Step`s appear in `DespawnLog()` in order; `ClearDespawnLog()` empties it;
  a `Step` does not.

**`OrderTests.cpp`**

- **`AnOrderFromTheWrongFactionSteersNothing`** — a hostile ship ordered with the default issuer
  keeps `order == Idle` and an unchanged `steerTargetPos`; a mixed list of two player ships and one
  hostile steers the two and not the one; the same hostile ordered with
  `_issuerFaction = FACTION_HOSTILE` moves.

**The existing suites**

- Every existing `GameLogicTests` test passes unchanged except for the one extension named above.
  `MovementTests::TheSameOrderProducesTheSameRun` in particular: a default-faction world ticks
  bit-identically to today.
- `NeuronCoreTests`, `NeuronServerTests`, `NeuronClientTests` untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` (clang-format 18.1.3) pass.
- Debug|x64 builds; the game runs as before — three green ships, F4 on a selected ship still
  explodes it (the F4 path is unchanged, and this is the check that the destroyed list did not
  break the leave it rides beside).
- No screenshot: nothing visual.
- Two decision records, in the same pull request, numbered next after 0012 and added to the
  index: **allegiance is identity on the wire, not a relation** (design §4.1, §13 decision 2) and
  **command authority is gated in the simulation, not the adapter** (design §4.3). Both turn down
  alternatives someone will propose again.
- `Design/Hostiles.md` §14 marks slice 1 `landed`; this file moves to `Design/Archive/`.

---

## 5. Assumptions the implementer may make

- **Both ends are one binary.** The record and header grow with no version byte, as the design
  says; the format comment already claims the field-by-field layout is what lets it grow.
- **A destroyed handle was always subscribed.** The split in 2.4 only ever sends a destroyed
  handle that `Left()` also lists; a despawn the subscriber never held is dropped from the log
  silently. That is correct: you cannot be told of the death of something you never knew about.
- **The log is bounded by ship count per interval** and clears every `INTEREST_UPDATE_EVERY_TICKS`
  ticks; no cap is needed and none is added.
- **`_issuerFaction` is a faction, not a player.** Ownership finer than faction arrives with the
  second subscriber (design §12) and is not stubbed here.
