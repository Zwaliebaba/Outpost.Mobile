# 0040 — A departure carries a cause on the wire

Status: accepted
Date: 2026-08-30

## Context

A client learns that a ship is no longer in its view from a departure message. Until docking there
were two kinds: a ship that *died*, which the client detonates, and a ship that merely left the
interest radius, which it removes quietly. `Design/Archive/Hostiles.md` §4.4 built that distinction
and described it as "the door, opened the width of one list and no wider" — a `destroyed` list beside
the `left` list, so a client could stop inferring a death from an absence and stop detonating every
hostile that crossed the edge of its radius.

Docking is a third thing. A docked ship is gone from the world exactly as a dead one is — not
simulated, not collided, not snapshotted — but a client that detonates it is telling the player their
ship was destroyed when it arrived safely. The absence is identical; only the cause differs.

## Decision

The despawn log becomes a log of records:

```cpp
enum class DespawnCause : std::uint8_t { Destroyed, Docked };
struct DespawnRecord { ShipHandle handle; DespawnCause cause = DespawnCause::Destroyed; };
```

`DespawnShip` takes a cause, defaulted to `Destroyed`. `Publisher::SplitTheLost` produces three sets
from two, and the `KIND_LEAVE` message on the reliable lane grows a third run of handles with a
`dockedCount` beside `destroyedCount`. `SnapshotReceiver` exposes `Docked()` and `ClearDocked()`
beside the destroyed pair.

This widens Hostiles' mechanism rather than adding a parallel one. Jump-out, wreck-and-salvage and
capture are each one more cause through the same door.

**The docked handles ride the reliable lane, not the snapshot header**, and this is where the code
diverges from the design that asked for it. `Design/Archive/Stations.md` §7.4 puts `dockedCount` "in the
update header beside `destroyedCount`" and says `ShipsPerSnapshotFragment` follows it. That was true
when it was written and stopped being true when [ADR 0029](0029-departures-and-orders-take-the-reliable-lane.md)
moved departures onto the reliable lane. The design's *argument* is untouched — its conclusion is
that a docking travels beside a death, and it does — but the byte layout moved underneath it, and
`ShipsPerSnapshotFragment` derives from `SNAPSHOT_HEADER_BYTES`, which a docking never touches.

## Alternatives considered

- **A `Docked` order state, leaving the ship in the world.** The intuitive model: the ship is at the
  station, not gone. Rejected because a docked ship is not simulated, not collided, not snapshotted
  and not interesting — which is what *despawned* already means. Keeping it would leave a ghost entry
  in every pass, every index and every interest walk for a hull that is not there, and every one of
  those loops would need a new exception.
- **A separate `docked` message kind.** Symmetrical with the order kinds, and it would avoid touching
  a working message. Rejected because it duplicates the leave message's entire structure — the tick,
  the handle runs, the capacity check, the read-it-all-before-applying discipline — to carry the same
  kind of fact. Two paths for one thing is two paths to keep in step.
- **Let the client infer it.** A docked ship's last position is next to a station; a client could
  guess. Rejected as the exact sin the destroyed list was built to prevent, and it fails on its own
  terms: a ship destroyed next to a station is indistinguishable from one that docked at it.
- **Reuse the plain `left` list for dockings.** Free, and the visible behaviour is nearly right — the
  hull vanishes without an explosion. Rejected because it is a lie the moment anything is built on
  it: a leave means "no longer in your view", and the ship may still be alive somewhere. The station
  menu will need to know who is actually inside, and the log line the player reads says `DOCKED`.

## Consequences

- `DespawnsSince` returns `std::span<const DespawnRecord>`. That is a mechanical change at every
  reader — `Publisher::SplitTheLost` and about a dozen assertions in `WorldTests` — and the default
  cause means no *caller* of `DespawnShip` changed at all, F4's debug despawn included.
- `LEAVE_HEADER_BYTES` 17 → 21. The lane's capacity is unchanged in practice: `(8192 − 17) / 8` and
  `(8192 − 21) / 8` both floor to 1 021 handles.
- All three lists leave the receiver's held set the same way. They differ in what they *say*, not in
  what they do to the set, and only the client's effects care which — which is the shape the leave
  path already had.
- `Destroyed()` and `Docked()` clear independently, so a consumer that has drawn its explosions has
  not thereby forgotten the dockings it still owes a log line.
- The client needs no change to stop detonating docked ships: a docked handle is simply absent from
  `Destroyed()`. The log line that says `DOCKED` is a later slice's, and its absence is a missing
  line rather than a wrong explosion.
- Seven more causes fit in the enum before anything about this costs a byte.
