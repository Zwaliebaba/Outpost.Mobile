# 0042 — The wire's sector index is 32 bits, and a position rides a 0.125 m lattice

Status: accepted
Date: 2026-08-30

## Context

`WorldPos` is a sector pair and a local offset (ADR 0007, `Design/Archive/Collision.md` §3). The
sector fields are `std::int64_t`, which reaches ±7.6×10²² m — about eight million light years — and
the local offset carries a uniform 0.49 mm of precision everywhere.

The ship record on the wire carried that representation whole: two `WorldPos` per ship, each 24
bytes, inside an 83-byte record. `MmoScalabilityReview.md` finding E5 measured what that costs — 13
ships per 1,152-byte datagram, so a 500-ship update is 39 fragments, and at 2% datagram loss an
update that size completes 45% of the time. It also noted that the tree's own prose had been
describing a quantized wire for a year: `.clang-tidy` speaks of "metres to centimetres, radians to
turns16", and `Collision.md` §3 promises that "a snapshot position compresses to a sector id plus a
quantised local offset". No such layer existed.

Sixteen of those 24 bytes are the sector pair — a number that is 0, ±1 or ±2 in every world this
tree has built, and whose range is spent on distances no client will ever be told about, because a
subscriber's interest radius is 2,000 m.

## Decision

The wire's ship record quantizes, and the wire's sector index narrows to `std::int32_t`:

| | |
|---|---|
| Lattice | 0.125 m a step. `SECTOR_SIZE_METRES` is 8,192, so a sector is exactly 65,536 steps and a local offset is a `std::uint16_t` with nothing left over |
| Sector | `std::int32_t` per axis — ±2,147,483,647 sectors, **±1.76×10¹³ m, about ±1,858 light years** |
| `prevPos` | an integer step delta from `posWorld`, two `std::int16_t` — ±4,096 m per tick |
| Angles | turns16: a `std::uint16_t` of 65,536 steps to the circle |

The record goes 83 → 47 bytes and a fragment 13 → 23 ships. Rounding is to nearest, so the cost is
**6.25 cm** on a position axis and **π/2¹⁶ rad** on an angle. A sector outside the wire's range
**saturates** rather than wrapping.

`WorldPos` itself does not change. This is the wire's range and the wire's precision; the simulation
keeps i64 sectors and float offsets, no simulated value passes through the lattice, and the replay
contract does not see any of it.

## Alternatives considered

- **Keep i64 sectors and quantize only the offsets.** The obvious minimal move, and it is what the
  plan's slice sentence literally asks for. Rejected on arithmetic: it lands the record at 55 bytes
  and 20 ships a fragment, against 47 and 23 — so it forgoes a third of the remaining win to keep a
  range whose nearest use is eight million light years away. The wire is versioned by ALPN; the
  simulation's range is not, and that asymmetry is the whole reason it is safe to spend here.
- **A centimetre lattice**, which is what two comments in this tree promised. Rejected because
  819,200 steps per sector needs 20 bits: the field either spans a byte boundary or costs three
  bytes to carry two bytes of information. 0.125 m needs exactly 16, and the error it buys is a
  twentieth of the smallest hull's capsule radius.
- **A sector origin in the fragment header, with `std::int8_t` deltas per record.** Cheaper still —
  2 bytes a record instead of 8, so 39 ships a fragment. Rejected because it couples every record to
  a header field and introduces a failure case the full-world path cannot rule out: `Write` has no
  subscriber to centre on, and a world spanning more than ±127 sectors would encode ships into the
  wrong place. Revisitable the day interest is the only path.
- **Derive `prevPos` receiver-side** rather than sending a delta, which is what
  `MmoScalabilityPlan.md` §6 proposed. Rejected on three counts, in
  `Design/Archive/QuantizedWire-work-order.md` §2.3: the server cannot know what a client holds
  (datagrams drop, and an incomplete update is dropped whole); updates are six ticks apart while
  `prevPos` means *the tick before*, so a derived value would be a six-tick average wearing an
  instantaneous name; and it makes the record variable-length, which
  `ShipsPerSnapshotFragment` derives from. The delta costs four bytes and has none of that.
- **Quantize the three remaining floats** — `speed`, `accelSample`, `turnRateRadPerSec` — and
  `hullId`. Measured: it would take the record to about 33 bytes and 33 ships a fragment. Not
  rejected, deferred: they are neither positions nor angles, and each needs its own argument about
  what precision the view actually needs. Recorded here so the number is not rediscovered.
- **Delta-against-baseline compression.** The larger win and the larger commitment: it makes a
  record depend on an earlier one, which is the property that makes datagram loss compound. Out of
  scope for the same reason `Design/Archive/Collision-slice-6.md` gave — the payload shrinks by
  sending fewer entities first, and by sending smaller ones second.

## Consequences

- **The ALPN bumps to `outpost-3`.** Two builds that disagree about the record now refuse at the
  handshake rather than decoding each other's bytes as a different field, which is the same
  reasoning `outpost-2` landed under (ADR 0029).
- **A 500-ship update is 22 fragments instead of 39**, and completes 64% of the time at 2% loss
  against 45%. A hundred-ship update is 5 fragments instead of 8, 4,835 bytes instead of 8,516.
- **A position a millimetre short of a sector border encodes into the next sector**, because the
  rounding carries rather than clamps. That keeps the encode monotonic across a border; a clamp
  would put such a ship 0.124 m back inside the sector it was leaving and flip the sign of the
  error at the boundary.
- **`WorldView` starts a new interpolation sample only when a record differs from the one it
  holds**, so a ship moving less than one step between updates now holds its pose. At 10 Hz that is
  a ship slower than 1.25 m/s, and the pose it holds is within 6.25 cm — under the wire's own error.
  No client change was made for it.
- **The order messages keep a full `WorldPos` destination.** One per message rather than one per
  ship, and it feeds `IssueMoveOrder` — rounding a player's click would change recorded outcomes.
- **`.clang-tidy`'s narrowing-conversions note becomes true**, and is corrected where it was not:
  the tree quantizes metres to eighth-metres, never to centimetres.
- The saturation paths are unreachable from anything the simulation can produce — the fastest hull
  covers 4.5 steps in a tick against a delta range of 32,767, and the nearest content to the sector
  bound is five sectors from the origin — so they are stated and commented rather than tested
  against a case that does not exist.
