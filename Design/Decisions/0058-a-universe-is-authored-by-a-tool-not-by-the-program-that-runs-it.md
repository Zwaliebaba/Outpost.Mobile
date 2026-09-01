# 0058 — A universe is authored by a tool, not by the program that runs it

Status: accepted
Date: 2026-09-01

## Context

ADR 0057 gave the universe a file, and left genesis where it had always been: four private methods on
`OutpostApp`, run on a first boot when no file was found.

That is stable exactly while there is one program. The design's whole trajectory is a second: a
headless shard server that boots a universe and ticks it. That server has no business running
genesis — it would have to link the client's composition root to get at the starting content, which
is the seam ADR 0008 cut and ADR 0037 defended.

And two programs that both generate "the same" universe are two chances to disagree. The failure is
not loud: a client and a server whose starting content had drifted by one constant would each build a
plausible galaxy and disagree about where the stations are.

## Decision

**`Tools/UniverseGen` writes a universe; `Outpost` runs one and authors nothing.**

- `Game::BuildStartingUniverse` and the shipped content move to `GameLogic/StartingUniverse.h`, out
  of `Outpost/ViewTuning.h` and `OutpostApp`.
- The tool lays out the galaxy, builds the universe, and writes it with `WriteSaveFile` through
  `BinaryFile::WriteFileAtomic`.
- The game's boot has two failures and no fallback: **no file stops the boot naming the tool**, and
  an unreadable file stops it telling you not to overwrite it.
- **The tool reads `argv`** — a seed and an output path — under an exemption the owner granted and
  AGENTS.md §5 now records.

## Alternatives considered

- **Leave genesis in the game.** Free today. Rejected on the day there are two programs, which the
  design is explicitly building towards: the server would need the client's content, or its own copy
  of it, and the copy is the failure this record exists to prevent. Deferring it makes it more
  expensive, not less, because the second consumer arrives with its own deadline.
- **Put genesis in `GameLogic` and let both the tool and the game call it.** The game would keep its
  "no file means first boot" fallback and nothing would break. Rejected because it leaves two things
  that can author a universe, which is one more than the number that can be right: the fallback would
  be the path nobody runs and therefore the path that rots, and it would fire exactly when something
  had already gone wrong with the file.
- **Generate as a post-build step.** No manual step, no README instruction. Rejected outright: it
  would run on every build and overwrite the save the player had. A tool that destroys progress as a
  side effect of compiling is worse than a tool you have to remember.
- **A tool that reads a description file rather than `argv`.** More capable, and it is what a content
  pipeline eventually looks like. Rejected as premature: a seed and a path are what there is to vary
  today, and a file format invented before it has a second field is a file format invented twice.
- **Keep the game able to generate, behind a key.** Rejected for the same reason as the fallback, and
  because a debug key that writes the save file is a debug key that can destroy a universe.

## Consequences

- **A first checkout cannot run the game.** Build, run `UniverseGen` once, then run `Outpost`. The
  boot failure names the tool and `README.md` says it. This is the price of the decision and it is
  paid at the least dangerous moment.
- `GameLogic` holds content now — hull ids, factions, positions, seeds. It holds no presentation, and
  that is the line: the day `StartingUniverse.h` names a texture it has drifted back into the layer
  it came from. What a Bomber *looks* like is still the client's (ADR 0002, ADR 0037).
- **Genesis is testable, and that is half the value.** It was four methods in an executable with no
  suite, exercised by launching the game and looking at it. `StartingUniverseTests` now pins the
  census, the determinism, the gate graph's integrity and the round trip through the save file.
- It exposed a defect ADR 0057 shipped: a universe saved at tick zero did not reload faithfully,
  because route currency is stored as a relation to a path-island version that a never-ticked
  universe has not built. `Universe::SettleDerivedState` is the fix, and the tool is the first caller
  that ever saved at tick zero (`Universe-slice-5b.md` §7).
- `UNIVERSE_SAVE_FILE` moved to `UniverseSnapshot.h`. Two programs that disagree about a save file's
  name fail as completely as two that disagree about its format.
- The tree gains a console executable, which it has never had, and a tenth project for
  `CheckProjectFiles.py` to hold in step.
- The exemption is the tools only. `Outpost` still reads neither `argv` nor the environment, and a
  library still reads nothing at all.
