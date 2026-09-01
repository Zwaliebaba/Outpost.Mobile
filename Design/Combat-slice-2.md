# Work order — Combat slice 2: the combat wire

Implements slice 2 of [`Combat.md`](Combat.md) §16: the hull fraction in the ship record, the fire
block on the datagram lane, the receiver accessors that carry both, and the handshake bump that
makes two builds which disagree refuse rather than misparse (design §9).

**Status: landed 2026-09-01 and in review**, with
[ADR 0053](Decisions/0053-fire-events-ride-the-datagram-lane.md). Two rows landed under names other
than §4's — `AFireBlockOverTheCapKeepsTheNewest` and `SilenceIsNotSent`, both saying what they
check rather than what they forbid — and nothing else in this order changed on contact.

**Layer:** `GameLogic` (the wire seam) and `GameLogicTests`, plus one constant in `NeuronCore`.
**Depends on:** slice 1, merged or on this branch — there is nothing to report until something
fires and something loses hull points.
**Blocks:** slice 4, and slice 6 after it. The view cannot flash a muzzle, draw a tracer, slew a
turret or fill a pip row for a shot the wire never mentioned. Not slice 3, which reads a mesh and
never the wire.

---

## 1. Why this is a slice

After slice 1 the world is lethal and says nothing about it. A client watching a battle sees ships
vanish with a shatter and has no way to know that anything was ever *shot* — no damage, no gunfire,
no reason. That is a true picture of the tree today and a useless one for a player, and it is the
whole of what this slice fixes.

It is deliberately the smaller half of the pair. Everything here is codec and cursor: two new things
on the wire, both derived from state slice 1 already keeps, with no new simulation behavior at all.
What it must get right is which lane each one takes and why, because that decision is the one ADR
0029 exists to make and the one a later reader will want the argument for.

---

## 2. Scope

### 2.1 `GameLogic/World.h` / `.cpp` — a shot log, read by cursor

Slice 1's `m_shotScratch` is cleared every tick, and an update is sent every
`INTEREST_UPDATE_EVERY_TICKS` (6). A view fed only the newest tick's shots would miss five sixths of
the gunfire in the game, and with cooldowns from 30 to 360 ticks that is most of every mount's
working life. So the shots become a log, read by cursor and trimmed by the publisher — the despawn
log's shape and ADR 0027's argument, one mechanism along:

```cpp
struct ShotRecord
{
  EntityId shooter = INVALID_ENTITY_ID;
  EntityId victim = INVALID_ENTITY_ID;
  std::uint32_t mount = 0; // which of the shooter's mounts fired, so the view knows which muzzle
};

[[nodiscard]] std::uint64_t ShotHead() const noexcept;
[[nodiscard]] std::span<const ShotRecord> ShotsSince(std::uint64_t _cursor) const noexcept;
void TrimShotsBefore(std::uint64_t _cursor) noexcept;
```

`StepMounts` appends one record per landed shot, beside the `Shot` it already collects, and the
entities are read while both ships are still live — which is the same reason a `DespawnRecord`
carries an `EntityId` rather than being resolved later (ADR 0047).

**It is not in the save format, and that is a decision rather than an omission.** Nothing in `Step`
reads it, so it changes no recorded outcome; a reloaded world with no tracers pending is the correct
picture of a world that has just resumed; and `WORLD_STATE_FORMAT` therefore stays at 6. The despawn
log is saved because a client that missed a death has a ghost ship for the rest of the match, and a
client that missed a muzzle flash has nothing at all.

### 2.2 `GameLogic/WorldSnapshot.h` / `.cpp` — one byte in the record

`ShipSnapshot` gains `std::uint8_t hullFraction = 255` — 255 is whole — and `SHIP_RECORD_BYTES` goes
47 → 48. Written as `hullPoints * 255 / maxHullPoints`, with a hull whose maximum is zero reading
back 255: an indestructible thing is undamaged, which is the only honest answer and the one that
keeps a station's bar from reading empty.

**It belongs in the record rather than in an event, and ADR 0029's own question settles it**: a lost
fraction is corrected by the next update six ticks later, so late is worse than lost. It is on every
subscribed ship, hostiles included, because a target bar has to read something (design §10.3).

Two consequences to state rather than discover. `ShipsPerSnapshotFragment()` re-derives itself from
the record size and falls from **22 to 21** — `SnapshotTests::TheFragmentSizeIsDerivedFromTheDatagram`
asserts the number and moves with it, keeping its argument about what the figure is *for*. And the
exact hull points stay server-side: a fraction is what a bar draws, and the number is intent of the
kind the snapshot exists to withhold.

### 2.3 `GameLogic/WorldSnapshot.h` / `.cpp` — the fire block

A new datagram message, `KIND_FIRE = 10`:

```
kind, tick, count, then count x { shooter EntityId, victim EntityId, mount u8 }
```

with a `FireEvent` decoded type and `MAX_FIRE_EVENTS = 64` — comfortably inside one datagram at 17
bytes each, and past what any battle this envelope can hold produces in one update period. Over the
cap the **oldest are dropped**, because the newest gunfire is the gunfire a player is looking at.

**Its own message rather than a block in the fragment header**, which is where design §9.2's word
"block" would have put it, and the reason is duplication: the fleet status block rides *every*
fragment precisely so it heals, and a fire list stamped on every fragment would draw every tracer
once per fragment. One message, sent once per update, is the shape that carries a list of events
rather than a piece of state.

**On the datagram lane, and this is the slice's decision record.** ADR 0029's test is "if this
message is lost, does a later one make it right?", and for a fire event the honest answer is *no* —
which is the reliable lane's case. It goes on the datagram lane anyway, and the argument is that the
question is the wrong one for a message whose only consumers are a muzzle flash, a tracer and a
turret slew. Every authoritative consequence of a shot already travels elsewhere and reliably: the
damage in the record (2.2), the death in the leave runs. A lost flash is not a lie, and late is
worse than lost — a tracer that arrives after its target has moved draws a line into empty space.

The writer filters to shots where the shooter **or** the target is in this subscriber's interest set:
being shot at from outside your view is exactly the event a player must not be denied, and the shooter
alone would deny it.

### 2.4 `GameLogic/Publisher.h` / `.cpp` — the cursor

`Subscriber` gains `shotCursor`, opened at `World::ShotHead()` for a subscriber joining a running
world exactly as `despawnCursor` is, and `Desc` gains `openingShotCursor` for the same reason the
despawn one is a field: `Add` has no `World` to read a number out of.

`PublishOne` writes the fire block after the interest update, and `Publish` trims the shot log to the
minimum cursor across every subscriber — the despawn trim's sentence, beside it, and for its reason.

The empty-update guard in `PublishOne` gains fire to its list of things that make an update worth
sending: a subscriber whose camera is over empty space while its fleet is shot at somewhere else has
nothing in its interest set and something to be told.

### 2.5 `GameLogic/WorldSnapshot.h` / `.cpp` — the receiver

`SnapshotReceiver::Accept` dispatches `KIND_FIRE`; `Fire()` returns the events accumulated since the
last `ClearFire()`, which is `Destroyed()`/`Docked()`'s idiom and for their reason — two messages in
one drain must not lose the first one's events.

### 2.6 `NeuronCore/QuicApi.cpp` — the handshake

`QUIC_ALPN` goes `"outpost-3"` → `"outpost-4"`. The record changed size and a kind byte was added, so
two builds that disagree must refuse at the handshake rather than at the parser — the rule that
string already states about itself.

### 2.7 What this slice does not touch

- **The view.** Nothing draws a tracer, flashes a muzzle, slews a turret or fills a pip row; the
  client decodes all of this and shows none of it. Slices 3 and 4.
- **Kill attribution.** The fire block says who shot at whom, and the leave run still says only that
  a ship was destroyed. Who *killed* you is deliberately absent (design §9.2, §14).
- **The simulation.** No behavior changes: no new pass, no new gate, no tuning constant that a
  recorded game would notice.
- **`AGENTS.md` and `README.md`**, for slice 1's reason: they change in slice 4, when the game a
  player boots is the one they describe.

---

## 3. What to build on

- **The despawn log** — `DespawnHead`, `DespawnsSince`, `TrimDespawnsBefore`, `Desc::openingDespawnCursor`
  and the minimum-cursor trim in `Publish` (ADR 0027). The shot log is that mechanism with a
  different record, and it should read as one.
- **`hostileMask` and the fleet status block** — the precedent for per-update state that heals, and
  the reason the hull fraction is a record field rather than an event.
- **`WriteLeaves` / `AcceptLeaves`** — the shape of a standalone message with a count and a run, and
  the `Destroyed()`/`ClearDestroyed()` accumulate-and-drain idiom the fire accessor copies.
- **`SplitTheLost`** — how a per-subscriber list is intersected with an interest set before it is
  written.

---

## 4. Acceptance

**`Tests/GameLogicTests/SnapshotTests.cpp`** — extended.

| Test | Decides |
|---|---|
| `TheFragmentSizeIsDerivedFromTheDatagram` | 21 records a fragment, and the argument for why the number matters is kept |
| `AShipRecordCarriesItsHullFraction` | a damaged ship round-trips its fraction; a whole one reads 255; an indestructible one reads 255 |
| `AFireBlockRoundTrips` | shooter, target and mount survive the wire, in order |
| `AFireBlockOverTheCapDropsTheOldest` | more than `MAX_FIRE_EVENTS` shots keep the newest |
| `AnEmptyFireBlockIsNotSent` | a quiet tick puts no fire message on the wire |

**`Tests/GameLogicTests/PublisherTests.cpp`** — extended.

| Test | Decides |
|---|---|
| `AFireEventReachesBothEnds` | a shot in a subscriber's interest set arrives at its receiver |
| `AFireEventReachesTheShotAsWellAsTheShooter` | a shot from outside the set at a ship inside it is still delivered |
| `TheShotLogIsTrimmedToTheSlowestSubscriber` | two subscribers on different phases, and the log holds what the furthest behind still needs |
| `AJoiningSubscriberHearsNoOldGunfire` | a subscriber opened at `ShotHead()` is told about shots from now on and none from before |

**The existing suites** — every other row passes unedited, `WorldStateTests` included: the save
format does not move in this slice.

**The tree**

- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`, clang-tidy clean.
- Debug|x64 builds and all four suites run. **Run them with the debug STL's bounds checking on**,
  which is what caught slice 1's stale `ShipId` after the Linux run missed it.
- No screenshot: nothing visual until slice 4.
- One decision record: **fire events ride the datagram lane** — ADR 0029 applied to a message whose
  answer to its question is "no", and why that is still right. Next free number is **0053**.
- `Combat.md` §16 marks slice 2 *in review*.

---

## 5. Assumptions the implementer may make

- **Nothing consumes any of this yet.** `GameLogicTests` is the only reader of a fire event or a
  hull fraction until slice 4, and the client compiles unchanged.
- **The shot log grows without a publisher.** So does the despawn log, for the same reason and with
  the same answer: the trim belongs to whoever is reading, and a `World` with no subscribers is a
  test or a benchmark whose life is measured in seconds.
- **A shot whose shooter or victim has since died still reports.** Both entities were recorded while
  they were live, and an id is an identity rather than a reference (ADR 0047) — the view resolves
  what it can and draws the rest from the last position it held, which is what it already does for a
  ship it is exploding.
- **One byte of fraction is the whole damage model on the wire.** No shields, no armor, no
  attribution, and no exact hull points.
