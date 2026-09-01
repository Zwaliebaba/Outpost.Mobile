# Universe slice 4b — the scenery follows the camera

Work order for slice 4b of [`Universe.md`](Universe.md). Depends on slices 1–4, and exists because
slice 4 could not carry it: see [`Universe-slice-4.md`](Universe-slice-4.md) §7.

## 1. Scope

After a crossing, the *records* were already right — ships, stations and gates all arrive over the
wire correctly. What was a system behind was everything static: the worlds on screen, the asteroids
between them, and the minimap's station marks. This slice closes that.

- **`Game::SystemAt`** in `GameLogic`: which system a point is in, as the index of the nearest star.
  The one piece of real logic in this slice, put where a suite can reach it.
- **`OutpostApp::SystemAtCamera`** — two lines: where the camera is looking (only the view can say)
  and what is there (`Game::SystemAt`).
- **`OutpostApp::RebuildLocalSystemScenery`** — re-lay `m_layout`, replace the station marks,
  release the old bodies and build the new system's, through F5's own upload bracket.
- **`UniverseView::ClearStationMarks`** — the marks stop being add-only.
- **An anchor in `SpawnStartingBodies`**, so a scene is placed around *its* star.

## 2. Out of scope

- **The galaxy map screen**, still. Slice 4's reason has not changed.
- **A transition effect.** The swap is a hard cut. It is a look rather than a mechanism.
- **A different sky per system.** Deliberately not done, and argued at the code: a crossing moves
  the camera from one system's gate ring to the other's — 43 km at the guaranteed minimum, 117 km on
  the shipped lattice pitch — and a background that visibly turned over at that range would be
  claiming the galaxy is a few hundred kilometres across.
- **Unloading the far system's ships.** That is interest-set work and it already works.
- The save file (5) and the island-scoped replan (6).

## 3. What to build on

- `OutpostApp::ReseedBodies` — F5. It already does the hard half of this: release a whole scene
  mid-session and build another, in one bracket, with the copy queue ordered ahead of the graphics
  queue (ADR 0044). The rebuild is that function with a different seed and a layout behind it.
- `LayOutGalaxySystem` — already called per-system by `SpawnVanguardStations`, and already right.
- `UniverseView::UniversePosAt` — the render-space-to-universe conversion, already const.
- `StarDistanceSquared` in `GalaxyLayout.cpp` — the double-accumulation argument, already written
  down once, and `PointDistanceSquared` is its second caller.

## 4. How it must behave

1. **The trigger is where the camera IS, not that a jump happened.** A jump is one way to arrive in
   another system; the day there is a second — a galaxy map that flies you somewhere, a spectator
   following a fleet, a save reloaded elsewhere — this already covers it. It is also the only
   formulation that cannot get out of step, because the question it asks is a fact about the camera
   rather than a memory of an event.
2. **Nearest star, not a radius.** A radius leaves a band between systems where nothing is in
   anything. Nearest always has an answer.
3. **A tie keeps the lower index**, so a camera parked exactly on the midline between two stars
   picks one and stays there instead of rebuilding on whichever way the last rounding fell.
4. **Every place inside a system resolves to that system** — its star, every planet, and every gate
   on its ring. A gate is the furthest from its star that anything authored gets, so this is the
   property that bounds `gateRingMetres` against the lattice pitch.
5. **The marks are replaced, not added to.** The minimap's half-range is 4 km against a guaranteed
   57 km between stars, so a mark left behind for the system the camera came from draws pinned to
   the edge forever, which is a lie about where the government is.
6. **The old scene is released before the new one is built**, or every crossing leaks the system it
   left (ADR 0044).
7. **A system looks the same every time it is entered**: the seed is the system's own, out of the
   galaxy layout, and nothing offsets it by a count of visits.
8. **The boot scene does not move.** Home's star is the universe origin, so the anchor is a no-op
   there — see §7.

## 5. Acceptance

- `GalaxyLayoutTests::EverySystemOwnsItsOwnStar`, `EveryPlaceInASystemResolvesToIt`,
  `ATieKeepsTheLowerSystem`, `AnEmptyGalaxyAnswersZero`.
- The whole `GameLogicTests` suite green.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy over the changed `GameLogic` sources.
- **A screenshot on each side of a crossing, showing different worlds. Waived — see below.**

> **Waived by the owner on 2026-09-01.** This check was never run: CI-green was accepted in its
> place. Recorded rather than deleted, because a check that quietly stops existing is
> indistinguishable from one that passed — and the gate-ring bug (`Universe.md` §10) is exactly what
> that looks like when it goes wrong.


## 6. Assumptions

- Rebuilding mid-session is safe outside a frame. `BeginUploads` drains the GPU before it resets,
  which is documented on it and is the reason F5 is allowed to do this at all.
- One rebuild per crossing is affordable. It is F5's cost, which is already paid on a keypress, and
  a crossing is rarer than a keypress.

---

## 7. What changed on contact, and what is deliberately not here

**The bodies were positioned from the universe origin, not from their star.** This is the find that
justified the slice existing. `SpawnStartingBodies` placed every world and rock with
`Game::LocalPos(sin(bearing) * distance, cos(bearing) * distance)` — absolute from the origin — and
it has always been correct, because home's star *is* the origin: `HOME_PIN` sits at lattice cell
(0, 0), a pinned system takes no jitter, and `LayOutGalaxy` is called with `UniversePos{}`. So the
bug was invisible and would have stayed invisible until the first rebuild in another system, at
which point the new system's worlds would have been drawn tens of kilometres from the system they
belong to.

Fixed by anchoring the scene at `m_layout.starPos` and going through `Translate`. **The boot scene
is provably unchanged**, not merely probably: `Translate` on a default `UniversePos` computes
`SectorCarry` on exactly the same two floats `LocalPos` does and stores the same two remainders, so
the result is bit-identical while the anchor is the origin.

**`SystemAt` moved out of the composition root and into `GameLogic`.** It was written first as a
private member of `OutpostApp`, which is where the caller is — and that is the wrong layer twice
over. It is a question about the galaxy, not about a view, so a server deciding which system a
position belongs to would have needed its own copy, which is the second opinion ADR 0037 exists to
prevent. And it is the only part of this slice a suite can reach at all: left in `Outpost` it would
have shipped with zero tests, and moved it carries four. The client keeps the two lines that are
genuinely the view's.

**The check was in the wrong place in the frame, by one frame.** It went in at the end of
`OutpostApp::Update`, which is where app logic lives and which is wrong here: `UniverseView::UpdateFocus`
runs *after* `Update` returns — `Update`'s own pan-detection comment already says so —
and `UpdateFocus` is what SNAPS the camera across a crossing. So on the arrival frame the check
would read the pre-snap target, decline to rebuild, and `Render` would draw the new system's ships
among worlds now tens of kilometres behind the camera and outside the frustum: one frame of empty
sky. Moved to the frame loop, after `UpdateFocus` and before `Render`, where nothing can move the
camera between the question and the answer.

**The new functions landed underneath F5's comment block.** `SystemAtCamera` and
`RebuildLocalSystemScenery` were inserted directly above `ReseedBodies`, between that function and
the paragraph explaining it, so the F5 argument came to sit over a function it says nothing about.
Caught by re-reading the diff rather than by any tool. Moved above the block.

**The stale forward reference in `SpawnVanguardStations` is now true.** It said the re-marking day
"is slice 4's". It was not, and the comment now names the function that does it.

**A hazard named rather than engineered around.** The camera can be panned across the void without
a jump, and it will swap the scenery when it crosses the midline. That is the intended reading of
"the scenery follows the camera" and not a bug. The bounded cost is a player dragging slowly *along*
a midline, who could cross it repeatedly and pay a GPU flush each time; the midline is 65 km from
either star, in empty space, so this is named here rather than given a hysteresis band and a tuning
constant. A parked camera cannot oscillate: the answer is a pure function of a static input.

## 8. What was verified, and how — and the honest gap

**`GameLogic` half — compiled, run, and measured against itself.**

- The whole `GameLogicTests` suite: **291 methods, 581 061 assertions, green** (287 and 580 701
  before this slice).
- **Six mutations of the shipped code, five red:**

  | # | mutation | result |
  |---|---|---|
  | 1 | tie takes the higher index (`<` → `<=`) | **red** — `ATieKeepsTheLowerSystem` |
  | 2 | nearest → farthest (`<` → `>`) | **red** — 3 rows, 356 assertions |
  | 3 | always answers system 0 | **red** — 3 rows, 348 assertions |
  | 4 | `double` accumulation → `Game::DistanceSquared` (float) | **survives** |
  | 5 | `gateRingMetres` 7 000 → 70 000, past half the lattice pitch | **red** — `EveryPlaceInASystemResolvesToIt`, and `EverySystemFitsItsOwnPathIsland` with it |
  | 6 | the empty-galaxy guard removed | **red** — SEGV under ASan/UBSan, on a null `span` deref |

  **Mutation 4 surviving is reported rather than buried**, and it is the same result slice 1 got for
  `StarDistanceSquared` — which is why that function's comment already says the double is insurance
  against a map that is slightly wrong, not a fix for one that is broken. `PointDistanceSquared` is
  in the same position and its comment says so. A test that could kill it would have to place two
  stars within centimetres of equidistant at galactic range, which is a test about float, not about
  the galaxy.
- The suite also runs clean under **ASan and UBSan** — which is how mutation 6 was measured, since
  "does the test go red" is not a well-posed question about undefined behaviour.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy clean over `GalaxyLayout.cpp` under LLVM 22.

**Client half — read, not compiled, and not seen.** `OutpostApp.cpp` and `UniverseView.h` are
`Outpost`, which needs MSVC and D3D12; there is neither in this container. CI is first contact for
all of it, and **the screenshot this slice owes does not exist for the same reason slice 4's did
not.** The GPU bracket is F5's, line for line, which is the strongest thing that can be said for it
without a machine that can run it.

**A reviewer on Windows should build `Debug|x64`, cross a gate, and confirm the worlds and rocks on
the far side are different ones, the minimap's azure diamonds are the new system's, and pressing F5
there rerolls that system rather than home's — and take the screenshot this slice owes.**
