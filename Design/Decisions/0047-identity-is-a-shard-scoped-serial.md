# 0047 — Identity is a shard-scoped serial, carried for life; the handle stays in-process

Status: accepted
Date: 2026-08-30

## Context

`ShipHandle` is `{slot, generation}` and both are allocated per-`World` instance. ADR 0005's
argument for it stands: a `ShipId` is a dense array index, despawn swap-and-pops, and anything
storing a reference across a tick boundary needs the slot indirection so a stale reference resolves
to nothing rather than to a stranger.

What a handle is not is an *identity*. `MmoScalabilityReview.md` finding U3: a ship handed from one
region server to another gets a fresh slot and generation at the destination, so the wire cannot say
"same ship, new region". A client keyed on handles would see the handle it held disappear and an
unfamiliar one arrive — destroy and enter, which is exactly the continuity ADR 0005 exists to
provide, lost at the shard boundary.

Nothing hands anything anywhere today. That is why this is now: it is additive and cheap while the
only thing keyed on a handle is the client's own display state, and expensive once combat stores
targets and a station's ledger stores customers.

`MmoScalabilityPlan.md` §6 slice 16 named `{shard:16, slot:24, generation:24}` as the candidate and
said the record decides the shape.

## Decision

**An `EntityId` is a `std::uint64_t`: sixteen bits of shard and forty-eight of serial.** It is
minted by `World` from a shard id the composition root configures, carried by the entity for its
whole life including across a `World`, and is what every wire message names. `ShipHandle` is
unchanged and stays in-process.

```cpp
using EntityId = std::uint64_t;   // {shard:16, serial:48}; 0 is never issued
ShipId World::SpawnShip(...);                    // mints the next serial
ShipId World::SpawnShipAs(EntityId, ...);        // takes one issued elsewhere
EntityId World::EntityIdOf(ShipId) / (ShipHandle) const;
ShipId    World::ResolveEntity(EntityId) const;
ShipHandle World::HandleOfEntity(EntityId) const;
void      World::ConfigureShard(ShardId);
```

The id lives in `World::Slot`, beside the ship index and the generation — the slot is already the
indirection that survives swap-and-pop, so this is one fewer table for despawn to keep in step. The
reverse direction is a `(EntityId, slot)` vector sorted by id and searched by `std::lower_bound`.

`DespawnRecord` gains the entity beside the handle and the cause, because the departure runs on the
wire name ships that are already gone and the publisher cannot ask the world who they were.

**The translation happens at the publisher.** Server-side code goes on using handles — `InterestSet`,
`Patrol`, `Docking`, the protector duties, every test that holds a reference across a tick. Outgoing,
a handle becomes an id; incoming, an id becomes a `ShipId` in the resolve loop that already existed.

**`FactionId` stays `std::uint8_t`**, which answers the plan's §4 decision 4.

## Alternatives considered

- **`{shard:16, slot:24, generation:24}`, the plan's candidate.** Lost on two counts. First, the
  slot and generation stop meaning anything the moment the entity moves: an entity born on shard A
  and handed to B keeps A's id, whose embedded slot names one of *A's* slots — so the id is opaque
  wherever it is not local, and a shape that *looks* derivable invites code that derives it.
  Second, a 24-bit generation wraps: it guards slot reuse, so a hot slot reused 16.7 M times
  reissues a live id, which is a weekend of churn on one shard. A 48-bit serial is never reused at
  all — 281 trillion ids, 8.9 million years at a million spawns a second.
- **Derive the id from the handle and store nothing.** Free, and correct for every entity that never
  leaves the world that minted it — which is every entity today. Rejected because it does not solve
  U3 at all: the destination shard would derive a *different* id for the same ship, and the finding
  is precisely that. A mechanism that works until the day it is needed is not a mechanism.
- **A hash map from id to slot.** The obvious index, O(1) rather than O(log N). Rejected because
  `World.h`'s own rule at the top of the class is "one dense array per entity kind, indexed by id —
  no maps, no pointers between entities, no iteration order that is not array order", and ADR 0010
  already chose a sorted vector over a map for interest sets for the same reason. The measured cost
  of the sorted vector is 13 compares at N = 5,000 for a lookup, O(1) amortized for a spawn
  (locally minted serials increase, so the row appends), and a 60 kB memmove for a despawn.
- **Keep the handle on the wire and map ids at the boundary only.** The review's own phrasing ("a
  globally unique id mapped at the wire boundary"). Rejected because the mapping has to be somewhere,
  and the client is the half that has to remember something across the boundary — so the id has to
  be what the client is given, not something the server translates back and forth around a handle
  the client would still be holding.
- **An id per entity kind rather than one namespace.** Attractive when there are projectiles and
  stations and asteroids. Rejected as premature: everything in this world is a ship record today,
  and a discriminator can take bits out of the serial's 48 later without changing anything already
  issued.
- **Widening `FactionId` to `std::uint16_t` while it is cheap.** The plan flagged this as the last
  cheap moment. Rejected: `FACTION_LIMIT` is 8, the standings table is `FACTION_LIMIT²`, and the
  wire's `hostileMask` is a `std::uint8_t` — so widening the id alone buys nothing and widening all
  three is a wire change nobody is asking for. The premise was "if player corporations are to become
  factions", and they should not be: a faction is an identity the server states and every client
  maps to a relation (ADR 0013), while a corporation is a membership. They are different axes, and
  making one the other would put ten thousand corporations into a table that is quadratic in its
  own size. The day something genuinely needs more than 256 identities, the mask and the table move
  with the id, and `ShipState.h` already says so.

## Consequences

- **The wire costs nothing.** An `EntityId` is 8 bytes where `{slot, generation}` was 8 bytes, so
  the ship record stays 47, a fragment stays 23 ships, and `MaxShipsPerOrder` stays 139. Identity
  became global for no bytes at all.
- **`Outpost` follows mechanically.** `WorldView`'s display state — the carried set, control groups,
  `RecallableIndex` — keys on ids instead of handles, and the F4 debug despawn goes through
  `HandleOfEntity`. That is the composition root crossing back the way the publisher crosses
  forward, which it is entitled to do, being the only thing that holds both halves.
- **The explosion seed improved by accident.** It used to be a slot shifted over a generation, which
  was the same 64 bits assembled by hand — and which would have made the same ship shatter
  differently after a shard handed it on. It is the id now.
- **`Publisher::SplitTheLost` got simpler, not more complex.** The `set_union` of two sorted handle
  lists followed by a `set_difference` collapsed to one sorted "departed" list and a walk, because
  the causes no longer have to be sorted in the same currency they are sent in.
- **`SpawnShipAs` advances the serial counter past a local id it is handed**, so a reloaded world
  cannot go on to mint an id its own file already used. That is one line written for slice 17, and
  it is here because it is one line now and a corruption bug later.
- **A duplicate id is refused rather than resolved.** `SpawnShipAs` returns `INVALID_SHIP_ID` for an
  id already present or a null one: an entity existing twice in one world is the failure the whole
  mechanism exists to make impossible.
- **A foreign id resolves only where it was handed in**, which is what stops a client ordering a
  ship this world does not own — the resolve loop's existing "resolves to nothing and is left out"
  now covers a whole shard's worth of ids for free.
- **The handoff protocol is still not built.** `SpawnShipAs` is the door; the message, the ownership
  transfer and the commit that stops an entity existing on two shards at once are not this record's.
