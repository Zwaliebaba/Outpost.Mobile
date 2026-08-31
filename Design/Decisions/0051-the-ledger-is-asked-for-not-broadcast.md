# 0051 — The ledger is asked for, not broadcast

Status: accepted
Date: 2026-08-31

The first request/reply pair on this seam. Everything on it until now either announces a fact or
issues an order; this one asks a question and is answered.

## Context

[`Design/Fleets.md`](../Fleets.md) §9.4 opens an assembly screen on a long-press of a station: it
lists the hulls the player has docked there, and a draft of them becomes a fleet. The screen cannot
be drawn from what a client already holds. A station's ledger is deliberately absent from the ship
record — `Design/Archive/Stations.md` 6.2 put it on the withheld list beside the garrison numbers
and the target list, because whose ships are inside a station is nobody else's business.

So the client needs something it is not being sent. Every existing shape on this seam is a poor fit:

- **In the ship record.** It is private, it is per-faction, and it would grow a 47-byte record into
  something that carries a station's whole contents to everyone who can see the station.
- **In the interest update header**, beside the hostile mask and the fleet status block. Those are
  there because they are small, they concern the subscriber, and they are wanted continuously. A
  ledger is none of the three: it is tens of bytes per station, it is wanted by one client at one
  moment, and it changes at docking speed.
- **On the reliable lane on change**, like the roster. That is a broadcast of every station's
  contents to every subscriber who might one day open a screen over it — most of whom never will.

## Decision

A **request/reply pair** on the reliable lane. `LedgerRequest { EntityId station }` goes up;
`LedgerReply { EntityId station; u32 hullCounts[HULL_COUNT] }` comes back, answered in
`Publisher::ApplyOrders` on the tick the request was read rather than queued for the next update.

Three properties are load-bearing:

- **The asker is the subscriber, never the message.** There is no faction field in the request, so
  a client cannot ask on somebody else's behalf. This is ADR 0014's rule applied to a read.
- **The rules live in `World::LedgerFor`**, which both the reply and `ComposeFleet` call. Only the
  asker's own rows are counted, and a station whose owner holds the asker hostile reads all zeros.
  One function rather than two that agree today, because the failure a screen can produce is
  precisely offering hulls that a compose will then refuse.
- **An unanswerable request is answered with zeros**, not with silence. A station that is gone, a
  ship that is not a station, a hostile port — all three reply. A reply that never comes is
  indistinguishable from a lost one, and a screen has to open on something.

## Consequences

The seam now has three kinds of traffic rather than two: announcements (records, the masks, the
roster), orders (move, dock, fleet, compose), and this. A future reader adding a message has a
third precedent to reach for, and should reach for it under the same test — large, private,
slow-changing, and wanted at a moment rather than continuously.

It is also the first message whose *cost is paid by the asker*. A client that spams requests spends
its own order budget doing it, which is why the request is metered by `ordersPerTick` like every
other message on the lane rather than being waved through as cheap.

What it is not: a query language, a subscription, or an acknowledgement scheme. There is one
question, it has one answer, nothing is cached and nothing is retried. The reply carries the station
it answers for, so a client with two screens open can tell the answers apart, and `LedgerReplyCount`
lets one tell a fresh answer from the one already on display — replies are identical for identical
questions, so the payload alone cannot say whether the wire has spoken.

## Alternatives considered

- **Ledger rows in the interest update header.** Rejected above: continuous cost for a moment's
  need, and it would put every station's private contents on the wire of everyone in range.
- **A `ComposeOrder` that carries no draft, letting the server pick.** Removes the need for a reply
  entirely — and removes the feature: the player composing a specific mix of hulls is what the
  assembly screen is.
- **An acknowledged compose, so the screen can report a refusal.** Every order on this seam is
  fire-and-forget, and a compose has no more claim on an exception than a dock order does. The
  affordance already knew — the screen was drawn from a reply that had passed the same gates — and
  the roster and the mask are the confirmation. Adding an ack here would be adding the first one
  anywhere, for the least urgent case.
