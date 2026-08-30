# 0034 — A route's version is the whole world's, not its island's

Status: accepted
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

The rebuild is likewise all-or-nothing for now: an obstacle set that changed rebuilds every island's
grid, not just the islands whose membership moved. That is this design's own slice 4
(`Design/RegionalPathfinding.md` §9), and it is a separate cost from this one — slice 4 makes the
*rebuild* local, and this record is about what makes the *replan* global.

`PathGrid` keeps its own version and its own unchanged-obstacles gate even though nothing reads
them through `PathIslands` today. They are what slice 4 needs to recognise an island that did not
change, and deleting them now to add them back then would be churn.

The measurement this wants is the one slice 4 already owes: rebuild cost against island count and
obstacle count. A replan count belongs beside it, because the argument for keying islands by cell
is a number and not a feeling.
