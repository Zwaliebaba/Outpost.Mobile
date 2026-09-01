# Universe slice 6 — the replan scoped to its island

Work order for slice 6 of [`Universe.md`](Universe.md), the last one. Depends on slice 2.

## 1. Scope

Building in one system stops re-planning routes in every other.

- **`PathIslands::PlanStamp`** — the key of the island that planned a route, that island's version,
  and whether a single island owns the plan at all.
- **A version per island**, carried across a rebuild by the grid it belongs to.
- **`IsStampCurrent`** — whether a plan still stands, answered per island.
- **`Route::stamp`** replaces `Route::gridVersion`; `AdvanceRoute` asks the new question.
- The state codec carries the key, which is world-fixed and means the same thing in every process.
- [**ADR 0059**](Decisions/0059-a-route-is-scoped-to-the-island-that-planned-it.md), superseding
  [0034](Decisions/0034-a-routes-version-is-the-whole-worlds.md).

## 2. Out of scope

- **Scoping a multi-island plan.** A run that met several islands is aimed at the open water between
  two of them, so it depends on both. It stays checked against the whole partition — and see §7:
  scoping it is not merely optimistic, it is wrong, and the suite says so.
- **Scoping an open-water plan.** A run that met no island depends on the *absence* of architecture
  along it, so any new island can invalidate it. Making that local means knowing which islands a
  corridor passes near, which is more machinery than the case is worth.
- **A portal graph**, still. Optimality *between* islands is the trade `PathIslands` already made.
- Anything outside `GameLogic`.

## 3. What to build on

- `PathIslands::Partition` already computes each island's lowest occupied path cell, to order the
  islands. ADR 0034 named that exact value as the key that would eventually win. It is now the name
  as well as the order.
- Slice 4 of `RegionalPathfinding.md` already matches islands **by content, not by slot**, so a
  rebuild knows which islands did not change. A version per island is that knowledge, written down.
- The state codec's existing trick for `gridVersion`: store the *relation* ("was it current?") and
  resolve it on the way in, because a version is an epoch counter with no meaning across runs.

## 4. How it must behave

1. **An island that did not change keeps its version**, so routes planned against it stand.
2. **An island that vanished or merged upward loses its key**, so routes naming it re-plan.
3. **An island that grew keeps its key and must fail anyway** — on its version, because growing
   means it was rebuilt.
4. **A repartition does not invalidate anything by itself.** Building something with a lower cell
   renumbers every island; an index-keyed stamp would silently come to mean a different island, and
   a cell-keyed one does not notice.
5. **Only a run that met exactly one island is scoped to it.**
6. **Fail closed.** An unknown key re-plans: that costs a route, and the other direction flies a ship
   into a station.
7. **A rebuild on unchanged architecture leaves every stamp alone**, scoped or not — the gate that
   predates this slice, still holding.

## 5. Acceptance

- `PathfindingTests::ArchitectureInOneSystemReplansNoRouteInAnother` — the design's own acceptance
  sentence, measured through `RoutePlanCount` rather than asserted in a comment.
- `ArchitectureInItsOwnSystemDoesReplan`, `AStampSurvivesARepartition`, `AGrownIslandReplansItsOwnRoutes`,
  `AVanishedIslandReplansItsRoutes`, `AnUnscopedPlanReplansOnAnyChange`, `AnUnchangedRebuildKeepsEveryStamp`.
- The whole `GameLogicTests` suite green, both standing replay gates included.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy.

## 6. Assumptions

- Two islands cannot share a lowest cell. `Partition` already argues this: two obstacles in one cell
  are at most 45 m apart, the smallest architecture in the hull table is over 100 m across, so their
  surfaces overlap and the gap rule has already made them one island.
- The state format may move. It does: 6 → 7.

---

## 7. What changed on contact, and what is deliberately not here

**Scoping a multi-island plan to its first island is wrong, not just optimistic.** It was tempting —
those routes re-plan on arrival anyway, so the staleness window is short — and the mutation that does
it breaks `ARouteAcrossTwoIslandsIsStitched` and `TheFirstLegOfACrossingReachesPastTheFirstIsland`,
**two rows that predate this slice**. The reason is in `FindPath`'s own comment: the first leg is
aimed at the midpoint of the open water between the first island and the *next*, so it depends on
where both sit. The conservative choice turned out to be the correct one, and the suite said so
rather than the design.

**`IsStampCurrent` and the codec's restore wanted the same lookup**, and two binary searches ordered
differently is a defect that reports itself as "everything re-plans" and never as an error — it would
have looked like the slice simply not working. Factored to one `FindIsland`.

**The key is worth as much on disk as in memory.** A cell index is a universe coordinate, so it means
the same thing in a process that never saw the one that wrote it. The codec stores it verbatim and
only the *version* goes through the existing relation trick. That fell out of the design rather than
being aimed at, and it is why the save format's change is small.

## 8. What was verified, and how

**This slice is entirely `GameLogic`, so for once there is no gap.** Everything it touches is
compiled, run and mutated here, and CI's only new information is that MSVC agrees.

- The whole `GameLogicTests` suite: **309 methods, 587 165 assertions, green** (302 and 586 735
  before this slice), including both standing replay gates and the save-file round trip.
- **Six mutations, six red:**

  | # | mutation | result |
  |---|---|---|
  | 1 | scoping removed — `IsStampCurrent` back to the whole-universe version (ADR 0034's behaviour) | **red** — `ArchitectureInOneSystemReplansNoRouteInAnother`, `AStampSurvivesARepartition` |
  | 2 | a grown island keeps its version (the key half without the version half) | **red** — `ArchitectureInItsOwnSystemDoesReplan`, `AGrownIslandReplansItsOwnRoutes` |
  | 3 | a missing key passes instead of failing closed | **red** — including `DespawningAStructureStillReplans`, which predates this slice |
  | 4 | a run meeting several islands scoped to the first | **red** — 3 rows, two of which predate this slice |
  | 5 | a kept island takes the new version instead of keeping its own | **red** — the two scoping rows |
  | 6 | the codec drops the island key from the file | **red** — both replay gates, 602 assertions |

  Mutation 1 is the one that matters: it is exactly the behaviour ADR 0034 decided, and the row that
  catches it is the design's own acceptance sentence.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy clean over `PathIslands.cpp`, `Universe.cpp`
  and `UniverseSnapshot.cpp` under LLVM 22, with the invocation proved against a planted violation
  first.

**What a reviewer still cannot see here:** nothing specific to this slice. The universe design's
outstanding debts — the screenshots slices 4 and 4b owe, and the play-quit-relaunch pass slice 5
owes — are unchanged by it.
