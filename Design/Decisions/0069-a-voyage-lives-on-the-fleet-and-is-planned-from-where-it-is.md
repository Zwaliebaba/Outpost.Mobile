# 0069 — A voyage lives on the fleet, and is planned from where the fleet is

Status: accepted
Date: 2026-09-04

## Context

`Design/GalaxyMap.md` §4.3 asked for one order that sends a fleet across as many gates as the route
takes. Two things in the tree decide what shape that order can have.

The first is [ADR 0056](0056-a-jump-is-a-despawn-and-a-spawn-under-one-identity.md): a crossing is
`DespawnShip` followed by `SpawnShipAs` under the same identity. Every ship that starts a voyage is
destroyed and recreated once per gate, and its route, its docking, its duty and its mounts all come
back at their rest state — deliberately, because the far side should re-derive them. So **nothing a
ship holds can carry the rest of a journey**. The fleet row is the one thing in the simulation that
survives a crossing; `orderGate` already lives there for exactly that reason.

The second is that `Universe` had no idea a galaxy existed. `GalaxyLayout.h` said so in its opening
paragraph. Gates are rows naming each other by `EntityId`, and nothing anywhere in the simulation
could answer "which door out of this system leads toward that one" — which is the whole content of
a hop. The client holds a layout (`OutpostApp::m_galaxy`); the server-side root did not lay one out
at all, and `Publisher`, where a `FleetOrder` becomes an `IssueFleetOrder` call, holds nothing.

## Decision

**A voyage is a standing fleet order, and the universe is given the galaxy to plan it against.**

`FleetOrderKind::Voyage` is appended to the enum. A fleet holding it carries its destination in
`orderPoint` — the same field a Move aims at, because it is the same question one level up — and the
door it is flying at now in `orderGate`. `StepJumps` crosses it exactly as it crosses a Jump, and
clears the gate rather than the kind; `StepVoyages`, immediately after, names the next door or
stands the fleet down.

`Universe::ConfigureGalaxy` takes the layout from the composition root, beside `ConfigureShard` and
on its terms: a plain value, from the seed the save header already carries, never a file and never a
second layout of the universe's own. It is not saved, because it is derived from that seed and a
saved copy could disagree with the file it came from ([ADR 0057](0057-the-save-is-a-versioned-file-and-a-refused-one-stops-the-boot.md)).

**The next hop is planned from where the fleet is, every time, and no route is stored.**
`RouteAcrossGates` runs a breadth-first search over the links at each arrival and the first hop of
its answer is the door taken. `GameLogic` gains a graph search, which is a new kind of thing there —
it runs on an order and on an arrival, at human cadence, over 54 systems and 68 links.

## Alternatives considered

- **Plan the whole route at the order and store the hops on the fleet row**, which is what
  `Design/GalaxyMap.md` §6.4 specified. It needs a fixed-capacity array on the row and therefore a
  cap: the shipped galaxy's diameter is fourteen gates, measured, so any cap is close enough to the
  real number to be wrong for a differently-seeded galaxy — and a route longer than the cap is a
  destination the player can see and cannot be sent to. It also makes the row able to be *wrong*: a
  plan is a statement about where the fleet was when it was made, and a fleet reloaded from a save,
  shoved through the wrong door or re-ordered mid-voyage is somewhere else. Planning from the
  fleet's own position cannot be stale, and the work it repeats is a search over 68 links.
  §6.4 was amended in place to say what was built (ADR 0054), and the design's own argument for the
  rule — that the graph is a pure function of the layout — is exactly why the two agree wherever
  both are defined.
- **Let the client plan the route and send the hops.** The client already holds the layout and could
  walk it. It is the second opinion [ADR 0037](0037-the-universe-layout-is-static-content-in-gamelogic.md)
  exists to prevent — a client that plans is a client that can disagree with the universe about
  which doors exist — and it would make `FleetOrder` variable-length, giving up the property
  [ADR 0049](0049-orders-name-a-fleet-not-ships.md) bought: one small fixed-size message whatever
  the fleet and whatever the journey. No NPC could ever route, either.
- **Give the `Gate` row the lattice cell it leads to**, so a hop resolves by matching names instead
  of asking the layout. It is durable content and it survives a shard boundary, which the position
  test does not. It also puts a copy of the galaxy's naming inside the save format — a second place
  for the file and the seed to disagree — and it bumps the format for a field, where the layout the
  root already holds answers the same question for free. Worth revisiting the day a voyage has to
  cross shards, which is the day the position test genuinely stops working.
- **Match a hop's door by computing `GateSite` and taking the nearest gate to it.** No galaxy
  knowledge beyond the sites, but it needs a distance tolerance, and a tolerance is a number that is
  right until two roads out of one system leave on close bearings. Asking where the road *comes out*
  — the far structure's nearest star — is exact and needs no constant.
- **Keep the row on `Jump` and hold the destination in a new field.** It reads more explicitly, and
  it costs a field in the save format that says exactly what `orderPoint` already says for a Move.
  A voyage is a Move whose road runs through doors; the field is the same field.

## Consequences

- **The simulation knows a galaxy exists**, and `GalaxyLayout.h`'s opening paragraph changed to say
  so. What it knows is the sites and the links, read by `NextVoyageStep` and by nothing else: there
  is still no galaxy in the tick, no record, no collision and no cost at a system nobody is in.
- **A universe that was never given a layout refuses every voyage** with `NoRoute` and is otherwise
  unchanged. Fail-closed on purpose: a fleet that will not leave beats one sent at a door picked out
  of an empty table. Both roots configure one; `GameLogicTests` proves the refusal.
- **A voyage cannot cross a shard boundary**, and stands the fleet down in a system rather than
  stepping through. A fleet *row* does not travel in a handoff — only ships do — so a voyage that
  crossed would arrive with nothing to tell it where it was going. `GateBetween` never returns a
  door whose far side is not in this universe, which is what makes that the quiet default rather
  than a case to remember.
- **The state format moved to 10 without adding a field.** A fleet's order byte can carry a seventh
  value now, so a build that predates it refuses the fleet section of a file it would previously
  have read. That is a format change whatever the layout did, and a stamp naming two shapes would
  make every gate under it a guess ([ADR 0061](0061-the-save-is-migrated-on-read.md)).
- **A voyage is slow, and that is the design's intent** (`Design/GalaxyMap.md` §8). The fleet is out
  of interest for most of it, watched as a dot on the map and a status block that reads `VOYAGING`.
