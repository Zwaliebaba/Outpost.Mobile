# 0039 — Standings are simulation state, stated per subscriber

Status: accepted
Date: 2026-08-30

## Context

`Design/Stations.md` needs one judgment in two places: does a station's owner hold this issuer
hostile? The dock gate asks it before admitting a ship (§7.1) and the protector response asks it
from the other side (§8). It changes recorded outcomes, and a spectator watching a replay would need
it to understand why a ship was turned away.

The client needs an answer too, and this is the harder half. An order datagram is fire-and-forget,
so a dock order refused by the server is, to a client that cannot tell, ships that simply never go —
which reads as a broken game rather than as a refusal.

ADR 0013 already settled the shape of the answer: the server states identity and each client maps it
to a relation. What it did not settle is where a *relation between factions* lives, or how a client
learns its own.

## Decision

**Simulation state.** A `FACTION_LIMIT × FACTION_LIMIT` table of `Standing` in `World`, read as
`StandingOf(owner, other)` — the owner's opinion of the other — initialised from a `constexpr`
`DEFAULT_STANDINGS` and mutated by exactly one function, `RecordAggression`, which arrives from
outside the tick like any order.

**Stated per subscriber, every update.** One byte in every interest-update header: bit *f* set means
faction *f* currently holds *your* faction hostile. `SnapshotReceiver` exposes it; the client's
livery table, its dock affordance and its contact count all read it and none of them infers anything.

Directional, because "CVC despises you" and "you despise CVC" are different facts and only the first
decides whether CVC lets you dock. Faction-granular, because one subscriber is one faction today.

## Alternatives considered

- **Presentation state, in the client.** The client is already the half that turns identity into a
  relation (ADR 0013), so a standings table there looks consistent. Rejected on AGENTS.md §5's own
  test: it changes recorded outcomes — who docks, who is hunted — and a spectator would need it.
  A client-side table would also have to be *right*, which means the server would have to send it,
  which is this decision with an extra step.
- **A standings *event* message when a standing changes.** The efficient answer: one message instead
  of a byte on every update. Rejected because it invents reliability this wire does not have. A lost
  "you are now criminal" leaves a client believing itself honest for the rest of the match, and the
  fix — acknowledgements, retransmission, sequence numbers — is a reliable channel for one byte.
  Restating it on every update is idempotent by construction and costs 1 byte against an 83-byte
  record.
- **The whole standings table on the wire.** More general, and no client needs third-party opinions
  yet. Rejected as broadcasting private state: what CVC thinks of the Vandals is nobody's business
  until something displays it, and the byte can widen into a record the day one does.
- **Let the client declare aggression, or write a standing.** Never. Aggression is a server-side
  judgment about acts the server observed; a client that could declare one could make anybody a
  criminal. There is no message for it and this record exists partly so that nobody adds one.
- **Per-player standings now.** The MMO-shaped answer, and where this goes. Rejected as premature in
  exactly the way ADR 0014's authority gate is faction-granular: today one subscriber is one faction,
  so "your faction is criminal" and "you are criminal" are the same sentence. The widening is a keyed
  row and a different lookup, in the same functions.

## Consequences

- `FACTION_LIMIT` is 8 because the mask is a `u8`. The two are joined at the hip and the comment at
  each says so; the day factions outgrow a byte, the mask becomes a small standings record and the
  limit moves with it.
- An id at or past `FACTION_LIMIT` reads back `Hostile`, not `Neutral`. Every caller is a gate or a
  warning colour and the failure directions are not symmetric: a stranger refused a dock is a bug
  report, a stranger admitted is a hole.
- Both snapshot writers stamp the byte, because both emit `KIND_SNAPSHOT` and one reader parses
  both. Neither knew whose view it was writing, so both gained a viewer argument, defaulted so that
  every existing caller compiles unchanged; `Publisher` is the only thing that knows the answer.
- The receiver takes the mask from any fragment that passes the header and staleness checks, rather
  than on apply like the upserts. An incomplete update is dropped because half a set of records is
  half a world; a mask is coupled to no record, so taking whatever arrives is strictly more robust —
  which is the whole argument for spending the byte.
- Aggression is permanent: no decay, no fines, no amnesty. That is the owner's decision
  (`Design/Stations.md` §15, decision 3) rather than this record's, and a standings-repair design is
  where it changes.
- `SHIP_RECORD_BYTES` 82 → 83 and `SNAPSHOT_HEADER_BYTES` 26 → 27, which happens to leave
  `ShipsPerSnapshotFragment` at 13: `(1152 − 26) / 82` and `(1152 − 27) / 83` both floor the same.
  The number is derived and not chosen, so it moves when it moves.
