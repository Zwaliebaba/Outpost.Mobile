# Work order — the save migrates forward

Slice 1 of [`GameDesignPlan.md`](GameDesignPlan.md), phase 0. One slice, `GameLogic` with a test
fixture and two lines in the composition root, and the head of the whole plan: every later slice
that adds a table to `Universe` bumps `UNIVERSE_STATE_FORMAT`, and today a bump deletes the live
universe at the next boot — [ADR 0057](Decisions/0057-the-save-is-a-versioned-file-and-a-refused-one-stops-the-boot.md)
refuses an unknown format and never falls to genesis, which is right, and `UniverseSnapshot.cpp:117`
says why nothing softens it: "there is nothing to migrate from yet". After this slice there is.

The finding it retires is the migration half of E14 in [`GameDesignReview.md`](GameDesignReview.md)
§Economy 14, pulled forward to phase 0 by the review's cross-cutting section: six format bumps
landed in three days, the plan schedules a dozen more, and each one is a wipe until a reader
carries the format before it.

It adds no feature. The game plays exactly as it does today; what changes is what a file written by
yesterday's build means to tomorrow's.

## 1. Scope

1. **`GameLogic/UniverseSnapshot.h`, `.cpp` — the reader accepts a window of formats, the writer
   writes one.**

   | Was | Is |
   |---|---|
   | `UNIVERSE_STATE_FORMAT` — the one byte both ends check | `UNIVERSE_STATE_FORMAT` — what `WriteUniverseState` writes, unchanged at 7 |
   | — | `UNIVERSE_STATE_FORMAT_OLDEST` — the oldest byte `ReadUniverseState` accepts, 7 |
   | `SAVE_FILE_FORMAT` — likewise for the file header | `SAVE_FILE_FORMAT` unchanged at 1, and `SAVE_FILE_FORMAT_OLDEST` at 1 |

   `ReadUniverseState` reads the format byte into a local and refuses only a byte outside
   `[OLDEST, CURRENT]`. **A field added by a later format is read behind a gate on that local** —
   `if (format >= 8) x = in.U32(); else x = <the default the field had before it existed>` — at the
   point in the stream where the field lives, and nowhere else. The gate is the whole migration:
   there is no separate reader per format, no rewrite step, and no table of transforms. A universe
   read from format N is a current universe the moment the read returns, and the next save writes it
   at `UNIVERSE_STATE_FORMAT`. `ReadSaveFile` applies the same window to the file header's byte.

   This slice adds **no gate**, because there is no older format to gate for. It adds the window,
   the locals, and the place a gate goes, so that slice 3 — the first bump after this lands — adds
   one line per new field and nothing else.

2. **`SaveHeader` reports what was read.** Two fields, `fileFormat` and `stateFormat`, filled by
   `ReadSaveFile` and ignored by `WriteSaveFile`, which always writes the current bytes. The
   composition root reads them for §1.4, and a test reads them to prove a fixture was taken at the
   format its name claims.

3. **A fixture per retired format, and the row that loads it.**
   `Tests/GameLogicTests/Assets/UniverseFormat7.sav`, copied to the output directory exactly as
   `NeuronClientTests` carries `Assets\NmoFixture.nmo`, and read the same way through
   `Neuron::BinaryFile::ReadFile`. **The fixture is `UniverseGen 0` run at this slice's parent
   commit**: the shipped galaxy at tick zero, byte-identical on any machine by ADR 0058, which is
   what makes it reproducible by a reviewer and what makes it honest — it is exactly the file a
   deployment has on disk. The census the tool prints for it (systems, gates, stations, ships,
   bytes) is recorded as constants in the test beside the file, so a reader that mis-parses an
   older layout into a plausible universe shows up as a wrong count rather than as nothing.

   The rule this slice establishes, and the rulebook's checklist gains (§1.5): **the commit that
   bumps `UNIVERSE_STATE_FORMAT` to N+1 commits `UniverseFormatN.sav`, written by the tool at its
   parent commit, and its row is green in the same commit.** No format is ever retired by that
   rule; `OLDEST` moves only by a decision record naming the fixtures it deletes.

4. **`Outpost/OutpostApp.cpp` — the boot says which format it read, and keeps the file it migrated
   from.** `RestoreUniverse` logs `SAVE | FORMAT 7` on every restore. When `stateFormat` or
   `fileFormat` is older than the current byte, it logs `SAVE | MIGRATED 7 -> 8` and, **before the
   first save of the run**, writes the bytes it read — unchanged, the buffer it already holds — to a
   sidecar named for the format, `Universe.sav.7`, through `WriteFileAtomic`, and only if no file of
   that name exists. The earliest copy is the valuable one, so a sidecar is never overwritten. The
   reason for the copy is the one bug this slice makes possible: a gate that reads an older field
   wrongly produces a universe that loads, saves at the new format thirty seconds later, and has by
   then destroyed the only file that could have shown what the field was. ADR 0057 rejected
   renaming a *refused* file aside because it decides for the player that the game is new; a copy
   of an *accepted* file decides nothing and costs one write.

5. **Prose that this change makes true or false, in the same commit** (AGENTS.md "What is actually
   here", rule of the section):
   - `UniverseSnapshot.cpp:117`'s "there is nothing to migrate from yet" becomes what the reader
     now does.
   - AGENTS.md §8's checklist gains, under the `WriteUniverseState` line: bumped
     `UNIVERSE_STATE_FORMAT` or `SAVE_FILE_FORMAT`? The new field is read behind a gate on the
     format the reader took, the previous format's fixture is committed and its row is green, and
     `OLDEST` did not move.
   - AGENTS.md's "and no save *file* — … no format versions it" sentence is already false since
     Universe slice 5 and is re-trued here, because this slice is about exactly that sentence's
     subject.
   - `Design/Universe.md` §8 gains a bullet, amended in place per ADR 0054: a reader per known older
     format, a fixture per retired one, the sidecar, and where `OLDEST` may move.
   - `GameDesignPlan.md`'s status block and its slice-1 row: landed, and the layer corrected (§6).

6. **A decision record**: the save is migrated on read, and a format is retired only by record.
   Alternatives it must weigh, each of which lost: a forward-rewrite step in `UniverseGen` (it needs
   the same old reader, and then keeps it in a tool nobody runs on a live shard); a reader per
   format as a separate function (duplicates the whole stream to change one field); an explicit
   transform table in the file (a format for the format); keeping the refusal as it is (a wipe per
   bump, which the plan cannot afford); and keeping every format forever (the gates accumulate in
   the one codec the tick trusts, so retirement has to be possible and has to be deliberate).

## 2. Out of scope

- **A format bump.** This slice writes format 7 and reads format 7. The first gate is slice 3's,
  and the first migration this mechanism performs is that one.
- **The other half of E14** — the input log and recovery from the last snapshot plus the log. That
  is phase 2, slice 19.
- **The wallet's home** — plan §4 decision 2. Nothing here keys on a shard.
- **`UniverseGen`.** It writes the current format and knows nothing of older ones; it is not given a
  `--migrate`. The game migrates, because the game is what has the file.
- **A writable data directory.** The sidecar lands beside `Universe.sav` under `Assets\`, wrong for
  a real install for the reason `UNIVERSE_SAVE_FILE`'s comment already gives, and moves with it.
- **Retiring a format, or a policy for when.** `OLDEST` is 7 and stays 7 until a record moves it.
- **Any change to what is saved.** No field, no table, no reordering. A byte-for-byte identical
  file is what proves the window was added without moving anything.

## 3. What to build on

- `ReadUniverseState`'s discipline — read into locals, check `Ok()` and every count against
  `Remaining()`, move into the caller's universe only once the whole buffer has been read — and its
  test, `AMalformedStateIsRefusedAndChangesNothing`, whose `wrongFormat` row this slice splits in
  two (§5).
- `ReadSaveFile`'s "changes neither out-parameter on refusal", which the two new header fields
  inherit.
- `Tests/NeuronClientTests/NmoReaderTests.cpp` `ReadFixture` and the `<None Include>` with
  `CopyToOutputDirectory` in that project's `.vcxproj` — the one place the tree already carries a
  binary fixture. `GameLogicTests` gets an `Assets\` directory the same way; `python
  Build/CheckProjectFiles.py` must agree with both the `.vcxproj` and the `.filters`.
- `StartingUniverseTests` — the census it already pins for the shipped galaxy is the census the
  fixture's row records.
- `OutpostApp::RestoreUniverse` (`OutpostApp.cpp:594-610`) holds the file's bytes and the header at
  the one moment both are known, which is where §1.4 goes; `SaveUniverse` (`:612-636`) is where
  "before the first save" is enforced, by a flag set in restore and cleared by the sidecar write.
- `ThrowBootFailure`'s sentence shape for the refusal, which does not change: a file older than
  `OLDEST` or newer than current still stops the boot, and the message now says which and names
  both bytes.

## 4. How it must behave

1. A state at any format in `[OLDEST, CURRENT]` loads; a byte below `OLDEST` or above `CURRENT` is
   refused and changes nothing. Both ends of the window are tested explicitly, not by `+1` alone.
2. The file written by a restored universe is at the current format, whatever format it was read
   from.
3. Reading a fixture and writing it back at the current format, then reading *that* and writing it
   again, gives identical bytes on the second pair. Migration is idempotent.
4. A fixture, loaded, replays: save it at the current format, reload into a second universe, step
   both 600 ticks, and the two states are byte-equal — the standing replay gate, applied to a
   migrated universe rather than a built one.
5. A fixture's census matches the constants recorded beside it.
6. `SaveHeader::stateFormat` and `fileFormat` after `ReadSaveFile` are the bytes the file carried.
7. The sidecar is written once, before the first save, only on a migrated boot, and never over an
   existing one.

## 5. Acceptance

- `UniverseStateTests`, new or rewritten rows:
  - **`AFormatOutsideTheWindowIsRefused`** — split from the `wrongFormat` case: `OLDEST - 1` and
    `CURRENT + 1` are each refused and change nothing, and a `static_assert` holds
    `OLDEST <= CURRENT`.
  - **`AnOlderFixtureLoadsAndReplays`** — `UniverseFormat7.sav` loads through `ReadSaveFile`, its
    header reports `stateFormat == 7` and `fileFormat == 1`, its census matches the recorded
    constants, and behaviours 3 and 4 hold. Written as a loop over the fixture list, which has one
    entry today, so slice 3 adds a file name and nothing else.
  - **`AWriteIsAtTheCurrentFormat`** — a universe loaded from the fixture writes byte 4 as
    `UNIVERSE_STATE_FORMAT`.
  - The existing thirteen rows unchanged and green; **`ASavedUniverseReplaysToTheSameRun` and
    `TheSameOrderProducesTheSameRun` in particular**, since nothing about the tick may move.
- **A fresh `UniverseGen 0` at the finished commit is byte-identical to the fixture** — the
  proof that the window was added without moving a byte of the format. Stated in the hand-back
  with the byte count.
- **The gate proven by mutation, then reverted**: locally bump `UNIVERSE_STATE_FORMAT` to 8, add
  one gated `U32` field with a default after the tick, and show the fixture row still green, the
  refusal row refusing 6 and 9, and the idempotence row green; then remove the field and the bump.
  Recorded in the hand-back as the six-mutation table in `Universe-slice-5.md` §8 is, because this
  is the only way this slice can demonstrate the mechanism it exists for before slice 3 uses it.
- The whole suite set green — `NeuronCoreTests`, `GameLogicTests`, `NeuronClientTests`,
  `NeuronServerTests` — and Debug|x64 building, with the configurations stated.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass; clang-tidy over the
  changed `GameLogic` sources.
- The decision record is in `Design/Decisions/`, indexed, and every sentence in §1.5 changed.
- **The disk half is `Outpost` and is compiled by CI and demonstrated by nobody**, as slice 5's
  was. A reviewer on Windows: boot on the shipped file, see `SAVE | FORMAT 7` in the log, and
  confirm no sidecar appeared. The migrated path cannot be exercised until slice 3 exists, and this
  order says so rather than inventing a bump to show it.

## 6. Assumptions the implementer may make

- **The fixture is a genesis file, not a mid-flight one.** Every table `Step` reads is present at
  tick zero, but several are empty — no routes in flight, no docking, no protector duty. That is
  the real shape of a deployed file before its first tick, and it is what the tool can reproduce
  byte-for-byte; a richer fixture would need a program that steps and saves, which the tool is not
  and the game cannot be made to do from a test. Behaviour 4 steps the loaded fixture, so the gated
  default of a mid-flight field is exercised by the replay rather than by the file.
- **The sidecar's name is `UNIVERSE_SAVE_FILE` plus `.` plus the state format in decimal**, one
  constant beside `UNIVERSE_SAVE_FILE`. A file format migration alone (rare; the header has bumped
  once) names the state format anyway, since one copy is enough.
- **Format 7 is the oldest there will ever be a fixture for.** Formats 2 to 6 have no deployment
  and no file, and `OLDEST` starts where a file first existed to keep.
- **The layer moved on contact with the plan's table**: `GameDesignPlan.md` §6 lists slice 1 as
  `GameLogic` + `Tools/UniverseGen`. The tool changes nothing; the root gains the log line and the
  sidecar. The row becomes `GameLogic` + `Outpost`, in this order's commit.
- `.gitattributes` has no rule for `.sav`; Git detects the file as binary by content. Adding
  `*.sav binary` beside the existing rules is allowed and not required.

---

## 7. What changed on contact, and what is deliberately not here

**The sidecar is written at restore, not before the first save.** §1.4 said "before the first save
of the run", which a flag on the app would have had to enforce. At restore the bytes are still in
hand and nothing has happened yet, so the copy is taken there; "before the first save" is then
true by construction rather than by a flag, which is the same shape slice 5 chose for the shutdown
save.

**A refused boot now names the bytes.** ADR 0057's reader changes nothing when it refuses, so the
sentence "not a universe this build can read" could not say which build. `Game::PeekSaveFormats`
reads the two format bytes behind both magics without parsing anything else, and the boot failure
says `file format 1 and state format 9; this build reads … 7 to 7`. Small, and the one line a
player with a file from a newer build actually needs.

**The identity row is the regeneration path.** `TheNewestFixtureIsTheToolsOutput` compares the
committed fixture with what `WriteSaveFile` produces for the shipped galaxy, and on a mismatch
writes the correct bytes into the test log as base64 behind a `FIXTURE-BASE64|` marker. That is
how the fixture in this slice was going to be obtained if the local build below had disagreed
with MSVC, and it is how anyone with nothing but CI regenerates one.

**The fixture was written on Linux, by clang, from `GameLogic` compiled behind a shim for the
NeuronCore umbrella** — the same `LayOutGalaxy`, `BuildStartingUniverse` and `WriteSaveFile` the
tool calls, with contraction off and SSE2 only, which is what `/fp:precise` on x64 amounts to. It
prints the census the tool would (54 systems, 136 gates, 165 stations, 307 ships, 1 fleet, 124,438
bytes). Whether it is byte-identical to what MSVC's Debug|x64 build writes is exactly what the
identity row decides on CI, and if it is not, the log carries the MSVC bytes and the fixture is
replaced in a commit that says so. The order's acceptance line "a fresh `UniverseGen 0` at the
finished commit is byte-identical to the fixture" is therefore checked by CI on every commit that
does not move the format, rather than once by hand.

**Not here, as ordered:** no bump, no gate, no field, no change to a written byte. The generator's
output before and after this slice's codec change hashes the same, which is the proof the window
was added without moving anything (§8).

## 8. What was verified, and how — and the honest gap

**Compiled and run here, on Linux, with clang 18 behind a shim for `NeuronCore.h`:**

- Every `GameLogic` source compiles, before and after the change.
- The generator built from the changed `GameLogic` writes 124,438 bytes whose SHA-256 equals the
  committed fixture's, which was written from the unchanged tree: the format did not move.
- `python Build/CheckFormat.py` and `python Build/CheckProjectFiles.py` pass.

**Run by CI (Debug|x64, run 231 on the pull request, 2026-09-02):** the build, clang-tidy over
`GameLogic` and `NeuronServer`, and 546 tests, of which 545 passed. The one that failed was
`TheNewestFixtureIsTheToolsOutput`, and it failed exactly as designed: MSVC's `WriteSaveFile` for
the shipped galaxy differs from the clang-built fixture in **110 of 124,438 bytes** — last-bit
floating-point drift in positions and headings, the two compilers' transcendentals disagreeing by
an ulp — and the log carried MSVC's bytes behind the `FIXTURE-BASE64|` marker. The fixture was
replaced from that dump in the commit after the slice's, which is the regeneration path §7
describes, used once before anybody needed it. The fixture in the tree is therefore what MSVC
writes, and the identity row is what says so on every later commit. The window-refusal rows and
the fixture's load, count, re-save and 600-tick replay all passed on the clang-written fixture,
and run again on the MSVC one in the same commit's CI.

**The mutation check in §5** (a local bump to 8 with one gated field) cannot be run here, since
nothing here runs the suite, and is owed by the first slice that actually bumps.

**The disk half** — the `SAVE | FORMAT` line, the sidecar, the refusal sentence — is `Outpost`,
compiled by CI and demonstrated by nobody, as slice 5's was. It cannot be exercised until slice 3
bumps the format.
