# Work order — slice 15: the quantized wire

Cut from [`MmoScalabilityPlan.md`](MmoScalabilityPlan.md) §6 slice 15, against the tree at `f301e0e`.
It retires finding E5 of [`MmoScalabilityReview.md`](MmoScalabilityReview.md): the ship record is 83
uncompressed bytes and the tree's own prose has been describing a quantized wire that does not exist.

One slice, one branch, one pull request. Layer: `GameLogic`, plus the one-line ALPN bump in
`NeuronCore` that the plan schedules here.

---

## 1. Scope

**The ship record's positions become a sector pair plus lattice offsets, and its angles become
turns16.** Nothing else about the format moves.

| Field | Today | After | Bytes |
|---|---|---|---|
| `handle` | slot u32, generation u32 | unchanged | 8 |
| `posWorld` | i64, i64, f32, f32 | **i32, i32, u16, u16** | 24 → 12 |
| `prevPos` | i64, i64, f32, f32 | **i16, i16** — a lattice delta from `posWorld` | 24 → 4 |
| `headingRad` | f32 | **u16 turns16** | 4 → 2 |
| `prevHeading` | f32 | **u16 turns16** | 4 → 2 |
| `speed`, `accelSample`, `turnRateRadPerSec` | f32 each | unchanged | 12 |
| `order`, `factionId`, `flags` | u8 each | unchanged | 3 |
| `hullId` | u32 | unchanged | 4 |
| | **83** | | **47** |

Three things this table decides, each argued in §2:

1. **The lattice is 0.125 m.** `SECTOR_SIZE_METRES` is 8,192, so a sector is exactly 65,536 steps
   and a local offset is a u16 with nothing left over and nothing wasted.
2. **The wire's sector index is 32 bits**, where the simulation's is 64. A decision record is due
   for it (§6) — it narrows a stated range, and it is the first thing a reader will ask about.
3. **`prevPos` travels as a delta on that same lattice**, not derived receiver-side. The plan's
   §6 sentence proposed derivation; §2.3 says why the delta is what landed and what derivation
   would have cost.

**The ALPN bumps to `outpost-3`** (`NeuronCore/QuicApi.cpp`). The record changed; the negotiation
says so, exactly as it did for `outpost-2` (ADR 0029).

**Prose that is describing this slice becomes true in the same commit**, and prose that is *still*
not true is corrected rather than left:

- `.clang-tidy`'s `-bugprone-narrowing-conversions` block says quantization is pervasive — "metres
  to centimetres, radians to turns16". Turns16 becomes real here; centimetres never do, because the
  lattice is eighth-metres. The sentence states what the tree actually does.
- `GameLogic/WorldSnapshot.h`'s header comment gains the record's shape, since "fields are written
  one at a time" is now doing more work than it says.
- `Collision.md` §3's "a snapshot position compresses to a sector id plus a quantised
  local offset" **is not edited**. It is an archived design and `Design/README.md` forbids rewriting
  one to match what was built; the sentence was a promise and this slice keeps it.
- `AGENTS.md` R6's "a wire in centimeters" is **already gone** — slice 21 removed it. So is
  `WorldSnapshot.h`'s copy of the same claim. The plan's §6 sentence for this slice predates that
  and is stale; nothing is owed there.

## 2. The decisions this order takes

### 2.1 Why 0.125 m and not centimetres

A centimetre lattice needs 819,200 steps per sector — 20 bits, so a local offset is three bytes or
a packed field spanning a byte boundary. 0.125 m needs exactly 16. The error it buys is **6.25 cm**
at worst (round-to-nearest of a 12.5 cm step), against ship capsule radii of 1.1 m and upward and a
hull that is metres across: a quarter of the smallest hull's radius is 27 cm, so the quantization is
invisible at the scale anything is drawn or collided at, and collision never sees it at all — this
is the wire, and the simulation is untouched.

The rounding **carries** rather than clamps. A position 1 mm short of a sector border scales to
65,535.99 steps, rounds to 65,536, and that is the next sector's origin — not a seventeenth bit. So
the encode is monotonic across a border, which is the property a clamp would break by putting the
ship at the far corner of the sector it was leaving.

### 2.2 Why the wire's sector is 32 bits

`WorldPos` carries i64 sectors and therefore ±7.6×10²² m, about eight million light years
(`WorldPos.h`, re-trued by slice 21). Sixteen bytes of every record spend that range on a number
that is 0, ±1 or ±2 in every world this tree has ever built — which is finding E5 restated.

An i32 sector index addresses ±2,147,483,647 × 8,192 m = **±1.76×10¹³ m, about ±1,858 light years**.
That is the wire's range, not the simulation's: `World` keeps i64 and nothing about the plane
changes. A position outside it **saturates** rather than wrapping, so the failure mode is a ship
pinned at the edge of the addressable universe rather than one that appears on the other side of it.
Widening is a format revision behind an ALPN bump, which is what this slice is doing today.

### 2.3 Why `prevPos` is a delta, not a derivation

The plan proposed deriving `prevPos` receiver-side for handles already held, and sending it in full
on first sight. Three things are wrong with that against this tree:

- **The server cannot know what the receiver holds.** Snapshots are datagrams and an incomplete
  update is dropped whole, so "this subscriber's interest set contained the handle last update" is
  not "this client holds a record for it". Every dropped entering-update would produce a record
  whose `prevPos` the client has no way to build.
- **Derivation changes what the field means.** Updates go out every `INTEREST_UPDATE_EVERY_TICKS`
  (6), so the previous record is six ticks old, while `prevPos` is defined as *the tick before* and
  is what `WorldView::SampleOf` divides by one tick to get velocity. A derived value would be an
  average over six ticks wearing the name of an instantaneous one.
- **It makes the record variable-length**, which `ShipsPerSnapshotFragment` is derived from.

A delta costs 4 bytes and has none of that. Both positions are quantized to the same world lattice
first, and the wire carries the *integer step difference* — so the receiver reconstructs the
quantized `prevPos` exactly, and its only error against the true one is its own 6.25 cm rounding.
Quantizing the delta against the raw `posWorld` instead would have stacked two roundings and
doubled the bound; this is why the encode quantizes `posWorld` first and measures from the result.

Range: ±32,767 steps is **±4,096 m per tick**. The fastest hull covers 34 / 60 = 0.567 m — 4.5 steps
— so the headroom is about 7,000×. The delta saturates too, and a saturated one costs a single
interpolation sample.

### 2.4 What quantization changes in the view, stated rather than discovered

`WorldView.cpp:127-130` starts a new interpolation sample only when a record's position or heading
differs from the one it holds. On a quantized wire, a ship moving less than one step between
updates reads as unchanged and holds its pose. At 10 Hz updates that is a ship slower than
**1.25 m/s**; the pose it holds is within 6.25 cm of where it should be, which is smaller than the
error the wire already carries. No change is made in `Outpost` for this, and the client is not
touched by this slice at all: `ShipSnapshot` stays float-valued and the codec is the only thing
that knows about steps.

## 3. Out of scope

- **The three remaining floats** — `speed`, `accelSample`, `turnRateRadPerSec` — and `hullId`.
  Twelve bytes and four; quantizing them would take the record to about 33 and the fragment to 33
  ships. They are not positions and not angles, the plan's scope names positions and angles, and
  each needs its own argument about what precision the view actually needs. Named here so the next
  reader knows the number was measured and left, not missed.
- **Delta-against-baseline compression.** A record still states itself and depends on no earlier
  one. `prevPos`'s delta is against `posWorld` *in the same record*, which is not a baseline.
- **The order messages.** `WriteMoveOrder` and `WriteDockOrder` keep a full `WorldPos` destination:
  one per message rather than one per ship, it is a player's click and it feeds the *simulation*
  through `IssueMoveOrder`, so rounding it changes recorded outcomes. Twenty-four bytes on a
  reliable message that carries at most one of them is not where the bytes are.
- **The snapshot and leave headers.** Untouched at 27 and 21 bytes.
- **Anything in `Outpost` or `NeuronClient`.** §2.4.
- **`WorldPos` itself.** The simulation's representation does not change, and no simulated value
  passes through the lattice.

## 4. What to build on

- `GameLogic/WorldSnapshot.cpp` — `ByteWriter`/`ByteReader` in the anonymous namespace, and
  `SHIP_RECORD_BYTES`, from which `ShipsPerSnapshotFragment` already derives.
- `GameLogic/WorldPos.h` — the invariant that `localX`/`localZ` are in `[0, SECTOR_SIZE_METRES)`,
  and `Translate`, whose sector carry is exact for a power-of-two sector and is what reconstructs
  a `prevPos` from a delta.
- `GameLogic/SimTuning.h` — `SECTOR_SIZE_METRES` = 8,192, and the `static_assert` that it is a power
  of two.
- `Tests/GameLogicTests/SnapshotTests.cpp` — `CaptureTransport`, `SpawnAt`, `Find`, and
  `OneShipRoundTripsFieldForField`, which asserts exact equality on all four changed fields and
  becomes a bounded assertion here.
- `NeuronCore/QuicApi.cpp` — `QUIC_ALPN`.

## 5. Acceptance

- [ ] **Round-trip error bounds asserted**: ≤ 0.0625 m on `posWorld` and on the reconstructed
      `prevPos`, ≤ π/2¹⁶ rad on `headingRad` and `prevHeading`, over a swept set of positions and
      angles including sector borders and both signs.
- [ ] **Interpolation continuity across a sector boundary tested**: a ship carried across a border
      decodes to a position whose offset from the previous one is one tick of travel, with no jump
      of a sector's width.
- [ ] **`prevPos`'s delta is exact on the lattice**: the reconstructed `prevPos` equals the
      quantized true `prevPos` bit for bit, not merely within the bound.
- [ ] **A heading round-trips as an angle**, not as a number: the assertion is on the wrapped
      difference, because the decode's range is (-π, π] and a source of exactly -π comes back as +π.
- [ ] **Records per fragment stated in the pull request** against the 13 of today, with the loss
      arithmetic E1 cares about.
- [ ] **The replay gate is untouched.** The wire is not simulated; `TheSameOrderProducesTheSameRun`
      and the rest of `GameLogicTests` pass unchanged.
- [ ] `python Build/CheckFormat.py` and `python Build/CheckProjectFiles.py` green.

## 6. Decision record due

**The wire's sector index is 32 bits.** §2.2 narrows a range this tree has stated in two places and
re-trued once; a reasonable person will propose keeping i64 again. The record carries the
arithmetic, the saturation behaviour, and what widening it would cost.

The plan's §6 table forecast no record for this slice. AGENTS.md §9's own trigger — "an approach is
rejected that a reasonable person will propose again" — outranks that forecast, and the record is
the cheaper of the two ways to find out.

## 7. Assumptions the implementer may make

- **No screenshot is owed.** Nothing visual changes: the client's decoded types are unchanged and
  §2.4's held pose is under the wire's own error.
- **`Debug.h`'s asserts stay out of `GameLogic`**, which uses none today. Saturation is the failure
  behaviour and it is commented at the clamp, per AGENTS.md §5's rule that a library reports rather
  than crashes.
- **Both ends are one binary today**, so the ALPN bump is a statement rather than a migration; there
  is nothing on `outpost-2` to keep talking to.
