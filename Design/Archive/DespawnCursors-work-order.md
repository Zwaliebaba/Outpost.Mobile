# Work order — despawn delivery is cursored

Slice 2 of [`MmoScalabilityPlan.md`](../MmoScalabilityPlan.md). One slice, `GameLogic`, and the head of
the simulation track: slice 3's publisher cannot hold N subscribers until the despawn log stops
being drain-once.

The finding it retires is E2 in [`MmoScalabilityReview.md`](../MmoScalabilityReview.md): with two
subscribers, whichever one publishes first drains the log and the second never hears the death.
`World.h` predicted this in as many words -- "there is one publisher today; the day there are
several it becomes per-subscriber" -- so the sentence changes in this commit and the day is here.

## Scope

1. `GameLogic/World.h`, `World.cpp` -- the log gains a monotonic sequence and three functions
   replace the two it has:

   | Was | Is |
   |---|---|
   | `DespawnLog()` -> the whole vector | `DespawnsSince(cursor)` -> the handles at or after `cursor` |
   | `ClearDespawnLog()` | `TrimDespawnsBefore(cursor)` |
   | -- | `DespawnHead()` -> the sequence one past the last death |

   The sequence counts deaths for the life of the `World` and is never reset, so a cursor is
   comparable across trims. Trimming is the caller's call because only the caller knows the minimum
   cursor across its subscribers; the log itself must never drop what somebody has still to hear.
   A cursor older than what the log still holds returns everything held rather than failing -- the
   over-report direction, because the publisher intersects deaths with its own interest set anyway.

2. `Outpost/WorldSimulation.h` -- one `m_despawnCursor`, advanced to `DespawnHead()` on each due
   update and passed straight back as the trim. The behavior is identical to the drain it replaces
   while there is one subscriber, which is the point: this slice changes the contract, not the game.

3. `Tests/GameLogicTests/WorldTests.cpp` -- `TheDespawnLogHoldsUntilDrained` becomes
   `TheDespawnLogHoldsUntilTrimmed` against the new API, plus the two properties the old shape could
   not express: two readers each see every death exactly once, and a reader that starts at
   `DespawnHead()` sees only deaths after it.

4. `Tests/GameLogicTests/SnapshotTests.cpp` -- the one `DespawnLog()` call site follows the rename.

5. A decision record: the despawn contract moves from drain-once to cursors.

## Out of scope

- **The publisher itself.** The subscriber table, phases and order budgets are slice 3. This slice
  makes room for it and leaves `WorldSimulation` holding exactly one cursor.
- **Anything on the wire.** `WriteInterest`'s destroyed list is unchanged, `KIND_*` is unchanged,
  and no test in `SnapshotTests` changes meaning.
- **Trimming policy.** No automatic trim, no cap on the log's length, no eviction. The publisher
  trims every update, so the log's length is bounded by the deaths in one update interval; a
  publisher that stopped trimming would grow it, and that is slice 3's problem to have.
- **`World::Step`.** It never touched the log and still does not.

## What to build on

`m_despawnLog` and the two accessors on `World.h`, `DespawnShip`'s existing push (the log entry is
already written before the slot is retired, which is what makes a death distinguishable from a
departure), and `WorldSimulation::SplitTheLost`, whose binary search into `Left()` is unchanged --
only where it gets its handles from moves.

`ShipHandle` is unchanged. This slice adds no file, so no `.vcxproj` or `.filters` edit is due.

## Acceptance

- `GameLogicTests` green, including three new or rewritten rows:
  - **`TheDespawnLogHoldsUntilTrimmed`** -- deaths across several ticks all survive to be read, in
    despawn order; a failed despawn logs nothing; a `Step` is not a trim; a trim to the head empties
    it.
  - **`TwoReadersEachSeeEveryDeath`** -- two cursors advanced independently each see all three
    deaths exactly once, and the log holds until the *minimum* cursor has passed them.
  - **`AReaderStartingAtTheHeadSeesOnlyWhatFollows`** -- a cursor taken from `DespawnHead()` after
    two deaths sees neither, and sees the third.
- `TheSameOrderProducesTheSameRun` and the permutation test unchanged and green: the log is
  publish-side and no pass of `Step` reads it.
- The whole suite set green -- `NeuronCoreTests`, `GameLogicTests`, `NeuronClientTests`,
  `NeuronServerTests` -- and Debug|x64 building, with the configurations stated in the hand-back.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- `World.h`'s "the day there are several it becomes per-subscriber" sentence is gone in the same
  commit, replaced by what the code now does.
