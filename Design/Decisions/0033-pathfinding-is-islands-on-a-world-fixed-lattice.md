# 0033 — Pathfinding is islands of architecture, on a lattice fixed to the world

Status: accepted
Date: 2026-08-30

## Context

`PathGrid` is one clearance grid over every obstacle in the universe. `Design/Archive/Collision.md` §12
argued for exactly that shape and it was right: it is simple, it is correct for a scene that fits in
a box, and it is what a first planner should be.

`Design/MmoScalabilityReview.md` finding U1 is where it stops. `Rebuild` sweeps one bounding box over
all obstacles and declines to build past 512 cells per axis — 16,384 m. Two stations 20 km apart
therefore turn A\* off **for every ship in the world**, and `HullSpec.h` says plainly that this is
not a graceful degradation: a capital's look-ahead is deliberately shorter than its own turning
circle, so capitals require a planned route wherever architecture is dense. One distant outpost makes
every Carrier in the game fly into things.

Writing the replacement turned up a second problem the review did not have, and it is the one that
decided the shape. **The grid's lattice is a function of its contents.** `m_origin` is the corner of
the bounding box over the obstacles, so adding an obstacle 900 m west of a Structure moves the origin
900 m west — and 900 is not a multiple of the 32 m cell, so every cell centre shifts under every
fixed point in the world, by 4 m in that example and by up to half a cell in the worst one. Cell
centres are what A\* searches and what `ClearanceAt` samples. The same architecture, approached from
the same place, can therefore produce a different route because of something built somewhere else
entirely.

Today that is invisible: any change bumps the version, every route re-plans, and the shifted answer
is simply the new answer. It stops being invisible the moment routes are cached, compared across
machines or replayed — and `SimTuning.h` already declares these constants part of the replay
contract. More immediately: a grid whose lattice depends on its contents **cannot be regional**,
because two neighbouring regions would disagree about where a cell is.

## Decision

Two decisions, independent, and the first is a precondition for the second.

**The lattice is anchored to the sector grid.** A cell's identity is a pure function of a position:
`cellX = sectorX × 256 + floor(localX / 32)`, exact because `IsSectorAlignedCellSize` already
guarantees `SECTOR_SIZE_METRES / PATH_CELL_SIZE_METRES` divides evenly, and exact in `std::int64_t`
across the whole `WorldPos` range. A grid stores a *window* onto that lattice — an origin cell, a
width and a height — and never chooses where the cells are.

**Obstacles cluster into islands, and each island gets its own grid.** Two obstacles share an island
when the gap between their surfaces is narrower than the widest hull plus its clearance margin: a
wider gap is one the straight-line test can route a ship through, a narrower one is a wall A\* has to
find its way around. The islands are the connected components of that relation, ordered by the lowest
cell index among their members — a world coordinate, so the order cannot follow a `ShipId`, which
moves under swap-and-pop (ADR 0005).

Routing across islands re-uses the follower rather than adding machinery: a route through the first
island the straight line enters is returned with `reachesDestination` false, and
`World::AdvanceRoute` already re-plans on arriving at the end of an incomplete route. The stitching
is the behaviour that already ships.

`Design/RegionalPathfinding.md` is the design; this record is why the shape is that one.

## Alternatives

**One grid per sector, built lazily.** The natural unit, and it fixes the cliff and the lattice
equally well. It loses on size: a sector is 256 × 256 = 65,536 cells and 256 kB whether it holds one
Structure or none of it, so a hundred populated sectors is 26 MB spent almost entirely to say
"open". Islands give the same lattice guarantee and size themselves to the content — a lone Structure
is 9 kB.

**Keep one grid and raise the cap.** The cap is not the problem, the box is. A world 100 km across is
3,125 cells per axis and 39 MB, nearly all of it the empty space between outposts.

**Keep one grid and coarsen cells to fit.** Rejected for the reason the existing comment already
gives: cell size is in the replay contract, so coarsening under pressure changes recorded outcomes as
a side effect of where somebody put a building.

**HPA\* with portals between clusters.** The right answer when the space *between* clusters is
constrained. Here it is open, so the portal graph would be a complete graph over islands and would
buy nothing but its own maintenance. It is what to build the day content can author a constrained
corridor.

**A tangent-visibility graph over inflated discs.** Turned down once already in
`Design/Archive/Collision.md` §12 — prettier paths for sparse convex obstacles, and structurally unable to
handle a concave island or an interior. Islands do not revive it.

## Consequences

Rebuild cost stops being every cell against every obstacle. One Structure moving in a world of thirty
stations goes from 7.9 M distance evaluations to 2,304, because only its own island rebuilds.

The 16.4 km cliff becomes local. An island genuinely 16 km across still declines, but it declines
alone: its neighbours keep routing, where today one such island disables the world. That failure
should be traced rather than silent, which is a diagnostic the current code does not have either.

**Routes across islands are no longer globally optimal.** A ship cannot see that going round island
two is cheaper than through it until it arrives at island two. That is bought deliberately, and it is
what makes the portal graph unnecessary.

**A long crossing is several A\* runs where it was one.** Each is small — one island's box rather
than the world's — but the count is new, and the benchmark should show it rather than leave it to be
noticed.

`IslandGapMetres()` is derived from the hull table, so adding a hull larger than a Carrier changes
the partition and therefore changes routes. That is correct, and it is a replay-contract change; it
belongs with the other derived hull constants that are already checked against the table.
