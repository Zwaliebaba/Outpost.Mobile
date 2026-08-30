# Stations — Core Vanguard Command, docking, and the protector response

**Status: in progress. Slices 1-4 -- the whole `GameLogic` half -- merged on 2026-08-30 (pull
request #22) and their work orders are in `Archive/`. Slice 5, the Vanguard scene, is written and
in review; slice 6 is next.** Four decisions were
put to the owner on 2026-08-30 and taken (§15); each was the recommended option. §16 lists the
slices and the dependencies between them, and
[`Stations-slice-plan.md`](Stations-slice-plan.md) grounds that list against the tree.

This document designs the game's first *civic* presence: stations owned by **Core Vanguard
Command** — CVC, or the **Vanguard** — the government entity of known space. A station is a place
a ship can dock and be safe. This phase builds the system-owned kind: invulnerable, one at every
planet of the starting solar system, neutral to everyone who has not attacked one, and defended by
protector ships that launch from the station and hunt an attacker until it is dead. Docking is
select-ships-and-tap-a-station; a docked ship leaves the screen and stays docked — undocking
arrives with the station management menu, which is the next phase and deliberately not this one
(§14). User-owned stations are out of scope by the owner's brief; what this design does for them
is leave the right doors open (§6.1, §8.5).

The feature forces four things into existence, and each has an MMO-shaped answer and a cheaper
one, the same fork Hostiles faced: **a third faction and what factions think of each other**
(standings, §4), **a universe with places in it** (the solar-system layout, §5), **a ship that
leaves the world without dying** (docking on the wire, §7), and **the first NPC behavior that
reacts** (the protector response, §8 — the "different design with senses and thresholds" that
Design/Archive/Hostiles.md 5.5 explicitly deferred to). As with Hostiles, this design takes the
MMO-shaped answers while they are still one table, one byte and one pass.

There is still no combat. That is the load-bearing scope decision (§15, owner decision 2): nothing
in this phase can deal damage, so nothing in this phase can *actually* attack a station. What
lands is the whole framework around that absent act — the aggression judgment, the permanent
standing it flips, the dock refusal, the scramble, the pursuit — driven by tests and by one debug
key, so that the day the combat design lands it connects to a trigger rather than to a rewrite.

---

## 1. What is being built

- **A third faction**: `FACTION_VANGUARD`, and with it the first notion of **standing** — what one
  faction is to another. Standing is simulation state, faction-granular, and the server states each
  subscriber's standing on the wire so the client's affordances can tell the truth (§4).
- **A solar-system layout**: a pure seeded function in `GameLogic` that lays out the starting
  system — a star anchor, a few planets at real positions on the plane — consumed by both halves:
  the server half spawns a Vanguard station at every planet, the client half marks them (§5).
  Static content, so it can be marked without a live record, which is the owner's "static so can
  be marked".
- **Stations**: a side table in `World` that makes an existing `Structure` ship a station — an
  owner, a docked ledger, a garrison. The station *is* its structure ship; the table is what knows
  it admits ships. The record's wire form gains one flags byte so a client knows a station when it
  sees one (§6). The existing pirate base becomes a station in the same table, owned by the
  pirates — the **Vandal Collective** ("Vandal"), named by the owner with the Vanguard (§4.1) —
  and nothing about it changes for the player (§15, owner decision 4).
- **Docking**: a second order kind on the wire. Select own ships, tap a station: the ships fly to
  it through the existing order machinery, and each one that reaches capture range leaves the
  world into the station's ledger. The wire learns to say *docked* as distinct from *destroyed*
  and *left*, so the client removes the hull without detonating it (§7). Docking is refused — at
  order time and again at capture — where the station's owner holds the issuer hostile.
- **Aggression and the protector response**: `RecordAggression` flips the attacker's faction to
  hostile in the Vanguard's eyes, permanently and empire-wide (§15, owner decision 3), and the
  attacked station scrambles its garrison: protectors spawn from the station, pursue the attacking
  ship wherever it goes, are replaced if they die, and return and dock when their target is dead.
  They will not drop loot when a loot system exists; that rule is recorded now (§8.6).
- **Display**: Vanguard hulls tint azure the way hostiles tint red; the minimap draws station
  records at the structure dot size and *marks* — hollow, catalogue-fed, clamped to the map edge —
  for the stations too far away to be records; `CONTACTS` starts counting what is hostile *to
  you* rather than what is merely not yours; the event log narrates dock orders, refusals and
  completed dockings (§9).

---

## 2. What the tree already guarantees

Constraints, not preferences; a proposal below that broke one would be wrong however well it
docks.

| Constraint | Where it comes from |
|---|---|
| `GameLogic` depends on `NeuronCore` and nothing else; the engine never names the game | AGENTS.md §2 |
| No wall clock, no OS entropy, no iteration order that is not dense-array order; seeded PCG32 only | AGENTS.md §5, ADR 0012 |
| Two identical runs are bit-identical, tick for tick | `GameLogicTests`, the replay gate |
| The client sees the world only through snapshots over a `Transport`; record contents are a reviewable list | `GameLogic/WorldSnapshot.h`, ADR 0009 |
| Stored cross-tick references are `ShipHandle`, never `ShipId` | ADR 0005 |
| The server states identity; each client maps it to a relation | ADR 0013 |
| Command authority is gated in the simulation, not the adapter | ADR 0014 |
| NPC behavior lives in `GameLogic`, inside the tick | ADR 0015 |
| A death is stated on the wire, never inferred from absence | Design/Archive/Hostiles.md 4.4 |
| Presentation state does not live in the simulation; bodies are presentation | AGENTS.md §5, ADR 0016 |
| Tuning splits: contract in `SimTuning.h`/`HullSpec.h`, look-and-feel in `ViewTuning.h`, content passed in by the root | AGENTS.md §5, Design/Archive/Hostiles.md 5.1 |
| The path grid declines to build past 512 cells per axis (16.4 km at 32 m cells) | `SimTuning.h`, Design/Archive/Collision-slice-8.md |

---

## 3. What needs no new design

More of this feature exists than not, which is what keeps the slices small:

- **A station's body is a solved problem.** `HullId::Structure` is immovable, collidable, in the
  static index and the pathfinding obstacle set; the hostile base has stood on one since Hostiles
  slice 3. A Vanguard station is that spawn call with a different faction. No new hull row is
  needed: station-ness is deliberately *not* a hull property (§6.1).
- **Flying to a station is flying.** The approach leg is `PlanRoute` plus the same steering,
  avoidance and separation every order already uses. Docking adds a destination rule and a capture
  test, not a movement system.
- **The order path is two-way and faction-gated.** `MoveOrder` shows the shape: a datagram kind,
  a write/read pair, handle resolution in the adapter, a faction filter in `World`
  (ADR 0014). `DockOrder` is a second instance of that shape.
- **Standing NPC intent has a slot.** `StepPatrols` established where in the tick standing
  behavior issues orders and why it is order-independent there. The dock pass and the protector
  pass are two more passes in that slot (§10).
- **Identity already travels.** `factionId` is in the record; tinting by faction and refusing
  selection by faction landed with Hostiles. A third faction is a constant and two mapping rows,
  not a mechanism.
- **The wire already distinguishes kinds of leaving.** The destroyed list exists precisely so a
  departure can carry a cause (Hostiles §4.4 called itself "the door, opened the width of one
  list"). *Docked* is the second cause through the same door (§7.4).
- **Seeded content generation has a house pattern.** `BodyCatalogue::RandomBody` draws from
  `Pcg32(seed)` in one fixed order so a seed means one body forever. The system layout does the
  same for planets (§5.2).

---

## 4. Factions and standings — the Vanguard

### 4.1 The factions, named

```cpp
// ShipState.h, beside FACTION_PLAYER
inline constexpr FactionId FACTION_VANDAL = 1;   // was FACTION_HOSTILE: renamed, same value (below)
inline constexpr FactionId FACTION_VANGUARD = 2;
```

Core Vanguard Command is the government of known space; "CVC" and "the Vanguard" are the same
entity, and the HUD says **VANGUARD**. Its ships and stations are ordinary records with
`factionId = FACTION_VANGUARD` — no government flag, no station-faction special case anywhere in
the engine, for ADR 0013's reason: the server states whose a thing is, and what that *means* is a
mapping each side owns.

The pirates have a name now too: the **Vandal Collective** — "Vandal", and **VANDAL** where the
HUD needs a word. With the name comes a rename, and it is not cosmetic: this design introduces
`Standing::Hostile` as a *relation* (§4.2), and a faction *named* `FACTION_HOSTILE` beside it is
one word carrying two meanings — `StandingOf(FACTION_HOSTILE, …) == Standing::Hostile` is a
sentence nobody should have to parse twice. Identity constants name identities and standing
values name relations, which is ADR 0013's split spelled into the identifiers. So slice 2 renames
`FACTION_HOSTILE` to `FACTION_VANDAL`, same value, every caller in the same commit. The view's
`HOSTILE_*` constants — `HOSTILE_SHIP_COLOUR`, `HUD_ALERT_RED` and family — deliberately keep
their names: they color the relation (whatever faction the mask flags, §9.3), not the Vandal
Collective, and the day the Vanguard turns on a criminal it is these that paint it.

### 4.2 Standing — what a faction is to a faction

The dock gate needs one judgment: does the station's owner hold the issuer hostile? The protector
response needs the same judgment from the other side. That is a relation between factions, it
changes recorded outcomes, and a spectator would need it — so it is simulation state, in `World`:

```cpp
// ShipState.h
enum class Standing : std::uint8_t
{
  Neutral,
  Hostile
};
inline constexpr std::uint32_t FACTION_LIMIT = 8; // the mask in §4.3 is a u8; widen both together
```

`World` holds a `FACTION_LIMIT × FACTION_LIMIT` table of `Standing`, read as
`StandingOf(_owner, _other)` — *the owner's opinion of the other*, directional, because "CVC
despises you" and "you despise CVC" are different facts and the second is none of the
simulation's business. The table initializes from a `constexpr DEFAULT_STANDINGS` beside the
enum: everyone is `Neutral` to everyone except that the Vandal Collective holds every other
faction `Hostile` and is held `Hostile` by every other faction — the Vandals were never neutral,
the tree just had no word for it. The one mutation this design adds is `RecordAggression` (§8.1),
which sets `StandingOf(stationOwner, attackerFaction) = Hostile`. Permanently: no decay, no
forgiveness, no payment of dues — a standings-repair mechanic is a later design, and the owner
chose permanence over inventing one here (§15, decision 3).

Faction-granular, and that is knowingly coarse in exactly the way ADR 0014's authority gate is:
today one subscriber is one faction, so "your faction is criminal" and "you are criminal" are the
same sentence. The day two players share a faction, aggression widens to per-player standing the
same way command authority widens to per-player ownership — a keyed row and a different lookup,
in the same functions, with the same machinery load-bearing (§12).

### 4.3 The wire — the server states your standing

The client must not *infer* its standing — inference is the client guessing server state, the
exact sin the destroyed list exists to prevent. And there is nothing to infer from anyway: an
order datagram is fire-and-forget, so a refused dock order over a real wire would otherwise be
ships that simply never go, which reads as a broken game.

So every interest update header gains one byte:

```
u8 hostileMask   // bit f set: faction f currently holds YOUR faction hostile
```

Written on every update rather than on change, because updates are datagrams and a lost "you are
now criminal" would leave a client believing itself honest for the rest of the match; one byte per
update is the cheapest idempotence there is. `SnapshotReceiver` exposes the latest mask. The
client uses it three ways (§9): dockability before ordering, the red tint on a faction that has
turned on you, and an honest `CONTACTS`. `FACTION_LIMIT` is 8 because the mask is a u8; the day
factions outgrow a byte, the mask becomes a small standings record and the limit moves with it —
one comment at the definition says so.

What deliberately does **not** travel: the full standings table (no client needs third-party
opinions yet), aggression events (the client sees consequences, not judgments), and anything
letting a client *write* a standing — aggression is a server-side judgment about observed acts,
never a client message (§8.1).

---

## 5. The universe — one system, laid out by a seeded function

### 5.1 What "distributed across the Universe" means in this phase

The owner's brief, as confirmed (§15, decision 1): solar systems exist as a layout, every planet
of a system carries one static Vanguard station, and this phase instantiates **the starting
system only**. `WorldPos` already spans ±10^19 m of sectors, so nothing below assumes one system
is all there is — but only one is spawned, marked and pathfound today.

### 5.2 The layout function lives in GameLogic

```cpp
// GameLogic/UniverseLayout.h
struct PlanetSite
{
  WorldPos posWorld;          // where the planet — and its station — sit on the plane
  float radiusMetres = 0.0f;  // the body's visual radius, drawn by the client, ignored by the server
  float bearingRad = 0.0f;    // from the star, for anything that wants to face or frame it
  std::uint64_t bodySeed = 0; // what the client's BodyCatalogue generates the look from
};

struct SystemLayout
{
  WorldPos starPos;
  std::vector<PlanetSite> planets;
};

struct SystemDesc
{
  std::uint32_t planetCount = 3;
  float minOrbitMetres = 2500.0f;
  float maxOrbitMetres = 6500.0f;
  float minRadiusMetres = 400.0f;   // the client aligns these with its BODY_PLANET_* band
  float maxRadiusMetres = 1200.0f;
  bool pinFirstPlanet = false;      // true: planet 0 takes the two fields below verbatim
  float firstPlanetBearingRad = 0.0f;
  float firstPlanetOrbitMetres = 0.0f;
};

[[nodiscard]] SystemLayout LayOutSystem(std::uint64_t _seed, const WorldPos& _starPos, const SystemDesc& _desc);
```

A pure function of its arguments: one `Pcg32(_seed)`, draws in one fixed order — per planet, its
orbit, its radius, its body seed — so a seed means one system forever, the same rule
`BodyCatalogue::RandomBody` states for a body. Bearings are *not* drawn free: planet `i` sits at
`i · 2π / planetCount` plus a jittered fraction of half a slot, so planets are spread by
construction and no rejection loop is needed for the layout to be both deterministic and
non-overlapping. Positions are built through `Translate` from `_starPos`, never by writing local
fields, so a system laid out near a sector boundary holds `WorldPos`'s invariant.

**Why GameLogic and not the executable**, when `BodyCatalogue` — the obvious precedent — is
client content: the layout is what ADR 0008 calls wire-format-shaped. Both future executables
need it — the server binary to spawn the stations, the client binary to mark them — and content
in one executable is in the wrong place the day there are two. It is static content shipped with
both halves, the way an MMO ships its star map in the client install; identity and motion still
arrive only in snapshots. It also buys the layout a test suite, which the executable layer does
not have: the grid-ceiling bound below becomes a `GameLogicTests` assertion instead of a hope.
`BodyCatalogue` stays where it is — what a planet *looks* like is still nobody's business but the
client's; the layout hands it a seed and a radius and nothing else.

### 5.3 The starting system, and the numbers argued

The composition root calls `LayOutSystem` once at boot with a `SystemDesc` from `ViewTuning.h`'s
starting-scene block (content, like every spawn number): three planets, orbits 2 500–6 500 m,
and `pinFirstPlanet = true` with the bearing and range the framed world already has
(`BODY_START_PLANET_BEARING_DEG` −23°, `BODY_START_PLANET_DISTANCE_METRES` 3 500) — so the
carefully worked camera framing of ViewTuning survives as content instead of being superseded by
a die roll. The star anchor is the universe origin. No sun is drawn and no star body exists; the
star is a layout anchor this phase, stated here so nobody goes looking (§14).

Placement checks, so the numbers are argued rather than liked:

| Quantity | Value | Against |
|---|---|---|
| Farthest station | 6 500 m | interest radius 2 000 m — stations are *not* records at boot; the marks (§9.3) are what the player sees, which is the point of "static so can be marked" |
| Static span, worst case | 13 000 m across + 2 × 512 m grid margin = 14 024 m | 14 024 / 32 = 439 cells per axis, under the 512 ceiling with 14 % headroom — the grid still builds, routes still plan. `TheLayoutRespectsTheGridCeiling` (§11) pins this, because the ceiling fails silent-but-logged and the symptom would be ships steering like it is phase 5 again |
| Closest two stations | ≥ 2 500 m at 120°-slot bearings | Interest and minimap never hold two stations at once from the start position; each is its own destination |
| Travel to the first station | 3 500 m ≈ 117 s at a Bomber's 30 m/s | far enough to be a journey, near enough to demo docking without time compression |

The planet *visuals* follow the layout: `SpawnStartingBodies` places each world at its site's
bearing and range (depth below the plane stays the framing device it is), wearing `Planet1.dds` —
one picture for all three worlds this phase, a stated placeholder until more planet pictures are
content (§14). The six asteroids are untouched. **F5 reseeds looks, never the layout**: body
shapes and the sky reroll as today, but sites — and therefore stations — hold still, because a
debug reroll must not move simulation content. The layout seed is its own constant beside
`BODY_START_SEED`.

### 5.4 What a planet is to the simulation: still nothing

ADR 0016 stands. A planet has no ship, no record, no collision; ships fly "through" the place it
marks (its visual is kilometers below the plane regardless). What changed is only that a planet's
*position* is now shared content feeding two consumers — the station spawn and the body placement
— instead of being a view-only framing choice. The station is the only thing at a planet the
simulation knows.

---

## 6. Stations — the table and the wire

### 6.1 A station is a structure ship, plus a row that says so

```cpp
// World, beside the patrol table
struct Station
{
  ShipHandle structure;                    // the ship that is the station (ADR 0005: its death orphans the row)
  FactionId ownerFaction = FACTION_VANGUARD;
  // The garrison and the response (§8): all content, passed in by whoever makes the station.
  std::uint32_t protectorHullId = 0;
  std::uint32_t protectorComplement = 0;   // 0: this station never launches anything
  std::uint32_t launchEveryTicks = 90;
  std::uint32_t targetCap = 4;             // the most aggressors one station tracks at once
  std::uint32_t launchedCount = 0;         // protectors currently in space
  std::uint32_t launchCooldownTicks = 0;
  std::vector<ShipHandle> targets;         // who this station's protectors are hunting, capped
  std::vector<DockedShip> docked;          // the ledger: who is inside
};

struct DockedShip
{
  std::uint32_t hullId = 0;
  FactionId factionId = FACTION_PLAYER;
};

using StationId = std::uint32_t; // an index into the station table; stations do not despawn this phase
StationId MakeStation(ShipId _structure, const StationDesc& _desc); // owner, garrison numbers

[[nodiscard]] const Station& StationOf(StationId _id) const noexcept; // tests and debug overlays
```

The structure ship keeps doing everything it already does — static index, obstacle set, record on
the wire — and the row is what admits ships. Deliberately **not** a hull property and not a new
hull row: a `Structure` that is scenery and a `Structure` that is a station must both be
expressible, and user-owned stations (later) will be stations on other hulls. Deliberately not a
new entity kind either: a parallel entity array would fork snapshots, interest, picking and the
explosion for a thing that is 95 % a ship. The cost of the side table is a lookup by handle where
the simulation asks "is this a station" — a linear scan of a vector with single digits of rows,
free at this scale and replaceable by an index the day there are hundreds (§13).

Stations do not despawn this phase — nothing can destroy anything, and Vanguard stations are
permanently indestructible as a rule (§8.5) — but every read of `structure` still goes through
`Resolve`, so a row whose ship is gone deactivates instead of dangling, and the user-station
design inherits a table that already tolerates death.

The **Vandal base** is registered in this same table at boot — owner `FACTION_VANDAL`,
complement 0 (its patrol is not a garrison and does not change) — so "may I dock here" has one
answer path for every station in the game, and the player is refused by standing (§4.2) rather
than by a special case. Nothing about the base's behavior or look changes (§15, decision 4).

### 6.2 The record — one flags byte

A client tapping a structure needs to know it is tapping a station before any order is worth
sending, and "immovable hull of faction 2" is inference of exactly the kind §4.3 bans.
`ShipSnapshot` gains:

```cpp
std::uint8_t flags = 0; // bit 0: this record is a station that admits ships
```

written as one `U8` after `factionId`; `SHIP_RECORD_BYTES` goes 82 → 83 and
`ShipsPerSnapshotFragment` follows. On the reviewable list (ADR 0009) the field is identity — what
the thing *is* — beside `factionId` and `hullId`; what deliberately stays off the wire beside it:
the ledger (who is docked where is nobody's business but the station's until the management menu
asks, and then it will be asked for, not broadcast), the garrison numbers, and the target list —
all of it intent or private state of the kind the snapshot exists to withhold.

---

## 7. Docking

### 7.1 The order, up the wire

```cpp
// WorldSnapshot.h, beside MoveOrder
struct DockOrder
{
  std::vector<ShipHandle> ships;
  ShipHandle station; // the station's structure
};
[[nodiscard]] bool WriteDockOrder(const DockOrder& _order, Neuron::Transport& _transport);
[[nodiscard]] bool ReadDockOrder(std::span<const std::uint8_t> _datagram, DockOrder& _outOrder);
```

A second datagram kind beside the move order, discriminated the way the existing kinds already
are, with the same size arithmetic (`MaxShipsPerOrder` less the station handle's eight bytes).
The adapter resolves handles exactly as it does for moves and calls:

```cpp
// World. Returns what happened, for the local host's log and for tests; nothing returns over the wire.
enum class DockOrderResult : std::uint8_t { Ordered, NotAStation, RefusedStanding };
DockOrderResult IssueDockOrder(std::span<const ShipId> _ships, ShipId _station, FactionId _issuerFaction);
```

Three gates, in the simulation and not the adapter, all for ADR 0014's reason:

1. `_station` must be a live station row — anything else is `NotAStation`, a no-op.
2. Ships not of `_issuerFaction` are dropped from the order exactly as the move gate drops them.
3. `StandingOf(stationOwner, _issuerFaction) == Hostile` refuses the whole order — the owner's
   brief: an aggressor is not allowed to dock. Refused means *nothing changes*; the client's UI
   already knew (§4.3, §9.2) and said so before sending, so the silent wire costs nothing.

An accepted order, per ship: the patrol is cleared (an explicit order outranks a standing
behavior — the line `IssueMoveOrder` already has), the docking intent below is set, and the first
approach leg is issued immediately so the order feels like an order and not like a next-tick
suggestion. **`IssueMoveOrder` clears docking intent** in return: a later move order is a change
of mind, and before capture a docking ship is just a ship flying somewhere. There is no undock
and no cancel-into-hold: cleared intent leaves the ship doing whatever it was last told.

### 7.2 The intent — a parallel table, like patrols

```cpp
struct Docking
{
  ShipHandle station;
  bool active = false;
};
std::vector<Docking> m_dockings; // parallel to m_ships, swap-and-pop repaired with them
```

Not on `ShipState`, for the exact sentence the patrol table cites: an intent is what the snapshot
exists to withhold. The despawn repair extends to a third table the way it covered the second,
and the same test shape proves it (§11).

### 7.3 The dock pass — approach and capture

A dock is captured against a range derived per pair, the way arrival radii are:

```cpp
// SimTuning.h — contract: it decides on which tick a ship stops existing in space
inline constexpr float DOCK_CAPTURE_METRES = 60.0f;

// HullSpec.h, beside ArrivalRadiusMetres
[[nodiscard]] constexpr float DockRangeMetres(const HullSpec& _station, const HullSpec& _ship) noexcept
{
  return _station.BoundingRadiusMetres() + _ship.BoundingRadiusMetres() + DOCK_CAPTURE_METRES;
}
```

Worked against the table: an Interceptor docks within 315 m of a station's center, a Carrier
within 419 m — outside the hull-plus-clearance band where separation is shoving and routes cannot
end (the 263 m the patrol design measured for an Interceptor), and generous enough that the
approach destination below sits in unblocked path cells with a 32 m cell size. The 60 m is the
one new contract constant docking adds, and it is a *slack*, chosen against the path grid's
quantization, not a feel number.

The pass, first in the standing-intent slot (§10):

```
for each ship with active docking, in array order:
  resolve station structure; gone, or its row inactive -> active = false (stand down where it is)
  else if distance to structure centre <= DockRangeMetres(station hull, own hull)
       -> if StandingOf(owner, ship's faction) is Hostile -> active = false     (turned criminal mid-approach: aborted at the door)
          else collect the ship's handle for capture
  else if order == Idle
       -> issue the approach: destination = the point on the ship's current bearing from the
          station at exactly its own dock range; PlanRoute with the ship's ordinary clearance
after the loop, in collection order:
  append {hullId, factionId} to the station's ledger; despawn the ship with cause Docked (§7.4)
```

> **Amendment, 2026-08-30 (slice 3).** The capture range above and the approach destination cannot be
> the same number, and the code no longer makes them one. A ship is declared *arrived* when it is
> within `ArrivalRadiusMetres` of its destination **in any direction**, so a ship sent to a point
> exactly on the capture boundary can settle just outside it, go Idle, be re-aimed at the point it is
> already standing on, and stay there for ever. Measured before the split existed: a Corvette parked
> at 328.66 m against a 324.88 m boundary and never docked. So `DockApproachRangeMetres` is this
> section's formula — 60 m clear of the two skins, which is what the no-go-band argument below
> checked — and `DockRangeMetres` is that plus the ship's own arrival radius, which makes *arriving*
> and *docking* the same event by construction rather than by luck. No fixed margin inside the
> boundary could have done it: a Carrier's 37 m tolerance is more than half the 60 m slack. The
> argument of this section stands; only the knife-edge is gone
> ([slice 3](Archive/Stations-slice-3.md) §2.2).

The approach is re-issued whenever the ship goes Idle short of range — shoved off by traffic,
replanned, blocked — so docking is patient the way patrols are, with no arrival logic of its own.
Captures are collected during the walk and applied after it, because `DespawnShip` swap-and-pops
and mutating the array mid-iteration would make the visit order depend on who docked — the
determinism argument is §10's. The standing re-check at capture closes the window between an
accepted order and an aggression recorded during the flight: the door is guarded, not just the
doorbell (`AggressionAbortsAnApproach`, §11).

The ledger row is deliberately minimal — hull and faction — because that is the whole of what a
ship *is* today. When undocking arrives (next phase) it spawns a fresh ship from the row; the
handle is new, so control groups will have pruned the docked member and will not reclaim it. That
is the honest consequence of handles naming lives, it is what groups already do for any despawn,
and it is stated here rather than discovered in the menu's review (§13).

### 7.4 The wire learns *docked*

Hostiles §4.4 opened the cause-of-death door "the width of one list and no wider"; docking is the
second cause, and it widens the mechanism rather than adding a parallel one. The despawn log
becomes a log of records:

```cpp
enum class DespawnCause : std::uint8_t { Destroyed, Docked };
struct DespawnRecord { ShipHandle handle; DespawnCause cause; };
```

`DespawnShip` takes the cause, defaulted to `Destroyed` so F4 and every existing caller keep
their meaning; the dock pass despawns with `Docked`. The publisher's split (`SplitTheLost`)
produces three sets from two — destroyed, docked, merely left — and `WriteInterest` carries the
docked handles as a fourth span beside the destroyed, with a `dockedCount` in the update header
beside `destroyedCount`; the header grows and `ShipsPerSnapshotFragment` follows it, as the
format's field-by-field design intends. `SnapshotReceiver` exposes `Docked()` beside
`Destroyed()`. On the client, `ExplodeTheLost` consumes destroyed exactly as today;
a docked handle removes the hull **silently** — no explosion, no camera shake, no SHIP LOST — and
pushes the log line instead (§9.4). A plain leave stays a plain leave.

> **Amendment, 2026-08-30 (slice 3).** `destroyedCount` is no longer in the snapshot header, so the
> paragraph above names a place that has moved: departures became their own message on the reliable
> lane with [ADR 0029](Decisions/0029-departures-and-orders-take-the-reliable-lane.md). The
> conclusion is unchanged and the reason is the same one — a docking is stated once and a lost one is
> a ghost ship for the rest of the match — so `dockedCount` joins `LEAVE_HEADER_BYTES` (17 → 21) and
> the docked handles are a third run there. What does **not** follow is `ShipsPerSnapshotFragment`,
> which derives from `SNAPSHOT_HEADER_BYTES` and which a docking never touches
> ([ADR 0040](Decisions/0040-a-departure-carries-a-cause.md), [slice 3](Archive/Stations-slice-3.md) §2.1).

The interpolation story needs no work: a docked ship's last record simply stops being refreshed
and the removal arrives within one update interval (≤ 6 ticks, ~100 ms), inside what the
extrapolation window already tolerates for any ship.

---

## 8. Aggression and the protector response

### 8.1 The judgment

```cpp
// World. The server judges; no client message exists or ever will for this.
void RecordAggression(ShipHandle _attacker, StationId _station);
```

Two effects, in this order: `StandingOf(stationOwner, attackerFaction) = Hostile` — permanent,
empire-wide, per §4.2 — and the attacker's handle joins `_station.targets` if there is room
(capped at the station's `targetCap`, content like the rest of the row; a full list drops the
newest, deterministically, and the standing flip already happened, which is the part that
matters). A second aggression against a second station scrambles that station too: standing is
imperial, response is local, and both sentences come straight from the owner's brief — the
*attacked* station's ships launch.

**Who calls it.** Today: `GameLogicTests`, and one debug key — **F6** marks the first selected
own ship an aggressor against the nearest Vanguard station, the composition root calling `World`
directly under exactly F4's charter (a tuning aid may reach past the wire; a gameplay path never
may). Tomorrow: the combat design calls it on the first hostile act against a station or its
garrison — that design owes the trigger; this one owes it a socket that already works.

### 8.2 Launching

While a station's target list is non-empty, its garrison spills out on a metronome: one protector
per `launchEveryTicks` (default 90 — 1.5 s between launches) until `launchedCount` reaches
`protectorComplement`. A protector is a `SpawnShip` at the station's skin — on the bearing toward
the first live target, at station bounding + own bounding + `AVOID_MARGIN_METRES`, heading
outward — in the station owner's faction, entered into the protector table below. Losses are
replaced by the same metronome for as long as a target lives: the station is the law and its
reserve is bottomless, which is safe precisely because of §8.6 — there is nothing to farm.

> **Amendment, 2026-08-30 (slice 4).** `launchedCount` is a *derived* quantity in the code, not a
> stored field: the launch pass counts the active protector duties whose home is this station. Storing
> it needs a decrement when a protector **dies** as well as when one docks home -- otherwise "losses
> are replaced by the same metronome" never fires -- and `DespawnShip` has no business knowing what a
> protector is. Counting removes that repair path and a counter from the replay contract's shadow, and
> cannot drift from the truth because it is the truth
> ([ADR 0041](Decisions/0041-the-protector-response-reacts-to-stated-acts.md),
> [slice 4](Archive/Stations-slice-4.md) §2.3).

Launches happen inside the tick, so every number that shapes them is either per-station content
(hull, complement, cadence, cap — passed to `MakeStation` by the root, the patrol-ring precedent)
or already contract. The Vanguard's content this phase: **Corvette** protectors, complement **3**,
the mid-weight silhouette — reading distinct from the Vandals' Interceptors — with the faction
tint carrying IFF as it has since Hostiles slice 3.

### 8.3 The pursuit — the first behavior that reacts

```cpp
struct ProtectorDuty
{
  StationId home;
  ShipHandle target;
  bool active = false;
};
std::vector<ProtectorDuty> m_protectors; // parallel to m_ships, repaired with them
```

The protector pass, last in the standing-intent slot (§10), per active duty in array order:

- **Resolve the target.** Dead or docked (a stale handle either way): take the first live handle
  from the home station's target list — the list is pruned of stale entries as it is read, dense
  and in order. No target left: the duty ends and the ship goes home — its docking intent is set
  to its home station, through the same table §7.2 built, and §7.3 flies and captures it like
  anyone else. A protector docking home is **not** a ledger row: `launchedCount` decrements and
  the hull returns to the complement, because a garrison is not a guest.
- **Pursue.** If the ship is Idle, or its target has moved more than `PURSUIT_REPLAN_METRES`
  (64 m, `SimTuning.h` — the same figure and the same reasoning as `PATH_REPLAN_DEVIATION_METRES`)
  from the point last steered at: issue a move order at the target's current position, full speed,
  no facing, `PlanRoute` as any order does. Between re-aims, the tested machinery flies the leg.

> **Amendment, 2026-08-30 (slice 4).** "The duty ends and the ship goes home" is one word too strong
> and the code keeps the duty *active* until the ship actually docks. `ProtectorDuty::active` means
> "this ship is a garrison ship of `home`, and it is in space", not "it is hunting"; a null target
> with nothing to replace it is what standing down means. Three things need that distinction: a
> protector flying home still counts against the complement, so the station does not relaunch behind
> it; it still has to be told from a visitor at the door, or it writes a ledger row; and a *new*
> aggression turns it round rather than letting it dock and be relaunched a tick later. The duty ends
> by being swap-and-popped away with the ship it belonged to
> ([slice 4](Archive/Stations-slice-4.md) §2.4).

That is a chase built entirely out of standing parts: the reaction is *choosing the point*, and
everything after the choice is the same code a player's click exercises. The pursuit never gives
up — not by range, not by time; "until it is killed" is the brief — and the server simulates
beyond every interest radius, so flight is postponement. What a protector does on *arrival* — it
has no weapon — is shadow: avoidance and separation hold it off the target's hull, and it re-aims
as the target moves. The teeth are the combat design's to add (§15, decision 2); the socket it
needs — a ship that is always in weapons range of its target — is exactly what shadowing is.

### 8.4 Senses, deliberately absent

The response starts from a *stated* act (`RecordAggression`), not from perception: no protector
or station scans for enemies, no radius makes you a criminal, and a Vandal flying past a Vanguard
station is unmolested however hostile the standings table says it is. Aggro radii, threat
assessment and target switching by proximity are the combat design's senses; this phase's NPC
reads exactly two things it did not write — its target's position and liveness — which is the
narrowest reaction that satisfies the brief.

### 8.5 The station is invulnerable, as a rule

Nothing this phase can damage anything, so invulnerability lands as a recorded rule rather than
code: **a Vanguard station takes no damage; an attack on it is an aggression event, not a damage
event.** The combat design implements that sentence (however it models damage, a Vanguard
station's is discarded), and the user-station design gets the counter-sentence — theirs are the
destructible kind — against a station table that already resolves its structure per read (§6.1).

### 8.6 No loot from protectors, as a rule

Same shape: there is no loot system, so the rule is recorded where the loot design will look.
**A protector drops nothing when destroyed.** With §8.2's bottomless relaunch this is what makes
an infinite response farmable-proof rather than an exploit: the punishment ships spend nothing
worth taking. The rule keys off the garrison (a ship spawned by §8.2), not the faction — a
Vanguard freighter, if one ever flies, is not covered by it.

---

## 9. The client — affordances, colors, the overview

### 9.1 Picking and the tap

`PickShip` stays own-faction-only — stations are still not selectable, hoverable or box-selectable
(what you cannot command you must not appear to hold). A second picker, `PickStation`, ray-tests
records whose flags say station, any faction, and it is consulted from exactly one place: a tap
with a non-empty selection. `OnTap` becomes, in order: own hull → select it (unchanged); station
hull and something is selected → dock order; ground → move order (unchanged). With nothing
selected a tap on a station does nothing this phase — selection-for-inspection is the management
menu's, next phase, and the long-press that will open it is likewise absent: no gesture is
half-landed here, and `PointerTracker` gains long-press support when the menu exists to open
(§14).

### 9.2 The affordance tells the truth first

Before sending, the view checks the received `hostileMask` (§4.3): if the station's owner holds
this client hostile, no order is sent and the log says `DOCKING REFUSED | %s HOSTILE` — the
owner's name from `FACTION_NAMES`, a table beside `HULL_NAMES` in the root reading `PLAYER`,
`VANDAL`, `VANGUARD`. Hostiles §12 deferred exactly this table "until something displays it";
this line is the something, and refusal at the Vandal base and at a Vanguard station is one
format string either way. The simulation gate (§7.1) still stands behind it, per the
twice-on-purpose rule Hostiles set: affordances tell the truth, and clients are not trusted. An
accepted tap sends the order and logs `DOCKING | %d SHIPS`, and the station under the tap flashes
the marker treatment orders already get, in the station's faction color, so the tap visibly
landed on the thing and not the ground.

### 9.3 Colors, the minimap, and the marks

The faction-to-tint mapping generalizes from a branch to a table the day it holds three rows:

| Identity | Scene tint | Overview |
|---|---|---|
| Own faction | `SHIP_COLOUR` / selection green | green, as today |
| Faction whose `hostileMask` bit is set | `HOSTILE_SHIP_COLOUR` red family | `HUD_ALERT_RED` |
| `FACTION_VANGUARD`, mask bit clear | `VANGUARD_SHIP_COLOUR` — an azure, with `VANGUARD_ACCENT_COLOUR` for exhaust and trails | `HUD_VANGUARD_BLUE`, derived from the accent the way the red is |

The mask row outranks the Vanguard row: turn criminal and the law turns red — hulls, plumes and
dots together, one mapping change, which is ADR 0013 doing precisely what it promised.

**How the scene column is actually painted** is no longer this document's to say. NMO's
[slice 5](Archive/NmoFormat-slice-5.md) replaces the whole-hull tint with liveries: a material declares
whether it is the model's paint or the faction's ([NmoFormat.md](Archive/NmoFormat.md) §5.5), and only the
declared surfaces take the faction's colour. Three consequences for the table above, none of them
to its precedence, which stands exactly as written and is the part that mattered: the scene column
becomes **one** constant per faction rather than a ship colour and an accent colour, because the
accent falls out of the shade ladder; `VANGUARD_SHIP_COLOUR`/`VANGUARD_ACCENT_COLOUR` are therefore
spelled `LIVERY_VANGUARD` when they land; and exhausts are liveried by the same lookup rather than
by a constant of their own. The overview column is untouched — the minimap answers friend-or-enemy
and deliberately does not follow a livery (slice 5 §2.6). Station
records draw at `HUD_MINIMAP_STRUCTURE_DOT_PX` like any structure.

**Marks** are the new thing: the minimap draws every station of the root-supplied layout — a
hollow diamond in the owner's color at the station's position, and, when the position is beyond
the map's 1 400 m half-range, clamped to the map edge at reduced alpha, direction honest and
distance saturated. Marks come from static content (§5), not from records: they exist from the
first frame, which is what "static so can be marked" buys, and a live record at the same spot
draws its filled dot over the hollow mark. The composition root hands the view the mark list at
boot the way it hands body placements; nothing about marks touches the wire.

### 9.4 Counting and narrating

`CONTACTS` stops meaning "not mine" and starts meaning "hostile to me": records whose faction bit
is set in `hostileMask`. At boot that is the Vandal four — unchanged number, honest definition —
and Vanguard stations do not inflate it; turn criminal and every Vanguard record joins the count,
which is the HUD saying what just happened without a new widget. The event log gains: `DOCKING |
%d SHIPS`, `DOCKING REFUSED | %s HOSTILE` (§9.2), `DOCKED | %d SHIPS` (on consuming docked
handles), and `VANGUARD PROVOKED` from F6 so a screenshot of the response names its cause. The
boot line stays the player's own count; a `STATIONS ONLINE | %d` line beside it says the grid
spawned what the layout described.

---

## 10. Determinism

The three new passes live where `StepPatrols` lives — the standing-intent slot before pass 0 —
and run in a fixed order: **dockings, patrols, protectors**. Every argument Hostiles §8 made is
inherited: reads are end-of-last-tick state, each pass writes only the ship it is visiting, no
clock, no entropy, no pointer keys. What is new, and argued rather than assumed:

- **Despawn inside the tick.** The dock pass despawns ships, and `DespawnShip` swap-and-pops four
  parallel tables. Captures are therefore collected during the walk and applied after it, in
  collection order — which is array order, which is deterministic. Applying them before
  `StepPatrols` runs means later passes iterate the repaired arrays, exactly as if the despawn had
  come from outside between ticks, which is a case every table already survives.
- **Spawn inside the tick.** Launches append to the same tables the protector pass iterates, so
  they are likewise collected and applied after the pass. A ship spawned in the slot enters pass 0
  with `prevPos = posWorld` and participates from its first tick, the same as a boot spawn.
- **The standings table** is a fixed-size array indexed by two integers — no map, no hashing, no
  iteration at all on the hot path; it is read pointwise and mutated only by `RecordAggression`,
  which arrives from outside the tick (adapter, root, tests) like any order.
- **Target lists** are dense vectors pruned in place, in index order, as they are read; the
  metronome counters are integers ticked in the pass. Nothing inside `Step` draws randomness.
  `LayOutSystem` is the first `GameLogic` code to draw at all — the "one seeded PCG32 when
  randomness arrives" that AGENTS.md §5 and ADR 0012 have promised since the tree began — and it
  draws `Neuron::Pcg32` from a caller-supplied seed, at boot, in whoever calls it: a pure function
  whose output is then ordinary spawn input, so the replay contract sees positions, never a
  generator.
- **A world with no stations ticks bit-identically to today**: three empty tables, three passes
  that visit nothing, a default `Destroyed` cause on every existing despawn. The existing
  `GameLogicTests` passing unchanged is the claim, and the first slice makes it explicitly.

New contract constants: `DOCK_CAPTURE_METRES`, `PURSUIT_REPLAN_METRES`, `DEFAULT_STANDINGS`,
`FACTION_LIMIT` — in `SimTuning.h`/`ShipState.h` with the reason each changes recorded outcomes.
Station garrison numbers and the layout desc are content, passed in by the root, the
patrol-radius precedent. The layout function itself is pure and boot-time; its seed is content
the way `BODY_START_SEED` is.

---

## 11. Tests

`GameLogicTests`, in the house style of naming the property:

| Test | Decides |
|---|---|
| `TheLayoutIsAFunctionOfItsSeed` | two calls with one seed agree field-for-field; adjacent seeds differ |
| `TheLayoutRespectsTheGridCeiling` | the shipped `SystemDesc` bounds keep worst-case static span under 512 path cells |
| `PlanetsKeepTheirDistance` | bearing-slot placement holds a stated minimum separation for any seed tried |
| `AStationIsItsRow` | `MakeStation` on a live structure resolves; on a dead handle deactivates; the Vandal base registers with complement 0 |
| `TheStandingTableStartsAsAuthored` | defaults: the Vandal Collective hostile both ways, Vanguard neutral to the player |
| `StandingSurvivesTheWire` | the hostileMask byte arrives with the Vandal bit set at boot and gains the Vanguard bit after `RecordAggression` |
| `TheStationFlagSurvivesTheWire` | a station's record decodes with bit 0 set; a plain ship's does not |
| `AShipDocksAndLeavesTheWorld` | order → approach → capture within `DockRangeMetres` → despawn, ledger row `{hull, faction}` |
| `ADockAndADeathDifferOnTheWire` | a docked handle arrives in the docked list, a destroyed one in the destroyed list, and a range-leaver in neither |
| `AMoveOrderCancelsDocking` | move after dock: intent cleared, ship diverts, never captures |
| `TheStandingGateRefusesADock` | hostile issuer: `RefusedStanding`, no ship's state touched |
| `AggressionAbortsAnApproach` | aggression recorded mid-flight: ships arrive and are turned away at capture range |
| `AggressionIsImperialAndPermanent` | after one aggression, a *second* Vanguard station refuses the dock, and a thousand ticks change nothing |
| `TheStationScramblesItsComplement` | aggression → launches on the cadence, capped at the complement, all in the owner faction |
| `AProtectorPursuesItsTarget` | distance to a fleeing target decreases; the aim point refreshes when the target moves past the replan threshold |
| `AProtectorStandsDownWhenItsTargetDies` | target despawned → protector flies home, docks, `launchedCount` returns to zero, no ledger row |
| `ALossIsReplaced` | despawn a protector mid-response: the metronome launches a replacement |
| `ADespawnRepairsEveryTable` | swap-and-pop moves docking intent and protector duty with the moved ship (the patrol test's shape, widened) |
| `TheSameResponseProducesTheSameRun` | the full scene — layout spawns, a dock, an aggression, the response — twice, compared field-for-field every tick: the replay gate over everything new |

The client slices are decided by screenshots where only a screen can (Design/README.md): the
azure station and its mark on the minimap at two window sizes; the mark clamped to the map edge
at boot; a dock order flying and the hulls winking out with no explosion and a `DOCKED` line; F6
then the garrison launching and the map turning red — plus the suites green and no `GameLogic`
file touched by an `Outpost` slice.

---

## 12. The MMO ledger

What this design commits to, kept in the shape the target needs:

| This feature adds | Where it lands | The day the MMO arrives |
|---|---|---|
| A third faction and standings | a table in `World`, mutated only by server-side judgment | granularity widens to per-player rows exactly as ADR 0014 planned for authority; the table is already load-bearing |
| "What am I to them" on the client | one mask byte in every interest update | becomes a standings record when factions outgrow a byte; the client never inferred, so nothing unlearns |
| Static universe content | a pure seeded function in `GameLogic`, consumed by both roots | ships with both binaries as install content; a server-authored layout is the same struct arriving by download instead of by call |
| Station identity on the wire | one flags byte in the record | user stations, conquerable stations, more flags — the byte has seven doors left |
| A departure with a cause | `DespawnCause` on the log, a docked list beside the destroyed list | jump-out, wreck-and-salvage, capture — each is one more cause through the same door |
| Docking | an order kind, an intent table, a capture rule in the tick | unchanged; per-player dock rights ride the standings widening |
| A reacting NPC | target-driven pursuit built on the order machinery | combat adds weapons to a chassis that already chases; fleets add coordination above a pass that already resolves per ship |

And the traps stepped around, named for review: no client-declared aggression, no per-viewer
record contents, no ledger on the wire, no NPC senses, no wall-clock cooldowns (ticks
everywhere), no behavior keyed on interest membership (the server chases you whether or not
anyone watches), and no special-cased government — the Vanguard is a faction like any other with
a strong opinion of criminals.

---

## 13. Design choices

The choices made inside the design, as opposed to those put to the owner (§15); each with its
cost.

- **Station-ness is a side table, not a hull and not an entity kind.** One lookup by handle on a
  vector of single digits, against: a hull property that cannot say "this structure is scenery",
  or a second entity array that forks every system that today says "ship". Cost: the lookup, and
  a table the despawn path must deactivate rows against.
- **Standing is directional and faction-granular.** Directional because the dock gate and the
  response both read *the owner's* opinion; faction-granular because that is what one subscriber
  is. Cost: a per-player widening later — priced in, beside ADR 0014's identical debt.
- **The mask travels every update.** Idempotent against datagram loss for one byte; the
  alternative — a standings event message — invents reliability this wire does not have.
- **The layout is GameLogic's, the looks are the client's.** ADR 0008's argument applied to
  content both binaries need; `BodyCatalogue` keeps the looks because a server has no business
  knowing what a planet wears. Cost: one more public header in `GameLogic`, and the discipline
  that it stays position-and-seed only.
- **Docking reuses despawn rather than inventing a docked ship-state.** A docked ship is not
  simulated, not collided, not snapshotted, not interesting — which is what despawned *is*. A
  `Docked` order-state instead would leave ghost entries in every pass and every index for a hull
  that is not there. Cost: undock is a fresh spawn with a fresh handle, so groups forget docked
  members (stated at §7.3), and the ledger must carry whatever a future ship-identity needs — today
  that is hull and faction, all a ship is.
- **Capture range is derived per pair with one new constant.** `DOCK_CAPTURE_METRES` as slack
  over the two bounding radii, not a flat range: a flat 300 m is inside a Carrier's no-go band and
  a canyon for an Interceptor. The 60 m is argued against path-cell quantization at §7.3.
- **The response is a metronome of spawns, not a pre-existing wing.** Docked complements
  patrolling at boot would cost three NPCs per station forever to animate a courtesy nobody
  attacked; spawning on aggression costs nothing until the player buys trouble. Cost: the launch
  cadence is one more counter in the replay contract's shadow (per-station content, §8.2).
- **Protectors go home through the docking machinery.** Standing down is "dock at home" — one
  intent write — rather than a return-behavior of its own. Cost: none found; it is the reuse the
  docking table exists for.
- **F6 exists.** A response nobody can trigger is a response nobody can tune or screenshot; F4
  set the charter for a root-called debug act. Cost: one more line in the key list to delete when
  combat lands.
- **New identifiers extend standing families as spelled**: `DOCK_CAPTURE_METRES`,
  `PURSUIT_REPLAN_METRES`, `VANGUARD_SHIP_COLOUR`, `DockRangeMetres` — the `_METRES` and
  `*Colour*` families keep their UK spellings per AGENTS.md R11 until their own rename slices.

---

## 14. Deliberately left out

Named so nobody goes looking, and so the next design knows its edges:

- **The station management menu, and everything behind it** — undocking, the docked-ship list,
  cargo, repair, trade. Next phase, per the owner's brief. The long-press gesture that opens it
  is absent too: `PointerTracker` learns long-press when there is a menu to open, not before.
- **Undocking by any path.** A docked ship stays docked. The ledger is designed so the menu can
  spawn it back out; nothing this phase reads the ledger but tests.
- **Combat** — weapons, damage, hit points, death by violence — and therefore the *act* of
  attacking a station. The framework meets it at two named sockets: the combat design calls
  `RecordAggression` on the hostile act, and gives the shadowing protector (§8.3) its guns.
- **User-owned stations**, per the brief. The table rows, the standing gate and the
  destructibility counter-rule (§8.5) are the doors left open.
- **Standings repair** — decay, fines, amnesty. Criminal is forever until a standings design says
  otherwise (§15, decision 3).
- **NPC senses** — aggro radii, threat scans, proximity triggers (§8.4).
- **Loot**, entirely; only the §8.6 rule is recorded for it.
- **A sun, a system map, more planet pictures.** The star is an anchor; the rail's universe icon
  stays decorative; all three worlds wear `Planet1.dds` until planet art is content. The minimap
  marks are the whole of this phase's cartography.
- **More systems than the starting one**, and the per-region path grids and interest regions that
  a second system 100 km away would force. `LayOutSystem` takes a star position precisely so the
  second call is content, not redesign — but nothing calls it twice today.
- **Station names.** Factions have names now (`FACTION_NAMES`, §9.2); individual stations do not
  — log lines count ships and name owners, marks are anonymous. A designation scheme
  ("VGR KEPLER-2") belongs to the menu that will display it.
- **Docking animations and bay geometry.** A captured hull is removed on the docked statement, at
  the skin, without ceremony. A fade or an approach lane is presentation polish for the phase that
  gives stations an inside.

---

## 15. Decisions taken with the owner

Put to the owner on 2026-08-30 and answered as follows; each was the recommended option.

| Question | Decision | What lost |
|---|---|---|
| The brief's "ensure that a System is distributed across the Universe… every planet… has a system… static so can be marked" | **"Station" was meant. One seeded solar-system layout as static content; this phase instantiates the starting system** — a few planets, one Vanguard station each, all inside the path grid's ceiling | instantiating many systems now — per-region grids, streaming and interest regions for content no ship can reach in a session; reading it literally — no coherent feature answers to it |
| Protectors must "attack until it is killed", but nothing can attack | **Framework now, combat later**: aggression, standings, scramble, pursuit and shadowing land testable via `RecordAggression` and F6; a separate combat design supplies the trigger and the teeth | folding a minimal combat model in here — doubles the design, and a damage model deserves its own argued document rather than a stowaway berth |
| Which stations refuse an aggressor, for how long | **All Vanguard stations, permanently** — CVC is one government, and forgiveness is a standings design of its own | per-station grudges — a government that forgets you at the next port; a decay timer — invents half a standings system to expire a flag |
| The existing hostile base | **Stays, as pirates** — registered as a Vandal-owned station with no garrison change; the standing rule refuses the player for free | converting it to the Vanguard — loses the hostile contrast and the combat-test dummy; removing it — deletes landed content for tidiness |

After these were taken, the owner named the pirate faction: the **Vandal Collective** ("Vandal").
§4.1 adopts the name and renames `FACTION_HOSTILE` to `FACTION_VANDAL` with it, for the
two-meanings-of-one-word reason argued there.

---

## 16. Slices

Six, in dependency order. 1–4 are `GameLogic` and therefore strictly serial (one slice per layer
at a time); 5 and 6 are `Outpost`, serial with each other, and each starts only when its
`GameLogic` dependencies have merged. Each is one branch, one pull request, in Design/README.md's
shape; work orders are written per slice when it is picked up.

| # | Slice | Layer | Depends on | Decision records due |
|---|---|---|---|---|
| 1 | **The layout**: `UniverseLayout.h/.cpp`, `LayOutSystem`, the three layout tests — *landed*, [work order](Archive/Stations-slice-1.md) | `GameLogic` | — | the layout is static content in `GameLogic` ([ADR 0037](Decisions/0037-the-universe-layout-is-static-content-in-gamelogic.md)) |
| 2 | **Who is who**: `FACTION_VANGUARD`, the `FACTION_HOSTILE` → `FACTION_VANDAL` rename at every caller (§4.1), `Standing` + `DEFAULT_STANDINGS` + the table in `World`, the standing half of `RecordAggression`, the station table + `MakeStation` + `StationDesc`, the record's flags byte, the update header's `hostileMask`, their tests — *landed*, [work order](Archive/Stations-slice-2.md) | `GameLogic` (+ the rename's `Outpost` call sites) | — | [stations are ships with a side table](Decisions/0038-stations-are-ships-with-a-side-table.md); [standings are simulation state stated per subscriber](Decisions/0039-standings-are-simulation-state-stated-per-subscriber.md) |
| 3 | **Docking**: `DespawnCause` + the docked list on the wire, `DockOrder` write/read, `IssueDockOrder` + gates, `m_dockings` + the dock pass + capture + ledger, `DOCK_CAPTURE_METRES` + `DockRangeMetres`, move-order cancellation, despawn repair, their tests — *landed*, [work order](Archive/Stations-slice-3.md) | `GameLogic` | 2 | [a departure carries a cause on the wire](Decisions/0040-a-departure-carries-a-cause.md) |
| 4 | **The response**: target lists + the launch metronome, `m_protectors` + the pursuit pass + `PURSUIT_REPLAN_METRES`, stand-down-and-dock-home, the full `RecordAggression`, the replay test over the whole scene — *landed*, [work order](Archive/Stations-slice-4.md) | `GameLogic` | 2, 3 | [the protector response reacts to stated acts, not senses](Decisions/0041-the-protector-response-reacts-to-stated-acts.md) |
| 5 | **The Vanguard scene**: root calls `LayOutSystem` + spawns the stations + registers the Vandal base, planet visuals follow the sites (F5 reseeds looks only), `VANGUARD_*`/`HUD_VANGUARD_BLUE` colors + the faction-tint table, `FACTION_NAMES` beside `HULL_NAMES`, minimap station dots + hollow marks + edge clamping, `hostileMask` consumption, `CONTACTS` by mask, `STATIONS ONLINE` boot line, AGENTS.md's what-is-here sentences, screenshots at two sizes — *in review*, [work order](Stations-slice-5.md) | `Outpost` | 1, 2 | — |
| 6 | **Docking and the response, on screen**: `PickStation` + the tap order + refusal affordance + marker flash, docked-list consumption (silent removal, `DOCKED` line), F6 + `VANGUARD PROVOKED`, log lines, screenshots of a dock and of a scramble at two sizes | `Outpost` | 3, 4, 5 | — |

Slices 1–4 are decided by their tests and by the existing suites staying green — slice 2's claim
that a station-less world ticks bit-identically is exactly `GameLogicTests` passing unchanged.
Slices 5 and 6 are decided by screenshots and by what they must not touch: no `GameLogic` file,
no new information reaching the client outside the record, the header byte and the layout call.

What the next phase inherits: a station that knows who is inside it, a wire that can say docked,
a standing that can refuse, and a long-press with nowhere yet to go — the management menu opens
onto all four.
