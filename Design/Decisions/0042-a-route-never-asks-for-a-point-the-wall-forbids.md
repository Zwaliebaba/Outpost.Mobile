# 0042 — A route never asks for a point the wall forbids

Status: accepted
Date: 2026-08-30

## Context

A ship pressed against a Vanguard station stopped answering orders: full thrust, nose in, no turn.
The owner's rule on seeing it — *we can never have a ship getting stuck* — is a property the tree
had assumed rather than stated. `Design/Archive/Collision.md` made ships arrive without passing
through anything and `Design/Archive/RegionalPathfinding.md` made them route around it; neither
said what happens when the point a ship is steering at is one the wall will not let it reach.

Two such points existed. The router produced one itself: `PathGrid::BlockedAlong` sampled a run
from its own start, so a ship standing in a wall-band cell had every run blocked at zero and the
string-pull anchored on the ship's own cell centre, a few metres inside the band. The player
produces the other with a tap on a station, which is an order into the middle of it; the router
answers "as close as the geometry allows" in a comment and then appends the unreachable point as
the final waypoint anyway.

The first idea was reverse thrust — a hull that backs off when it has been blocked for a while, which
is what a player would do by hand. This record is about why that was not built.

## Decision

**A run's own start cell is exempt from the clearance test**, in `BlockedAlong`, exactly as
`FindPath` already exempts it for the cell a ship leaves. A route from a wall starts at the first
clear cell, never at the cell the ship is in.

**A waypoint the wall will not allow is taken as reached.** `World::Route` counts consecutive ticks
in which the blocking pass pushed the ship away from its own steer target; at
`BLOCKED_WAYPOINT_TICKS` the waypoint is treated as arrived at — the next one becomes the leg, or
the order completes where the ship stands. No new steering mode, no new state on the wire, and the
rule is stated as tests rather than as a hope.

## Alternatives considered

- **Reverse thrust when blocked.** Natural, and it would have freed the ship the owner saw. Rejected
  because the tests showed the turn was never the problem: `IntegrateShip` turns a hull at any
  speed, and a hull on the skin with a route that begins at a clear cell turns and leaves. A
  reverse mode is a second motion model — a second thing the avoidance fan, the limiter, the
  replay contract and every future behavior would have to know — bought to hide a router defect
  that had to be fixed anyway. If a case ever needs it, this record is the place to argue it from.
- **Clamping an order's destination to the nearest reachable point at order time.** Cleaner for the
  station tap, but it fixes one producer of unreachable points and not the class; the router's own
  defect was not an order-time problem. The blocked-waypoint rule covers both and whatever comes
  next (a user station with a shape the collision circle does not describe, a waypoint the grid
  moved under).
- **Making `BlockedAlong` exempt the start *point* rather than the start cell.** Insufficient: the
  sample half a cell along the run is in the same band, and the anchor lands on it instead.

## Consequences

- A ship may now end an order without reaching its point, deterministically, one second after the
  wall says no. That is a change to where orders end and `BLOCKED_WAYPOINT_TICKS` is in the
  contract for it.
- `Route` carries one more field, repaired with the rest by swap-and-pop; nothing on the wire
  changes, and a clear sky is bit-identical: the counter is zero on every tick that is not blocked.
- The station tap's walled-in order ends at the skin. Stations slice 6 turns that tap into a dock
  order whose approach point is outside the skin by construction, so the common trigger disappears;
  the move-order path stays and stays covered.
- `AShipPressedAgainstAStationBacksOffAndTurns` keeps its name as a record of the hypothesis it
  refuted.
