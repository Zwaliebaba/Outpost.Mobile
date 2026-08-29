# 0007 — The spatial index stores a whole position, and pays for it

Status: accepted
Date: 2026-08-29

## Context

Slice 8 gave `WorldPos` the `int64` sector pair `Design/Collision.md` §3 specifies, taking it from
8 bytes to 24. `SpatialIndex::Cell` holds one, so `Cell` went from **24 bytes to 48** — a doubling
of the hottest array in the simulation, the one every neighbour query walks.

`Design/Collision-slice-8.md` §2.5 anticipated this and specified the compact answer: store each
cell's offset from the grid's own origin sector, keeping `Cell` at 24 bytes and reconstructing the
full position on the way out. Implementing it turned up the problem. The grid needs an origin
sector, and the obvious source — the first entry — makes the stored offsets, and therefore the
rounding of every distance comparison in `Gather`, depend on the order entries arrived in.
`WorldTests::ArrayOrderCannotChangeTheAnswer` exists to catch exactly that, and this is the class of
bug it was written for: an order dependence that is invisible until two machines disagree.

Measured on the slice-2 benchmark, same machine, same build, immediately before and after the
change:

| N | cell | rebuild before → after | 250 m sweep before → after |
|---|---|---|---|
| 1000 | 256 m | 0.015 → 0.018 ms | 1.214 → 1.576 ms |
| 5000 | 256 m | 0.084 → 0.107 ms | 8.181 → 9.276 ms |
| 5000 | 512 m | 0.082 → 0.103 ms | 7.721 → 9.559 ms |

Roughly **+25% on rebuild and +13% to +30% on query**. Hits per query are identical at every one of
the twelve configurations — 36.3, 49.2, 53.2 — so the index returns the same neighbours in the same
order; the cost is memory traffic and nothing else.

## Decision

`SpatialIndex::Cell` stores the whole `WorldPos` and the index pays the 24 bytes. Neither the
compact layout nor any origin scheme lands in slice 8.

## Alternatives considered

- **Offset from the first entry's sector**, as the work order specified. Rejected: order-dependent,
  for the reason above. Nothing else about it was wrong, which is what made it worth writing down.
- **Offset from the minimum sector over all entries.** Order-independent — minimum is commutative
  and exact on `int64` — and it fixes the objection above. Rejected for slice 8 only: it adds a
  reduction pass and an origin to reason about, in a slice whose entire claim is that it is
  mechanical and changes no behaviour. It is a real option for whoever takes the optimisation.
- **Derive the sector from `cellX`/`cellZ`, and store neither.** `Cell` already carries the cell it
  is really in, so `sectorX` is `floorDiv(cellX, cellsPerSector)` — exact, order-independent, no
  origin, and `Cell` stays at 24 bytes. Rejected for slice 8 because reconstructing costs two
  integer divisions per candidate in the inner loop, and whether that is cheaper than the memory it
  saves is a question for a profile rather than for an argument. **This is the first thing to try**
  if the index shows up on one.
- **Pay it and say nothing.** Rejected: a 25% regression in the simulation's hottest loop, arrived
  at silently, is how a tree ends up slow with nobody able to name the commit.

## Consequences

- The index costs about a quarter more to rebuild and up to a third more to query. At the sizes the
  game runs today — 3 ships — this is unmeasurable. At the 5000 the benchmark sweeps it is 1.1 ms
  per full sweep, which is real but not yet anyone's bottleneck.
- The benchmark is the tripwire. It already prints these numbers on every run, so the next person to
  touch the index sees the cost rather than discovering it.
- The optimisation stays available and is now written down with the objection to each variant, so
  whoever takes it starts from the third alternative rather than the first.
- `Design/Collision-slice-8.md` §2.5 said the opposite of what landed. It has been marked with what
  the implementation found, in the same commit, per the rule that a document a change made false
  changes with it.
