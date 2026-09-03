# Work order — Galaxy map slice 1: the map, drawn

Implements slice 1 of [`GalaxyMap.md`](GalaxyMap.md) §7 — the screen, the projection, the graph, the
fleets, and the rail button that opens it.

**Layer:** `Outpost`.
**Depends on:** nothing. Slices 2, 3 and 4 all depend on it.
**Blocks:** the rest of the galaxy map design.

---

## 1. Why this slice is small, and what it is not

Nearly everything it needs already landed for other reasons (§1):

- **`GalaxyLayout`** — 54 systems with real positions and 68 links, held by the client at boot from
  the seed in the save header, and therefore the same graph the server spawned gates against.
- **`FleetStatus`** — a position and a size for **all five fleets**, decoded out of the snapshot
  header rather than the interest set, so a fleet the player cannot see is still a dot. Four of five
  routinely are not visible, and the map's hardest data question was answered by a decision taken
  for the fleet bar.
- **`Game::SystemAt`** — a position to the system it is in, which turns each of those five positions
  into a dot without the client doing geometry of its own.
- **The rail's `Universe` button**, which lights on press and is read by nothing.

So this slice draws. It sends no order, changes no simulation state, and adds nothing to the wire.

## 2. Scope

1. **`Outpost/GalaxyScreen` — a modal screen in `AssemblyScreen`'s shape and for its reasons** (§2):
   it holds state the HUD has no business in, it consumes every pointer event rather than letting
   them fall through to the universe underneath, and `Hud.cpp` is already eight hundred lines.
   `Open`, `Close`, `IsOpen`, `Draw`, `HandlePointer` — the same five the assembly screen has, so a
   reader who knows one knows this.

2. **The projection** (§3), and it is the one piece with an argument in it:

   - Systems are drawn from **`starPos`, not `cellQ`/`cellR`**. The lattice is a clean hexagon and
     the cells would draw a prettier picture, but the jitter is what makes the map *true*: a player
     reading distances off a map that has quietly regularised them will misjudge which of two gates
     is the long one.
   - The galaxy's bounding box fits the screen with a margin, **isotropically** — one scale for both
     axes, because a galaxy stretched to fill a 16:9 screen makes the same misjudgement in the other
     direction.

3. **The graph, drawn**: every system a node at its projected position, every link an edge, the
   system the camera is in marked, and each held fleet a digit on the system `SystemAt` puts it in.
   The fleet digits are the bar's digits and the minimap's, so a player reads one number everywhere.

4. **`Esc` closes it**, taking its place in the existing chain — the assembly screen, then the fleet
   sheet, then the selection — and the rail button toggles it.

5. **Prose in the same commit**: `GalaxyMap.md` §7's slice 1 row records what landed, and §1's "read
   by nothing" sentence about the rail button stops being true and says what reads it.

## 3. Out of scope

- **Tapping.** Slice 2 flies the camera; slice 3 sends a fleet. This slice's pointer handling
  consumes events and does nothing with them, which is the modal contract and not a placeholder.
- **System names.** Slice 4. A node is drawn by its position and its index until then.
- **Anything on the wire or in `GameLogic`.** The graph search slice 3 needs is `GameLogic`'s and is
  slice 3's.
- **Zoom and pan.** The galaxy fits the screen; a map you must navigate to read is a second design.

## 4. How it must behave

1. The screen opens on the rail button, consumes every pointer event while open, and closes on `Esc`
   or the button, ahead of the fleet sheet in the chain.
2. The projection is isotropic and derived from the layout's own bounding box, so it is correct at
   any window size and at any galaxy the seed produces.
3. Every system and every link is drawn once. A fleet in no system — which `SystemAt` cannot return,
   but a defaulted `FleetStatus` can imply — draws no digit rather than a digit at the origin.
4. Nothing here writes to `UniverseView` except through its existing selection calls, and nothing
   reaches `Universe` at all (§6.6).
5. The screen costs nothing when closed: no work per frame, no allocation.

## 5. Acceptance

- **Screenshots at two window sizes**, which is what accepts a screen: the map open over the boot
  scene, with the camera's system marked and Fleet 1's digit on it.
- `NeuronClientTests` or a new `Outpost`-side test for the projection alone — it is pure arithmetic
  over a bounding box, and the isotropy claim is exactly the kind that is wrong silently: a fit at
  two aspect ratios keeps one scale for both axes and centres the margin.
- The whole suite set green; `CheckProjectFiles.py` with the new files in both the `.vcxproj` and
  the `.filters`, `CheckFormat.py`.
- No decision record: a screen that draws what the client already holds decides nothing. Slice 3's
  multi-hop route is where this design's record is due.

## 6. Assumptions the implementer may make

- **The client's layout is laid out at boot and never again**, which is right while the galaxy is
  static (§8). The day a *gate* can be built or destroyed the map needs the graph on the wire, and
  that day is named in the design rather than guarded here.
- **A fleet is a dot on a system, not a position between two.** A fleet in transit between gates is
  drawn on the system it is in, which is what `SystemAt` answers and what the status block carries.
- **The map is read at human cadence.** Drawing 54 nodes and 68 edges through the overlay pipeline
  every frame is not a number worth optimising, and this slice does not.

---

## 7. What changed on contact, and what is deliberately not here

- **The map sits behind the HUD in the pointer chain, not ahead of it, and §2.1 and §4.1 were in
  tension about that.** They said both "consumes every pointer event while open" and "the rail button
  toggles it", and those cannot both be true of a screen whose switch is a HUD button: a modal that
  swallows its own switch is a modal you can only leave with Escape. The chain is now assembly →
  sheet → HUD → map → tracker, with the HUD narrowed to its rail while the map is up
  (`Hud::HandlePointer`'s `_railOnly`). The minimap, the bottom bar and the fleet buttons are under
  the map's scrim and take nothing; the map consumes everything the rail did not, which is what the
  modal contract meant.
- **The rail button is the state, and the screen follows it.** `Hud::ActiveRail` was already a toggle
  that lit itself, so pushing the screen's open state into the HUD would have been a second truth
  that could drift from it. The composition root reads the rail each frame and opens or closes the
  map to match; Escape and a ledger reply both call `Hud::ClearActiveRail`, because closing the screen
  without unlighting the button would have the sync reopen it on the next frame.
- **The projection is `Neuron::FitBoxIsotropic` in `NeuronClient`, not a private function of the
  screen.** §5 asked for a test of the projection alone and there is no `Outpost`-side suite to put
  one in — `TickStats` from an earlier slice has the same gap. The fit is arithmetic over two
  rectangles with no game type and no graphics type in it, so it belongs in the presentation library
  beside `ViewCulling`, where `NeuronClientTests` can reach it. That is where the isotropy claim is
  actually proved rather than looked at.
- **A digit is drawn for every held slot, and `SystemAt` is why there is no "in no system" case.**
  §4.3 asked for a fleet in no system to draw nothing. `Game::SystemAt` is a *nearest* and always has
  an answer, so a fleet mid-crossing draws on the system it is closest to instead of vanishing — which
  is better, and is what the design's own §1 says the function is for. The only no-digit case is a
  slot the server does not hold, which `IsFleetHeld` answers.
- **No system is named and no tap does anything**, per §3. The pointer handler takes an unnamed
  parameter, which is the honest spelling of a screen that looks at nothing about the event yet.

## 8. What was verified, and how — and the honest gap

`Neuron::FitBoxIsotropic` was run against every case its suite asserts, and the numbers match the
test to the digit: one scale for both axes, the source inside the destination with no slack left in
both axes at once, the panel's offset carried, a flat source fitted by the axis that has extent and
centred in the one that does not, a single point at scale 1 in the middle, and an empty destination
at a non-positive scale that draws nothing rather than inside out.

The projection was then run over the **shipped galaxy** at four window sizes, through the screen's own
layout arithmetic:

```
systems 54 links 68 | bounds x 1300550 m across, z 1167623 m down
1280x720   dpi 1.0 | 100 km reads 43.16 px across and 43.16 px down | every node inside the plot
1920x1080  dpi 1.0 | 100 km reads 74.00 px across and 74.00 px down | every node inside the plot
2560x1440  dpi 1.5 | 100 km reads 95.58 px across and 95.58 px down | every node inside the plot
 800x1400  dpi 1.0 | 100 km reads 46.44 px across and 46.44 px down | every node inside the plot
```

Landscape fits to the height and portrait to the width, which is the fit choosing the tighter axis.
At 1920x1080 the closest two stars a galaxy of this description can hold — 56,930 m by
`MinimumStarSeparationMetres` — are 42 px apart, so the nodes are separable at every size a window
takes.

**The gap, and it is the acceptance criterion:** §5 asks for screenshots at two window sizes, and
nothing in this container can produce one — there is no MSVC, no D3D12 and no window. `GalaxyScreen.cpp`
was compiled clean at `-Wall -Wextra` against stubbed presentation types, which proves it is
well-formed and proves nothing about what it looks like. The owner waived Windows-only manual checks
on 2026-09-02 and made CI-green the gate; this is the slice where that waiver costs the most, because
a screen is the one thing a suite cannot accept.
