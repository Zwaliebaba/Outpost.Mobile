# Work order — Collision slice 6: interest management

Implements slice 6 of [`Collision.md`](Collision.md) §19: a snapshot carries what one subscriber can
see rather than the whole world, with entering and leaving derived rather than diffed by the client,
and distant entities updated less often than near ones.

**Layers:** `GameLogic` and `Outpost`, plus their suites.
**Depends on:** slice 2 (`QueryCircle`, the cell decomposition, the static/dynamic split) and slice
2b (the transport, the wire format, the client that reads a snapshot).
**Blocks:** nothing. This is the last slice in `Design/Collision.md`.

---

## 1. What this is for, and the claim it has to make good

§1 says the spatial index's second customer is interest management, and that it is the hardest
problem in the MMO target because **snapshot cost is O(N²) in connected players without it and
O(N · k) with it**. Slice 2 built the primitive. This spends it.

§1 is also careful about what is *not* free, and this work order inherits that care verbatim:

> `QueryCircle`, the cell decomposition and the static/dynamic split are all reused unchanged; the
> subscription set, the enter/leave delta and the priority accumulator are new code that sits on top
> of them.

---

## 2. The constraint to settle before any code: there is one client

Slice 2b's work order assumed one client — "the loopback has two ends, not N" — and the code
implements exactly that. There is no connection handshake, no player, no second viewer. So the
quadratic this slice exists to defeat has, today, nothing to be quadratic in.

That is not a reason to defer it, and it is a reason to be precise about what "done" means.

**The payoff is demonstrable with one subscriber, and this slice must demonstrate it.** Put N ships
in a world, one subscriber in the middle with radius r, and measure the bytes an update costs.
Without interest management that number grows with N. With it, it grows with **k**, the number of
ships within r, and stops growing once N exceeds the neighbourhood. That is the whole claim, it is
measurable today with the tree as it stands, and a benchmark that prints it is worth more than a
second client would be.

**What genuinely waits for a second client**, and must not be built here: anything per-connection
that only makes sense in the plural — fairness between subscribers, a global send budget divided
between them, prioritising one player's view over another's. Say so rather than leaving a hook.

---

## 3. Scope

### 3.1 `InterestSet`: one subscriber's view of the world

New, in `GameLogic/InterestSet.h/.cpp`. It owns the per-subscriber state §1 says is the new work.

```cpp
struct InterestDesc
{
  float radiusMetres = 2000.0f;   // what this subscriber can see
  std::uint32_t updateEveryTicks = 6;  // 10 Hz against a 60 Hz tick
};
```

Per update it produces three things from `QueryCircle` and the previous set:

- **entered** — handles in the new set and not the old. Sent in full; the client has never seen them.
- **left** — handles in the old set and not the new. Sent as bare handles; the client drops them.
- **refreshed** — handles in both, whose priority came due this update.

### 3.2 The update rate is decoupled from the tick, and counted in ticks

§1's table asks for 5–20 Hz against a 60 Hz tick. `updateEveryTicks = 6` is 10 Hz and the default.

Counted in ticks for the same reason slice 2b counted latency in ticks: a wall clock makes the
result depend on how fast the machine ran, and `AGENTS.md` §5 bans one in the simulation. It also
makes "10 Hz" a thing a test can assert rather than approximate.

### 3.3 Enter and leave are derived from sorted sets, not from a map

Both sets are `std::vector<ShipHandle>` kept sorted by `(slot, generation)` — a total order, so the
result does not depend on the order `QueryCircle` returned. `std::set_difference` both ways gives
entered and left in one pass each.

**No `unordered_map` and no map keyed on anything.** `AGENTS.md` §5 bans iteration order that is not
dense-array order, and this is precisely the code where a hash map would be the obvious choice and
would put hashing into the replay contract. The sort is O(k log k) on a set capped by the radius,
run at 10 Hz rather than 60, which is not the cost §1 warns about — that warning is about diffing
the *whole world* per player per tick, and this diffs a bounded neighbourhood six times less often.

### 3.4 Priority: near entities every update, far ones less often

A float per subscribed entity, parallel to the sorted handle vector.

```
weight = clamp(1 - distance / radius, INTEREST_MIN_WEIGHT, 1)
priority += weight
if (priority >= 1) { refresh it; priority -= 1 }
```

An entity at the subscriber's own position refreshes every update. One at the edge of the radius
refreshes every `1 / INTEREST_MIN_WEIGHT` updates. An entity that enters starts at priority 1, so it
is sent the moment it is seen and never waits for its turn.

Subtracting one rather than zeroing is what keeps the average rate correct: zeroing would round
every entity's rate down to the next whole number of updates.

### 3.5 The snapshot becomes incremental

The client stops receiving a whole world and starts receiving changes to one, so
`SnapshotReceiver` changes from **replace** to **upsert**:

- a record for a handle it holds updates it in place;
- a record for a handle it does not hold appends it;
- a handle in the leave list removes it.

The datagram gains a leave-count and a run of bare handles after the records. Fragmentation, the
`snapshotId`, the drop-an-incomplete-snapshot-whole rule and the ignore-a-stale-tick rule all stay
exactly as slice 2b built them — an incomplete *delta* is more dangerous than an incomplete
snapshot, not less, because the client cannot resynchronise by waiting.

**One consequence to state plainly:** a dropped update is no longer self-healing. Under slice 2b the
next full snapshot corrected anything lost; a delta stream does not. This slice does **not** add
acknowledgement or retransmission — it adds the thing that makes them necessary, and names it here
so the next slice starts from a written problem rather than a bug report.

### 3.6 The composition root gains a subscriber

`WorldSimulation` holds one `InterestSet` beside its `SnapshotWriter` and consults it each tick. The
subscriber's centre is the camera focus the client already sends nothing about — so for now it is
**the centroid of the client's own ships**, computed server-side from the world, which needs no new
message and no new client state. When there is a real player with a real viewpoint, that is where it
comes from instead.

---

## 4. Out of scope

- **A second client, a connection, a handshake.** §2 above says what waits for one and why.
- **Acknowledgement, retransmission, reliable delivery.** Named in §3.5 as the thing this creates
  the need for.
- **Region sharding and ghost zones.** §3 of the design notes the sector is the unit for both. Not
  here, and no hook.
- **Compression or quantisation.** The payload shrinks by sending fewer entities, not smaller ones.
- **Interest in anything but ships.** Weapons range, sensor range and blast radius are §1's
  customers three through five and all sit on the same `QueryCircle`. None is built here.
- **Changing `QueryCircle`, the cell decomposition or the static/dynamic split.** §1 says they are
  reused unchanged, and if this slice needs to change one, that is a finding worth a record.

---

## 5. What to build on

| File | What it already gives you |
|---|---|
| `GameLogic/SpatialIndex.h` | `QueryCircle`, whose signature §1 says was chosen for this customer |
| `GameLogic/WorldSnapshot.h` | The record, the fragmenting writer, the reassembling receiver |
| `GameLogic/ShipState.h` | `ShipHandle`, and why a wire uses one |
| `Outpost/WorldSimulation.h` | The tick where both directions already happen, in order |
| `Design/Collision.md` §1 | The distinction between what is reused and what is new |

---

## 6. What will surprise the implementer

### 6.1 The client's ship order stops being the world's

Under slice 2b a snapshot listed every ship in world order, so `WorldView::m_ships` could be kept
parallel to it by index. An upserted set has no such order: a ship appended when it entered sits
wherever it was appended. `ApplySnapshot` already carries presentation state by handle, so it
survives — but anything that assumed snapshot index equals world index is now wrong, and the
minimap, the bottom bar and the selection all index by position in the snapshot.

They stay correct, because they only ever index into the snapshot they were handed. It is worth
checking each one rather than assuming.

### 6.2 A ship leaving interest looks exactly like a ship dying

Both arrive as "this handle is gone". The client cannot tell them apart and must not try: a ship
that leaves the radius and comes back is a leave followed by an enter, and the presentation state in
between is lost — its selection ring, its trail. For the player's own ships this never happens,
because the interest centre is derived from them. For anything else it is correct behaviour and not
a bug, and it is the reason a real client eventually keeps a longer-lived record than `ShipView`.

### 6.3 The benchmark is the deliverable, not a nicety

§2 makes it the demonstration that this slice did what it exists to do. It must print bytes per
update at several N with a fixed radius, and the shape of that curve is the acceptance criterion.

---

## 7. Decision records due

At least one: **enter and leave come from sorted-vector set differences, not a hash map.** The map is
the obvious implementation, someone will propose it, and the reason it loses — iteration order in the
replay contract — is exactly the kind of thing a record exists to hold.

A second if §3.4's priority scheme is argued: the stateless alternative (an update period per
distance band, phased by handle) needs no per-entity state and someone will prefer it.

---

## 8. Acceptance

**The set arithmetic**, in `GameLogicTests`:

- A ship inside the radius is in the set; one outside is not; one exactly on the boundary is treated
  the same way every time.
- A ship moving in produces exactly one enter and no leave; moving out, one leave and no enter.
- A ship that stays produces neither, whatever order `QueryCircle` returned.
- Spawning the same ships in a different order produces the same entered and left sets, in the same
  order. This is `ArrayOrderCannotChangeTheAnswer` for interest.
- A despawned ship leaves the set, and its handle does not resolve afterwards.

**The update rate:**

- With `updateEveryTicks = 6`, sixty ticks produce ten updates, not eleven and not nine.
- A ship at the centre is refreshed on every update.
- A ship at the edge is refreshed at about `INTEREST_MIN_WEIGHT` of that rate, measured over enough
  updates to be a rate rather than a coincidence.
- An entering ship is sent on the update it entered, never later.

**The wire:**

- An entered ship round-trips every field, as slice 2b's test does.
- A left handle removes exactly that ship from the client's world and nothing else.
- A refreshed ship updates in place: the client's set does not grow.
- An incomplete delta is dropped whole and the client's world is unchanged rather than partly
  updated.

**The payoff, which is the point:**

- A benchmark printing bytes per update for N = 100, 1,000 and 5,000 at a fixed radius, with the
  count of ships actually sent. Bytes must track k and not N once N exceeds the neighbourhood.
- The same numbers with interest disabled, for contrast. One line each, in the test output, the way
  the slice-2 benchmark already does it.

**The tree:**

- `CheckProjectFiles.py` and `CheckFormat.py` pass; new files registered in both project files.
- All existing tests pass. The simulation is untouched by this slice, so any that fail are a
  finding.
- Debug|x64 green in CI including clang-tidy.
- No screenshot is due: at a radius that covers the starting fleet the client sees exactly what it
  saw before, and this work order says so as the stated assumption.

**The documents:**

- §19 marks slice 6 `landed`, and the design's status line stops saying it is not implemented.
- §1's "not free" paragraph becomes past tense, and anything it predicted that turned out wrong is
  corrected where it stands.
- The records from §7 exist and the index lists them.

---

## 9. Assumptions the implementer may make

- **One subscriber.** Not a design limit written into the types — `InterestSet` is per-subscriber by
  construction, so a second is another instance — but nothing needs to serve two today.
- **The interest centre is the centroid of the client's own ships.** It needs no new message, and
  the day there is a camera on the wire it comes from there instead.
- **A fixed radius.** Per-hull sensor ranges are §1's customer four and are not this slice.
- **Loss is what slice 2b's `dropOneInN` provides.** No jitter, no reordering; the transport is
  ordered and the reassembly relies on it, as it already did.
- **Recordings are unaffected.** Interest management changes what is *sent*, not what is
  *simulated*, so nothing here is in the replay contract.
