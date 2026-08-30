# Regional pathfinding — routing when the architecture is bigger than one grid

`PathGrid` is one clearance grid over every obstacle in the universe. That was the right first
shape: it is simple, it is exactly the structure `Design/Archive/Collision.md` §12 argued for, and it is
correct for a scene that fits in a box. It does not survive a universe, and this design is what
replaces it.

This is a design, not a work order. It states the problem, the options, the shape chosen, what that
shape deliberately leaves out, and the slices it yields. Nothing here is implementable as written
until a work order is cut from §9.

---

## 1. The problem, in three parts

### 1.1 It declines to exist past 16.4 km, and takes the universe with it

`PathGrid::Rebuild` sweeps one bounding box over *all* obstacles and refuses to build when either
axis needs more than `PATH_GRID_MAX_CELLS_PER_AXIS` cells — 512 × 32 m = **16,384 m**
(`PathGrid.cpp`, `SimTuning.h`). Two stations 20 km apart therefore disable A\* **for every ship in
the world**, not for the space between them. Every route degrades to straight-line steering, and
`HullSpec.h` is explicit that this is not a graceful degradation: a capital's look-ahead is
deliberately shorter than its own turning circle, so capitals *require* a planned route anywhere
architecture is dense. A single distant outpost turns every Carrier in the game into a ship that
flies into things.

The existing comment knows: *"sectors are what will bound this properly."* This is that.

### 1.2 Rebuild is every cell against every obstacle

`Rebuild` computes each cell's clearance as the exact minimum over all obstacles — O(cells ×
obstacles), both of which grow with the world:

| Grid | Obstacles | Distance evaluations per rebuild |
|---|---|---|
| 262,144 cells (the legal maximum) | 8 | 2.1 M |
| 262,144 cells | 30 | 7.9 M |

Slice 13 already made these rebuilds rare — they happen only when the architecture actually changes
— but "rare and enormous" is still a frame-time spike at the worst possible moment, which is when a
station is destroyed in a fight.

### 1.3 The cell lattice moves when the obstacle set changes

This one is not in the review, and it is the reason the shape below is what it is.

The grid's origin is its own minimum extent: `m_origin` is the corner of the bounding box over the
obstacles, plus a margin. So the lattice is a function of *where the architecture happens to be*,
not of the world. Add an obstacle 900 m west of an existing Structure and the origin moves 900 m
west — and because 900 is not a multiple of the 32 m cell, **every cell centre in the grid shifts
under every fixed point in the world**. Measured on that example, the clearance a fixed probe reads
moves 4.0 m; the shift is the displacement modulo the cell, so the worst case is half a cell, 16 m,
and a rock dropped 4 km west moves it 24.2 m by putting the probe in a different cell entirely.

Cell centres are what A\* searches and what `ClearanceAt` samples, so the same architecture,
approached from the same place, can produce a different route depending on what else was built
somewhere else entirely. Today this is invisible: the grid version bumps on any change and every
route re-plans anyway, so the shifted answer is simply the new answer. It stops being invisible the
moment routes are cached, compared across machines, or replayed — and `SimTuning.h` already says
these constants are in the replay contract.

A grid whose lattice is a function of its contents cannot be regional, because two neighbouring
regions would disagree about where a cell is.

---

## 2. What the seam must keep

The review, the plan and `Design/Archive/Collision.md` all say the same thing, and it is a real constraint
rather than a courtesy:

- **`FindPath(from, to, requiredClearance, outWaypoints) -> bool`** keeps its signature and its
  meaning, including that `false` means "the last waypoint is the furthest safe point, re-plan from
  there".
- **The route follower is untouched.** `World::AdvanceRoute` re-plans on a version change, on
  arrival at an incomplete route's end, and on deviation. Those three rules are load-bearing below.
- **Nothing mobile is ever an obstacle.** Ships route around architecture and *avoid* each other,
  and keeping the two apart is what keeps both small.
- **Determinism is total.** Same static set, same endpoints, same route, on every machine.

---

## 3. The shape: islands of architecture, on a lattice fixed to the world

Two decisions, and they are independent.

### 3.1 The lattice is anchored to the sector grid, not to the obstacles

A cell's identity becomes a pure function of a position:

```
cellX = sectorX * (SECTOR_SIZE_METRES / PATH_CELL_SIZE_METRES) + floor(localX / PATH_CELL_SIZE_METRES)
```

`IsSectorAlignedCellSize` already guarantees the division is exact — `SimTuning.h` asserts it, and
8,192 / 32 = 256 exactly — so a cell never straddles a sector boundary and the index is exact in
`std::int64_t` across the whole `WorldPos` range.

A grid then stores a **window** onto that lattice: an origin cell and a width and height in cells.
Building a grid never chooses where the cells are, only which of them it holds. §1.3 goes away by
construction, and two grids that overlap agree about every cell they share — which is what makes
more than one grid possible at all.

This is worth landing on its own, before anything below it. It changes no behaviour that anyone can
observe today and removes the hazard that would otherwise be designed on top of.

### 3.2 Obstacles cluster into islands, and the rule is "can a ship fly between them"

Two obstacles belong to the same island when the gap between their surfaces is narrower than the
widest hull that might need to pass, plus its clearance margin:

```
sameIsland(a, b)  ⟺  distance(a.pos, b.pos) - a.radius - b.radius  <  ISLAND_GAP_METRES
```

with `ISLAND_GAP_METRES` derived, not invented: it is
`2 × (largest hull bounding radius + PATH_CLEARANCE_MARGIN_METRES)`, because a gap wider than that
is a gap a ship can be routed through by the straight-line test alone, and one narrower is a wall
that A\* has to find its way around.

The relation is symmetric and the islands are its connected components — union-find over the static
store. **The partition does not depend on iteration order**, which matters here more than it looks:
the static store is keyed by `ShipId`, and `ShipId`s move under swap-and-pop (ADR 0005). Only the
*order* of the islands could depend on that, so islands are sorted by the lowest cell index among
their members, which is a world coordinate and moves for nobody.

### 3.3 One grid per island, sized to the island

Each island gets a grid over its own bounding box plus `PATH_GRID_MARGIN_METRES`, snapped outward to
the lattice of §3.1. The existing `PATH_GRID_MAX_CELLS_PER_AXIS` stays as a **per-island** ceiling,
where it is now reachable only by a single island genuinely 16 km across — an object no content in
the tree can build.

| Island | Extent | Cells | Clearance field |
|---|---|---|---|
| One Structure, alone | 1,526 m | 48 × 48 = 2,304 | 9.0 kB |
| One Stargate, alone | 1,287 m | 41 × 41 = 1,681 | 6.6 kB |
| Five stations over 2 km | 3,024 m | 95 × 95 = 9,025 | 35.3 kB |
| The per-island ceiling | 16,384 m | 513 × 513 = 263,169 | 1,028 kB |

A hundred separate Structures is 900 kB and a hundred rebuilds of 2,304 cells each. Today it is one
grid that refuses to build.

### 3.4 Routing: within an island, and between them

`FindPath` becomes three cases, and the third is the interesting one.

1. **`IsClearBetween(from, to)` succeeds.** One waypoint, the destination. This is most orders and
   it is what happens today.
2. **The straight line enters exactly one island.** A\* in that island's grid, string-pulled, exactly
   as today. `reachesDestination` is true.
3. **The straight line enters more than one.** Plan through the **first** island the line enters,
   to a point on its far side, and return `false`.

Case 3 needs no new machinery, because the follower already has it. `World::AdvanceRoute` re-plans
when it arrives at the end of a route whose `reachesDestination` is false. So a ship crossing three
islands plans through the first, flies it, re-plans through the second on arrival, and so on. The
stitching is incremental and it is the behaviour that already ships — the route follower was written
for a route too long for `MAX_PATH_WAYPOINTS`, and a route across islands is the same shape.

What this gives up is global optimality: a ship cannot see that going *round* island two is cheaper
than through it until it gets there. That is the right trade at this scale — the alternative is a
portal graph (§8) whose whole purpose is answering a question no content in this tree asks yet.

---

## 4. Cost

| | Today | With islands |
|---|---|---|
| Rebuild when one Structure moves, 30 stations in the world | 7.9 M evaluations, whole grid | 2,304 evaluations, one island |
| Rebuild when architecture spreads past 16.4 km | the grid declines; A\* off worldwide | each island builds; A\* everywhere |
| Resident clearance field, 30 scattered stations | 1,028 kB (or nothing) | ~270 kB |
| A\* search space | the whole world's box | one island's box |

Only the islands whose membership changed rebuild. An island is dirty when an obstacle enters it,
leaves it, or moves — which is the cadence slice 13 already established for the static store.

---

## 5. Determinism, restated as rules

1. A cell's world position is a function of the position alone (§3.1) — never of the obstacle set.
2. The island partition is a function of the obstacle set alone; island *order* is by lowest cell
   index, so it cannot follow a `ShipId` (§3.2).
3. A\* keeps its total `(f, g, cellIndex)` tie-break, with `cellIndex` now the global lattice index
   rather than a per-grid one — which is what makes it total *across* grids as well as within one.
4. `PATH_CELL_SIZE_METRES`, `PATH_CLEARANCE_MARGIN_METRES` and the new `ISLAND_GAP_METRES` are all in
   the replay contract, and `SimTuning.h` says so where they are defined.

---

## 6. What this deliberately does not do

- **Portals or a hierarchical graph (HPA\*).** §3.4 gives up global optimality across islands and
  buys the whole design's simplicity with it. A portal graph is what to build the day a route
  *between* islands is genuinely constrained — which needs content this tree cannot author.
- **Flow fields.** Still the right tool when hundreds of ships share one goal, and still landable
  behind the same waypoint seam. `Design/Archive/Collision.md` §12's reasoning is unchanged.
- **Dynamic obstacles.** Nothing mobile is ever an obstacle. Unchanged, and load-bearing.
- **Station interiors.** Routing *around* an island assumes an island has an outside. A station with
  an interior breaks that, and breaks the tangent-visibility alternative just as hard; it needs a
  navmesh and is a different design.
- **Coarsening cells to fit.** Cell size is in the replay contract, so coarsening under pressure
  would change recorded outcomes as a side effect of where somebody put a building. The existing
  comment makes this argument and it survives intact.
- **Evicting cold islands.** ~270 kB for a scattered world does not need a cache. If a world ever
  makes it need one, the accessor is the place to put it.

---

## 7. Alternatives, and why each lost

| Option | Why not |
|---|---|
| **One grid per sector**, built lazily where obstacles are | The natural unit, and it fixes §1.1 and §1.3 — but a sector is 256 × 256 = 65,536 cells and 256 kB whether it holds one Structure or none of it. A hundred populated sectors is 26 MB to say "open" almost everywhere. Islands give the same lattice guarantee and size themselves to the content. |
| **Keep one grid, raise the cap** | The cap is not the problem; the box is. A world 100 km across is 3,125 cells per axis and 39 MB, nearly all of it empty space between outposts. |
| **Keep one grid, coarsen cells to fit** | Changes recorded outcomes as a side effect of content placement (§6). |
| **Tangent-visibility graph over inflated discs** | Prettier paths for sparse convex obstacles, and structurally cannot handle a concave island or an interior. `Design/Archive/Collision.md` §12 turned this down once already; islands do not revive it. |
| **HPA\* with portals between clusters** | The right answer when inter-cluster space is constrained. Here it is open, so the portal graph would be a complete graph over islands and would earn nothing (§6). |

---

## 8. Risks

- **An island that spans the ceiling.** Content could, in principle, chain stations across 16 km at
  gaps narrower than `ISLAND_GAP_METRES` and produce one island that declines to build. The failure
  is now local — that island's routes degrade, the rest of the world is unaffected — but it should
  be *reported* rather than silent, which is a diagnostic the current code does not have either.
- **`ISLAND_GAP_METRES` is derived from the hull table.** Add a hull larger than a Carrier and the
  partition changes, which changes routes. That is correct and it is also a replay-contract change;
  it belongs in the same place the other derived hull constants are already checked.
- **Case 3 re-plans on arrival**, so a ship crossing many islands plans many times. Each plan is
  small, and the follower already re-plans for two other reasons; but a long crossing is now a
  sequence of A\* runs where it used to be one, and the benchmark should say so.

---

## 9. Slices

Four, in dependency order. The first changes no observable behaviour and is worth landing alone.

| # | What | Layer | Size | Depends on | Record | State |
|---|---|---|---|---|---|---|
| 1 | The lattice is fixed to the world: global cell indices, grids as windows | `GameLogic` | S | — | — | landed |
| 2 | Islands: cluster the static set, one grid each, `FindPath` picks the island | `GameLogic` | M | 1 | ADR | |
| 3 | Crossing islands: case 3, and the diagnostic for an island that declines | `GameLogic` | S | 2 | — | |
| 4 | Dirty-island rebuilds and the benchmark row that shows the cost | `GameLogic` | S | 2 | — | |

**Slice 1 — the fixed lattice. Landed.** `PathCellX`/`PathCellZ`/`PathCellCentre` in `PathGrid.h`
are the lattice, and `PathGrid` holds a window on it: `m_origin` is gone and `m_originCellX`/
`m_originCellZ` replace it, so a build chooses which cells it holds and never where they are.
`ClearanceAt` and `FindPath` index by integer cell rather than by a float offset from a moving
origin, which also retired the half-cell tolerance the old bounds test needed.

Landed with two rows. `ADistantObstacleDoesNotMoveTheCells` builds one Structure, reads the
clearance under a fixed probe, adds a rock 4 km west and reads it again; the two must be *exactly*
equal, and before this they were 136.5 m and 160.6 m. `ACellIndexIsAFunctionOfThePositionAlone`
round-trips an index through its centre either side of the universe origin and at a sector join,
which is where the floor division is the thing that can be wrong. The other eighty-three GameLogic
test methods pass unchanged.

**Slice 2 — islands.** Union-find over the static store, one `PathGrid` per island, and a `PathGrids`
owner that `World` holds instead of a single grid. `FindPath` chooses the island the straight line
first enters. Acceptance: two stations 20 km apart both route correctly, which is the case that
turns A\* off worldwide today; the replay gate green; the agreement test unchanged.

**Slice 3 — crossing.** Case 3 and the partial route. Acceptance: a route across three islands
arrives, in more than one plan; an island at the ceiling declines and traces, and its neighbours
still route.

**Slice 4 — dirty islands.** Only islands whose membership changed rebuild. Acceptance: a benchmark
row showing rebuild cost against island count and obstacle count, beside the 7.9 M figure in §1.2.
