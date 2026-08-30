# 0038 — Stations are ships with a side table

Status: accepted
Date: 2026-08-30

## Context

`Design/Archive/Stations.md` §6 needs a thing a ship can dock at: something with an owner, a ledger of who
is inside, and a garrison it can launch. The tree already has the body — `HullId::Structure` is
immovable, collidable, in the static index and the pathfinding obstacle set, and the Vandal base has
stood on one since Hostiles slice 3. What it has no way to say is that a particular structure
*admits ships*.

Two things make this worth a record rather than a commit message. It is a shape that later designs
inherit — user-owned stations, conquerable stations, the management menu — and each of the
alternatives is one somebody will propose again.

## Decision

A side table in `World`, indexed by `StationId`, whose rows hold a `ShipHandle` to the structure,
the owner faction, the garrison content and the docked ledger. `MakeStation(ShipId, StationDesc)`
adds a row; `StationAt(ShipId)` finds one; every read of the structure goes through `Resolve`.

The station **is** its structure ship. The row is what knows it admits ships, and nothing else about
the ship changes: it keeps its place in the static index, the obstacle set, and the wire.

One byte of the record says so to a client — `ShipSnapshot::flags` bit 0 — because a client tapping
a structure has to know it is tapping a station before an order is worth sending, and "immovable
hull of faction 2" is the client inferring server state.

## Alternatives considered

- **A hull property, or a new `HullId::Station` row.** The obvious answer, and it reads well until
  the second station. A `Structure` that is scenery and a `Structure` that is a station must both be
  expressible — the tree already has scenery structures and will have more — and user-owned stations
  will be stations on hulls that are not `Structure` at all. A hull table that also carried
  station-ness would have to be duplicated per hull the day either of those happened.
- **A new entity kind: a parallel `m_stations` array of its own entities.** Rejected as the
  expensive one. A station is 95 % a ship — it has a position, a hull, a faction, a record, a place
  in the index, an explosion — so a second entity array forks snapshots, interest, picking,
  pathfinding obstacles and the despawn path, for the 5 % that is a ledger. Every system that today
  says "for each ship" would have to learn to say it twice.
- **A flag on `ShipState`.** Cheap, and wrong for the reason `ShipState`'s own comment gives: it
  promises nothing in it that a snapshot could not carry, and a station's ledger, garrison and
  target list are exactly what the snapshot exists to withhold. The flag would arrive with nowhere
  to put the rest.
- **A `std::unordered_map<ShipHandle, Station>`.** The obvious way to make the lookup O(1).
  Rejected on AGENTS.md §5: no pointers as keys and no iteration order that is not dense-array
  order. A map's iteration order would enter the replay contract the moment anything walked it.

## Consequences

- `StationAt` is a linear scan of a vector with single digits of rows, called once per record
  written. At 13 records a fragment against four stations that is free; the day there are hundreds
  it becomes an index, and nothing above it changes when it does.
- The row holds a `ShipHandle`, so the death of a structure orphans its row rather than retargeting
  it to whichever ship swap-and-pop moved into that index. Nothing can destroy a station this phase
  and a Vanguard station is indestructible as a rule (`Design/Archive/Stations.md` §8.5) — but the
  user-station design inherits a table that already tolerates death, which is why it was done now
  and not then.
- Stations do not despawn, so `StationId` is a bare index with no generation. The day one can, it
  gains a slot table exactly as ships have, and every current caller keeps compiling.
- The Vandal base registers in the same table with complement 0, so "may I dock here" has one answer
  path for every station in the game and the player is refused by standing rather than by a special
  case.
- The flags byte has seven bits left. User stations, conquerable stations and whatever else needs
  saying about a record are what they are for; each is a bit and a mapping row, not a format change.
