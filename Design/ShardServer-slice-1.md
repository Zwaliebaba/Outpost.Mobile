# Work order — Shard server slice 1: the executable

Implements slice 1 of [`ShardServer.md`](ShardServer.md) §7 — a process that loads a universe, ticks
it, saves it, and stops cleanly. **One shard, no clients, no links.**

**Layer:** `Server` (new), reaching `NeuronCore`, `NeuronServer` and `GameLogic`.
**Depends on:** nothing. §5's three decisions were taken on 2026-09-02.
**Blocks:** every other slice of this design, and phase 2 of [`GameDesignPlan.md`](GameDesignPlan.md).

---

## 1. Why a slice this useless is the right first one

**Nobody can watch it and it plays no game.** That is deliberate. It is the smallest thing that
proves the claim `Simulation.h` and `ServerHost` have made since they were written — that the engine's
run loop does not want a window — and if that claim is false, this is where it fails, before a
session layer, a link, or a client reconnect has been built on top of it.

Everything after it is an addition rather than a rewrite: slice 2 adds a listener, slice 3 adds
links, and neither changes the ordering this slice establishes.

## 2. Scope

1. **An eleventh project, `Server/`**, a console executable. `Main.cpp`, `ShardApp.h/.cpp`, `pch.h`,
   `pch.cpp` — `Outpost`'s own shape, because a reader who knows one root should recognise the other.
   It lists `NeuronCore`, `NeuronServer` and `GameLogic` as include paths and nothing else; it is the
   second place in the tree entitled to see more than one layer (AGENTS.md §3).

2. **`ShardApp`, a composition root and nothing else**, in the order `ShardServer.md` §3 states:
   read the config, read the save, run, save on the cadence, save once on the way out. No genesis and
   no fallback — ADR 0058 gave authoring to a tool, and a *server* that could invent a universe is
   exactly the second author that record exists to prevent.

3. **`argv` for the shard number and nothing else.** `Server 0` runs shard 0, reading
   `Universe.0.sav`; no argument means shard 0 and `Universe.sav`, which is what `UniverseGen` writes
   for a one-shard galaxy. That is the same `ShardPath` rule slice 1 of `CrossShard` put in the tool,
   and it must be spelled the same way — a server and a tool that disagree about a filename fail as
   completely as two that disagree about a format.

   AGENTS.md §5 exempts a `Tools/` command line, not this one, so **the exemption has to be widened
   or refused**, and §3 below says which.

4. **A clean stop.** `Ctrl+C` sets a flag, the loop finishes its tick, one save is written, the
   process exits 0. A server killed mid-tick must lose at most the ticks since its last save, which
   is what the save already guarantees; what this adds is that a *deliberate* stop loses nothing.

5. **It says what it is doing on stdout**, because nothing else can: the shard it loaded, the format
   it read, the tick it is on at each save, and the reason it refused to start. `Outpost` has an
   `EventLog` on screen for this; a console has a console.

6. **Prose in the same commit**: `README.md` gains how to run it, `AGENTS.md` counts eleven projects
   rather than ten and describes `Server/` in its layer table, and `ShardServer.md` §7's slice 1 row
   records what landed.

7. **A decision record**: the tree has a second composition root, and why not one root with an
   optional window.

## 3. The one rule this slice has to bend, and how

**AGENTS.md §5's `argv` exemption names `Tools/` and this is not `Tools/`.** The rule's own stated
reason is that a library must not reach around its caller for configuration and that *a command-line
tool **is** its caller*. A server is its caller in exactly the same sense, and the alternative — the
shard number in `Server.cfg` — is worse in a specific way: one config file per shard means four
near-identical files that differ in one integer, and the day one is edited and another is not, two
shards believe they are the same shard and both write `Universe.2.sav`.

**Widen the exemption to name the executables that are their own caller, and keep the ban where it
matters**: `Outpost` still reads neither `argv` nor the environment, and a library still reads
nothing at all. The decision record carries this, because a rule bent without a record is a rule
gone.

## 4. Out of scope

- **Any client, listener or session.** Slice 2. This process opens no socket.
- **Any `ShardLink`.** Slice 3. It has no neighbours and pumps nothing.
- **Threading.** Slice 15 of the plan, and it wants this to exist first.
- **Deployment**: no service wrapper, no container, no daemonising. It runs in a terminal.
- **Sharing a composition root with `Outpost`.** §5.1 chose two, and this slice must not quietly
  start factoring one out.

## 5. How it must behave

1. `Server` with no `Universe.sav` beside it **refuses to start**, names the tool, and exits
   non-zero — `Outpost`'s existing failure, in a console.
2. A universe loaded, ticked N times and saved is byte-identical to the same universe ticked N times
   in `Outpost` and saved. **The run loop changes no outcome**, which is the claim the engine has
   been making and this slice is the first to test.
3. A save happens every `saveEveryTicks` and once at shutdown, and the tick it was taken at is
   printed.
4. It allocates nothing per tick after boot, which is the standing rule and is a code read.
5. Nothing in `Server/` names a graphics type, a window, or `NeuronClient`. `CheckProjectFiles.py`
   already holds the layer rule and will hold this one.

## 6. Acceptance

- **Run it.** A terminal, a universe, some ticks, a save, `Ctrl+C`, a clean exit — and the save it
  wrote loads in `Outpost`. This is the acceptance a screenshot would be for a screen, and unlike a
  screenshot it can be automated later.
- `GameLogicTests` unchanged and green: this slice adds no simulation behaviour and must prove it did
  not.
- `CheckProjectFiles.py` with the eleventh project registered and its settings agreeing with the
  other ten, `CheckFormat.py`, `CheckViewAccess.py`, clang-tidy over the new sources.
- The decision record written and indexed.

## 7. Assumptions the implementer may make

- **`ServerHost` needs no change.** It is a fixed-rate accumulator over a `Simulation`; nothing in it
  refers to a frame, a window or a swapchain. If that turns out to be false, it is this slice's
  finding and §6 of the design says so.
- **`UniverseSimulation` can be used as it stands**, or lifted — it lives in `Outpost/` today and
  names no graphics type. Moving it is allowed and is a judgement call for the implementer; leaving a
  copy is not.
- **One shard has no neighbours**, so the partition is not consulted at all this slice. Deriving them
  is slice 3's.

## 8. What changed on contact

- **`ServerHost` needed no change at all**, which §7 assumed and §6 of the design said to report
  either way. It is a fixed-rate accumulator over a `Simulation` and nothing in it referred to a
  frame, a window or a swap chain. The claim it had been making since it was written turns out to
  have been true; it had simply never been tested, because the only thing that had ever driven it was
  a program that also owned a window.
- **`Advance` runs the ticks; it does not ask permission to.** Written the first time as
  `for (int i = 0; i < m_host.Advance(dt); ++i) m_host.Step();`, which double-steps. The return is a
  count of what it already ran, and the game's loop reads it to pump the network once per tick. A
  shard has nothing to do per tick this slice, so the count only decides whether to yield.
- **`ServerConfig` moved from `Outpost/` to `GameLogic/`**, namespace `Outpost` → `Game`. Not
  anticipated by this order and the one genuinely structural change in it. There are two composition
  roots now and one deployment's `Server.cfg` is read by both; two parsers for one file is two
  programs that disagree about what their own configuration said. It sits in `GameLogic` rather than
  an engine project because its defaults are taken from `InterestSet`, `Publisher` and `SimTuning`,
  which no engine project may name. Reading the file is still the root's and only the root's — the
  moved code parses a `string_view` and opens nothing, so [ADR 0043](Decisions/0043-a-server-is-told-what-to-be-by-a-file.md)
  is unchanged.
- **AGENTS.md §5's `argv` exemption widened**, from "a command-line tool under `Tools/`" to "a program
  that is its own caller". The shard number has to come from somewhere, and the alternative — putting
  it in `Server.cfg` — means one near-identical file per shard differing in one integer, and the first
  time one is edited and another is not, two processes believe they are the same shard and both write
  `Universe.2.sav`. The reasoning is [ADR 0058](Decisions/0058-a-universe-is-authored-by-a-tool-not-by-the-program-that-runs-it.md)'s,
  transferred: a library must not reach around its caller for configuration, and a console program is
  its caller. The game still reads neither argv nor the environment.
- **`Build/CheckProjectFiles.py` grew a real check rather than a claimed one.** §5.5 says
  "`CheckProjectFiles.py` already holds the layer rule and will hold this one" — it did not. The
  layer rule there only stopped an *engine* project naming `GameLogic`; nothing stopped a root
  including `<d3d12.h>`. `Server` is now in `HEADLESS_PROJECTS` alongside the two engine libraries,
  and the check also refuses any `#include` of a header `NeuronClient` publishes, read off disk
  rather than listed. All three failure shapes were planted and confirmed to be caught.
- **The torn-file refusal contradicted itself.** A file whose header reads fine and whose body is
  short fails `ReadSaveFile`, and the diagnosis peeked the formats and printed them — so a truncated
  save was reported as "file format 2 and state format 8 | this build reads file 1 to 2 and state 7
  to 9", which names formats it then says are readable. The peek is now a diagnosis only when a
  format is genuinely out of range; otherwise it says the file is torn and how many bytes it is.
- **`Server/Main.cpp` had to become `ShardMain.cpp`.** `CheckProjectFiles.py`'s repo-wide unique-name
  rule caught the collision with `Outpost/Main.cpp` — two files of one name resolve to whichever
  project root comes first on an include path, and the projects share one.

## 9. What was verified, and how

Everything below was **run**, on Linux, against the real `GameLogic` and the real `ShardApp.cpp`
compiled behind a `NeuronCore` shim — the file the server ships, not a copy of it.

**The run loop changes no outcome (§5.2), which is the load-bearing claim.** A universe was booted by
`ShardApp`, run to a stop, and saved; the same universe was loaded from a pristine copy and ticked the
same number of times through `Universe::Step` with no server anywhere:

```
the server's save loads | tick 8276 | file 2 state 9 | 307 ships 136 gates 165 stations
ticked 8276 directly: 126967 bytes vs the server's 126967 -- BYTE-IDENTICAL
```

**It allocates nothing per tick after boot (§5.4).** §5 asked for a code read; a global `operator new`
counter is cheaper and settles it. After 600 warm-up ticks:

```
  100 ticks:      0 allocations (0.000 per tick)
 1000 ticks:      0 allocations (0.000 per tick)
 5000 ticks:      0 allocations (0.000 per tick)
```

**It refuses to start, and says something actionable, in every way a boot can fail (§5.1).** Eight
cases, each exiting non-zero:

```
no Universe.sav        NO UNIVERSE | Universe.sav is not there | run Tools/UniverseGen to write one
wrong shard            UNIVERSE REFUSED | Universe.3.sav holds shard 0 and this process was asked to be shard 3
torn, header readable  UNIVERSE REFUSED | ... both of which this build reads | the file is torn: 400 bytes is not a whole universe
present and empty      UNIVERSE REFUSED | Universe.2.sav is present and could not be read, or is empty
not a universe         UNIVERSE REFUSED | ... the magic is wrong or the file is too short to hold a header
a future file format   UNIVERSE REFUSED | ... is file format 3 and state format 8 | this build reads file 1 to 2 and state 7 to 9
a bad config value     CONFIG REFUSED | Server.cfg line 1: port must be 0 to 65535, found 'not-a-number'
an unknown config key  CONFIG REFUSED | Server.cfg line 1: unknown key 'wibble'
```

**A save happens on the cadence and once at shutdown (§5.3)**, and the whole acceptance §6 asks for
was run end to end — a universe, some ticks, a stop, a save that loads:

```
SHARD 0 | Universe.sav | file format 2 state format 8 | tick 0 | 307 ships 136 gates 165 stations
MIGRATED | state format 8 to 9 | the next save writes the newer one
RUNNING | 60 Hz | saving every 1800 ticks | stop with Ctrl+C
SAVE | tick 1800 | 126994 bytes
...
STOPPING | tick 8276
SAVE | tick 8276 | 126994 bytes
```

**`GameLogicTests` is unchanged and green (§6).** `git status` shows nothing under `Tests/`, which is
the first half; the second is that the suite still passes. All 29 translation units were compiled and
**339 rows run** against the asserting `CppUnitTest.h` stand-in:

```
GameLogicTests: 339 rows run, 1 failed
```

The one row is not this slice's and is not a regression. `MatchupTests::TheGroupRowsAreWhatTheyWere`
pins a 5,500-tick fleet fight, and under clang it ends at tick 5510 with five survivors where the
matrix, pinned on MSVC, says 5613 with four. The check that settles it: the same row was run in a
worktree of `cab41d3` — the commit that pinned the matrix and was green in CI — and produced
**character-for-character the same line**, MOVED and all. So the divergence is between the two
compilers' floating point over a long fight, not between this tree and that one. The other four rows
of the matrix, and the duel matrix beside them, agree exactly. MSVC is the compiler the matrix is
pinned against and CI is where that is decided.

**The gap, stated.** `ShardMain.cpp` is the one file here that was not run: `wmain`,
`SetConsoleCtrlHandler` and `wcstoul` are Windows, and Ctrl+C was stood in for by a thread setting
the same flag the handler sets. What that leaves untested is the handler's registration and its
return value, and nothing else — `RequestStop` and the loop's response to it are exercised. This is
the Windows-only manual check the owner waived on 2026-09-02; it is named rather than waved past.

The second gap is the one above: the shim is clang and the tree is MSVC, so a float-sensitive claim
verified here is verified against the wrong compiler. Everything this slice claims is either exact
(byte equality, allocation counts, refusal sentences) or structural, and none of it is float-sensitive
— which is why the matrix row is the only thing that moved.
