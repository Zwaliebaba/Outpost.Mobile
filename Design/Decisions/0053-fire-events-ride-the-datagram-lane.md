# 0053 — Fire events ride the datagram lane

Status: accepted
Date: 2026-09-01

## Context

ADR 0029 split this seam's two lanes with one question: *if this message is lost, does a later one
make it right?* Positions answer yes and take the datagram lane, where late is worse than lost.
Departures, orders, rosters and the ledger answer no and take the reliable lane, because each is
stated once and nothing repeats it.

Slice 1 (ADR 0052) made the simulation lethal and told nobody. Two things now have to reach a
client: **how hurt a ship is**, and **that a shot was fired**. The first is plainly state and goes
in the ship record beside the position and the faction — a lost fraction is corrected by the next
update six ticks later, which is the datagram lane's own case.

The second is the awkward one. A shot happens at an instant, and no later message restates it: by
ADR 0029's question a fire event is a departure, not a position, and belongs on the reliable lane.

## Decision

**Fire events ride the datagram lane anyway, as their own message kind, and the question ADR 0029
asks is the wrong question for them.**

That question is a proxy for the one that matters — *what breaks if this is lost?* — and it is an
excellent proxy for every message on this seam except this one. A fire event's only consumers are a
muzzle flash, a tracer and a turret slew. Every authoritative consequence of the shot travels
somewhere else and reliably: the damage as a fraction in the ship record, the death in a leave run,
the standings flip and the roused fleet as simulation state neither end can disagree about. So a
lost flash is not a lie about the world — it is one tracer a player never saw in a battle full of
them — while a *late* one draws a line between two ships that have both since moved, which is worse
than drawing nothing.

It is its own message (`KIND_FIRE`) rather than a block in the fragment header, where
`Design/Combat.md` §9.2's word "block" would have put it. The fleet status block rides *every*
fragment precisely so that it heals; a list of events stamped on every fragment would draw every
tracer once per fragment. State repeats, events do not.

The shots reach it through a log read by cursor and trimmed by the publisher — `DespawnsSince` and
`TrimDespawnsBefore`'s mechanism with a different record (ADR 0027). An update goes out every
`INTEREST_UPDATE_EVERY_TICKS`, so a view fed only the newest tick's shots would miss five sixths of
the gunfire in the game.

## Alternatives considered

**The reliable lane**, which ADR 0029's question literally asks for. Rejected because it buys
delivery of the one thing here that does not need it, and pays in the currency that lane is scarce
in: a battle produces far more shots than departures, they are worth nothing once stale, and a
retransmit spends the lane that carries orders and departures — messages that genuinely cannot be
lost — on tracers.

**A block in the fragment header**, beside the fleet status block. Rejected for duplication, above.

**Both lanes.** Rejected for ADR 0029's own reason for rejecting it: two paths carrying one fact is
two paths to reason about, and the unreliable copy arrives first anyway.

**No fire message at all** — let the client infer gunfire from a falling hull fraction. Rejected as
the exact sin the destroyed list was created to end (`Design/Archive/Hostiles.md` §4.4): a client
inferring a server's meaning from an absence. It also cannot work — a fraction says a ship was hurt,
never by whom, from where, or from which mount, and it says nothing at all about a shot at a station
that discards its damage.

**Kill attribution in the leave run**, since the wire is being opened anyway. Deliberately not
taken, and left to a design that wants it: a departure states that a ship was destroyed and never by
whom, which is the shape ADR 0040 gave it.

## Consequences

**The ship record grows a byte** — 47 to 48 — and `ShipsPerSnapshotFragment()` falls from 22 to 21.
It is derived, so it moved on its own; `SnapshotTests` states the number and moved with it.

**The shot log is not in the save format**, and `WORLD_STATE_FORMAT` stays at 6. Nothing in `Step`
reads it, so it changes no recorded outcome, and a reloaded world with no tracers pending is the
correct picture of one that has just resumed. The despawn log is saved because a client that missed
a death has a ghost ship for the rest of the match; a client that missed a muzzle flash has nothing.

**Either end being in view is enough** for a shot to be delivered, and the target end is the one
that matters: being shot at from outside your own interest set is exactly the event a player must
not be denied. A ship that died in view on the same update is matched against the departure lists
rather than against a handle it no longer has.

**`QUIC_ALPN` bumps to `outpost-4`.** The record changed size and a kind byte was added, so two
builds that disagree refuse at the handshake rather than at the parser.

**The exact hull points stay server-side.** A fraction is what a bar draws; the number is the
simulation's, like every other quantity it reasons with.
