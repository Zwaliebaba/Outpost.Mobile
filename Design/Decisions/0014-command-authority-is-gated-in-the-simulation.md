# 0014 — Command authority is gated in the simulation, not the adapter

Status: accepted
Date: 2026-08-29

## Context

`World::IssueMoveOrder` steered whatever ids it was handed, and
`WorldSimulation::ApplyIncomingOrders` handed it whatever handles arrived on the wire. With every
ship the player's own that was harmless. The first hostile makes it a client ordering the enemy's
fleet around — in one process a curiosity, over a real wire the first exploit anyone tries.

So a check has to exist. The decision is where it lives, and the two candidates are the simulation
and the host adapter that drives it.

## Decision

`World::IssueMoveOrder` takes a `FactionId _issuerFaction`, defaulted to `FACTION_PLAYER`, and drops
any ship whose `factionId` does not match — dropped exactly as a stale id already is, leaving the
rest of the order to be steered. `WorldSimulation` supplies its subscriber's faction. The simulation
refusing is a property of the simulation; an adapter refusing would be a convention.

## Alternatives considered

- **The check in `WorldSimulation::ApplyIncomingOrders`.** The obvious home: it is where untrusted
  handles are resolved, and it already drops the ones that resolve to nothing. Rejected because the
  executable's adapter has no test suite, and because it is one host of several to come — a
  dedicated server, a second adapter, a replay driver — each of which would have to remember the
  check independently. A rule that has to be remembered per host is a rule that will be missed by
  one of them.
- **Both places.** Cheap and belt-and-braces. Rejected as two statements of one rule that can
  disagree; the UI-side filter in `WorldView` is deliberately *not* this — it makes affordances
  honest (what you cannot command, you cannot select) and is not load-bearing.
- **Per-ship ownership now, rather than per-faction.** The eventual answer, since two players in one
  faction must not steer each other's ships. Deferred: it needs a second subscriber to be
  meaningful, and it is one more field and a different comparison *inside this same function*. What
  cannot be retrofitted cheaply is the existence of the gate, which is why it lands with the first
  hostile rather than with the second player.

## Consequences

- `IssueMoveOrder` grows a parameter with a default. Every existing caller means the player's own
  ships, so the default states their meaning rather than papering over it, and no call site changed.
- An order that loses every ship to the filter returns the facing it was given, exactly as an empty
  list already did. Callers cannot tell a refused order from an empty one, and nothing today needs
  to.
- The granularity is knowingly coarse. Widening it to per-player is a change to one comparison in
  one function, and the test that proves the filter is already there to be extended.
