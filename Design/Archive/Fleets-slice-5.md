# Work order — Fleets slice 5: the fleet wire

Implements slice 5 of [`Fleets.md`](Fleets.md) §16: the `FleetRoster` message and its join-time
delivery, the status block in the snapshot header with its publish-side centroid, the
`LedgerRequest`/`LedgerReply` pair, and the `ComposeOrder` that has no use without them (design §8,
§5.2, §9.4).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** slice 2 (compose and launch), for the roster and the ledger the request answers; and
slice 4 (the defense), for the two status bits.
**Blocks:** slices 6, 7 and 8 — every one of them, because after this slice the client has been told
everything it needs and the rest of the feature is drawing.

---

## 1. Why this is a slice

Four `GameLogic` slices have built a fleet the client cannot see. `World` knows which ships are in
which slot, where they are, what they were told and whether somebody is shooting at them; the wire
carries none of it. This slice is the whole of what crosses the seam, and it is one slice rather
than three because the three messages are one decision taken three times — **what may a client know
about a fleet, and how does it come to know it** — and taking that decision once is what stops the
next reader finding three different answers.

It is also where the feature's one genuinely new wire shape lands. Everything on this seam so far is
either *stated every update* (records, the hostile mask) or *stated once, reliably* (departures,
orders). A station's ledger is neither: it is large, it is private, it changes at docking speed, and
exactly one client wants it at exactly one moment — when somebody long-presses a station. Broadcasting
it would put every station's contents on every wire ten times a second for a screen nobody has open.
So it is **asked for**, and the reply is the first server→client message in this tree that answers a
question rather than announcing a fact ([ADR 0051](../Decisions/0051-the-ledger-is-asked-for-not-broadcast.md)).

---

## 2. Scope

### 2.1 `GameLogic/WorldSnapshot.h` — four kinds and their types

Four new datagram kinds, continuing the run:

```
KIND_FLEET_ROSTER  = 6   server -> client, reliable
KIND_LEDGER_REQUEST = 7  client -> server, reliable
KIND_LEDGER_REPLY  = 8   server -> client, reliable
KIND_COMPOSE_ORDER = 9   client -> server, reliable
```

and the decoded types beside `FleetOrder`:

```cpp
struct FleetRoster
{
  std::uint8_t slot = 0;
  std::vector<EntityId> members;      // may be empty: a composed fleet has none yet
};

struct LedgerRequest
{
  EntityId station = INVALID_ENTITY_ID;
};

struct LedgerReply
{
  EntityId station = INVALID_ENTITY_ID;
  std::uint32_t hullCounts[HULL_COUNT]{};
};

struct ComposeOrder
{
  EntityId station = INVALID_ENTITY_ID;
  std::uint8_t slot = 0;
  std::uint32_t hullCounts[HULL_COUNT]{};
};

struct FleetStatus                    // decoded out of the header block, not a message of its own
{
  WorldPos position;                  // the centroid, or the launch station while none is out
  std::uint8_t status = 0;            // bits 0-2 kind shown, bit 6 engaged, bit 7 under attack
  std::uint8_t count = 0;             // members in space + manifest
};
```

`hullCounts` is a fixed `HULL_COUNT` array on both the reply and the order, not a `std::vector`:
the array *is* the format — a count per hull id, indexed — and `ComposeFleet` already takes a
`std::span<const std::uint32_t>` in exactly that shape, so the adapter hands one straight through
with no repacking. The wire carries a `u8 hullCount` ahead of it so a reader from a build with a
different hull table refuses rather than misreads; that is the same fail-closed rule the fleet
order's slot and kind checks already follow (AGENTS.md 5).

The status byte's low three bits carry a `FleetOrderKind` value **or** `FLEET_STATUS_LAUNCHING`,
which is 6 — the first value no `FleetOrderKind` uses, and there is room for one more before the
bit-6 flag. `Launching` is not a `FleetOrderKind` because it is not an order: nobody can issue it,
`IssueFleetOrder` would have to refuse it, and putting it in the enum would make the codec's own
`kind > FleetOrderKind::Mine` gate accept a value no order may carry.

### 2.2 `GameLogic/WorldSnapshot.cpp` — the status block in the header

The block goes in **every snapshot fragment header**, immediately after `hostileMask`:

```
u8 fleetMask                       // bit s: this viewer's faction has a statable fleet in slot s
per set bit, ascending:
  i32 sectorX, i32 sectorZ         // the wire's own narrowed sector pair (ADR 0046)
  u16 qx, qz                       // the record's own 0.125 m lattice
  u8  status
  u8  count
```

14 bytes a slot, 71 bytes at five fleets, and it rides `Write` as well as `WriteInterest` because a
receiver parses one header and the two paths may not disagree about its shape.

**It costs a record per fragment, and that is the decision to argue.** `ShipsPerSnapshotFragment()`
is a free function of no arguments — every caller and three tests agree on one number — so it must
be sized against the **worst case**, `SNAPSHOT_HEADER_BYTES + 1 + FLEET_SLOTS * FLEET_STATUS_BYTES`,
whatever this particular update carries. That is 1,152 − 27 − 71 = 1,054 bytes of records, so **22
per fragment against 23**, and `SnapshotTests`' assertion on 23 moves to 22. The alternative — a
count that varies with how many fleets a subscriber happens to hold — buys back one record and
costs a number nobody can state, which is the argument `MaxShipsPerOrder` already makes in its own
comment for keeping the smaller of two caps rather than the truer of them. In fragments it is
almost always free: a hundred-record update is five fragments at 22 and five at 23.

Two placements were rejected. A **separate `KIND_FLEET_STATUS` datagram** loses the property the
block is being bought for — it is stamped on every update so that losing one costs nothing, and a
second datagram is a second thing to lose. **Fragment 0 only** would cost nothing at all, and gives
back the invariant slice 6 of the collision work established in as many words: "every fragment now
carries records and nothing else, so the first is no different from the rest". A format where one
fragment is special is a format with a special case in every future reader.

The receiver takes the block **where it takes the hostile mask** — from any fragment that passes the
header and staleness checks, above the assembly of the upserts and below the staleness test. Same
asymmetry and the same reason: an incomplete update is dropped whole because half a set of records
is half a world, and a status block is coupled to no record at all.

### 2.3 `GameLogic/WorldSnapshot.cpp` — the centroid, and which slots are stated

Derived at write time from live members' positions, or from the launch structure while no member is
out. A centroid, computed through `OffsetX`/`OffsetZ` from the first live member and `Translate`d
back, so it is right with a sector boundary through the middle of a fleet.

A slot is in the mask when its fleet's position **can be derived**: a live member, or a live launch
structure. A fleet with neither is the one-tick window between a manifest being dropped for a dead
station and the next tick's retire freeing the slot — so the bit clears one tick before the row
goes, which is the truth early rather than a position that means nothing.

Nothing here is simulation state and nothing may become it. The centroid is a readout: the publisher
already derives per-subscriber (`SplitTheLost`), sits outside the replay contract, and a number
nobody simulates against cannot desynchronize anything (design §8.2).

### 2.4 `GameLogic/WorldSnapshot.cpp` — the four codecs

`WriteFleetRoster`/`ReadFleetRoster`, `WriteLedgerRequest`/`ReadLedgerRequest`,
`WriteLedgerReply`/`ReadLedgerReply`, `WriteComposeOrder`/`ReadComposeOrder`, all in
`WriteFleetOrder`'s shape: free functions taking a `Neuron::Transport&`, `SendReliable` on the way
out, a kind byte that makes a wrong-kind read a `false` rather than a misparse.

Every reader fails closed on content it cannot mean: a slot at or past `FLEET_SLOTS`, a member count
past `MAX_FLEET_SHIPS`, a `hullCount` that is not this build's `HULL_COUNT`, a truncated buffer.

### 2.5 `GameLogic/World.h`/`.cpp` — `LedgerFor`

```cpp
void LedgerFor(StationId _station, FactionId _asker, std::span<std::uint32_t> _outCounts) const;
```

Fills `_outCounts` (which must be at least `HULL_COUNT` long) with how many of each hull **the asker
itself** has docked at `_station`, and with zeros when the station's owner holds the asker hostile —
the same two rules `ComposeFleet` enforces, which is exactly why they live in one function that both
call rather than in two that agree today. A screen that offers what a compose will refuse is the
failure this pair exists to prevent, and it is the failure `ComposeFleet`'s own comment already warns
about for the ledger's rows.

In `World` and not in the adapter for ADR 0014's reason, unchanged by the fact that this is a read:
the standing gate is an authority decision and authority decisions are the simulation's.

`ComposeFleet`'s `available` loop becomes a call to it. Its own `RefusedStanding` gate still runs
first and still returns its own result — the zeros are for a caller that only wants to look.

### 2.6 `GameLogic/Publisher.cpp` — the roster diff

Per subscriber, a `std::vector<EntityId> lastRoster[FLEET_SLOTS]`, diffed at the top of `Publish`
against `World::FleetInSlot(faction, slot)` and its members. Changed slots send a `FleetRoster`;
unchanged ones send nothing.

**Held in the publisher rather than in the world**, and that is the whole design of it: the diff is
per subscriber, is outside the replay contract, needs no simulation state, and is not saved. It also
makes **join-time delivery free** — a new subscriber's stored rosters are empty, so its first
`Publish` diffs every occupied slot as changed and sends the lot, which is the despawn cursor's own
joining rule arriving at fleets without a second mechanism (design §8.1, ADR 0027).

The diff runs on every `Publish` and not only when the subscriber is due: membership changes at
human speed, so a diff over five slots of at most eight ids is nothing, and a roster that waited for
a phase would arrive after the status block that describes it.

Rosters go out for **every** slot change the design names — compose, each launch, each pruned loss,
retire — because all four change either the mask or the member list, and the diff sees all four
without being told about any of them.

### 2.7 `GameLogic/Publisher.cpp` — the ledger answered where it is asked

`ApplyOrders` gains two branches beside the fleet order:

- `ReadLedgerRequest` → resolve the station, `LedgerFor(station, subscriber.faction, counts)`,
  `WriteLedgerReply` back down the same transport, **immediately**. Not queued for `Publish`: the
  reply is an answer to a question, so it belongs at the question, and holding it for a phase would
  make the assembly screen's opening wait on the subscriber's update slot for no reason.
  An unresolvable station is answered with zeros rather than with silence — the screen has to close
  on something, and a reply that never comes is indistinguishable from a lost one.
- `ReadComposeOrder` → resolve the station, `StationAt` it, `ComposeFleet(...)` with the
  subscriber's faction. `(void)` the result, like every other order: fire-and-forget, and the roster
  and the mask are the confirmation (design §5.2).

Both are gated by the same per-tick order budget as everything else on the lane. A ledger request is
the cheapest message here — one linear scan of one station's ledger — but budgeting it uniformly is
what stops a client discovering that one message kind is unmetered.

### 2.8 `GameLogic/Publisher.cpp` — the empty-update guard learns about fleets

`PublishOne` returns early when nothing entered, left or came due. That guard now also asks whether
this subscriber's faction holds any statable fleet, because **the case it would otherwise break is
the one the whole feature is for**: four of five fleets are routinely outside the interest set, and
a player whose camera is somewhere empty would be told nothing about any of them. A zero-record
fragment carrying a status block is 28 to 98 bytes ten times a second, and it is the only thing that
tells that player where their fleets are.

### 2.9 `GameLogic/WorldSnapshot.h` — the receiver's surfaces

`SnapshotReceiver` gains, in `HostileMask()`'s idiom — a last-known value with an accessor, no
callback, no event:

```cpp
[[nodiscard]] std::uint8_t FleetMask() const noexcept;
[[nodiscard]] const FleetStatus& FleetStatusOf(std::uint8_t _slot) const noexcept;
[[nodiscard]] std::span<const EntityId> RosterOf(std::uint8_t _slot) const noexcept;
[[nodiscard]] const LedgerReply& Ledger() const noexcept;   // the last reply that arrived
[[nodiscard]] std::uint32_t LedgerReplyCount() const noexcept;  // so a screen can tell a fresh
                                                                // reply from a stale one
```

`Accept` dispatches `KIND_FLEET_ROSTER` and `KIND_LEDGER_REPLY` alongside `KIND_LEAVE`. A roster for
a slot replaces that slot's list whole — it is a statement of membership, not a delta, which is what
lets a lost one be repaired by the next.

Nothing draws any of this in this slice. The surfaces exist so slice 6 is a client slice that
touches no `GameLogic` file, which is what §16 says decides it.

### 2.10 What this slice does not touch

The client. No `Outpost` file, no `NeuronClient` file, no `ViewTuning.h`. No button, no minimap
digit, no assembly screen, no log line — every one of those is slice 6, 7 or 8, and the reason the
seam is worth this much care is that they can then be written without reopening it.

Not the save format either: nothing this slice adds is simulation state, so `WORLD_STATE_FORMAT`
stays 5 and `ASavedWorldReplaysToTheSameRun` needs no extension. That is the check on whether §2.3's
claim about the centroid is true — if the format had to move, the centroid would have become state.

Not `MaxShipsPerOrder`, not the ship record, not the leave message.

---

## 3. What to build on

- **`WriteFleetOrder`/`ReadFleetOrder`** (slice 3) — the shape all four new codecs copy, down to the
  reserved order id and the fail-closed reader.
- **`SnapshotWriter::WriteInterest`'s header** — where the block goes, and `hostileMask`'s comment is
  the argument for stamping it on every update rather than on change.
- **`SnapshotReceiver::HostileMask`** — where a per-update value is taken from, and the comment
  explaining why that is above the assembly and below the staleness check.
- **`Publisher::SplitTheLost`** — the precedent for per-subscriber derivation that is outside the
  replay contract, and for state the publisher keeps about a subscriber (`despawnCursor`).
- **`Desc::openingDespawnCursor`** (ADR 0027) — the joining rule the roster diff reproduces for free.
- **`World::ComposeFleet`** — its `available` loop is `LedgerFor`'s body, and its comments are where
  the two rules the ledger read must share are already written down.
- **`Neuron::MAX_RELIABLE_BYTES`** — 8,192, which every message here is orders of magnitude inside.

---

## 4. Acceptance

`GameLogicTests`, the four suites green, and the guards clean.

| Test | Decides |
|---|---|
| `TheRosterFollowsTheFleet` | a roster on compose, on each launch, on each pruned loss and on retire; nothing sent on a tick that changed no membership; a subscriber added mid-match receives every occupied slot on its first publish |
| `TheStatusBlockStatesEveryFleet` | mask, centroid on the lattice, status bits and count decode for 0 through 5 fleets; the launching value while a manifest holds anything; the engaged and under-attack bits differing when the alert outlives the fight; a slot cleared when its fleet retires |
| `TheLedgerAnswersItsOwner` | the reply carries the asker's own rows only; a hostile asker reads zeros; a station that is not one reads zeros; the reply arrives on the tick the request was applied |
| `AComposeOrderArrivesThroughTheSeam` | `ComposeOrder` over a transport composes the fleet, with the subscriber's faction as the issuer and not the one in the message |
| `AFleetStatusReachesADistantSubscriber` | a subscriber whose interest set is empty is still told where its fleets are — §2.8's guard, and the test that fails without it |

plus, in `SnapshotTests`:

| Test | Decides |
|---|---|
| `AFleetRosterRoundTrips` | slot, count, ids; an empty roster; refusals on a bad slot, a count past `MAX_FLEET_SHIPS`, a truncated buffer, a cross-reader |
| `ALedgerExchangeRoundTrips` | request and reply both ways; a `hullCount` that is not this build's is refused |
| `AComposeOrderRoundTrips` | station, slot, every count; refusals on a bad slot and a bad hull count |
| `ASnapshotCarriesTheFleetHeader` | records still decode with a block in front of them, at zero fleets and at five — the format's own regression test |

and the existing `ShipsPerSnapshotFragment` assertion moved to 22, which is the one deliberate
change to a passing test in this slice and is named here so it is not mistaken for a break.

Each new invariant is mutation-tested: the test must fail when the property it names is broken.

Guards: `Build/CheckProjectFiles.py`, `Build/CheckFormat.py`, clang-tidy over `GameLogic`.

---

## 5. Assumptions the implementer may make

- **Nothing on this seam is saved or replayed.** If a change here needs `WORLD_STATE_FORMAT` to
  move, the change is wrong, not the format.
- **A reliable message may be refused.** The lane can be full or not yet up, and `WriteLeaves`
  already counts its refusals rather than retrying. A roster is the same: stated once, counted if
  refused, and the mask — which rides every update — is what a client may rely on for whether a slot
  is held. This is why §8.1's "`count == 0` is the slot freeing" is amended in the design: an empty
  roster means no members in space, which a composed fleet also has, and occupancy is the mask's.
- **One subscriber, one faction.** A subscriber's faction is what every gate here reads, and a
  client cannot name another one — the message carries no faction field, on purpose.
- **The status block is a readout of state that already exists.** Every field is read from `Fleet`
  or derived from member positions. If a bit needs a new field on the row, it belongs in a different
  slice.
