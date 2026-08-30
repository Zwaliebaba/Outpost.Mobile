# 0029 — The publisher lives in GameLogic

Status: accepted
Date: 2026-08-30

## Context

Until now the server half served exactly one subscriber, because it held exactly one of everything:
one `Transport*`, one `InterestSet`, one `SnapshotWriter`, one faction, all fields on
`Outpost/WorldSimulation.h`. `Design/QuicTransport.md` §10 named the debt when the migration landed
— "N clients is a `WorldSimulation` change — a table of `{transport, interest set, faction}` — and
belongs to a design of its own."

`Design/MmoScalabilityReview.md` found three defects that only exist once that table does, which is
why they are one slice and not three: fan-out is single-subscriber by construction (E2), every
subscriber's interest update would land on the same tick (E4), and order intake has no rate limit,
so wire bytes convert into formation solves and route planning at a leverage no other message has
(E6).

The question this record answers is not whether to build the table. It is where the table lives,
because that decision is the one that is expensive to reverse: it decides what a dedicated server
has to carry, and it cannot be deferred to the day there is one.

## Decision

`Game::Publisher`, in `GameLogic`. It owns N subscribers, each with its own transport, interest set,
snapshot writer, faction, phase, order budget and despawn cursor, and it exposes two calls the host
makes once per tick: `ApplyOrders` before the world steps, `Publish` after.

`Outpost/WorldSimulation` keeps a `Publisher` with one entry in it. Its behavior is unchanged to the
byte; only its shape moved.

## Alternatives considered

ADR 0008's three-way elimination, re-run for this type rather than cited, because the answer is not
self-evident and the reasoning is the useful part:

- **`NeuronServer`, beside `ServerHost`.** It is the server half, and hosting N subscribers sounds
  like a hosting concern. Rejected on the rule that made `NeuronServer` worth having: it never names
  GameLogic. A publisher holds an `InterestSet` and a `SnapshotWriter`, both game types, so putting
  it there would end the engine's independence from the game — the exact line `Simulation.h` exists
  to hold.
- **`Outpost`, where it already is.** No move, no new file, and it works today. Rejected for the
  same reason ADR 0008 kept the wire format out of the executable: the day there are two executables
  — a game client and a dedicated server — the fan-out is in the wrong one, and the server would
  either duplicate it or drag the client's composition root along. That day is the entire point of
  the seam.
- **A new library, `NeuronSession` or similar.** Tempting, because sessions are genuinely a
  different concern from simulation. Rejected as premature: a library is a dependency edge and a
  build unit forever, and what this slice actually adds is one type that needs `World`,
  `InterestSet` and `SnapshotWriter` — all GameLogic. The day it grows authentication, lifetimes and
  a config story, it will have earned its own library, and moving it then is a rename plus a project
  file. Moving it *back* would not be.
- **Leave the fan-out to each host, and give `GameLogic` only the pieces.** That is what exists
  today, and it is why the review found three defects rather than one: every host would have to
  remember the phase offset, the order budget and the despawn cursor, and the adapter has no test
  suite. The same argument ADR 0014 used for gating command authority in the simulation rather than
  the adapter applies unchanged — a rule the type enforces is a property, a rule each host
  remembers is a convention.

## Consequences

- A dedicated server is now `FrameClock + ServerHost + QuicListener + Publisher`, all in libraries,
  plus a composition root that calls `Publisher::Add` per accepted connection. What is left before
  that can exist is the configuration question in `MmoScalabilityPlan.md` §4 decision 3, not a code
  boundary.
- Phases are assigned from the subscriber's slot rather than its index, so a subscriber that had a
  tick to itself keeps it when somebody else leaves. `InterestSet::IsDueOn` takes the phase as an
  argument, defaulting to zero, which is the old behavior exactly.
- Each subscriber gets its own `SnapshotWriter`, which is the right grain rather than an
  accident: the writer carries `m_nextSnapshotId`, and a shared one would interleave two
  subscribers' fragment ids and make every reassembly ambiguous.
- The despawn log is trimmed once per tick to the minimum cursor across subscribers, which is what
  ADR 0026 built the cursors for. A removed subscriber stops holding the log back immediately.
- Order budgets are per subscriber per tick, and what is over budget stays queued rather than being
  discarded — it is read next tick, and the *event* is counted. A client that is permanently over
  budget is visible in `DroppedOrderCount` without any policy having been decided about it.
- `Publisher` is not a session layer and this record does not make it one. It does not know who a
  subscriber is, how it authenticated, or when it should go away. The day those exist they may well
  take it into a library of its own, and that will be a new record.
- Nothing here is in the replay contract: the publisher reads the world and writes to transports,
  and no pass of `World::Step` can see it. `TheSameOrderProducesTheSameRun` is the gate that says so.
