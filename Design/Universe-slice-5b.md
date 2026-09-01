# Universe slice 5b — genesis moves into a tool

Work order for slice 5b of [`Universe.md`](Universe.md). Depends on slice 5: without a save-file
codec there is nothing for a tool to write.

Cut on contact rather than planned. It was asked for while slice 5 was being built, and slice 5's own
work made it cheap — extracting `MarkLocalStations` for the restore path left genesis touching
nothing but the universe and the layout.

## 1. Scope

A universe is authored by a tool, and the game runs what it finds.

- **`GameLogic/StartingUniverse.h`/`.cpp`** — the shipped universe's content and
  `BuildStartingUniverse`, moved out of `OutpostApp` and `ViewTuning.h`.
- **`Tools/UniverseGen`** — a console tool: lay out the galaxy, build the universe, write the file.
  It reads `argv`, which the game may not.
- **`Outpost` becomes load-only.** No file means there is nothing to run and the boot stops naming
  the tool. The four `Spawn*` methods are gone.
- **`GameLogicTests::StartingUniverseTests`** — genesis, provable for the first time.

## 2. Out of scope

- **Several universes, or a save-slot layer.** One file, one universe, still.
- **A tool that reads a description file.** The seed and the output path are arguments; everything
  else is a constant in `StartingUniverse.h`. A content pipeline is a different project.
- **Generating as a build step.** It would regenerate on every build and quietly destroy the save
  the player had. A tool you run on purpose is the point.
- **Moving `Universe.sav` out of `Assets\`.** Still wrong for a real install, still one constant.
- The island-scoped replan (slice 6).

## 3. What to build on

- `Tests/GameLogicTests/GameLogicTests.vcxproj` — the closest existing project shape: links
  `GameLogic` and `NeuronCore`, no graphics, no MSIX, and it already agrees with the eight others on
  every setting `CheckProjectFiles.py` compares.
- `Outpost/Main.cpp` — how a composition root in this tree sets its home directory before reading
  anything.
- `OutpostApp::RestoreUniverse` and `ThrowBootFailure` from slice 5 — the boot already stopped for
  one reason and now stops for two.

## 4. How it must behave

1. **The tool's output is a function of its seed**, on any machine and any build. Nothing in it
   reads a clock, the environment, or anything but its arguments.
2. **What the tool writes is what the game runs.** Generate, write, read back, and step both: the
   same bytes.
3. **The game authors nothing.** No file is a stop, not a first boot — otherwise there are two
   things that generate a universe and two chances to disagree, which is the whole reason for the
   slice.
4. **The two failures say different things.** Missing means run the tool; unreadable means do not,
   because generating over it destroys whatever it holds.
5. **The move changes nothing.** A universe built by the moved code runs identically to one built
   by the code it replaced.
6. **`GameLogic` gains content but no presentation.** Every value that moved is a hull id, a
   faction, a position or a seed. The day that header names a texture it has drifted.

## 5. Acceptance

- `StartingUniverseTests`: the census; a function of its galaxy, at rest and after 200 ticks; the
  shard reaches every identity; every gate leads to a live gate; no island declined; the fleet is in
  slot 1; a generated universe survives the save file and replays.
- The whole `GameLogicTests` suite green, both standing replay gates included.
- `CheckProjectFiles.py` — which now has a tenth project to check — `CheckFormat.py`, clang-tidy.
- **A reviewer on Windows: build, run `UniverseGen`, run the game, and find the universe it wrote.**
  Owed and not supplied — see §8.

## 6. Assumptions

- A console executable is a shape this tree has not had. Everything else is a static library, a test
  DLL or the packaged app. The project file is `GameLogicTests`' with four changes:
  `ConfigurationType`, the subsystem, the include directories and the source list.
- The tool links `NeuronCore` for `FileSys` alone. `NeuronCore.h` pulls in the QUIC headers, but a
  header is not a link dependency and nothing in the tool calls one.

---

## 7. What changed on contact, and what is deliberately not here

**A universe generated at tick zero did not survive its own save file, and that is a defect slice 5
shipped.** `AGeneratedUniverseSurvivesTheSaveFile` caught it on its first run: generate, write, read
back, step both, and they diverged on tick one.

The cause is worth stating exactly, because it is invisible in the bytes. `WriteUniverseState`
records a route's currency as a **relation** to the path-island version, not as the version itself —
correct, because the absolute number means nothing across processes. But a universe that has spawned
and never ticked has an *unbuilt* island set at version 0, and its routes are at version 0 too, so
every route writes as "current". Read back, `ReadUniverseState` rebuilds the islands first, so those
routes are current against islands that exist. The original then builds its islands on its next tick,
finds its routes stale, and re-plans — while the reloaded copy does not.

**Every previous caller ticked before saving, so nothing had ever hit it.** The tool is the first
thing in the tree that saves a universe at tick zero, which is its entire job.

Fixed by `Universe::SettleDerivedState()`, called at the end of `BuildStartingUniverse`: a universe
about to be written is brought to the state a loaded one is in. Design §8 already said the codec's
contract is "a universe at rest"; slice 5b is where "at rest" turned out to mean more than "between
ticks".

**The move is proved behaviour-preserving rather than argued.** The pre-move genesis was transcribed
out of `05e06a0` and run beside the moved one over the same galaxy: **byte-identical after 900
ticks**. The one difference is at tick zero, where the settled universe now writes different bytes
from the unsettled one — and that difference *is* the fix, because the unsettled record was the one
that would not reload faithfully.

**`UNIVERSE_SAVE_FILE` had to move too.** It was in `Outpost/ServerConfig.h`, which a tool cannot
reach. Two programs that disagree about a save file's *name* fail as completely as two that disagree
about its format, so it sits beside `SAVE_FILE_MAGIC` now. `GameLogic` still opens nothing; it only
says what the thing is called.

**A bare output path lands beside the TOOL, not beside the game.** The two are separate executables
in separate output directories, and `FileSys::ResolvePath` resolves against whichever one is asking.
So `UniverseGen` alone does not feed a build of the game; it needs the game's path as its second
argument. This is the one thing about the tool that will surprise people, and it is said three times
— in `--help`, in `README.md`, and at the line that sets the home directory — rather than left to be
discovered.

**Two mistakes of mine that the guards caught, and one they did not.** `CheckFormat`/`clang-tidy`
found a `constexpr` local still in camelCase, which CI treats as an error; `CheckProjectFiles.py`
found `far` again. The one nothing caught was mine to find by re-reading the diff: rewriting a
comment in `ViewTuning.h` I deleted `BODY_START_PLANET_DEPTH_METRES` along with it, which
`SpawnStartingBodies` still uses. No tool in this container compiles `Outpost`, so that would have
been a CI break.

**`CheckProjectFiles.py` caught `far` again**, in a new test — the `<windows.h>` macro, the same trap
slice 4 hit. Second time that guard has paid for itself in this pull request.

**`argv` needed the owner's sanction and got it, and AGENTS.md now records it.** The ban is on the
game; a command-line tool is its own composition root, and a generator that cannot be pointed at a
seed has to be rebuilt to be used. The exemption is written into §5 rather than left as a violation
nobody flagged.

**A first checkout now cannot run the game.** That is the real cost of the decision and it is not
hidden: build, run `UniverseGen` once, then run `Outpost`. The boot failure names the tool, and
`README.md` says it. A build step that generated automatically was rejected — it would regenerate on
every build and destroy the save the player had.

## 8. What was verified, and how — and the honest gap

**`GameLogic` half — compiled, run, and measured against itself.**

- The whole `GameLogicTests` suite: **302 methods, 586 735 assertions, green** (295 and 585 994
  before this slice), both standing replay gates included.
- **The move is byte-identical**: pre-move genesis and moved genesis, same galaxy, compared through
  `WriteUniverseState` at tick 0 and after 900 ticks.
- **Seven mutations, six red, one survivor:**

  | # | mutation | result |
  |---|---|---|
  | 1 | the settle removed (the defect above, re-introduced) | **red** — `AGeneratedUniverseSurvivesTheSaveFile` |
  | 2 | the shard argument ignored | **red** — `TheShardReachesEveryIdentity`, 308 assertions |
  | 3 | the fleet spawned last instead of first | **red** — `TheStartingFleetIsInSlotOne` |
  | 4 | only one gate made per link | **red** — 2 rows, 70 assertions |
  | 5 | gate ring pushed past the grid ceiling | **red** — but by slice 1's `EverySystemFitsItsOwnPathIsland`, *not* by this slice's island row |
  | 6 | the galaxy's density nudged 0.55 → 0.60 | **red** — `TheShippedUniverseIsItsCensus` |
  | 7 | the hostile base moved from 850 m to 9 000 m | **survives** |

  **Mutation 7 is a hole, and it is not one this slice opened.** Moving the hostile base is a content
  change that no row in the suite notices: the census is unchanged, the islands still build, and
  `PatrolTests` spells its own copy of those numbers rather than reading the constants — which
  `ViewTuning.h`'s old comment admitted ("PatrolTests spells these same five numbers, and the two
  must agree") without doing anything about it. The fix is a row pinning the base against the
  constraints the design states for it — inside the interest radius, inside the minimap's half-range,
  the ring clearing the station's skin. It is named here rather than built, because it belongs to
  `Hostiles.md` and not to this slice.

  **Mutation 5 is honest about a row that did not fire.** `TheShippedUniverseDeclinesNoIsland` is
  belt-and-braces over `EverySystemFitsItsOwnPathIsland`: the layout row proves a bound on the
  layout, this one proves the universe actually built out of it declined nothing. They would diverge
  only for content the layout does not model — and mutation 7 shows the suite does not currently
  reach that case either.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy clean over `StartingUniverse.cpp` under
  LLVM 22, with the invocation proved against a planted violation first.

**The tool has never been built, and neither has the game's new boot.** `Tools/UniverseGen` is a
console executable with a hand-written `.vcxproj` in a tree that has never had one; `Outpost`'s
load-only boot is a composition root. Neither is compilable here, and **the project file is the
single most likely thing in this pull request to break CI.** It is `GameLogicTests.vcxproj` with four
deliberate changes, and `CheckProjectFiles.py` — which compares every shared setting across all ten
projects — passes on it, which is the strongest thing that can be said without a compiler.

**A reviewer on Windows should: build; run `UniverseGen` and read its census back; run `Outpost` and
find that universe; delete `Universe.sav` and confirm the game stops and names the tool; then
`UniverseGen 0xC0FFEE` and confirm a different galaxy.**
