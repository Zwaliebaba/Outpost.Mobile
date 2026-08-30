# 0009 — A snapshot carries a view record, not the simulation's ship

Status: accepted
Date: 2026-08-29

## Context

`ShipState` carries everything the simulation advances, and its own comment says "there is nothing
in it a renderer needs that a snapshot could not carry over a wire". When slice 2b came to define
what a snapshot actually contains, the cheapest thing was to send the struct: it cannot omit a field
the view turns out to need, and it needs no decision.

Four of its fields are not about where a ship *is*. `steerTargetPos`, `orderFacingRad`,
`orderHasFacing` and `avoidHeadingRad` are about where it is *going* — the current waypoint, the
ordered facing, and the heading the avoidance pass committed to for the next tick. Together they
tell whoever holds them what every ship on the field intends to do before it does it.

Auditing what the client actually reads settled the question on its own: of `ShipState`'s fields,
the renderer and HUD touch nine, and none of the four is among them.

## Decision

The snapshot carries `ShipSnapshot`: a `ShipHandle`, the two positions and two headings the view
interpolates between, speed, acceleration sample, turn rate, order state and hull id. The four
intent fields stay on the server. Adding a field to `ShipState` does not add it to the wire.

## Alternatives considered

- **Send `ShipState` verbatim.** Simpler, and impossible to get wrong by omission. Rejected: it
  makes "what the client may know" whatever a struct happens to hold, so the boundary moves whenever
  someone adds a field, silently and without review. It also broadcasts intent, which is the
  ingredient a cheating client wants most.
- **Send `ShipState` now, trim it in slice 6.** Rejected: interest management is about *which*
  entities are sent, not which fields, so the trim would have no natural home there. A field that
  has been on the wire for three slices is also a field something has started depending on.
- **Send the whole struct but redact the four fields to zero.** Rejected as the worst of both: the
  same bytes on the wire, plus a decoder that has to know which zeroes are real.

## Consequences

- The record is about 81 bytes against `ShipState`'s 120, so a datagram holds 13 ships rather than
  nine. That is a side effect, not the reason.
- Client-side prediction, when it is designed, cannot use the server's chosen avoidance heading and
  will have to re-derive intent from what it can see. `Design/Archive/Collision.md` §10 already argues
  avoidance should be server-only and unpredicted, so this is consistent with where that is heading
  — but it is a constraint that decision now inherits rather than chooses.
- Anything the view starts needing has to be added deliberately, in this file, with a reason. That
  is the cost and it is the point.
