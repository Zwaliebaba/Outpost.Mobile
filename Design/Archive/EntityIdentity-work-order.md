# Work order — slice 16: global entity identity

Cut from [`MmoScalabilityPlan.md`](../MmoScalabilityPlan.md) §6 slice 16, against the tree at `fff2211`
(slice 15 landed). It retires the first half of finding U3 of
[`MmoScalabilityReview.md`](../MmoScalabilityReview.md); the second half — a state codec — is slice 17.

Layer: `GameLogic`, plus the mechanical follow-on in `Outpost` that a changed wire type forces.

---

## 1. The problem, restated from the tree

`ShipHandle` is `{slot, generation}` and both are allocated per-`World` instance
(`World.cpp:26-41`). It is a perfectly good in-process reference and ADR 0005's argument for it
stands. What it is not is an *identity*: a ship handed from one region server to another gets a
fresh slot and generation at the destination, so the wire cannot say "same ship, new region". A
client keyed on handles sees the handle it held disappear and an unfamiliar one arrive — destroy and
enter, which is exactly the continuity ADR 0005 exists to provide, lost at the shard boundary.

Nothing today hands anything anywhere. That is the point of doing this now: it is additive and cheap
while the only thing keyed on a handle is the client's own display state, and expensive once combat
stores targets and a station's ledger stores customers.

## 2. Scope

**An `EntityId` — `std::uint64_t`, `{shard:16, serial:48}` — is minted by `World` from a shard id it
is configured with, carried by the entity for its whole life including across a `World`, and is what
every wire message names. `ShipHandle` stays exactly as it is and stays in-process.**

| | |
|---|---|
| `EntityId` | `std::uint64_t`, in `ShipState.h` beside `ShipHandle` |
| Layout | shard in the top 16 bits, serial in the low 48 |
| Null | `INVALID_ENTITY_ID` is 0; serials start at 1 so no shard ever issues it |
| Minted | `World::SpawnShip` takes the next serial; `World::SpawnShipAs` takes an id issued elsewhere |
| Stored | in `World::Slot`, beside the ship index and the generation |
| Resolved | `World::Resolve(EntityId)`, by binary search over a sorted index |

On the wire, `EntityId` replaces `ShipHandle` **byte for byte** — both are 8 bytes — so the ship
record stays 47 bytes, a fragment stays 23 ships, and `MaxShipsPerOrder` stays 139. The messages
that change type and not size: the ship record's identity field, the three departure runs in
`KIND_LEAVE`, and the ship lists in `KIND_MOVE_ORDER` and `KIND_DOCK_ORDER`.

**Where the translation happens.** Server-side code goes on using handles — `InterestSet`, `Patrol`,
`Docking`, the protector duties, every test that holds a reference across a tick. The publisher
translates at the wire: outgoing, a handle becomes an id; incoming, an id becomes a `ShipId` in the
same resolve loop that already exists (`Publisher.cpp:157-165`).

**`FactionId`'s width is revisited**, per the plan's §4 decision 4, and the record takes a position
rather than leaving it open a second time.

## 3. The decisions this order takes

### 3.1 `{shard:16, serial:48}`, not `{shard:16, slot:24, generation:24}`

The plan named the second as its candidate and said the record decides. The serial wins on two
counts:

- **The slot and generation stop meaning anything the moment the entity moves.** An entity born on
  shard A and handed to shard B keeps A's id, and on B the embedded slot names one of *A's* slots.
  So the id is opaque wherever it is not local, which means nothing may ever derive a handle from
  it — and a shape that looks derivable invites exactly that. A serial is honestly opaque.
- **A 24-bit generation wraps.** It guards slot reuse, so a hot slot reused 16.7 M times reissues a
  live id — 77 hours at 60 reuses a second, which is a weekend on one shard. A 48-bit serial is
  never reused at all: 281 trillion ids, 8.9 million years at a million spawns a second.

The cost is that the id no longer derives from the handle, so `World` stores it (in `Slot`, which is
already the identity indirection) and keeps a reverse index for the other direction.

### 3.2 The reverse index is a sorted vector, not a map

`World.h` says in as many words: "one dense array per entity kind, indexed by id — no maps, no
pointers between entities, no iteration order that is not array order". A sorted `(EntityId, slot)`
vector searched by `std::lower_bound` honours all three, and it is the shape ADR 0010 already chose
for interest sets for the same reason.

Costs, stated rather than assumed:

- **Lookup** O(log N) — 13 compares at N = 5,000.
- **Spawn** O(1) amortized for every spawn this tree can currently produce, because locally minted
  serials increase monotonically, so the new id is greater than the last and appends. Only an id
  issued elsewhere inserts in the middle, and nothing hands one in yet.
- **Despawn** O(N) as a memmove of 12-byte pairs — 60 KB at N = 5,000, tens of microseconds. Named
  here so that if churn ever makes it matter, the number is already written down.

### 3.3 A departure states an id, so the despawn log carries one

The three departure runs name ships that are already gone, so the publisher cannot ask the world for
their ids. `DespawnRecord` gains the entity beside the handle and the cause — which is the right
place for it anyway: the record of a departure has to be able to say what departed.

That makes `Publisher::SplitTheLost` simpler rather than more complex. The set arithmetic it does
today — a `set_union` of two sorted handle lists and then a `set_difference` — collapses to one
sorted "departed" list and a walk, because the destroyed and docked runs no longer have to be
subtracted in the same currency they are sent in.

### 3.4 The shard is configured, not compiled

`World::ConfigureShard` sits beside `ConfigureIndex`, called by the composition root before anything
spawns. That is AGENTS.md §5's rule exactly: a library takes a plain value from the root and never
reads a file or an environment. `Outpost` passes 0 and says why; a dedicated server would pass its
own, which is slice 24's configuration file.

`SpawnShipAs` advances the serial counter past any local id it is handed, so reloading a saved
world (slice 17) cannot reissue an id the file already used. That is the one line of this slice
written for a slice that does not exist yet, and it is here because it is one line now and a
corruption bug later.

## 4. Out of scope

- **The handoff protocol.** Nothing moves an entity between worlds. `SpawnShipAs` is the door;
  what walks through it — the message, the ownership transfer, the two-phase commit that stops an
  entity existing twice — is not this slice and not this plan (§5 of the plan: no second process).
- **Ownership transfer, authority, or who may despawn a foreign entity.**
- **Persisting the serial counter**, beyond `SpawnShipAs` advancing it. Slice 17 writes it.
- **Widening `FactionId`.** §3 of the decision record says why it stays u8; changing it is a wire
  change with nobody asking for one.
- **`ShipHandle`.** Untouched, in shape and in use. Every server-side reference that outlives a tick
  is still a handle, and ADR 0005 still says why.
- **The record's size.** An id is 8 bytes where a handle was 8 bytes. Nothing about slice 15's
  arithmetic moves.

## 5. What to build on

- `GameLogic/ShipState.h` — `ShipHandle`, `ShipId`, `FactionId`; `EntityId` joins them.
- `GameLogic/World.h` — `Slot`, `m_slots`, `m_shipSlot`, `m_freeSlots`, `HandleOf`, `Resolve`,
  `DespawnRecord`, `ConfigureIndex` as the precedent for `ConfigureShard`.
- `GameLogic/WorldSnapshot.cpp` — `ByteWriter::Handle`/`ByteReader::Handle` and `HANDLE_BYTES`,
  which become the id's.
- `GameLogic/Publisher.cpp` — `SplitTheLost` and the order resolve loop at 157-165.
- `Outpost/WorldView.{h,cpp}` — 15 `ShipHandle` sites, all display state; `OutpostApp.cpp:532-539`,
  the F4 debug despawn, which is the one place that crosses from a snapshot back into the world.

## 6. Acceptance

- [ ] **Two worlds, one entity.** A ship despawned in world A and spawned in world B with
      `SpawnShipAs` under the same id arrives at a receiver as the *same* record: the id is
      unchanged, and the receiver's held set never sees a destroy or a re-enter for it.
- [ ] **The same ship in two worlds has two handles and one id**, asserted directly, because that
      is the sentence the whole slice exists to make true.
- [ ] **Ids are unique and never reused**: a slot reused after a despawn issues a new id, and the
      handle-to-id mapping of the reused slot names the new entity, not the old.
- [ ] **A foreign id resolves**: an id minted with a different shard resolves in the world that was
      handed it, and does not resolve in a world that was not.
- [ ] **`SpawnShipAs` will not mint a collision**: after being handed a local id above the counter,
      the next `SpawnShip` issues a serial past it.
- [ ] **Orders round-trip by id**: a move order and a dock order written by a client and read by the
      publisher steer the ships the ids name, and an order naming a dead entity is dropped exactly
      as one naming a dead handle was.
- [ ] **Existing handle tests unchanged.** `WorldTests`' handle rows, `ProtectorTests`,
      `DockingTests`, `PatrolTests` all still hold handles and still pass.
- [ ] **The replay gate is green** and the record's size is unchanged at 47 bytes.
- [ ] `python Build/CheckFormat.py` and `python Build/CheckProjectFiles.py` green.

## 7. Decision record due

**Identity is a shard-scoped serial, carried for life.** ADR 0044: the shape and why the plan's
candidate lost, the sorted-vector index and why not a map, the despawn log carrying an id, and
`FactionId` staying u8 — which is the plan's §4 decision 4 answered.

## 8. Assumptions the implementer may make

- **`Outpost` passes shard 0** and there is exactly one world. The multi-shard case is designed for
  and not exercised, which the tests state by constructing two `World`s directly.
- **No screenshot is owed.** The client change is a type rename in display state; nothing about what
  is drawn moves.
- **Nothing hands an entity anywhere**, so the middle-insert path in the reverse index is exercised
  only by tests.
