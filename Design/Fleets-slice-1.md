# Work order — Fleets slice 1: the table

Implements slice 1 of [`Fleets.md`](Fleets.md) §16: the fleet table in `World`, the caps that make
it a rule rather than a convention, the pass that prunes and retires, and the codec coverage that
puts the row in the save format. Nothing composes a fleet from a station and nothing orders one;
those are slices 2 and 3.

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** nothing.
**Blocks:** slice 2 (compose and launch), which is this table's constructor plus a ledger and a
manifest; and through it every other slice.

---

## 1. Why this is a slice

The table is the one part of the feature that every other part needs and that needs none of them. A
slice that also composed from a ledger would be arguing about station gates while it was still
deciding what a fleet *is*, and the two questions have different answers to get wrong: the gates
are a policy, the table is a shape that the codec, the replay gate and four later slices all bind
to.

It is also where the design's first claim gets tested rather than asserted — that a fleet is
simulation state (design §4.1). The evidence for it is small and specific: the row survives a save
and a reload, a member that dies leaves the fleet on the tick it died, and a world with no fleets
in it ticks exactly as the tree ticks today. All three are decidable now, by tests, against nothing
else in flight.

---

## 2. Scope

### 2.1 `GameLogic/ShipState.h` — the two caps

Beside `FACTION_LIMIT`, because all three are per-faction ceilings that a later wire byte leans on:

```cpp
inline constexpr std::uint32_t FLEET_SLOTS = 5;     // live fleets per faction; the HUD's five buttons
inline constexpr std::uint32_t MAX_FLEET_SHIPS = 8; // members per fleet
```

Both are in the replay contract — they decide which `FormFleet` calls are accepted — and each
carries the one-line reason at its definition that `SimTuning.h`'s constants carry. Eight is argued
in design §4.2 against formation span, the separation measurements and the interest radius; the
comment cites the section rather than repeating the table.

### 2.2 `GameLogic/World.h` — the row, and only what this slice reads

Nested in `World` beside `Station`, because a fleet is `World`'s concept the way a station is:

```cpp
using FleetId = std::uint32_t;
static constexpr FleetId INVALID_FLEET_ID = 0xFFFFFFFFu;

struct Fleet
{
  FactionId ownerFaction = FACTION_PLAYER;
  std::uint8_t slot = 0;               // < FLEET_SLOTS, unique among this owner's live fleets
  ShipHandle members[MAX_FLEET_SHIPS];
  std::uint32_t memberCount = 0;
};
```

**The row is declared with the fields this slice reads, and no others.** Design §4.1 spells the
finished row — a manifest, a standing order, a threat and an alert — and each of those arrives with
the slice that reads it: the manifest with slice 2, the order with slice 3, the threat and the
alert with slice 4. That is AGENTS.md §8's checklist read forwards rather than backwards ("added a
field to `World` that `Step` reads? `WriteWorldState` carries it"): a field, its codec lines and
the test that exercises them land together, so no codec line is ever written that no test can
reach. The price is that `WORLD_STATE_FORMAT` moves four times instead of once, which costs one
literal per slice and nothing else — there is no migration and no saved file older than the build
reading it (§2.5).

The API, mirroring the station table's vocabulary so the two read alike:

```cpp
// Makes a fleet of live ships. Returns INVALID_FLEET_ID and changes nothing if any gate refuses.
FleetId FormFleet(FactionId _ownerFaction, std::uint8_t _slot, std::span<const ShipId> _ships);

[[nodiscard]] FleetId FleetInSlot(FactionId _ownerFaction, std::uint8_t _slot) const noexcept;
[[nodiscard]] FleetId FleetAt(ShipId _id) const noexcept;   // the fleet a ship is in
[[nodiscard]] const Fleet& FleetOf(FleetId _id) const noexcept;
[[nodiscard]] std::uint32_t FleetCount() const noexcept;
```

`FormFleet` is **not** `ComposeFleet`. Compose is slice 2's: it takes hull counts out of a station's
ledger, gates on standing, and leaves a manifest for the launch metronome. Form takes ships that are
already flying, which is what the boot scene needs (design §5.5 — the three starting hulls open as
Fleet 1) and what compose is written in terms of once the manifest exists. Splitting them here is
what lets this slice have a constructor at all without owning the station gates.

`m_fleets` is a dense `std::vector<Fleet>`, not parallel to `m_ships` and not repaired by
`DespawnShip`. That is the point of members being `ShipHandle`s: the despawn path already has four
parallel tables to keep in step and gains no fifth, and a dead member simply stops resolving.

### 2.3 `GameLogic/World.cpp` — `FormFleet` and the gates

Every gate refuses the **whole** call and changes nothing, which is `IssueDockOrder`'s
`RefusedStanding` rule rather than `IssueMoveOrder`'s drop-the-stranger rule: a fleet formed from
some of the ships asked for is a fleet the caller did not ask for, and its size is a rule.

1. `_slot >= FLEET_SLOTS`, or `_ownerFaction >= FACTION_LIMIT`.
2. A live fleet of `_ownerFaction` already holds `_slot`.
3. `_ships` is empty, or longer than `MAX_FLEET_SHIPS`.
4. Any id in `_ships` is not a live ship, or is not `_ownerFaction`'s.
5. Any id appears twice in `_ships`, or is already a member of some fleet.

Gate 5 is the fleet-only model (design §15, decision 1) held where it is cheapest to hold: one ship
is in one fleet, checked at the only place that can make a membership. `FleetAt` is a scan of at most
`FACTION_LIMIT × FLEET_SLOTS × MAX_FLEET_SHIPS` handles — 320 at the ceiling — and it runs per ship
in the list, so 2,560 resolutions for a full fleet at a full table. That is a quantity a person
composes, not a per-tick one, which is the whole reason a scan is the right shape here.

Which gate refused is not observable: every refusal is the same `INVALID_FLEET_ID` and the same
untouched table. The order above is the order the code checks in, for the reader's sake.

The row is appended and its index returned, exactly as `MakeStation` does, and refusal is
`INVALID_FLEET_ID` rather than a result enum for the same reason `MakeStation` has none: the callers
of this one — the boot root, slice 2, the tests — do not need to be told which gate refused. Slice
2's `ComposeFleet` does need to (its refusals reach a screen), and that is where the enum belongs.

### 2.4 `GameLogic/World.cpp` — `StepFleets`, the pass skeleton

Declared beside `StepProtectors` and called **last** in the standing-intent slot:

```cpp
StepDockings();
StepPatrols();
StepProtectors();
StepFleets();
```

Last, and that is behavior rather than tidiness: the dock pass despawns and the protector pass
spawns, both before this one, so a member that docked or died anywhere in this tick is pruned on the
tick it left rather than the tick after. Two halves, in this order:

- **Prune.** Per fleet in array order, compact `members` in place, dropping handles that no longer
  resolve — the same in-place idiom `StepProtectors` uses on a station's target list, so the survivors
  keep their relative order. The vacated tail is cleared to null handles, which the route table
  deliberately does not do for its waypoints: a `Route` is written out live-entries-only and its dead
  tail is never compared, while a fleet row is small enough to compare whole and a defined tail is
  what makes a reloaded row equal to the row that was saved.
- **Retire.** A fleet with no members left is removed by swap-and-pop, design §10's word for it,
  walking the table **backwards** so that the row swapped in from the end has already been visited
  and needs no second look. Nothing stores a fleet index across a tick — `(faction, slot)` is the
  stable name — so there is nothing for the swap to break.

Slice 2 widens the retire condition to "no members **and** an empty manifest"; until a manifest
exists, a memberless fleet is a fleet that has lost everything.

`StepFleets` does **not** call `RebuildStaticIfDirty`. The three passes before it do because they
plan routes; this one issues no order and plans nothing. Slice 3 adds the call with the order it
lowers.

### 2.5 `GameLogic/WorldSnapshot.cpp` — the codec

`WORLD_STATE_FORMAT` goes 1 → 2, and the format gains one table after the stations:

```
u32 fleetCount
per fleet: u8 ownerFaction, u8 slot, u32 memberCount, memberCount × Handle
```

Only the live members are written, for the reason `Route` gives about its waypoints. `ReadWorldState`
validates before it sizes anything, in the "fails closed" shape the rest of the function keeps —
locals first, `_outWorld` only once the whole buffer has been read:

- `fleetCount > FACTION_LIMIT * FLEET_SLOTS` — an exact bound, not a heuristic one: that product is
  every slot of every faction and no world can legitimately hold more.
- `ownerFaction >= FACTION_LIMIT`, `slot >= FLEET_SLOTS`, `memberCount > MAX_FLEET_SHIPS`.
- Two live fleets claiming one `(faction, slot)` — checked with a `bool[FACTION_LIMIT][FLEET_SLOTS]`
  on the stack, because `FleetInSlot` answers with the first row it finds and a file that made that
  answer ambiguous would corrupt an invariant rather than a value.

What is deliberately **not** validated is the member handles themselves. `Resolve` already
bounds-checks a slot and compares a generation, so a handle this file invented resolves to nothing
and the first `StepFleets` prunes it — the fail-closed direction already, where a second check here
would turn a world that repairs itself into a load that refuses.

Nothing is rebuilt on load: unlike `m_shipSlot` and `m_entityRows`, the fleet table is not an inverse
of anything, and unlike a route it carries no epoch counter that means something only inside the run
that wrote it.

### 2.6 What this slice does not touch

- **`AGENTS.md` and `README.md`.** Nothing they say becomes false: no `Outpost` file changes, the
  five HUD buttons are still control groups until slice 6, and the fleet table is empty in every
  running build because nothing calls `FormFleet` outside the tests.
- **`ShipState`.** No membership field on the ship. A ship does not know its fleet; the fleet knows
  its ships, and `FleetAt(ShipId)` is the scan that answers the other direction — `StationAt`'s
  shape, for `StationAt`'s reason (ADR 0005: a row holding a raw id names whoever swap-and-pop moved
  into it).
- **The wire.** No record field, no header byte, no message. The roster and the status block are
  slice 5's, and nothing in this slice is visible to a client.
- **`HullSpec`.** `combatant` is slice 4's.

---

## 3. What to build on

- **`World::Station` / `MakeStation` / `StationAt` / `StationOf`** (`GameLogic/World.h`,
  `World.cpp`) — the side-table pattern this one copies: a nested row struct, a dense vector, a
  bare-index id, refusal by returning the invalid id, and every stored reference a `ShipHandle`
  resolved on read (ADR 0038, ADR 0005).
- **`World::StepProtectors`** (`World.cpp`) — the in-place prune of `home.targets` is the exact
  idiom §2.4's prune uses, down to the `live` cursor.
- **`World::PatrolOf`** — the `static constexpr NONE` guard for an out-of-range accessor, which
  `FleetOf` takes rather than `StationOf`'s unguarded index.
- **`WriteWorldState` / `ReadWorldState`** (`WorldSnapshot.cpp`) — the station block is the closest
  precedent for the new one, including the `Remaining()` bound before a `resize` and the
  read-into-locals-then-commit shape.
- **`Tests/GameLogicTests/StationTests.cpp`** for the table-test shape and
  **`WorldStateTests.cpp`** for the round-trip and byte-comparison shape.

---

## 4. Acceptance

**`Tests/GameLogicTests/FleetTests.cpp`** — new file, registered in `GameLogicTests.vcxproj` and its
`.filters` under the `Tests` filter.

| Test | Decides |
|---|---|
| `AFleetIsFormedFromLiveShips` | the row holds the ships handed in, in order; `FleetInSlot`, `FleetAt` and `FleetOf` all agree about it |
| `TheSixthFleetIsRefused` | five slots held → any further `FormFleet` refuses; an occupied slot refuses; a retire frees the slot and the next form succeeds |
| `TheNinthShipIsRefused` | nine ships refuse, zero ships refuse, eight succeed — and a refusal leaves `FleetCount` where it was |
| `AFleetTakesOnlyItsOwnersShips` | one foreign ship refuses the whole call, and none of the others is claimed |
| `AShipBelongsToOneFleet` | a ship already in a fleet cannot join a second; a list naming one ship twice refuses |
| `ALossPrunesTheFleet` | a despawned member is gone from the row on the next `Step`, and the survivors keep their handles and their order |
| `MembersSurviveSwapAndPop` | despawning a **non-member** moves a member's `ShipId`; the fleet still names the same ships (ADR 0005's property, at fleet grain) |
| `TheLastLossRetiresTheFleet` | the last member gone → the row is removed, `FleetInSlot` reports none, and a second fleet in the table keeps its identity across the retirement |
| `TheFleetTableSurvivesTheRoundTrip` | two fleets, one of them pruned, written and read back: owners, slots, members and counts all equal, and a truncated buffer refuses without touching the world |
| `AFleetlessWorldTicksAsToday` | a scene with ships, a station, patrols and a docking in flight holds `FleetCount() == 0` for 240 ticks, and its state bytes equal a second identical world's |
| `TheSameFleetProducesTheSameRun` | the same forms and the same losses in two worlds, compared state-for-state every tick: the replay gate over the row |

**The existing suites**

- Every existing `GameLogicTests` test passes **without edits**. Nothing outside the tests calls
  `FormFleet`, so every scene in the suite has an empty fleet table and a pass that visits nothing —
  which is what "ticks as today" actually means, and the pull request states that the suite was run
  unchanged rather than adjusted.
- `WorldStateTests::ASavedWorldReplaysToTheSameRun` is untouched and still green: its scene forms no
  fleet, so the new table is written as a zero count and the byte comparison is unchanged.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds and all four suites run; the game runs exactly as before, because the composition
  root does not call `FormFleet` until slice 6.
- No screenshot: nothing visual until slice 6.
- One decision record: **fleets are simulation state at fleet grain**, and the five slots are the
  player's whole command surface — why the client-side control group could not grow into this, and
  what a per-ship membership field would have cost. Next free number is **0048**; the index in
  `Design/Decisions/README.md` lists it.
- [`Fleets.md`](Fleets.md) §16 marks slice 1 *in review* until the pull request merges, and this file
  moves to `Design/Archive/` in the merge commit, per `Design/README.md`.

---

## 5. Assumptions the implementer may make

- **Nothing outside the tests forms a fleet.** `FormFleet` is dead code in the running game until
  slice 6, and that is the slice boundary rather than an oversight — it is what makes "every existing
  test passes unchanged" a claim worth making.
- **A fleet may hold any hull.** `FormFleet` does not refuse an immovable one, exactly as
  `MakeStation` does not check that its structure is a `Structure`. The only content path that will
  exist is slice 2's ledger, which cannot hold a station, and a rule nothing can violate is a rule
  nobody remembers to remove.
- **A fleet is not an interest or a wire concept.** It is invisible to `Publisher`,
  `SnapshotWriter` and every client; nothing in this slice changes a byte of an update.
- **NPC factions may form fleets.** The table is faction-generic (design §5.4) and the gates are
  written against `FactionId`, not against `FACTION_PLAYER`; nothing composes one and no NPC pass
  reads the table.
- **`FleetId` is a bare index with no generation**, exactly as `StationId` is, because nothing holds
  one across a tick — `(faction, slot)` is the name that survives, and the day something needs to
  hold an index it gains a slot table the way ships have one.
- **The order of `members` is the order they were handed in**, and prune preserves it. Nothing reads
  the order yet; slice 3's formation solve does, and it is stated here so that it is a property
  rather than an accident.
