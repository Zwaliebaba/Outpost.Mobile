# Universe slice 3 — genesis composes the galaxy

Work order for slice 3 of [`Universe.md`](Universe.md). Depends on
[slice 1](Universe-slice-1.md) for the layout and [slice 2](Universe-slice-2.md) for the gate table.

## 1. Scope

The composition root stops laying out one system and lays out a galaxy.

- **`OutpostApp` calls `LayOutGalaxy`** once at boot, with `GALAXY_SEED` and the one shipped pin,
  and takes the starting system *out of* the galaxy rather than laying it out beside it.
- **A Vanguard station at every planet of every system.** The government is everywhere, which is
  what makes a gate worth flying through.
- **`SpawnGates`**: a Structure at each end of every link, then the rows, in two passes — a gate
  names its far side by `EntityId` and the far side does not exist while the near one is spawned.
- **`GateSite`/`GateHeadingRad`** in `GameLogic`, because where a gate stands is layout content and
  the bound on it has to be provable in a suite (§7).
- **The boot log** says what genesis built.

## 2. Out of scope

- **Any client change beyond the marks.** No gate icon, no `JUMP` affordance, no camera that
  follows a crossing — slice 4.
- Only the **local system's** stations are marked on the minimap. Marking fifty-four systems would
  pin a ring of azure diamonds to the map's edge and say nothing.
- The save file (slice 5), the island-scoped replan (slice 6), NPC presence beyond the Vandal base.

## 3. What to build on

- `Game::LayOutGalaxy`, `LayOutGalaxySystem`, `GalaxyDesc`, `SystemPin` (slice 1).
- `Universe::MakeGate`, `GateDesc`, `GateCount` (slice 2).
- `OutpostApp::SpawnVanguardStations`, which already does this for one system.
- `ViewTuning.h`'s `UNIVERSE_LAYOUT_SEED` and `STARTING_SYSTEM` — the pin's contents.

## 4. How it must behave

1. **Home is unchanged.** The pin holds cell (0,0), zero jitter and the shipped seed, so
   `m_layout` is what it always was and the opening shot is pixel-identical.
2. **The starting fleet keeps its ids**, by being spawned before anything else — the rule the root
   already follows.
3. **Gates are spawned in two passes**, structures then rows, and every gate's far side resolves.
4. **`PathIslands` must not decline.** It declines *quietly*; the symptom is ships that stop
   routing, a long way from this file.

## 5. Acceptance

- A code read of the root (which has no suite — ADR 0014's standing assumption).
- `GalaxyLayoutTests::EverySystemFitsItsOwnPathIsland` extended to measure against the **gates**,
  which stand further out than any orbit and therefore decide a system's span.
- An out-of-tree genesis harness that runs the shipped constants through a real `Universe`: the
  census, `DeclinedPathIslandCount() == 0`, every gate's destination resolving, and a hundred ticks
  stepped.
- The whole `GameLogicTests` suite green.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy over GameLogic.

## 6. Assumptions

- Screenshots are owed by slice 4, not here: nothing the player can see changes at boot except two
  extra event-log lines.
- `m_localSystem` exists and never moves. It is here because the marks and the bodies both need to
  ask which system is being placed, and answering "home" twice would be two places to change.

---

## 7. What changed on contact

**The design's gate ring would have broken pathfinding, silently.** `Universe.md` §10 specified
`GATE_RING_METRES = 8 000`. A gate stands further from its star than any planet, so it — not the
outermost orbit — decides a system's static span: `2 × 8 000 + 2 × PATH_GRID_MARGIN_METRES` is
17 024 m, which is **532 cells against `PATH_GRID_MAX_CELLS_PER_AXIS` of 512**. `PathIslands`
declines to build past its ceiling and it does so quietly, so the symptom would have been ships that
stop routing in a system nobody could connect to this constant.

Two things followed. The ring moved to **7 000 m** — still outside the widest orbit (6 500), and
470 cells, which leaves real headroom. And it moved *into `GalaxyDesc`*, so the bound is a
`GameLogicTests` assertion instead of a hope: the executable layer has no suite, which is ADR 0037's
argument for putting the layout in `GameLogic` in the first place, applied to one more number.
`EverySystemFitsItsOwnPathIsland` now measures the widest of the gate ring and the orbit band, so
the next person to move either is caught by the same row.

## 8. What was verified, and how

`Debug|x64` is not buildable in this container, and the root is the one layer with no test suite.

- **The genesis logic was run**, out-of-tree, against the shipped constants through a real
  `Universe` — the same calls in the same order as `OutpostApp::Init`. It builds **54 systems, 164
  stations, 136 gates, 300 ships**; `PathIslandCount()` 299 with **`DeclinedPathIslandCount()` 0**;
  every one of the 136 gates resolves to a live gate on the far side; a hundred ticks step in 17 ms.
- **The whole `GameLogicTests` suite**: 286 methods, 580 697 assertions, green.
- `CheckProjectFiles.py` and `CheckFormat.py` pass; clang-tidy clean over GameLogic under LLVM 22.
- **`OutpostApp.cpp` and `.h` were read, not compiled.** No Windows, no D3D12, no SDK here. That is
  the one real gap in this slice and CI is where it closes.
