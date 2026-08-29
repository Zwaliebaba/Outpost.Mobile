# 0010 — Interest sets are sorted vectors, not hash maps

Status: accepted
Date: 2026-08-29

## Context

Slice 6 keeps, per subscriber, the set of entities it can see, and derives from it three things each
update: who entered, who left, and who came due for a refresh. That is a set membership problem with
a per-entity payload — the priority accumulator — and the structure every instinct reaches for is a
hash map from `ShipHandle` to priority.

`AGENTS.md` §5 bans "iteration order that is not dense-array order" in `GameLogic`, and
`Design/Collision.md` §7 already turned down an `unordered_map` spatial hash for the same reason:
iteration order depends on hashing and on allocation, so it differs between machines and between
runs, and it would fail the determinism gate non-reproducibly.

The counter-argument is real and has to be answered rather than waved at. `Design/Collision.md` §1
warns specifically against "diffing sets per player per tick to derive spawn and despawn messages —
which is the O(N · k) cost this section claims to avoid", and a sorted vector means sorting, which
a hash map would not.

## Decision

The subscribed set is a `std::vector<ShipHandle>` kept sorted by `(slot, generation)`, with a
parallel `std::vector<float>` of priorities. Entered, left and the carried priorities all come from
one merge walk over the old and new sets.

## Alternatives considered

- **`std::unordered_map<ShipHandle, float>`.** The obvious structure. Rejected: it puts hashing and
  allocation into the answer. Enter and leave would come out in bucket order, so two machines could
  send the same update with its records in a different sequence — which is not a divergence today,
  when nothing depends on record order, and is one the moment anything does. The ban exists to stop
  that being discovered late.
- **`std::map`, which does iterate in order.** Rejected for cost, not correctness: a red-black tree
  allocates per node and scatters them, for a set that is walked in full every update and is small
  enough to fit in cache as a vector.
- **Two `std::set_difference` passes over sorted vectors**, which is what this slice's work order
  specified (`Design/Archive/Collision-slice-6.md` §3.3). Correct and clearer to read. Not taken, because
  the two differences give entered and left but not the *carried priority* of everything that
  stayed, which then needs a third pass over the intersection to recover. One merge walk gives all
  three and touches each element once. The work order was written before that was obvious and is
  marked with what the implementation found.

## Consequences

- An update costs O(k log k) for the sort and O(k) for the merge, where k is the neighbourhood, not
  the world. §1's warning is about diffing the whole world per player per tick; this diffs a bounded
  neighbourhood at 10 Hz rather than 60, which is a different quantity by two orders of magnitude.
  The benchmark in `InterestTests` prints what it actually costs.
- The sort is an insertion sort over the parallel handle and distance arrays. That is quadratic in
  the worst case and the right choice for k in the hundreds, where it beats anything with an index
  permutation — but it is the first thing to revisit if the radius grows a lot.
- Enter and left come out in handle order, deterministically, so the wire carries records in an
  order that does not depend on the machine that produced them. Nothing requires that yet. It is
  free here, and it is expensive to add back later.
