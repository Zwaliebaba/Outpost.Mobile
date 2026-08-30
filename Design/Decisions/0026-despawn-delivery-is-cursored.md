# 0026 — The despawn log is read by cursor, not drained

Status: accepted
Date: 2026-08-30

## Context

`World` logs every despawn so the wire can say *destroyed* where it would otherwise say only *left*:
a client that infers a death from an absence detonates every ship that merely leaves its interest
radius, which is where a hostile patrol lives (`Design/Hostiles.md` §4.4).

The log arrived with one reader and an API shaped for exactly one: `DespawnLog()` returned the whole
vector and `ClearDespawnLog()` emptied it. Its own header said what that would cost —
"it is the publisher's, and there is one publisher today; the day there are several it becomes
per-subscriber."

`Design/MmoScalabilityReview.md` finding E2 is that day arriving with a number on it. With two
subscribers, whichever publishes first drains the log and the second is never told: not a dropped
datagram that the next update repairs, but a ship that stays alive on one client's screen for the
rest of the match, because nothing will ever mention it again. It is silent, it is permanent, and it
appears the moment a second subscriber exists — which is the next slice.

The choice had to be made before the publisher was built rather than after, because the publisher's
shape depends on it: a table of subscribers can only be written against a log that several readers
can share.

## Decision

Deaths are numbered for the life of the `World`, and the log is read by sequence:

- `DespawnHead()` — the sequence one past the last death. A subscriber joining a running world takes
  this as its opening cursor and hears about no ship it never held.
- `DespawnsSince(cursor)` — the handles at or after `cursor`, in despawn order.
- `TrimDespawnsBefore(cursor)` — drops what is older, called by whoever knows every subscriber's
  cursor with the minimum of them.

The numbering never resets, so a cursor stays valid across a trim and the difference between two
cursors is exactly the number of deaths between them. Trimming belongs to the caller because the log
does not know who is reading it; a log that trimmed itself would be the drain again, wearing a
sequence number.

A cursor older than what the log still holds returns everything held, rather than reporting the gap.
That is the over-report direction on purpose: the publisher intersects what it reads with the
subscriber's own interest set, so a handle that subscriber never knew about is dropped there anyway,
while a death silently skipped here is the permanent ghost this record exists to prevent.

## Alternatives considered

- **Keep the drain and give each subscriber its own log**, written by `World` at despawn time.
  Rejected: `World` would have to know its subscribers, which inverts the dependency — the
  simulation would hold a list of the things watching it, and every despawn would cost a write per
  subscriber instead of one.
- **Reference-count each entry, dropping it when every subscriber has read it.** Rejected: it is the
  cursor scheme with per-entry bookkeeping instead of per-reader, and it makes the log's contents
  depend on the order readers arrive, which is a worse thing to have to reason about for no gain.
- **Never trim; let the log grow for the life of the match.** Rejected, though tempting for its
  simplicity: a death is 8 bytes, but an MMO match is long and a persistent world has no end. The
  trim is three lines and bounds the log at the deaths in one update interval.
- **A ring buffer with a fixed capacity**, overwriting the oldest. Rejected: it converts "a
  subscriber fell behind" from a slow read into a silently missed death, which is the defect being
  fixed. The unbounded vector with an explicit trim fails loudly instead — it grows.

## Consequences

- Two subscribers can read the same death independently, which is what makes
  `MmoScalabilityPlan.md` slice 3's subscriber table possible.
- `WorldSimulation` holds one `std::uint64_t`. Its behavior is unchanged while there is one
  subscriber, which is why this slice changes the contract without changing the game.
- The trim is an erase from the front of a vector — a memmove of what is left, which is the deaths
  in one update interval. Dense-array order is preserved, so the determinism rule in `AGENTS.md` §5
  is untouched; the log is publish-side and no pass of `Step` reads it.
- The day a subscriber can fall arbitrarily far behind — a slow link, a paused client — the log
  grows until it catches up, and nothing caps it. That is deliberate for now: it is visible, and the
  policy for a subscriber that never catches up is a session decision, not a log decision.
