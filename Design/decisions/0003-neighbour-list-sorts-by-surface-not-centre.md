# 0003 — The neighbour list sorts by surface proximity, not centre distance

Status: accepted
Date: 2026-08-29

## Context

[`Design/Collision.md`](../Collision.md) §7 specifies the sense pass as: gather every candidate from
the covering cell ring, sort by `(distanceSquared, ShipId)`, then truncate to the per-hull
neighbour cap `K`. The sort-then-truncate order is load-bearing and stays — truncating cell by cell
would put cell size in the replay contract. What is wrong is the key.

Centre distance and surface distance are the same question only when hulls are the same size. §3 of
the same document establishes that they are not: the size ratio is 72:1, and 31:1 among mobile
hulls alone. Measured on a twelve-ship column driving at a Structure, the Structure's centre sits
263 m from the front ship while its wall is 11.5 m away, and a dozen Corvettes 29 m to 237 m clear
all sorted above it. It landed at rank 10 of a cap of 10. One more ship in the column and it would
have been truncated off the list, and the column would have driven through the wall — silently,
because a missed contact reads as a tuning problem rather than as a bug.

## Decision

Sort the candidate list by `(centre distance − the neighbour's bounding radius, ShipId)`: the K
nearest by surface. `ShipId` stays the tie-break, so the order is still total and `std::sort`'s
instability cannot show through. The cost is one square root per candidate, on a list of tens.

## Alternatives considered

- **Keep centre distance, as written.** Rejected: it is not a near-miss at this size ratio, it
  loses the largest and most dangerous obstacle first, and the failure is silent.
- **Keep centre distance and exempt `immovable` hulls from truncation.** Rejected: §7 warns against
  special-casing a class, because that is how a fourth class becomes a rewrite. It would also still
  be wrong between a Carrier and an Interceptor, neither of which is immovable.
- **Raise `K` until the problem goes away.** Rejected: `K` is in the replay contract and the cap
  needed would scale with crowding, so there is no value that is right. Measured directly — raising
  the Interceptor's cap from 8 to 20 did not change the case it was meant to fix.

## Consequences

- The list is now ordered by the quantity separation and avoidance actually care about, so the
  neighbours that get truncated are the ones least able to hurt.
- `Neighbour` carries a `proximityMetres` field alongside `distanceSquared`; both are computed in
  the sense pass and neither is recomputed later.
- §7 of the design document now disagrees with the code. The Slices section of that document points
  here.
