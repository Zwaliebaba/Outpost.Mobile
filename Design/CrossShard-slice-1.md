# Work order — Cross-shard slice 1: the partition, and a file per shard

Implements slice 1 of [`CrossShard.md`](CrossShard.md) §8, the first slice of that design and the
only one that lands before anything is handed anywhere.

**Layer:** `GameLogic` + `Tools/UniverseGen`.
**Depends on:** nothing. Slices 2 to 5 depend on it.
**Blocks:** everything else in the cross-shard design.

---

## 1. Why this is a slice on its own

The design's §2 makes one claim: **which systems a shard holds is a function of the galaxy layout,
not a table somebody maintains.** Every participant — each shard, and every client — must agree
without being told, for the reason the galaxy is a seed rather than a file: two copies of an
authored partition are two chances to disagree, and the disagreement is a ship that arrives nowhere.

That function, the shard count that parameterises it, and a tool that can write a universe for a
shard other than 0 are the whole of this slice. Nothing is handed off, no outbox exists, and the
game still runs one universe. What changes is that the tree can *describe* a galaxy split across
shards, and can write the files for one.

It is separable because it is provable on its own: a partition is a pure function, and pure
functions are what `GameLogicTests` is best at.

## 2. Scope

1. **`GameLogic/GalaxyLayout` — `ShardOfSystem(site, count)`.**

   A pure function of `SystemSite::cellQ`/`cellR` — which are world-fixed — and the shard count.
   Deterministic, total, and the same answer on every machine and in every build.

   **Contiguity is the requirement and cheapness is not** (§2). A partition that scatters
   neighbours across shards turns most gates into wire crossings, which is the one cost the whole
   design exists to bound. A block or ring split over the lattice keeps neighbours together; a hash
   does not and is rejected for exactly that. The implementer picks between block and ring on
   measured neighbour retention (§4), not on taste.

   `count == 1` answers 0 for every system, which is what makes this landable today: the shipped
   galaxy is a one-shard galaxy and this function says so.

2. **`GalaxyDesc` gains the shard count**, and the save header carries it.

   It is deployment configuration (`Server.cfg`, ADR 0043) and it is in the header **because a
   universe generated for four shards is not a universe four other shards can run** (§2). A reader
   that finds a count it did not expect refuses the file, on ADR 0057's terms and with ADR 0061's
   window: this is a `SAVE_FILE_FORMAT` bump, the first since the file arrived, and the format-1
   fixture is committed with it under the rule ADR 0061 sets.

3. **`Tools/UniverseGen` writes one file per shard.**

   A third argument, the shard, defaulting to 0 so every existing invocation means what it meant.
   The tool lays out the one galaxy, then writes the universe each shard holds — the systems
   `ShardOfSystem` gives it, and the gates and stations standing in them.

   `UniverseGen --help` says so, and the census it prints gains the shard and the count.

4. **Prose in the same commit**: `CrossShard.md` §8's slice 1 row records what landed; ADR 0058's
   consequences gain a line, since "the tool always writes shard 0" stops being true and that
   sentence is in the record's own consequences rather than its decision.

5. **A decision record**: the partition is a function of the layout, naming the hash that lost and
   why contiguity is the property that decided it.

## 3. Out of scope

- **The outbox, the inbox, and any handoff.** Slice 2, and it is where the design is proved.
- **`StepJumps`.** It is unchanged. A gate whose destination is on another shard is not yet
  detectable *and must not be*: §3's one-branch change is slice 2's, and adding it here would leave
  a branch nothing can complete.
- **Anything on the wire.** No message, no ALPN bump.
- **A second process.** Slice 5, and it waits on a headless root that does not exist.

## 4. How it must behave

1. `ShardOfSystem` is pure, total and deterministic: the same site and count answer the same shard
   in every build, and no clock, allocation or global is consulted.
2. **Every shard is non-empty for every count from 1 to the number of systems.** A partition that
   can hand a shard nothing is a deployment that boots a process with no universe.
3. **Neighbours stay together, measured.** Over the shipped 54-system galaxy at counts 2, 3 and 4,
   the fraction of the 68 links whose ends share a shard is recorded in the test as a floor, not
   asserted as a guess. The chosen split beats a hash of the same cells on that number, and the test
   says by how much.
4. A universe written for shard N holds exactly the systems `ShardOfSystem` gives N, and the union
   over every shard is the whole galaxy with nothing counted twice.
5. A save whose header names a different shard count than the reader was configured for is refused,
   and the refusal names both numbers.

## 5. Acceptance

- `GalaxyLayoutTests`: purity and determinism; every shard non-empty at counts 1 to 54; the
  contiguity floor at 2, 3 and 4 with the hash comparison beside it; `count == 1` answers 0
  throughout.
- `StartingUniverseTests`: the shipped one-shard universe is **byte-identical** to what the tool
  writes today, which is the claim that this slice changed no shipped content.
- `UniverseStateTests`: the header's shard count round-trips; a file whose count disagrees is
  refused and changes nothing; the format-1 fixture loads through the new reader.
- The whole suite set green, both replay gates included; `CheckProjectFiles.py`, `CheckFormat.py`,
  clang-tidy over `GameLogic`.
- The decision record is written and indexed.

## 6. Assumptions the implementer may make

- **The shipped galaxy stays one shard.** `Server.cfg`'s count defaults to 1 and nothing in the
  shipped deployment changes; this slice is the ability to describe more, not a decision to run more.
- **A gate whose ends fall on two shards is still a gate.** It keeps working exactly as it does
  today within the one universe that holds both ends; the day the ends are in two universes is
  slice 2's, and until then no such universe is written.
- **`EntityId` needs nothing.** ADR 0047 already carries the shard, which §3 leans on and this slice
  does not touch.

## 7. What changed on contact, and what is deliberately not here

- **§5's "every shard non-empty at counts 1 to 54" was wrong, and the measurement is the reason it
  is not in the suite.** The lattice has `2 * ringCount + 1` columns — 11 for the shipped galaxy —
  so 54 was never reachable, and a column split cannot exceed the column count. Worse, occupancy is
  **not monotonic** against the drawn layout: 9 shards leaves 8 occupied, 10 leaves 10. There is no
  clean invariant to assert, so the slice ships two honest things instead of one false one:
  `MaxShardCount` documented as a ceiling, and `OccupiedShardCount` measured against the systems that
  were actually drawn. `OccupancyIsMeasuredAndNotAssumed` pins that, and `UniverseGen` refuses a
  count that would write an empty shard rather than writing it.
- **ADR 0058 is not amended.** §4 of this order told the implementer to add a line to that record's
  consequences. `Design/README.md` forbids it — a decision record is append-only and superseded by a
  later one naming it, never edited to agree with what came after. ADR 0063 names it instead. The
  order was wrong; this section is the correction, and the order itself stays as it was written.
- **Genesis grew a whole-galaxy entry point.** `BuildStartingGalaxy(layout, desc, span<Universe>)`
  builds every shard in one pass, because a gate leaving a shard has to name the shard it leads to
  and the only honest way to know that is to have drawn the destination first. Building shards one
  at a time would have meant predicting the far side. `BuildStartingUniverse` is untouched and is
  still what the one-shard path calls.
- **`SaveHeaderBytes` became a function of the format.** The header grew four bytes for the shard
  count, so `PeekSaveFormats` could no longer assume a fixed 23-byte header when deciding whether a
  format-1 file was well formed. This is the first header field ever added, and it is the reason
  ADR 0061's migrate-on-read applies to the *file* format and not only the state format.
- **No new fixture was cut for the file-format bump, and that is a stated bend of AGENTS.md §12.**
  The rule wants the previous format's file committed as `UniverseFormat<N>.sav`, run by the tool at
  the parent commit. `UniverseFormat7.sav` *is* a file-format-1 file, so the 1 → 2 gate is exercised
  by a real artifact; what is missing is a file at state 8 **and** file format 1, which would isolate
  the two gates from each other. That artifact can only honestly come from MSVC, which nothing in
  this container is, and a clang-built stand-in would pass the replay row while being the wrong bytes
  — exactly the failure the fixture rows exist to catch. The bend is recorded rather than papered
  over; the next bump that happens on a Windows machine closes it.
- **`Server.cfg` is not touched.** §2 scoped the shard count to `GalaxyDesc` and the save header; the
  deployment side of it belongs with the process that reads it, which is slice 5's.

## 8. What was verified, and how — and the honest gap

Measured locally against the same `GameLogic`, clang 18 on Linux behind the usual shims:

```
count  2 | block kept 61/68 (90%) | hash kept 25 (37%)
count  3 | block kept 55/68 (81%) | hash kept 26 (38%)
count  4 | block kept 48/68 (71%) | hash kept 11 (16%)

count 1 | ships 307 gates 136 stations 165 fleets 1 | leads out  0 | deterministic: yes
count 2 | ships 307 gates 136 stations 165 fleets 1 | leads out 14 (all resolve in their shard)
count 4 | ships 307 gates 136 stations 165 fleets 1 | leads out 40 (all resolve in their shard)

one-shard partitioned build equals the shipped build: yes (307 ships)
format-1 fixture loads through file 1->2 and state 7->8 and replays to byte equality: yes
```

The census is conserved across every count, which is the claim that a partition cuts the galaxy up
rather than changing it.

**The gap:** nothing here ran on MSVC, and nothing here ran a second process. The first is CI's to
answer and is the gate. The second is not a gap in this slice — no universe with a gate whose ends
are in two `Universe`s is written by anything yet, and the day one is, is slice 2's.
