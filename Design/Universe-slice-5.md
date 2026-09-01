# Universe slice 5 — the save file

Work order for slice 5 of [`Universe.md`](Universe.md). Depends on slice 2 (the state codec carries
gates), and on 3 in practice (there is a galaxy to save).

## 1. Scope

The universe stops dying with the process.

- **`SaveHeader` and the file codec** in `GameLogic`, beside `WriteUniverseState`: magic, a format
  byte, the galaxy seed, the shard, and the state's length — then the state bytes as they are.
- **`BinaryFile::WriteFileAtomic`** in `NeuronCore`: a sibling temporary, flushed, renamed over the
  target.
- **`saveEveryTicks`** in `ServerConfig` and `Server.cfg`, beside the port.
- **A boot that restores or stops.** Absent means first boot and genesis runs. Present and readable
  means the universe comes out of the file and *nothing spawns*. Present and refused **stops the
  boot naming what refused** — it never falls through to genesis.

## 2. Out of scope

- **The camera.** Not saved, so a reload puts the view back at home even if the fleet is elsewhere.
  It is presentation, like the shot log the design already excludes, and slice 4b's rule makes this
  correct rather than broken: the scenery follows the camera, so the client shows home's worlds
  because home is where the camera is. Fixing it means saving a *view*, which is a different file.
- **A save on demand, a save slot, a menu.** One file, one universe.
- **Migration between format bytes.** A file this build does not know is refused, not upgraded.
  Upgrading is worth building the day a shipped build exists to upgrade *from*.
- **The shot log**, as the design already says: a resumed universe with no tracers pending is the
  correct picture of one that has just resumed.
- The island-scoped replan (slice 6).

## 3. What to build on

- `WriteUniverseState`/`ReadUniverseState` and their refusal discipline — read into locals, move out
  only once the whole buffer checks. The file codec is a header in front of that, and inherits it.
- `ParseServerConfig`'s per-key shape: a `seen` flag, a range, a message naming the line.
- `ThrowLinkFailure` — how this program already refuses to boot, and the sentence shape to match.
- `OutpostApp::RebuildLocalSystemScenery` (slice 4b) — already re-marks without spawning, which is
  exactly what a restored boot needs.

## 4. How it must behave

1. **The header carries the galaxy seed**, and a restored boot lays the galaxy out from *it* rather
   than from the compiled `GALAXY_SEED`. A binary whose seed has moved on still boots the universe
   the file holds, and a shard and its clients never disagree about where anything is.
2. **The header's shard must agree with the body's**, or the file is refused. The shard is in the
   file twice — see §7 — and two sources of truth that can disagree are worth a check, not a shrug.
3. **The length must be exact.** `SAVE_HEADER_BYTES + stateBytes == file size`, or refused. That is
   what catches a torn file *and* a file with junk appended, which `ReadUniverseState` alone does
   not: it stops at the end of the state and never looks past it.
4. **A refusal changes nothing** — neither the header out-parameter nor the universe.
5. **The write is atomic.** A crash mid-write leaves the previous good universe, never half of a new
   one. Temporary first, contents flushed, then the rename.
6. **The save happens between ticks, never inside one.** The codec's contract is a universe at rest.
7. **A restored universe replays.** Save, reload, step both — the same bytes, tick for tick.

## 5. Acceptance

- `UniverseStateTests`: the header round-trips; a restored universe replays to byte equality; a
  wrong magic, an unknown format, a header/body shard disagreement, a wrong length, a truncated
  file and an appended one are each refused and change nothing.
- The whole `GameLogicTests` suite green, both standing replay gates included.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy over the changed `GameLogic` sources.
- **A reviewer on Windows: play, quit, relaunch, and find the universe where it was left.**
  **Waived — see below.**

> **Waived by the owner on 2026-09-01.** This check was never run: CI-green was accepted in its
> place. Recorded rather than deleted, because a check that quietly stops existing is
> indistinguishable from one that passed — and the gate-ring bug (`Universe.md` §10) is exactly what
> that looks like when it goes wrong.


## 6. Assumptions

- **The file lands under `Assets\`**, because that is where `FileSys::ResolvePath` resolves a
  relative name and there is no writable-data directory in this tree. That is wrong for a real
  install — a read-only program directory breaks it — and it is one constant to move on the day
  there is somewhere better. Named here rather than discovered later.
- `saveEveryTicks = 0` disables the *periodic* save; the shutdown save still happens. A period of
  zero has no other sensible reading.
- The state codec's format byte and the file's are independent. The file's says how to find the
  state; the state's says how to read it. Bumping one need not bump the other.

---

## 7. What changed on contact, and what is deliberately not here

**`ReadFile` cannot tell an absent file from an unreadable one**, and that is the bug that would have
caused exactly the catastrophe §8 of the design forbids. Both answer with an empty buffer — the right
answer for content, where a missing texture and a locked one are equally missing, and the wrong one
here. A save file present but unopenable would have read as *absent*, genesis would have run, and the
next periodic save would have landed on top of it. The player's universe, deleted by a bug in reading
it. `Neuron::FileSys::Exists` is new for this one distinction, and `RestoreUniverse` asks it first.

**`Shutdown()` runs on the exception path too.** `wWinMain` calls `app.Shutdown()` from both catch
blocks, so a save placed there would fire after a *failed* boot — writing a half-built universe over
a good file, or, worse, over the very file that had just been refused. The save at clean shutdown is
therefore at the end of `Run()`, where "the loop returned rather than threw" is exactly what clean
means, and the throwing paths skip it by construction rather than by a flag.

**`m_simulation.Connect` ran before the universe existed.** The subscriber opens its despawn cursor
at `DespawnHead()` so a ship that died during boot is not replayed as news (ADR 0027) — and
`UniverseSimulation::Connect`'s own comment claims "the fleet is spawned before the link is opened",
which was *already false*: `Connect` sat some forty lines above `SpawnStartingFleet`. It cost nothing
while every boot began at head zero. On a restored boot the head is the saved one, thousands of
deaths in, and the publisher would have walked the entire despawn log on the first tick. `Connect`
now sits behind the universe, and the comment is true for the first time.

**The cadence is a distance, not a modulo.** `tick % saveEveryTicks == 0` skips whenever a frame
advances more than one tick — rarely, on a slow frame, and therefore in exactly the run that most
wanted the save. It is `tick - lastSave >= period`.

**The header gained a length the design did not ask for.** Design §8 says version, shard, seed.
`ReadUniverseState` stops at the end of the state and never looks past it, so a file with rubbish
appended would load and be believed; and a torn one is only caught deep inside the body parser,
after it has allocated. Eight bytes at the front make "torn" checkable in both directions before
anything is read. §8 of the design now says so.

**The shard is in the file twice, and the reader cross-checks it.** The design puts it in the header;
the state codec already had it. Dropping either would have been reasonable — the header's, and a
launcher cannot ask which shard a file is without decoding it; the body's, and the state codec stops
being self-sufficient. Keeping both and refusing a file where they disagree is the only option that
does not create a second answer, and it is a row in the suite.

**`ThrowLinkFailure` became `ThrowBootFailure`.** The wire used to be the only thing that could
refuse a boot; the saved universe is the second, and it refuses for the same reason — there is no
acceptable other thing to run instead. One helper, one sentence, each stage naming itself.

**The genesis tool, asked for mid-slice and deferred to 5b.** With `MarkLocalStations` extracted, the
four genesis functions touch `m_universe`, `m_galaxy`, `m_layout` and `m_localSystem` and nothing
else — no view, no GPU, no window. So a tool that generates a universe and writes it is now a small
change, and it is the right shape: a headless shard server has no business running genesis either. It
is **slice 5b** rather than part of this one because it needs this slice's codec to have something to
write, because it changes what design §8 says an absent file means, and because its real work is
moving the starting-universe content out of `Outpost/ViewTuning.h`, which is a client header a tool
cannot reach.

## 8. What was verified, and how — and the honest gap

**The codec half — compiled, run, and measured against itself.**

- The whole `GameLogicTests` suite: **295 methods, 585 994 assertions, green** (291 and 581 061
  before this slice), both standing replay gates included.
- **Six mutations of the shipped code, six red:**

  | # | mutation | result |
  |---|---|---|
  | 1 | the length check removed | **red** — `AMalformedSaveFileIsRefusedAndChangesNothing`, 10 assertions |
  | 2 | the header/body shard cross-check removed | **red** — `ASaveFileThatDisagreesWithItselfIsRefused` |
  | 3 | read straight into the caller's universe (the "changes nothing" guarantee dropped) | **red** — same row |
  | 4 | the header's seed and shard written in the other order | **red** — 3 rows, 8 assertions |
  | 5 | `SAVE_HEADER_BYTES` off by one | **red at compile time** — the `static_assert` |
  | 6 | the format byte not checked on read | **red** — 10 assertions |

- `ARestoredSaveFileReplaysToTheSameRun` is the row that matters: save, reload, step sixty ticks on
  both, compare bytes. It is also what proves the `std::move` out of `ReadSaveFile` is sound — a
  derived structure left dangling by the move would diverge within a tick.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy clean over `UniverseSnapshot.cpp` under
  LLVM 22, with the invocation proved against a planted violation first.

**The disk half is not verified by anything here, and that is the whole of this slice's gap.**
`WriteFileAtomic`, `Exists`, the cadence, the restore and the boot that stops are `NeuronCore` and
`Outpost` — Win32 and a composition root, neither of which this container can compile or run. **The
headline claim of slice 5 — that the universe survives the process — is argued, not demonstrated.**
CI will compile it; nobody has watched it work. What *is* proven is that the bytes a file would hold
are correct, and that a damaged one is refused, which is the half where a mistake is permanent.

**A reviewer on Windows should: play for a minute, quit, relaunch, and find the universe where it was
left; then corrupt a byte of `Universe.sav` and confirm the program says so and stops rather than
starting a new galaxy over the top of it.**
