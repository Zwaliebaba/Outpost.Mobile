# 0055 — The galaxy is one seed and a pin table, and its gates are the relative neighborhood graph

Status: accepted
Date: 2026-09-01

## Context

[`Design/Universe.md`](../Universe.md) needs the frontier to stop being one system. Two questions had
to be answered before any of it could be built, and both are the kind a reasonable person proposes
again: **where do the systems come from**, and **which of them are joined**.

The precedent for the first is one level down and points the right way. `LayOutSystem`
([ADR 0037](0037-the-universe-layout-is-static-content-in-gamelogic.md)) lays a solar system out
from a seed as a pure function, with `pinFirstPlanet` holding the one place the game's opening shot
depends on — and its consequence list already says that it takes a star position "precisely so that
a second system is content rather than redesign". This record is that day arriving.

The second question has no precedent in the tree at all. A galaxy's link graph is the map the game
is played on: it decides whether there are chokepoints worth holding, whether a fleet can be cut
off, and how far anywhere is from anywhere. It is also the one part of a procedural galaxy that can
silently produce an unplayable result — a pocket of systems nothing reaches.

## Decision

**The galaxy is one seed and a pin table** (`GameLogic/GalaxyLayout.h`, `LayOutGalaxy`).

A hex lattice of candidate cells is walked in one fixed spiral order, ring 0 outward, over one
`Neuron::Pcg32(galaxySeed)`. Every cell takes the same four draws — occupancy, jitter x, jitter z,
system seed — **occupied or not, pinned or not**, and a pin overwrites what its cell drew without
ever skipping a draw. That is `LayOutSystem`'s rule about `pinFirstPlanet`, and it buys strictly
more here than it does there: because a cell's stream belongs to the *cell* rather than to the
census, raising the density reveals cells without moving, reseeding or removing any system a lower
density already had. **A galaxy can be retuned without being rerolled**, which is what lets density
be a balance knob rather than a new universe.

Jitter is bounded, so there is no rejection loop anywhere and the minimum separation between two
stars is a property of the description rather than of the seed.

**The gate links are the relative neighborhood graph**: A and B are linked unless some third system
C is strictly closer to both, `max(d(A,C), d(B,C)) < d(A,B)`.

It is chosen for a theorem rather than for a look. **The relative neighborhood graph contains the
minimum spanning tree, so the galaxy is connected for every seed by construction** — connectivity is
proved, not sampled, and no repair pass is needed. A repair pass is what the alternatives need, and
a repair pass is a rejection loop by another name: it would make the map depend on how many times
the layout rolled.

The blocking test is **strict**, so a tie leaves the link alone. That is not a detail: a rule that
cut on a tie would strand a system in the one configuration a lattice cannot roll but a pin table
can author, and `GalaxyLayoutTests::ATieLeavesTheLinkAlone` is the row that holds it.

Both live in `GameLogic`, for ADR 0037's reason re-run rather than cited: the server half spawns a
station and a gate at every system, the client half marks them, so it is content both binaries need,
and content living in one executable is in the wrong one the day there are two.

## Alternatives considered

**For where the systems come from:**

- **Pure procedural, no pins.** Simplest, and it makes the carefully framed opening shot a die roll.
  The same argument `pinFirstPlanet` already won, one level up.
- **A fully authored universe file.** Total control, and a galaxy exactly as big as somebody writes.
  Rejected as the wrong default for a frontier; an authored *pin* gets the control where control is
  wanted and costs nothing everywhere else.
- **Square-cell lattice.** Cheaper arithmetic, and every cell has four neighbors at pitch and four
  at pitch·sqrt(2) — so a relative neighborhood graph over it is dominated by axis-aligned links and
  the map reads as a grid. A hex lattice has six neighbors all at the same distance, and no
  direction is privileged.
- **Poisson-disc scatter with no lattice.** The most organic, and it needs a rejection loop by
  construction — the thing this tree's layouts have avoided everywhere else, because an iteration
  count that depends on the draw is a layout that depends on how many times it rolled.
- **Server-authored, downloaded at join.** The eventual delivery, and not a redesign when it comes:
  ADR 0037 already established that it is the same struct arriving by download instead of by call.

**For the gate graph:**

- **Gabriel graph.** The same connectivity theorem with a denser graph — its empty-circle rule
  blocks strictly less often. Lost on gameplay rather than on mathematics: more redundant routes
  means fewer bridges, and a map with no chokepoints has nothing to hold.
- **Minimum spanning tree, plus drawn extra links.** Sparsest possible, and every skeleton link is a
  chokepoint until an extra covers it. Lost because the extras need a knob, the knob needs its own
  argument, and the result is tuned where the relative neighborhood graph is derived.
- **k-nearest neighbours.** The obvious first answer and the one that fails: it strands pockets, so
  it needs a connectivity repair pass, which is the rejection loop above.
- **Delaunay triangulation.** Connected and planar, but far denser still — nearly every system gains
  five or six gates, and the map stops having shape.

## Consequences

- **Connectivity is a theorem the suite states**, not a property somebody sampled:
  `TheGateGraphConnectsEverySystem` runs a union-find over 64 seeds, and it would still be right if
  it ran over one.
- **Density is safe to retune**, and `RaisingTheDensityLeavesTheSurvivorsAlone` is what keeps it
  that way. The day somebody changes the shipped 0.55, the systems that were there are still there,
  with the same seeds, and therefore with the same planets.
- **The separation bound is the pitch's job, and it was got wrong once.** The bound is
  `(1 - 2 * sqrt(2) * cellJitter) * pitch`, not `(1 - 2 * cellJitter) * pitch`: the jitter is a
  square of two independent draws, so a star's worst displacement is its diagonal.
  `Design/Universe.md` §3.4 stated the disc's arithmetic, the suite caught it on the first run, and
  the shipped jitter moved from 0.30 to 0.20 so the separation the design asked for still holds
  (`Design/Universe-slice-1.md` §7). `MinimumStarSeparationMetres` exists so that the number lives
  in one place and cannot drift from the test that proves it.
- **`LayOutSystem` gained a second caller and kept its meaning.** The planet loop is now
  `LayOutPlanets`, over a generator the caller owns; `LayOutSystem` is that function with a
  generator seeded from its own argument, and produces the same bytes it always did
  (`TheSharedPlanetLoopDidNotMoveTheShippedSystem`). Sharing it rather than copying it is what stops
  a galaxy's systems and the starting system meaning two different things by the same seed.
- **`GameLogic` gains one public header and the discipline that it stays position-and-seed only** —
  ADR 0037's discipline, inherited. What a planet wears is still the client's; the day this header
  names a texture it has drifted.
- **The graph costs O(n^3) at boot**, over a table in the dozens. It is stated here rather than
  discovered: at a few hundred systems this wants the standard locality pass, and the rule is local
  enough to take one without changing what it means.
- **Nothing calls any of it yet.** Slice 1 ships the function and its suite; the composition root
  still calls `LayOutSystem`, so the game boots exactly as it did. Slice 3 is where genesis moves.
