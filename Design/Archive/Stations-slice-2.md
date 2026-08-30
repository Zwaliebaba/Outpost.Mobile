# Work order — Stations slice 2: who is who

Implements slice 2 of [`Stations.md`](../Stations.md) §16: the third faction and its name, standing as
simulation state, the station side table, and the two fields the wire needs so a client can tell a
station from a hull and knows who holds it hostile (design §4, §6).

**Layer:** `GameLogic` and `GameLogicTests`, plus the rename's two `Outpost` call sites.
**Depends on:** nothing. Slice 1 is independent of it and either may land first.
**Blocks:** slices 3, 4 and 5.

---

## 1. Why this is a slice

It is the only slice that adds nothing anybody can see and everything the rest of the phase stands
on: a relation between factions, a row that makes a `Structure` a station, and the two bytes that
let a client tell the truth about both. Landing it alone is what makes its central claim checkable
— **a world with no stations and no standings mutation ticks bit-identically to today** — because a
pull request that also added a pass could not prove it.

The rename rides here rather than in a slice of its own because it is the same claim from the other
end. `Standing::Hostile` is a *relation*; a faction named `FACTION_HOSTILE` beside it makes
`StandingOf(FACTION_HOSTILE, …) == Standing::Hostile` a sentence that has to be read twice. Identity
constants name identities and standing values name relations, which is ADR 0013's split spelled into
the identifiers (design §4.1).

---

## 2. Scope

### 2.1 `GameLogic/ShipState.h` — the factions and the relation

`FACTION_HOSTILE` becomes `FACTION_VANDAL`, same value 1, every caller in the same commit. The
`FACTION_VANGUARD` comment, which today says the rename "is Stations' to make", becomes a statement
of what is rather than a note about what is not yet.

```cpp
enum class Standing : std::uint8_t
{
  Neutral,
  Hostile
};

// The mask in 4.3 is a u8, so eight is the ceiling; widen both together.
inline constexpr std::uint32_t FACTION_LIMIT = 8;
```

`DEFAULT_STANDINGS` is a `constexpr` table beside them, **built by a `constexpr` function rather
than written out as sixty-four literals**. Everyone is `Neutral` to everyone, except that the Vandal
Collective holds every other faction `Hostile` and is held `Hostile` by every other faction — the
Vandals were never neutral, the tree just had no word for it. Written as a loop, that rule is one
readable sentence; written as a grid, nobody can see it.

The Vandals are not hostile to themselves: "every other faction" excludes the diagonal.

### 2.2 `GameLogic/World.h` / `.cpp` — the table and the judgment

```cpp
[[nodiscard]] Standing StandingOf(FactionId _owner, FactionId _other) const noexcept;
[[nodiscard]] std::uint8_t HostileMaskFor(FactionId _viewer) const noexcept;
void RecordAggression(ShipHandle _attacker, StationId _station);
```

Directional — *the owner's opinion of the other* — because "CVC despises you" and "you despise CVC"
are different facts and the second is none of the simulation's business. A fixed-size array indexed
by two integers: no map, no hashing, no iteration on any path.

**An id at or past `FACTION_LIMIT` reads back `Hostile`.** A faction nobody authored is a stranger,
every caller of this is a gate or a warning colour, and a stranger admitted as a friend is the one
mistake this table must not make — the same direction `WorldView::LiveryOf` already takes for the
same reason.

`RecordAggression` in this slice is its standing half only: resolve the attacker for its faction,
resolve the station for its owner, set `StandingOf(owner, attackerFaction) = Hostile`. Permanently:
no decay, no forgiveness (design §15, decision 3). The target list and the scramble are slice 4's,
and the function grows there rather than being replaced.

There is no client message for this and there never will be. Aggression is a server-side judgment
about observed acts (design §8.1).

### 2.3 `GameLogic/World.h` / `.cpp` — the station side table

```cpp
struct DockedShip { std::uint32_t hullId = 0; FactionId factionId = FACTION_PLAYER; };

struct StationDesc          // all content, passed in by whoever makes the station
{
  FactionId ownerFaction = FACTION_VANGUARD;
  std::uint32_t protectorHullId = 0;
  std::uint32_t protectorComplement = 0;   // 0: this station never launches anything
  std::uint32_t launchEveryTicks = 90;
  std::uint32_t targetCap = 4;
};

using StationId = std::uint32_t;
inline constexpr StationId INVALID_STATION_ID = 0xFFFFFFFFu;

StationId MakeStation(ShipId _structure, const StationDesc& _desc);
[[nodiscard]] const Station& StationOf(StationId _id) const noexcept;
[[nodiscard]] StationId StationAt(ShipId _id) const noexcept;   // INVALID_STATION_ID if not one
[[nodiscard]] bool IsStation(ShipId _id) const noexcept;
[[nodiscard]] std::uint32_t StationCount() const noexcept;
```

The station **is** its structure ship; the row is what knows it admits ships. Deliberately not a
hull property — a `Structure` that is scenery and a `Structure` that is a station must both be
expressible — and deliberately not a new entity kind, which would fork snapshots, interest, picking
and the explosion for a thing that is 95 % a ship (design §6.1).

`Station::structure` is a `ShipHandle` and **every read of it goes through `Resolve`**, so a row
whose ship is gone reports inactive instead of dangling. Nothing can destroy anything this phase,
but the table the user-station design inherits already tolerates death.

**The garrison numbers land here and stay unused until slice 4.** They are content the composition
root passes, and slice 5 — which registers the stations — depends on this slice and not on slice 4,
so a `StationDesc` without them would make slice 5 unable to say what it means. What slice 4 adds is
the *state* those numbers drive: `launchedCount`, `launchCooldownTicks`, `targets`.

`docked` is declared and stays empty; slice 3 is what appends to it.

### 2.4 `GameLogic/WorldSnapshot.h` / `.cpp` — the flags byte

```cpp
std::uint8_t flags = 0; // bit 0: this record is a station that admits ships
```

Written as one `U8` immediately after `factionId` and read in the same position;
`SHIP_RECORD_BYTES` goes 82 → 83. On the reviewable list (ADR 0009) it is identity — what the thing
*is* — beside `factionId` and `hullId`. What stays off the wire beside it: the ledger, the garrison
numbers and the target list, all of it intent or private state of the kind the snapshot exists to
withhold (design §6.2).

A client tapping a structure has to know it is tapping a station before an order is worth sending,
and "immovable hull of faction 2" is inference of exactly the kind §4.3 bans.

### 2.5 `GameLogic/WorldSnapshot.h` / `.cpp` — the standing byte

```
u8 hostileMask   // bit f set: faction f currently holds YOUR faction hostile
```

Appended after `recordCount`, so the existing field order is untouched; `SNAPSHOT_HEADER_BYTES`
goes 26 → 27. On **every** update rather than on change: updates are datagrams, and a lost "you are
now criminal" would leave a client believing itself honest for the rest of the match. One byte per
update is the cheapest idempotence there is.

Three things follow that the design does not spell out, and each is a decision this work order
takes:

- **Both writers emit it.** `Write` and `WriteInterest` both stamp `KIND_SNAPSHOT` and
  `SnapshotReceiver::Accept` parses one header shape for both. A byte in one and not the other
  desynchronises the reader on the first full snapshot.
- **Neither writer knows whose view it is writing**, so both gain a trailing
  `FactionId _viewer = FACTION_PLAYER`. The default keeps every existing caller and test compiling
  unchanged; `Publisher::PublishOne` passes `_subscriber.faction`, which is the only place that
  knows it.
- **The receiver takes the mask per fragment, not on apply** — deliberately unlike the upserts,
  which are held until an update is whole. An incomplete update is dropped because a half-applied
  set of records is a half-updated world; a mask has no such coupling, and taking it from any
  fragment that arrives is strictly more robust, which is the entire argument for the byte. It is
  taken after the header validates and after the stale-tick check, so a late fragment of an old
  update cannot walk the mask backwards.

`ShipsPerSnapshotFragment` follows `SNAPSHOT_HEADER_BYTES` as it always has. State the arithmetic
in the pull request, because a reviewer will expect the number to move and it does not:
`(1152 − 26) / 82` and `(1152 − 27) / 83` both floor to **13**.

### 2.6 `GameLogic/Publisher.cpp` — one argument

`PublishOne` passes `_subscriber.faction` to `WriteInterest`. Nothing else about the publisher
changes; it is not a second authority check, it is the mask's only source of "whose".

### 2.7 `Outpost/OutpostApp.cpp` — the rename

Two sites, both in `SpawnHostileBase`. Mechanical. `HOSTILE_BASE_*`, `HOSTILE_PATROL_*` and the
function name keep their spellings: they name the scene the base is, and renaming the scene is
slice 5's business, not this slice's.

---

## 3. What to build on

- **`IssueMoveOrder`** (`World.cpp:596`) is the shape of a simulation-side gate: the check is in the
  simulation because the adapter has no test suite and every future host would otherwise have to
  remember it (ADR 0014). `StandingOf` is what slice 3's gate will read.
- **The patrol table** (`World::m_patrols`) is the precedent for a parallel side table: declared
  beside `m_ships`, appended in `SpawnShip`, swap-and-popped in `DespawnShip`. The station table is
  *not* that shape — it is indexed by `StationId`, not parallel to ships — and the difference is
  worth seeing: a patrol belongs to a ship, a station is a thing a ship happens to be.
- **`WriteShipRecord`** (`WorldSnapshot.cpp:200`) is where the flags byte is written, and
  `SnapshotReceiver::Accept`'s record loop is where it is read. The two orders must agree.
- **`Publisher::Subscriber::faction`** (`Publisher.h`) already carries whose view an update is.

---

## 4. Acceptance

**`Tests/GameLogicTests/StationTests.cpp`** — new file, in both project files under the `Tests`
filter.

- **`TheStandingTableStartsAsAuthored`** — the Vandal Collective hostile both ways with every other
  faction and neutral to itself; the Vanguard neutral to the player and the player neutral to the
  Vanguard; a faction id past `FACTION_LIMIT` reads `Hostile` from both sides.
- **`AStationIsItsRow`** — `MakeStation` on a live structure resolves and returns its owner and
  garrison verbatim; `StationAt` finds it by ship id and returns `INVALID_STATION_ID` for a plain
  ship; a row whose structure has been despawned reports inactive rather than resolving to whichever
  ship swap-and-pop moved into its index; the Vandal base registers with complement 0.
- **`AggressionIsImperialAndPermanent`** — one aggression against one station flips the standing for
  the whole owning faction, so a *second* station of the same owner holds the attacker hostile too;
  a thousand ticks change nothing. (Slice 3 extends this test to the dock refusal it exists for.)
- **`AnAggressionNamesTheAttackersFaction`** — the flip is keyed on the attacker's faction and not
  on the attacker, so a second ship of that faction is criminal too; a stale attacker handle is a
  no-op rather than a flip against faction 0.

**`Tests/GameLogicTests/SnapshotTests.cpp`**

- **`TheStationFlagSurvivesTheWire`** — a station's record decodes with bit 0 set and a plain ship's
  does not, over both `Write` and `WriteInterest`.
- **`StandingSurvivesTheWire`** — the mask arrives with the Vandal bit set at boot for a player
  subscriber, and gains the Vanguard bit after `RecordAggression`; a Vandal-faction subscriber sees
  the player's bit set instead, which is the byte being *directional* rather than a copy of one row.
- **`TheFragmentSizeIsDerivedFromTheDatagram`** — the existing test, updated to the new record and
  header sizes. It is the test that would have caught a header written in one place and read in
  another.

**The existing suites**

- Every existing `GameLogicTests` test passes with **no assertion changed and no scene changed**.
  Three test files are edited, for the rename and nothing else. That is the design §10 claim — a
  world with no stations and no standings mutation ticks bit-identically — and the pull request
  should say it in those words rather than the looser "the suite passes unchanged", which the rename
  makes untrue.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; the game plays exactly as before. Nothing calls `MakeStation` or
  `RecordAggression`, the flags byte is zero on every record, and the mask is the boot mask.
- No screenshot: nothing visual until slice 5.
- Two decision records, **0038** and **0039**, in the index:
  - *Stations are ships with a side table* (design §6.1, §13);
  - *Standings are simulation state, stated per subscriber* (design §4.2, §4.3, §13).
- `Design/Stations.md` §16 marks slice 2 landed and this file moves to `Design/Archive/` — both in
  the merge commit, per Design/README.md.

---

## 5. Assumptions the implementer may make

- **Nothing spawns a station.** The table is empty in every existing test and in the running game
  until slice 5. `MakeStation` is exercised by tests only, which is the slice boundary and not an
  oversight.
- **Stations do not despawn.** Nothing can destroy anything, and a Vanguard station is
  indestructible as a rule (design §8.5). The table therefore has no removal path in this slice —
  but every read of `structure` still goes through `Resolve`, so adding one later is adding a
  function and not repairing the readers.
- **`StationAt` is a linear scan** of a vector with single digits of rows. It is called per record
  written, which is 13 records a fragment against 4 stations. Design §6.1 prices the replacement —
  an index, the day there are hundreds — and this slice deliberately does not build it.
- **One subscriber is one faction.** "Your faction is criminal" and "you are criminal" are the same
  sentence today. The day two players share a faction, this widens to per-player rows exactly as
  ADR 0014's authority gate does, in the same functions (design §4.2, §12).
- **The mask is only as fresh as the last update sent.** `PublishOne` skips a subscriber with
  nothing entered, refreshed or left, so a standing that flips while a subscriber's interest set is
  completely static is not stated until the next update that is. A subscriber's own fleet is always
  in its own interest set, so this cannot happen in the running game; it is stated here rather than
  discovered.
