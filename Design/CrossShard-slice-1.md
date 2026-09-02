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
