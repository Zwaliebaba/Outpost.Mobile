# Universe slice 1 — the galaxy from one seed

Work order for slice 1 of [`Universe.md`](Universe.md). One branch, one pull request.

## 1. Scope

`GameLogic` gains the galaxy: a pure function of one seed producing where the systems are, what
seed each of them grows from, and which of them are joined by gates.

- **`GalaxyLayout.h`/`.cpp`** — `SystemSite`, `GateLink`, `SystemPin`, `GalaxyDesc`,
  `GalaxyLayout`, and:
  - `LayOutGalaxy(seed, origin, desc, pins)` — the lattice, the walk, the pins, the gate graph;
  - `LayOutGalaxySystem(site, desc, pins)` — one system's own layout, drawn from its own seed.
- **`UniverseLayout.h`/`.cpp`** — the planet loop is extracted to `LayOutPlanets`, over a
  generator the caller owns. `LayOutSystem` keeps its signature, its meaning and its output, and
  becomes that function with a generator seeded from its own argument.
- **`GameLogic.h`** — the umbrella gains `GalaxyLayout.h`.
- **`GameLogicTests`** — `GalaxyLayoutTests.cpp`.
- **A decision record** for the two decisions this slice takes: the galaxy is one seed and a pin
  table, and the gate graph is the relative neighborhood graph.

## 2. Out of scope

Nothing outside `GameLogic` and its suite. Specifically **not** in this slice:

- gates as ships, the gate table, the `Jump` order, `JumpedOut` — all slice 2;
- any composition root change: nothing calls `LayOutGalaxy` yet (slice 3);
- any client change, any wire change, any change to `Universe` — slice 1 adds no field to
  `Universe` and therefore touches neither the state codec nor the replay contract;
- the save file (slice 5) and the island-scoped replan (slice 6).

## 3. What to build on

- `UniverseLayout.h`/`.cpp` — `PlanetSite`, `SystemLayout`, `SystemDesc`, `LayOutSystem`, and
  `PLANET_BEARING_JITTER`. Its pin discipline is the one this slice repeats one level up.
- `NeuronCore/Pcg32.h` — `Float01`, `Signed`, `Below`, `Next`. The tree's one generator (ADR 0012).
- `UniversePos.h` — `LocalPos`, `Translate`, `OffsetX`/`OffsetZ`, `DistanceSquared`. Every position
  is built through `Translate` so the sector invariant holds (the rule `LayOutSystem` already
  follows).
- `SimTuning.h` — `SECTOR_SIZE_METRES`, and the path grid's ceiling constants the separation bound
  is argued against.
- `UniverseLayoutTests.cpp` — the test shapes this suite already uses for a seeded layout:
  determinism over two calls, adjacency of seeds, the pin leaving the rest alone, and a bound
  proved over the construction rather than sampled.

## 4. How it must behave

1. **The walk is one fixed order.** Ring 0 outward; each ring entered at `(k, 0)` and traversed
   in one fixed direction order. One `Pcg32(galaxySeed)` serves the whole walk.
2. **Every cell takes the same draws, occupied or not, pinned or not** — occupancy, jitter x,
   jitter z, system seed. A pin overwrites what was drawn and never skips a draw.
3. **Raising the density never moves or removes a system** that a lower density already had.
4. **Jitter is bounded** to `cellJitter` of the pitch per axis, so minimum star separation is
   `(1 - 2 * cellJitter) * pitch` by construction. No rejection loop anywhere.
5. **The gate graph is the relative neighborhood graph**: A and B are linked unless some third
   system C satisfies `max(d(A,C), d(B,C)) < d(A,B)`. Links are emitted with `systemA < systemB`,
   in ascending order, so the list is a function of the layout and not of the loop.
6. **A pinned system takes its authored description**; an unpinned one draws its planet count from
   its own seed and then lays its planets out through the shared loop, over the same generator.

## 5. Acceptance

Every row is a `GameLogicTests` test method unless it says otherwise.

- `TheGalaxyIsAFunctionOfItsSeed` — one seed, two calls, every field equal; adjacent seeds differ.
- `RaisingTheDensityLeavesTheSurvivorsAlone` — over a sweep of densities, every system present at
  the lower density is present at the higher one, at the same position, with the same seed.
- `APinnedSystemHoldsItsCellAndSeed` — the pinned cell is occupied at any density, sits exactly on
  its lattice point, carries its authored seed; and no unpinned system moves when the pin is added.
- `SystemsKeepTheirDistance` — over many seeds, no two stars closer than the construction's bound.
- `TheGateGraphConnectsEverySystem` — union-find over the links, every seed tested, one component.
- `TheGateGraphIsSymmetricAndUnique` — no self-links, no duplicates, `systemA < systemB`, ascending.
- `EverySystemFitsItsOwnPathIsland` — a system's own worst-case span is inside the grid ceiling,
  and the gap between two systems is wider than the widest island a system can make, so islands
  cannot merge across systems.
- `AGalaxyHoldsTheSectorInvariant` — every star position's local offsets are inside their sector,
  including a galaxy anchored one metre inside a sector's far corner.
- `AnUnpinnedSystemDrawsItsOwnPlanets` — planet count inside the configured band, planets inside
  their orbit and radius bands, and the same site laid out twice is the same system.
- `TheSharedPlanetLoopDidNotMoveTheShippedSystem` — `LayOutSystem` with the shipped seed and
  `SystemDesc` defaults produces what it produced before the extraction. **The regression guard on
  the refactor**: the existing `UniverseLayoutTests` rows must also stay green, unchanged.

Plus: `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass, and the new files
are registered in both the `.vcxproj` and the `.filters`.

## 6. Assumptions the implementer may make

- **Nothing calls `LayOutGalaxy` yet.** Slice 1 ships the function and its suite; the composition
  root keeps calling `LayOutSystem` exactly as it does today, so the game boots unchanged and the
  screenshots of this slice are the screenshots of the last one.
- **The census is not tuned here.** `GalaxyDesc`'s defaults are the shipped numbers of
  `Universe.md` §10, and what they come to for the shipped seed is measured and stated in the pull
  request rather than argued in advance.
- **`Debug|x64` cannot be built in the container this is written in** — no Windows, no MSVC. What
  stands in is stated in the pull request rather than implied: the algorithm compiled and run
  out-of-tree against the tree's own `Pcg32.h`, with every acceptance property above checked by
  the same assertions the suite makes.

---

## 7. What changed on contact

Append-only, per `Design/README.md`: the design is amended in place, and this is where the history
of the amendment lives.

**The separation bound was wrong, and the first run of the suite said so.** `Universe.md` §3.4
specified 0.30 jitter and proved `(1 - 2 * jitter) * pitch` = 52 428.8 m. That arithmetic belongs
to a *disc*, and the jitter drawn here is a *square* — two independent axes, which is the cheap and
deterministic way to draw one — so a star's worst displacement is its diagonal,
`jitter * sqrt(2) * pitch`, and the bound the shipped numbers actually held was 19 851 m.
`SystemsKeepTheirDistance` failed on its first run against a measured closest pair of 38 715.8 m.

Three things were considered and one was taken. Drawing the jitter on a disc would need either a
rejection loop — the thing every layout in this tree avoids, because an iteration count that depends
on the draw makes the layout depend on how many times it rolled — or a square-root mapping that
makes the constant mean something other than what it says. Restating the design's number as
19 851 m would have been honest and would have thrown away most of the margin the pitch was chosen
for. **What was taken: the formula was corrected to `(1 - 2 * sqrt(2) * jitter) * pitch` and put in
one function, `MinimumStarSeparationMetres`, and the shipped jitter moved from 0.30 to 0.20** — so
the bound is 56 926.5 m, which is more than the design asked for, and the number now lives where
the test that proves it can read it. `Universe.md` §3.4 and §10 say this, and ADR 0055's
consequences carry the reason.

**The suite gained a row nothing predicted: `ATieLeavesTheLinkAlone`.** Mutation testing (below)
found that changing the blocking rule's `<` to `<=` did not fail a single assertion — a jittered
lattice never produces an exact distance tie, so nothing in the galaxy rows could reach the case.
It is reachable by hand, and it matters: on a 36-48-60 triangle scaled to whole metres, blocking on
a tie cuts two of three links and strands a system. That row is why `LinkGates` is exposed beside
`LayOutGalaxy` rather than hidden inside it — a test has to be able to put systems where it wants
them and ask what the rule says.

**`SystemSite` carries its lattice cell**, which the order did not ask for. A cell is the one name
for a system that survives a density change; an index into the systems array does not. A save file
and a shard handoff will both want that name, and recovering it afterwards would be arithmetic
against the jitter.

**Two constants are spelled as literals rather than computed** — `HEX_ROW_SPACING` (sqrt(3)/2) and
`ROOT_TWO`. Both decide where every system in the galaxy is, and a value that came out of a
`std::sqrt` at boot would make that depend on a library rather than on the file.

## 8. What was verified, and how

**Not built in `Debug|x64`, because this container has no Windows, no MSVC and no Windows SDK.**
Stated plainly rather than implied, as §6 said it would be. What was done instead:

- **The real sources compiled**, `GameLogic/UniverseLayout.cpp` and `GameLogic/GalaxyLayout.cpp`,
  with `g++ -std=c++20 -Wall -Wextra`, clean and warning-free. Only `pch.h` and `<DirectXMath.h>`
  were shimmed — `Pcg32.h`, `UniversePos.h` and `SimTuning.h` are the tree's own.
- **`Tests/GameLogicTests/GalaxyLayoutTests.cpp` — the file this slice commits — was compiled and
  run**, against a small stand-in for the VS assertion macros: all twelve rows pass, 80 757
  assertions.
- **The regression guard ran too**: `UniverseLayoutTests.cpp`, untouched, passes against the
  refactored loop — 2 329 assertions — and `TheSharedPlanetLoopDidNotMoveTheShippedSystem` pins the
  shipped system's three planets to orbits, radii and body seeds captured from the *pre-refactor*
  binary.
- **The suite was measured against itself.** Nine deliberate defects were introduced one at a time
  and the suite re-run: a pinned cell skipping its draws, an unoccupied cell skipping its jitter and
  seed, the ring-step table reordered, a pinned system taking the jitter, links emitted high index
  first, the blocking test's `<` weakened to `<=`, `max` weakened to `min`, the separation bound
  returned to the disc arithmetic, and the squared distance dropped from double to float. **Seven
  went red immediately. Two did not, and both were holes rather than defects**: the tie, closed by
  the new row above; and the float, which is *not* closed and is documented instead — the strict
  blocking test means imprecision can only ever keep a link, never cut one, so connectivity is safe
  either way and the double buys exactness of shape rather than of correctness. `StarDistanceSquared`
  says so at the code.
- **The shipped census was measured**, not guessed: 54 systems, 68 links, 5 chokepoints, 8 jumps
  across at the widest, mean 4.07, no system with more than 4 gates, nothing unreachable.

**A reviewer on Windows should still build `Debug|x64` and run all four suites before this merges.**
Nothing visual changed, so no screenshots are owed: the composition root is untouched and the game
boots exactly as it did.
