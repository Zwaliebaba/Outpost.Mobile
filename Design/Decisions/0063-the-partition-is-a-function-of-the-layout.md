# 0063 — The partition is a function of the layout, and contiguity is what decides it

Status: accepted
Date: 2026-09-02

## Context

`CrossShard.md` §2 asked for one thing before anything else in that design could be built: every
participant — each shard process, and every client — has to agree on which shard owns a system, and
has to agree *without being told*. The disagreement is not a warning; it is a ship that arrives
nowhere.

The galaxy is already shaped for this. ADR 0055 made it one seed and a pin table, so the layout is
reproducible from nothing but a `GalaxyDesc`, and every `SystemSite` carries world-fixed `cellQ` and
`cellR` on the hex lattice. Nothing about a site depends on who drew it or in what order.

What was missing was the partition itself, and the temptation is to make it a table: a file, or a
column in the save, that says system 41 lives on shard 2. That is the same failure ADR 0058 spent a
record refusing at the level of content — two things that can author the same fact is one more than
the number that can be right.

## Decision

**`ShardOfSystem(site, desc)` is a pure function of the site's lattice cell and the shard count, and
the partition is a contiguous block split on the lattice's q columns.**

- `GalaxyDesc` gains `shardCount`, defaulting to 1, which means "no partition at all": every system
  answers shard 0 and the shipped galaxy is untouched.
- The function counts cells, not systems. `CellsInColumn(q, R) = 2R + 1 - |q|` and
  `CellsLeftOfColumn` give an exact integer position in the lattice, and the shard is
  `(before * shardCount) / total`. No floats, no ordering, no knowledge of which cells drew a system.
- **Contiguity is the property being bought.** A gate between two systems in the same shard is a
  function call; a gate across shards is a wire crossing with an ack and a re-send behind it
  (`CrossShard.md` §4). The partition's job is to make the first case the common one.
- `MaxShardCount(desc)` is the column ceiling — `2 * ringCount + 1`, so 11 for the shipped galaxy —
  and it is **a ceiling, not a promise**. `OccupiedShardCount(systems, desc)` is measured against the
  layout that was actually drawn, and `UniverseGen` refuses a count that would leave a shard empty.

## Alternatives considered

- **A hash of the system id, or of the cell.** Cheap, perfectly balanced, one line. Rejected on the
  measurement, which is the whole point of the record. Against the shipped galaxy's 68 gates:

  | shard count | block: gates kept in-shard | hash: gates kept in-shard |
  |---|---|---|
  | 2 | 61 / 68 (90%) | 25 (37%) |
  | 3 | 55 / 68 (81%) | 26 (38%) |
  | 4 | 48 / 68 (71%) | 11 (16%) |

  At four shards a hash turns 84% of the galaxy's gates into wire crossings. The block split turns
  29%. Balance is not the cost being minimised here and never was.
- **A table in the save, or in `Server.cfg`.** Rejected for ADR 0058's reason: a client that has to
  be *told* the partition is a client that can be told a stale one, and the failure surfaces as a
  handoff to a shard that does not think it owns the destination.
- **A ring-based split** (inner ring on one shard, outer rings on others). Also contiguous, and it
  matches how the galaxy is drawn. Rejected because ring areas grow linearly with the ring index, so
  the shards are wildly unequal in system count and the split point is not a free parameter — you get
  as many shards as there are rings, at the sizes the rings happen to be. Columns give an arbitrary
  count at roughly even cell counts.
- **Balance the split by system count rather than cell count.** Tempting, and it would even out
  occupancy. Rejected because it makes the partition a function of the drawn layout rather than of
  the lattice, which means a client cannot compute it without first drawing the whole galaxy, and any
  change to the pin table silently moves systems between shards.
- **Promise `MaxShardCount` as a usable count.** Rejected because it is not true, and it was measured
  rather than assumed: at the shipped `ringCount`, 11 columns exist but occupancy is **not
  monotonic** — 9 shards leaves 8 occupied, 10 leaves 10. A ceiling that lies is worse than no
  ceiling, so `OccupiedShardCount` answers against the real layout and the tool refuses rather than
  writing an empty shard.

## Consequences

- The shipped single-shard galaxy is **byte-identical** to what it was: `shardCount = 1` short-cuts
  to shard 0, and `BuildStartingGalaxy` at count 1 produces the same 307 ships as
  `BuildStartingUniverse`. This is pinned by a test, not by argument.
- **Genesis builds every shard in one pass.** `BuildStartingGalaxy` takes a span of `Universe`s
  because a gate leaving a shard has to name the shard it leads to, and the only honest way to know
  that is to have drawn the destination. Predicting it was the alternative and it is the class of
  thing this tree does not do.
- `UniverseGen` grows a third argument and writes `Universe.0.sav`, `Universe.1.sav`, … The one-shard
  path still writes `Universe.sav` under its own name, so a first checkout's instructions do not
  change.
- **The save header carries the shard count** (`SAVE_FILE_FORMAT` 1 → 2, migrated on read per
  ADR 0061). A universe generated for four shards is not a universe four *other* shards can run, and
  the header is where that is caught.
- **ADR 0058's "the tool always writes shard 0" stops being true**, and this record is where that is
  said. ADR 0058 itself is not touched: a decision record is append-only and is superseded by a later
  one naming it, never edited to agree with what came after (`Design/README.md`, "Decision records").
  ADR 0058's decision — that the tool authors and the game does not — is unchanged and is the reason
  the partition could be added in the tool at all.
- The partition is now a thing that can be measured, and the measurements above are the shape of the
  argument for every later shard-count change: if a split's contiguity is not reported, it has not
  been justified.
