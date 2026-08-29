# 0005 — A ship handle carries a stable slot, not the ship's array index

Status: accepted
Date: 2026-08-29

## Context

[`Design/Collision.md`](../Collision.md) §6 identifies the hazard correctly. `ShipId` is a dense
array index, which is what makes iteration cheap and is why it stays; despawn is swap-and-pop, so
the last ship moves into the freed slot and any id stored across a tick boundary silently retargets
to a stranger. Nothing in the tree stores one across a tick today, which is exactly why the fix is
cheap now and cross-cutting once neighbour lists, weapon targets and snapshot delta baselines
exist. The document's proposed shape is a generational handle: `{ index, generation }`, with the
generation bumped on despawn.

That shape only makes half of the hazard safe. It protects handles to the *despawned* ship: the
generation no longer matches, so they resolve to nothing. It does not protect handles to the ship
that swap-and-pop *moved*. That ship is alive, but its handle names an index that is now past the
end of the array, so it also resolves to nothing — a weapon would drop its target because something
unrelated died, and a snapshot baseline would treat a live ship as despawned. The document's own
test for the feature only checks the first half, so it would have passed.

## Decision

The handle carries a slot rather than the ship's index: `{ slot, generation }`. A slot is allocated
on spawn, is stable for the ship's life, and holds the ship's current array index; despawn repairs
the moved ship's slot entry and bumps the freed slot's generation. Resolving is one indexed load and
a compare, the dense array and its iteration are untouched, and the hot loops still use the raw
index. Free slots are reused last-in-first-out from a vector, so reuse is reproducible.

## Alternatives considered

- **The document's `{ index, generation }`.** Rejected: it leaves a live ship unreachable through
  its own handle whenever an unrelated ship despawns. Safe in the sense that nothing resolves to a
  stranger, and wrong in every other sense.
- **Stop swap-and-popping; leave holes in the array.** Rejected: it trades the problem for
  tombstones in every pass, and the dense array is the reason the tick is cache-linear.
- **A map from a stable id to an index.** Rejected: AGENTS.md §5 bans iteration order that is not
  dense-array order, and the slot table is that map as a dense vector anyway.

## Consequences

- One extra indirection per resolve, and one `std::uint32_t` per ship for the reverse lookup.
- Handles to a moved ship keep working, which is the property targeting and snapshot baselines will
  need and the shorter form does not give.
- §6 of the design document now disagrees with the code. The Slices section of that document points
  here.
