# Work order — the wire is laid once

Slice 2 of [`GameDesignPlan.md`](GameDesignPlan.md), phase 0, round two. One slice, `GameLogic` and
`Outpost`, and it waits for slice 1 because it edits the same codec files.

Two things are owed. **A shipping display bug**: `FLEET_STATUS_LAUNCHING = 6` and
`FleetOrderKind::Jump = 6` are the same value (`GameLogic/UniverseSnapshot.h:217`;
`GameLogic/ShipState.h:205-218`), so a fleet holding a Jump order with an empty manifest writes 6
and both readers draw it as LAUNCHING (`Outpost/UniverseView.cpp:569`; `Outpost/FleetSheet.cpp:151`).
The constant's own comment — "the first value no FleetOrderKind uses" — was true when it was
written and stopped being true when `Jump` was appended. **And a contended byte**: bits 3 to 5 of
the status byte are unused and three scheduled items want them (a stance, a cargo pip, and the
launching flag itself), while the kind is at exactly its three-bit capacity with seven of eight
values spent, so the next verb overflows it.

This slice fixes the bug by re-laying the block once, with room for all three, so that
`FLEET_STATUS_BYTES` moves in this ALPN bump and not again in three more. It adds no feature.

## 1. Scope

1. **`GameLogic/UniverseSnapshot.h`, `.cpp` — the fleet status block is re-laid.**

   | Was | Is |
   |---|---|
   | one `status` byte: bits 0-2 kind, bit 6 engaged, bit 7 under attack | a `kind` byte, a `flags` byte, a `stance` byte |
   | `FLEET_STATUS_LAUNCHING = 6`, a value in the kind | `FLEET_FLAG_LAUNCHING = 0x01`, a bit in the flags |
   | `FLEET_STATUS_ENGAGED = 0x40`, `FLEET_STATUS_UNDER_ATTACK = 0x80` | `FLEET_FLAG_ENGAGED = 0x02`, `FLEET_FLAG_UNDER_ATTACK = 0x04` |
   | `FLEET_STATUS_KIND_MASK` | gone: a whole byte needs no mask |

   The `kind` byte carries a `FleetOrderKind` and nothing else, so a launching fleet states both
   what it is doing and that it is launching, which the old byte could not. `flags` bits 3 to 7 and
   the whole `stance` byte are reserved, written zero and read as zero: they are the room this
   slice exists to leave, for item E4's cargo pip and item C11's stance. `FLEET_STATUS_BYTES` goes
   from 14 to 16, so the block goes from 71 bytes to 81 at five fleets, out of a 1,152-byte
   datagram.

2. **Both readers learn the two cases they were missing.** `UniverseView::FleetActivity` and
   `FleetSheet` read the launching flag rather than a kind value, and the kind switch gains
   `Jump` → `"JUMPING"`, which no reader has ever had. `UniverseView::FleetStatusBits` becomes
   `FleetStatusKind` and `FleetStatusFlags`, because one accessor returning a byte that now means
   two things is how the collision this slice repairs got in.

3. **A refused send says so.** `UniverseView::SendToSelectedFleets` drops a fleet's order silently
   when the reliable queue refuses it (`UniverseView.cpp:1224`; `NeuronCore/QuicTransport.h`'s
   `capacityReliableMessages = 32`). It logs `ORDER DROPPED | FLEET n` at Alert. One line; the
   order reply that would let the view retract a marker is item C1's second slice and plan §4
   decision 1, not this.

4. **The ALPN bumps**, `outpost-5` → `outpost-6` (`NeuronCore/QuicApi.cpp:21`), per Combat.md §9.3:
   two builds that disagree about the block's width refuse at the handshake rather than misparse.

5. **Prose in the same commit**: Combat.md §9.3's ALPN line, and the `FleetStatus` struct comment,
   which carries the new layout. **Not** `Design/Archive/Fleets.md` §8.2, which describes the old
   block and stays as it is — see §7.

## 2. Out of scope

- **`OrderReply`.** Plan §4 decision 1 and item C1's slice 1. This slice leaves the room and does
  not answer an order.
- **A second flags byte on the ship record**, which the plan's sketch and the review's
  cross-cutting section both called for in this bump. It is not owed — see §7.
- **Any stance or cargo behaviour.** The bytes are reserved and written zero; nothing reads them.
- **The kind's meaning.** No new `FleetOrderKind`, no verb, no refusal.

## 3. What to build on

- `WriteFleetBlock`'s existing shape (`UniverseSnapshot.cpp:513-620`) and its reader
  (`:834-846`), which already treat the block as coupled to no record.
- `PublisherTests`' four rows that assert the status bits, which name what each bit means and are
  the regression net for the re-lay.
- `JumpTests`, which has seven rows about jumping and not one assertion on what a jumping fleet
  *shows* — which is why the bug shipped.

## 4. How it must behave

1. A fleet holding a Jump order with an empty manifest reads back `kind == Jump` and no launching
   flag, and the readers say `JUMPING`.
2. A fleet mid-launch reads back the launching flag *and* its standing order's kind, both.
3. Engaged and under-attack mean exactly what they meant; the two bits differ exactly when the
   alert outlives the fight, as before.
4. The reserved flag bits and the stance byte are zero on the wire and zero on read.
5. A refused reliable send logs once per fleet it dropped, and the fleets that did send still do.

## 5. Acceptance

- `PublisherTests`' status rows, rewritten against the new fields and green.
- **A new `JumpTests` row**: a fleet ordered to jump shows `Jump` and not launching — the assertion
  whose absence let the collision ship.
- **A new `SnapshotTests` row**: the reserved flag bits and the stance byte survive a round trip as
  zero, so the day one is used the test says what it was.
- The whole suite set green; `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy over `GameLogic`.
- No decision record: a byte that moves inside a format that already versions itself decides
  nothing, and the collision was a defect rather than a choice.

## 6. Assumptions the implementer may make

- **The stance byte's meaning is C11's**, not this slice's. Reserved means written zero, and the
  first value is defined by the design that gives a fleet a stance.
- **Ten bytes per update is the price** of laying it once: five fleets by two bytes, against 1,152.

---

## 7. What changed on contact, and what is deliberately not here

**The ship record does not gain a second flags byte, and the plan's sketch was wrong to ask for
one here.** The review's cross-cutting section resolved that the ship record should grow a flags
byte in this same ALPN bump, "rather than rationing six bits three ways". Reading the record
rather than the review: `ShipSnapshot::flags` spends two bits (`SHIP_FLAG_STATION`,
`SHIP_FLAG_GATE`) and has **six free** (`UniverseSnapshot.h:99-106`). The three claimants are item
E3's resource-site flag (one bit, phase 1), item C7's fired-recently bit (one bit, phase 2) and
item C10's five effect flags (phase 4) — seven bits wanted against six free, so the shortage is
exactly one bit and it does not bite until phase 4. Against that, a byte on the ship record is
paid **per ship per update**: at 23 ships a fragment it costs a fragment its 24th ship, on every
update, from today until the phase-4 item that needs it. The fleet block's ten bytes are paid five
times per update and are contended by a phase-1 item; the ship record's byte would be paid three
hundred times per update and is contended by nothing for two phases. So the fleet block is re-laid
and the ship record is left alone, and the day C10 lands it bumps the ALPN itself — which slice 1
made cheap, and which this tree has already done five times.

**`JUMPING` is a kind, not a flag.** The cross-cutting section listed it beside LAUNCHING in the
flags byte. There is no state it could name: a jump is a despawn and a spawn under one identity
(ADR 0056) with nothing in between, so a fleet is either holding a Jump order — which the kind
byte says — or it is somewhere else. The flags byte carries LAUNCHING and reserves the rest.

**`FleetStatusBits` had to split.** One accessor returning "the status byte" was the shape that let
a kind and a flag share a value in the first place; two accessors that each return one thing make
the collision unspellable rather than merely fixed.

**The archived design is not amended, and this order is the record that supersedes it.**
`Design/Archive/Fleets.md` §8.2 describes the status byte as "bits 0-2 kind shown … bit 6 engaged,
bit 7 under attack", and its slice-5 amendment argues at length that the kind is "a `FleetOrderKind`
or the value 6, `Launching`" and must never become an enumerator. That argument was right about the
enum and wrong about the byte, and the value it defended is exactly the one `Jump` later took. But
`Design/README.md` is explicit that an archived design is finished and that amending one would be
rewriting history rather than maintaining a live document: what supersedes it is a later record
naming it, which is this section. §1.5 of this order originally said to amend that file, and that
instruction was itself against the rules; it is corrected above rather than quietly dropped.

**The ten bytes cost a fragment nothing, and that was measured rather than assumed.** The block goes
from 71 bytes to 81 at five fleets, and `ShipsPerSnapshotFragment()` sizes against that worst case
whatever an update actually carries (`Fleets.md` §8.2's slice-5 amendment). A fragment held 21 ship
records before this slice and holds 21 after: the datagram is 1,152, the header 27 and a record 48,
so the block would have to reach 117 bytes before a record fell out, and it is at 81. The function
was called rather than the arithmetic trusted, and it answers 21.

While checking it: **`Fleets.md` §8.2's "22 records a fragment against 23" is stale**, from before
combat put a hull fraction in the record and took it to 48 bytes. `UniverseSnapshot.cpp`'s own
comment has said 21 since. Named here rather than corrected there, for the archived-design reason
above.

## 8. What was verified, and how — and the honest gap

**Compiled and run here, on Linux with clang behind the shim slice 1 used:** every `GameLogic`
source, and `ShipsPerSnapshotFragment()` called for the number above. `PublisherTests.cpp` and
`JumpTests.cpp` compile clean behind a stub of the test framework, which catches a typo and proves
nothing about the framework. `python Build/CheckFormat.py` and `python Build/CheckProjectFiles.py`
pass.

**Run by CI and by nobody here:** the four suites and `Outpost`. The rows that matter are the new
`AJumpingFleetSaysSoAndNotLaunching`, which fails if the collision ever comes back, and the two
reserved-field assertions. The client half — the `JUMPING` label, the `ORDER DROPPED` line, the
sheet reading the flag — is `Outpost`, compiled by CI and demonstrated by nobody, as slices 1 and 5
were. CI-green is the gate the owner set on 2026-09-02.

**A thing a reviewer on Windows can check in a minute, if they ever want to:** order a fleet through
a gate and read its button. It said `LAUNCHING` before this slice and says `JUMPING` after.
