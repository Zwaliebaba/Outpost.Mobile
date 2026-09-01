# 0034 — A route's version is the whole world's, not its island's

Status: superseded by [0059](0059-a-route-is-scoped-to-the-island-that-planned-it.md)
Date: 2026-08-30

## Context

ADR 0033 made the world's architecture a set of islands with one `PathGrid` over each. A route is
now planned inside one island, and a route that crosses several is planned inside the first one and
finished later.

`Route` carries a `gridVersion`, and `World::AdvanceRoute` re-plans when it stops matching. That
number was the single grid's; with islands there is a choice about what it should be, and the
choice decides how much of the fleet a change costs.

The obvious answer is per-island: give each `PathGrid` its own version, have a `Route` remember
which island it was planned in, and re-plan only the ships whose island moved. A station built in
one corner of the world would then not touch a route planned in the other, which is the whole point
of being regional.

It does not survive contact with the partition. An island is not a thing with an identity — it is a
connected component recomputed from scratch on every change, and the islands are numbered by where
they sit in the world (0033), so building anything anywhere can renumber every island after it. A
`Route` holding an island index is holding a handle that goes stale silently and points at a
different island, which is exactly the failure ADR 0005 exists to state about `ShipId`. A route
planned in island 2 and checked against island 2's version after a repartition is a route checked
against somebody else's architecture, and the symptom is a ship flying confidently into a station.

The fix for that is island identity that survives a repartition — a key derived from the island's
lowest cell, say, since that is already how they are ordered. It is a real design and it is not
this slice's.

## Decision

`PathIslands` keeps **one version for the whole world**, bumped when the obstacle set changes and
only then. Every routed ship in the world re-plans when any architecture anywhere changes.

Islands are not versioned individually and a `Route` does not remember which island planned it.

## Alternatives considered

- **A version per island, and a `Route` that remembers its island index.** The version becomes
  regional and a distant change costs nothing. Rejected: an island index is not a handle. The
  partition is rebuilt and reordered on every change, so the index a route is holding refers to a
  different island afterwards, and the check silently passes against the wrong architecture — the
  `ShipId` failure of ADR 0005 with a worse symptom.
- **A version per island, keyed by the island's lowest path cell rather than its index.** The key is
  world-fixed and does survive a repartition, so this is the design that eventually wins. Rejected
  for now because it needs an island to be a durable thing rather than a slice of an array: a key
  that is *absent* after a change has to mean "re-plan", a key that merged with another has to mean
  "re-plan", and getting those two wrong fails silently in the same way. It is worth doing on top of
  a measured cost rather than instead of one.
- **Re-plan every route every tick and drop the version entirely.** No staleness to get wrong.
  Rejected on cost, which was the original reason the version exists: a plan is a pure function of
  the static set and the two endpoints, so re-running it per tick costs everything and changes
  nothing.

## Consequences

**A change to the architecture re-plans every routed ship in the world.** That is the same
behaviour as before islands, so nothing regressed — but it is the one place islands did *not* make
the world local, and it will be the next thing to bite at MMO churn. Building or destroying a
station in a busy system is a fleet-wide replan.

The rebuild was all-or-nothing when this was written: an obstacle set that changed rebuilt every
island's grid, not just the islands whose membership moved. That was this design's own slice 4
(`Design/Archive/RegionalPathfinding.md` §9), a separate cost from this one — slice 4 makes the
*rebuild* local, and this record is about what makes the *replan* global.

> **Slice 4 landed on 2026-08-30**, which discharges the two paragraphs above and sharpens the one
> below. A rebuild now claims the grid of any island handed exactly the obstacles it already holds,
> matched by content rather than by slot for the same reason this record gives: an island index is
> not a handle. `PathGrid`'s own version and unchanged-obstacles gate are what recognise that, so
> they are read now rather than merely kept. The numbers are in that design's §4: a hundred
> scattered stations cost 1.575 ms for a whole rebuild and 0.064 ms when one moves, and the
> evaluations go from the review's 7.9 M worst legal case to 2,304. **The replan is still global**,
> which is what this record decided and what remains true — the rebuild being local is what makes
> that the next thing worth measuring rather than the only thing.

`PathGrid` kept its own version and its own unchanged-obstacles gate through slice 2 even though
nothing read them through `PathIslands` then. They are what slice 4 needed to recognise an island
that did not change, and deleting them to add them back would have been churn.

The measurement this wants is a replan count beside slice 4's rebuild figures, because the argument
for keying islands by cell is a number and not a feeling. Slice 4 supplied the rebuild half.

---

> **Superseded on 2026-09-01 by [ADR 0059](0059-a-route-is-scoped-to-the-island-that-planned-it.md).**
>
> The measurement this record asked for in its last paragraph is what superseded it. A galaxy of
> fifty-four systems made "building anywhere re-plans everything" the cost this record predicted,
> and the design it named as eventually winning — key an island by its lowest occupied path cell —
> is the one that landed. Its two named failure modes are each handled by half of the stamp a route
> now carries: an island that vanished or merged upward loses its key, and one that grew keeps its
> key and fails on its version.
>
> Everything above about **why an island index is not a handle** is unchanged and is why 0059 keys
> by cell rather than by slot.

