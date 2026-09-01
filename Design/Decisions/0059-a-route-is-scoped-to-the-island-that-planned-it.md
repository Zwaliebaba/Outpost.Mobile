# 0059 — A route is scoped to the island that planned it

Status: accepted
Date: 2026-09-01

Supersedes [0034](0034-a-routes-version-is-the-whole-worlds.md).

## Context

ADR 0034 gave `PathIslands` **one version for the whole universe**, so any change to architecture
anywhere re-plans every routed ship. It said plainly why: an island is a connected component
recomputed on every change and numbered by where it sits, so a `Route` holding an island *index* is
holding a handle that goes stale silently and comes to name a different island — the `ShipId`
failure of ADR 0005, with a ship flying confidently into a station as its symptom.

It also named the design that eventually wins — key an island by its lowest occupied path cell,
which is world-fixed and survives a repartition — and declined to build it:

> It is worth doing on top of a measured cost rather than instead of one.

The measured cost arrived with the galaxy. One system was fine; fifty-four is the bite that record
predicted. Building a station in one system re-plans every routed ship in the shard, in fifty-three
systems that cannot see it and are not affected by it.

## Decision

**A `Route` carries a `PlanStamp`: the key of the island that planned it, that island's version, and
a flag saying whether a single island owns the plan at all.** `PathIslands::IsStampCurrent` answers
whether the plan still stands.

- **The key is the island's lowest occupied path cell**, exactly as ADR 0034 proposed. `Partition`
  already computed it, to order the islands; it is now also their name.
- **Each island carries its own version**, taken from the universe's monotone counter at the rebuild
  that produced it. An island handed the obstacles it already held keeps its grid *and* its version.
- **A plan is scoped only when the run met exactly one island.** A run that met none depends on the
  *absence* of architecture along it; a run that met several is aimed at the open water between two
  of them. Both are checked against the whole partition, as before.

The two failure modes ADR 0034 named are each handled by one half of the stamp: an island that
vanished or merged upward loses its key; an island that *grew* keeps its key and fails on its
version, because growing means it was rebuilt.

## Alternatives considered

- **Leave it global (ADR 0034 standing).** Rejected because the cost is now real and measured, which
  is the exact condition that record set for revisiting itself.
- **Key by island index.** Rejected for ADR 0034's reason, unchanged and still correct.
- **Scope a multi-island plan to the first island.** Tempting: multi-island routes re-plan on arrival
  anyway, so the window is short. Rejected because it is wrong rather than merely optimistic — the
  first leg is aimed at the midpoint of the gap between the first island and the *next*, so it
  depends on where both sit. Measured: the mutation that scopes it breaks two rows that predate this
  slice.
- **Track which islands a plan's corridor passes near, and scope to that set.** Precise, and it
  makes multi-island routes local too. Rejected as more machinery than the case is worth: those
  routes re-plan on arrival by construction, so the global check costs them a plan they were about to
  make anyway.
- **Version per island keyed by content hash rather than by cell.** Would catch a grown island
  without a separate version. Rejected: two islands with identical architecture in different places
  would collide, and the cell key is already there and already unique.

## Consequences

- **Building in one system no longer re-plans routes in another**, which is what the record ADR 0034
  wrote down as its own cost. `PathfindingTests::ArchitectureInOneSystemReplansNoRouteInAnother`
  measures it through `RoutePlanCount` rather than asserting it in a comment.
- The save format moves to 7: a route now stores a key and a flag beside the currency relation. The
  key is written as it stands, because a cell index means the same thing in every process — the
  second thing keying islands by cell buys, after surviving a repartition.
- `IsStampCurrent` fails closed. An unknown key re-plans, which costs a route rather than flying a
  ship into a station.
- **Unscoped plans are unchanged.** An open-water run still re-plans when anything is built anywhere.
  That is conservative and deliberately so: it is the smaller set, and the alternative needs to know
  which islands a corridor passes near.
- ADR 0034's closing paragraph asked for "a replan count beside slice 4's rebuild figures, because
  the argument for keying islands by cell is a number and not a feeling". That number is the row
  above, and this record is what it bought.
