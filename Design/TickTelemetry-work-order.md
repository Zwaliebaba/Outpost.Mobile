# Work order — the tick is measured

Slice 4 of [`GameDesignPlan.md`](GameDesignPlan.md), phase 0, round three. One slice, `Outpost`
alone, and it shares no file with slice 3, so the two run together.

The finding it retires is the first half of C5 in [`GameDesignReview.md`](GameDesignReview.md)
§Combat 5, joined with the counters half of E7. Both panels asked for the same instrument and
neither owned it: a per-tick statistics block the composition root samples. Nothing in the review's
scale track has an input without it — not the governor, not the byte budget, not the inflation
report — and the two behaviours the review calls farms, the free repair and the bottomless garrison,
cannot be shown to be farms without counters either.

It adds no feature and changes no simulation. It measures what the tick already does.

## 1. Scope

1. **`Outpost/TickStats.h`, `.cpp` — the block, and where the clock is read.**

   A `TickStats` type in `Outpost` and **not** in `GameLogic`, which is the whole placement
   argument: it holds wall-clock durations, and a wall clock is the first thing `GameLogic.h`'s
   determinism list forbids. The composition root may read a clock; the simulation may not, and this
   slice must not be the reason that stops being true.

   Per tick it records two spans — the step (orders applied, `Universe::Step`) and the publish — and
   over a window it holds the count, the sum, and the worst of each, plus the last tick's pair. A
   window is `saveEveryTicks` long, so the block is sampled and reset exactly where the save is
   taken, which is between ticks and is the one place the universe is at rest.

   Also carried, because they cost a read and answer questions the review asks: the tick, the ship
   count, the subscriber count, and the reserved economy counters E7 fills — issued and sunk per
   category, all zero and all unread, so that the first faucet has somewhere to be counted rather
   than a reason to add a file.

2. **`UniverseSimulation::Step` times its two halves.** It already runs `ApplyOrders`, `Step` and
   `Publish` in that order and for that reason; this slice brackets the first two and the third with
   a steady clock and hands the pair to the block. Nothing else moves: the ordering argument in that
   function's comment is untouched and the timing must not become a reason to reorder it.

3. **The block is written beside the save.** `Universe.stats`, a text file of `key = value` lines in
   the shape `Server.cfg` already uses, rewritten whole on the save cadence through
   `WriteFileAtomic`. Text and not binary: it is read by a person and by whatever a later slice
   points at it, it is tiny, and a binary format would need a version byte and a reader and would
   then be a second save file.

4. **The F1 readout gains a line.** `SHARD | STEP n.nn MS | PUBLISH n.nn MS | WORST n.nn | SUBS n`,
   from the same block, so the number is on the screen where the frame time already is rather than
   only in a file. `Hud::Stats` gains the four fields and `DrawDebug` the line.

5. **`Server.cfg` gains `statsEveryTicks`**, defaulting to `saveEveryTicks`'s 1800 and refusing
   outside 0 to 216000, with 0 meaning the file is never written. It is a deployment property for
   ADR 0043's exact reason, and it is separate from the save's cadence because a shard being tuned
   wants the numbers far more often than it wants a save.

6. **Prose in the same commit**: `Design/Universe.md` §8 gains a sentence naming the sidecar beside
   the save, since that section owns what lands next to `Universe.sav`.

## 2. Out of scope

- **The governor.** C5's second slice, phase 2. This slice measures the tick; it never changes it,
  never stretches wall time and publishes no rate.
- **Any counter the simulation would have to keep.** Records sent, bytes sent and fire events sent
  are the publisher's to count and the publisher does not count them; adding counters inside
  `GameLogic` for a readout is how a determinism contract acquires a clock. When C7's byte budget
  needs them it adds them with the budget that reads them.
- **The economy counters' values.** The fields exist and are zero; the first faucet fills them.
- **Any change to `Universe.sav`, the format, or the tick.**

## 3. What to build on

- `UniverseSimulation::Step` (`Outpost/UniverseSimulation.h`), which is already the one place both
  halves of a tick happen in a known order.
- `OutpostApp::SaveUniverse` and the cadence beside it in `Run` (`OutpostApp.cpp:1072`), which is
  already "between ticks" and already writes atomically.
- `ParseServerConfig`'s per-key shape — a `seen` flag, a range, a message naming the line — which
  every key in that file follows.
- `Hud::Stats` and `Hud::DrawDebug`, which is where every other number that goes quietly wrong is
  already shown.

## 4. How it must behave

1. `GameLogic` is unchanged. No clock, no counter, no field.
2. The two spans are measured with a steady clock and never with a wall clock that can step
   backwards.
3. The block resets on write, so each file is one window and not a running total since boot.
4. A refused write logs once and the game carries on, exactly as a refused save does.
5. `statsEveryTicks = 0` writes no file and costs no timing.

## 5. Acceptance

- **A new `NeuronCoreTests` or `GameLogicTests` row is not due**, and that is the point: this slice
  adds nothing to a library either suite covers. What is due is that **every existing suite is
  green**, since the claim is that nothing simulated moved.
- `python Build/CheckProjectFiles.py` with the two new files in both the `.vcxproj` and the
  `.filters`, `python Build/CheckFormat.py`, clang-tidy over `GameLogic` unchanged.
- **A reviewer on Windows: press F1 and read the line; find `Universe.stats` beside the save after
  thirty seconds.** Compiled by CI and demonstrated by nobody, as slices 1, 2 and 5 were.
- No decision record: a readout in the composition root decides nothing. If it had needed one field
  inside `GameLogic` it would have needed a record, and that is the line this slice does not cross.

## 6. Assumptions the implementer may make

- **A steady clock in the composition root is allowed.** AGENTS.md forbids a clock in the
  libraries; `OutpostApp` already owns a `FrameClock` and reads real time every frame.
- **The window is whole ticks**, so a file written mid-window reports the ticks it has and not a
  partial average of a longer one.
- **`Universe.stats` is not read back by anything in this slice**, so its shape is free to change
  until something parses it.

---

## 7. What changed on contact, and what is deliberately not here

**The layer is `Outpost` alone, not `NeuronServer` + `Outpost` as the plan's table said.** The two
halves of a tick are run by `UniverseSimulation::Step`, which is in the executable; `ServerHost` in
`NeuronServer` drives *how many* ticks a frame owes and knows nothing about what one contains. So
the brackets go where the halves already are, and the engine library gains nothing — which is the
better answer, because a timing field on `ServerHost` would be an engine type carrying a number only
one game's adapter fills.

**No counter was added to the publisher, and the plan's sketch asked for two.** "Records and fire
events sent" would each be a counter inside `GameLogic`, incremented per publish. That is not a
clock, so it would not break determinism outright — but it is state the simulation keeps for a
readout, and the tree's rule is that what `Step` reads is in the replay contract and what it does
not read has no business on those objects. When item C7's byte budget lands it needs those numbers
to *decide* with rather than to display, and it can add them where it reads them. §2 says so.

**The window resets and the last tick does not.** `ResetWindow` clears the count, the sums and the
worst, and deliberately leaves `stepLastMs` alone: the F1 line shows what the tick in front of you
just cost, and blanking it every thirty seconds would make the readout flicker to zero for one tick
each window. Run and checked here.

**The debug rows had to be renumbered.** The router's line was drawn at `lineHeight * 3` and so is
the new one; the router moved to 4. Caught by reading the offsets rather than by the compiler, which
had nothing to say about two lines drawn on top of each other.

## 8. What was verified, and how — and the honest gap

**Compiled and run here:** `TickStats` on its own, under clang, with two recorded ticks — the mean,
the worst and the last all come back right, and the window resets to zero while the last tick
survives. `python Build/CheckFormat.py` and `python Build/CheckProjectFiles.py` pass, the latter
with the two new files in both the project and the filters.

**Compiled by CI and demonstrated by nobody here:** everything else. `Outpost` needs D3D12 and Win32
and this container has neither, so the config key, the cadence, the sidecar and the F1 line are
argued and not watched. CI-green is the gate the owner set on 2026-09-02.

**A reviewer on Windows, in a minute:** press F1 and read `STEP … PUBLISH … WORST … SUBS 1`; wait
thirty seconds and find `Universe.stats` beside `Universe.sav` with a window in it.
