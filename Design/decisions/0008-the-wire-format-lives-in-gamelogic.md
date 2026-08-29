# 0008 — The wire format lives in GameLogic

Status: accepted
Date: 2026-08-29

## Context

Slice 2b puts a `Transport` between the two halves, so something has to turn a `World` into bytes
and bytes back into something a renderer can read. `NeuronCore/Transport.h` deals only in datagrams
and knows nothing about ships, which is correct and stays that way — so the encoding is somebody
else's job, and there are only four candidates.

## Decision

`GameLogic/WorldSnapshot.h/.cpp` owns the wire format: the snapshot record, the fragmenting writer,
the reassembling receiver, and the move-order encoding. `GameLogic` already depends on `NeuronCore`,
so it may include `Transport.h`; nothing else changes about the dependency graph.

## Alternatives considered

- **`NeuronCore`, beside `Transport`.** The intuitive answer, because a transport feels like engine
  work. Rejected: `AGENTS.md` §2 gives `NeuronCore` **zero game semantics**, and a snapshot of ships
  is nothing but game semantics. Taking this would have meant either moving `ShipState` into the
  engine or writing a parallel copy of it there, and both are worse than the problem.
- **`NeuronServer`.** Rejected outright: `Simulation.h` says in its own comment that NeuronServer
  never names GameLogic, never includes a game header and never links against one. That rule is what
  keeps the engine reusable, and it is not worth a snapshot.
- **`Outpost`, the composition root.** Legal — the executable is the one place allowed to see both
  halves. Rejected because it puts the format in the one component that a second executable would
  not share: the day a headless server exists, the encoder is in the client and has to move, and
  moving a wire format is exactly the change nobody wants to make under time pressure.

## Consequences

- `GameLogic` gains an include of `NeuronCore/Transport.h`. It had no engine includes before, so
  this is a real widening of its surface, taken knowingly: it is one header of pure interface with
  no implementation behind it.
- The format sits next to the types it encodes, so a change to `ShipState` and a change to what goes
  over the wire are edits to two files in the same directory, reviewed together. That is the same
  reasoning ADR 0002 used to put content readers with their consumers.
- A future headless server links `GameLogic` and gets the encoder with it, which is what makes the
  split "a transport swap plus a composition root" rather than a redesign.
- `GameLogicTests` can test the format with a stub transport of its own, keeping the format tests
  and the transport tests in the suites that own each.
