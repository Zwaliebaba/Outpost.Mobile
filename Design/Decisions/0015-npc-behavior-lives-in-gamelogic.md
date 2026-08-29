# 0015 — NPC behavior lives in GameLogic, inside the tick

Status: accepted
Date: 2026-08-29

## Context

The hostile patrol is the first intent in the tree that does not come from a client. Something has
to decide where an enemy Interceptor goes next, and that decision is authoritative: it moves a ship
in the world every player sees.

Three places could hold it. `GameLogic`, inside `World::Step`, where the rest of the simulation
lives. `Outpost/WorldSimulation`, the executable's server-side adapter, which already drives the
world once per tick and would be the smallest change. Or a bot client on the other end of a
`Transport`, sending move orders the way the player's client does — which has the appeal of proving
the seam works.

## Decision

NPC behavior is a pass inside `World::Step`: `StepPatrols` runs first, before pass 0, in the position
an adapter's incoming orders occupy from outside, and issues its legs through the same
`PlanRoute`/`SolveOrder` machinery a player's click uses. `World` gains a `Patrol` table parallel to
`m_ships`, and `AssignPatrol` is called by whoever composes the scene.

## Alternatives considered

- **The executable's adapter.** Smallest diff, and `WorldSimulation` already stands between the host
  and the world. Rejected for the reason ADR 0008 put the wire format in `GameLogic`: the day there
  are two executables, authoritative behavior must not be in one of them. It would also put a
  load-bearing rule in the one layer with no test suite, and outside the replay contract — a
  recorded match would not reproduce, because the thing that decided half its motion was not in the
  recording.
- **A bot client over `Transport`.** Tempting as a demonstration that the seam is real, and it is
  how a *player-like* agent would have to work. Rejected: it puts fleet AI outside the replay gate,
  pays a wire hop and the configured latency for every leg, and makes the server's own NPCs depend
  on a client staying connected. No MMO server drives its own NPCs this way, and the day the halves
  separate the bot would be a third process to deploy.
- **Continuous steering at a point led around the ring** — a carrot — rather than waypoint hops
  through the order machinery. Smoother, and a genuine option. Rejected because it bypasses
  `PlanRoute`: a patrol ship shoved off the ring by traffic would steer straight at its carrot,
  potentially through the station, where the order machinery replans around it. The cost is a
  twelve-sided ring, which at 30 degrees a leg reads as a patrol pattern rather than as a defect.

## Consequences

- The pass is inside the replay contract, so its order-independence has to hold and is tested:
  `PatrolTests::TheSamePatrolProducesTheSameRun` compares two runs field for field, waypoint index
  included. It reads only its own ship and the anchor's end-of-last-tick position, and writes only
  its own ship.
- A world that assigns no patrol ticks bit-identically to one built before the pass existed — the
  pass visits nothing and a zero speed cap clamps nothing. The existing suites passing without edits
  is that claim's evidence.
- `World` gains a second array despawn must repair, beside `m_routes`, and a test that proves it
  does. That is the price of keeping the assignment off `ShipState`, whose comment promises nothing
  in it a snapshot could not carry.
- `AssignPatrol` ships to any future server binary with `World`. There is nothing to port, which is
  the whole point.
