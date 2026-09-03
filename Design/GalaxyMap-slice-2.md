# Work order — Galaxy map slice 2: tap to look

Implements slice 2 of [`GalaxyMap.md`](GalaxyMap.md) §7 — tapping a system flies the camera there and
closes the map.

**Layer:** `Outpost`.
**Depends on:** slice 1, which landed 2026-09-02.
**Blocks:** nothing. Slice 3 reuses the hit test and slice 4 the node it names.

---

## 1. Why this is small, and why it is worth landing alone

It is the claim `Universe-slice-4b.md` made and could not test.

That slice put the scenery on **where the camera is** rather than on a jump event, and its own work
order named "a galaxy map that flies you somewhere" as one of the things the choice covered in
advance. Nothing has ever exercised it: the camera has only ever moved by pan, by a fleet focus, or
by a fleet crossing a gate — never by being put somewhere it has no ship near. So this slice is
worth landing on its own precisely *because* it is the first caller of a decision taken a design ago.

If the worlds, the rocks and the station marks do not follow, slice 4b was wrong and this slice is
where that surfaces.

## 2. Scope

1. **A hit test on the map's nodes** (`GalaxyScreen`). The projection is already a `Neuron::BoxFit`
   and already a member function; the hit test is its inverse over a radius, in the same layout the
   draw used, so a node cannot be drawn in one place and tapped in another. Nearest node within
   `HUD_MAP_TAP_RADIUS_PX`, or none.

2. **`HandlePointer` stops being a stub** and reports the tapped system upward, the way
   `Hud::HandlePointer` reports a sheet to open: this class knows where a contact landed, and what a
   *system* means is the composition root's. It still consumes every event, which is unchanged — what
   changes is that one of them now says something.

3. **The root flies the camera and closes the map.** `Camera::SnapGoal` at the system's `starPos`
   through `UniverseView::ViewX/ViewZ`, the map closed, the rail unlit — the same two lines Escape
   already runs, so there is one way to close this screen and not two.

4. **Prose in the same commit**: `GalaxyMap.md` §4.2 becomes what landed, and §7's slice 2 row
   records it.

## 3. Out of scope

- **Ordering a fleet.** Slice 3, and it is the one with a decision record in it.
- **System names.** Slice 4. A tap names an index until then.
- **An animated flight.** `SnapGoal` and not `SetGoal`: the map is a jump in attention, not a pan.
  `UniverseView::FollowFocusedFleet` already makes that distinction for a distance "beyond any
  worth watching", and a galaxy is further than that by three orders of magnitude.
- **Anything on the wire or in `GameLogic`.** The camera is not simulation state and this sends
  nothing.

## 4. How it must behave

1. A tap on a node flies the camera to that system and closes the map; a tap on empty space closes
   nothing and does nothing, which is what keeps the map readable under a stray finger.
2. The hit test uses the layout the draw used, at the same window size, so what is drawn is what is
   hit at every DPI.
3. A press that begins on a node and lifts elsewhere does nothing — the rule the HUD's buttons
   already keep, so a tap can be cancelled by sliding off.
4. The camera arrives with the focus **released**: flying somewhere is the player looking, and a
   fleet focus that survived it would drag the camera back the next frame.
5. Nothing simulated moves. Turn this slice off and the same ticks produce the same universe.

## 5. Acceptance

- **Screenshots**, which are what accept a screen — and this design now owes three, since slice 1's
  were not paid either. Recorded as owed if the environment still cannot produce one.
- `NeuronClientTests` for the hit test's arithmetic if it can be reached there; the projection's
  inverse is the same kind of claim `FitBoxIsotropic` is, and it is wrong in silence if a node is
  tapped a hundred pixels from where it draws.
- The whole suite green; `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy.
- No decision record: flying a camera to a place decides nothing.

## 6. Assumptions the implementer may make

- **`SystemAt` is not the inverse of the map.** The map hit-tests in *pixels*, against drawn nodes;
  `SystemAt` answers in metres, against stars. They agree in the middle of a system and need not
  agree in the void between two, and the map's answer is the one a finger meant.
- **The camera may be put anywhere.** Slice 4b's claim. If it turns out the scenery does not follow,
  that is this slice's finding and it is reported rather than worked around.

---

## 7. What changed on contact, and what was found

- **Slice 4b's claim held, and more completely than §1 expected.** The flight rebuilds no scenery: the
  frame loop already asks `SystemAtCamera` once per frame, after the last thing that moves the camera
  and before `Render`, and rebuilds when the answer changes. That check was written for a fleet
  crossing a gate and it covers a camera put anywhere for free — so `FlyToSystem` is `SnapGoal`, a
  cancelled focus, and the two lines that close the map, and nothing else. A first draft of this slice
  called a `RebuildSceneryForCamera()` that does not exist and did not need to; deleting it is the
  finding.
- **The tap is reported upward, not acted on.** `GalaxyScreen` says which system a contact named and
  the composition root decides what that means — `Hud::HandlePointer`'s division, and the one that
  makes slice 3 an addition rather than a rewrite: ordering a fleet is a second meaning for the same
  report.
- **Nearest node, ties to the lower index.** `Game::SystemAt`'s own rule, for the same reason: an
  arbitrary answer is one that can differ between two runs. The measurement below says the tie never
  actually has to be broken at any window size a player has.
- **A closed map names nothing and consumes nothing**, and `Close` drops any press it was holding, so
  a contact still down when Escape closes the screen cannot name a system on its way up.

## 8. What was verified, and how — and the honest gap

The claim worth testing is that **what is drawn is what is hit**, because a node tapped a hundred
pixels from where it draws is wrong in a way no unit test of the projection would catch. Run over the
shipped galaxy at four window sizes, hit-testing every node at exactly the pixel the draw puts it at:

```
1280x720   dpi 1.0 | 54/54 nodes hit at their own pixel, 0 wrong | closest pair 43.2 px, tap radius 18.0
1920x1080  dpi 1.0 | 54/54 nodes hit at their own pixel, 0 wrong | closest pair 74.1 px, tap radius 18.0
2560x1440  dpi 1.5 | 54/54 nodes hit at their own pixel, 0 wrong | closest pair 95.7 px, tap radius 27.0
 800x1400  dpi 1.0 | 54/54 nodes hit at their own pixel, 0 wrong | closest pair 46.5 px, tap radius 18.0
header taps at every size: nothing        closed map: consumed nothing, named nothing
```

The tap radius is comfortably inside the closest pair at every size — 18 px against 43, at the
tightest — so the nearest-node rule never has to break a tie a player would call wrong, and a miss
between two nodes names neither rather than guessing.

**The gaps, and both are §5's:** there is no `NeuronClientTests` row, because the hit test lives in
`Outpost` and reaches `Game::GalaxyLayout` — `NeuronClient` may not see `GameLogic`, so the suite that
proved `FitBoxIsotropic` cannot reach this. It is verified beside the tree instead, which is the same
gap `TickStats`, `HullParts` and slice 1 have and the same one that let a compile error reach CI on
this branch. And the screenshots are unpaid: this design now owes them for both landed slices.

## 9. What CI caught that nothing here did — and the guard that closes it

Run 252 failed the build on four errors, all from one line this slice added to `FlyToSystem`:

```cpp
if (m_log)
  m_log->PushFormat(EventLog::Severity::Info, m_view.SimTimeSec(), "MAP | SYSTEM %u", _system);
```

Two mistakes in it, and both are the same shape — **a name that exists but is not reachable from
where it was written**:

- `OutpostApp` holds `m_log` **by value**. `UniverseView` holds one by pointer, and that is where the
  idiom was copied from without noticing whose member it was.
- `UniverseView::SimTimeSec` is **private**. The root has no business reading the view's sim clock,
  and every other line the root pushes passes `0.0f`.

It is the same failure as run 249 in a different costume, and for the same structural reason:
**`Outpost/OutpostApp.cpp` is the one file in this tree that no compiler outside Windows ever sees.**
Every other `Outpost` source can be built here behind a stub — `GalaxyScreen.cpp` was, at
`-Wall -Wextra` — but the composition root reaches the whole graphics stack through `NeuronClient`,
so nothing local instantiates what is written into it.

Twice is a pattern, so this slice adds [`Build/CheckViewAccess.py`](../Build/CheckViewAccess.py). It
reads the headers of the six classes the root owns and checks the two things a script can actually be
sure about: that a member called on one of them is declared in that class's **public** section, and
that `->` is used through a pointer and `.` through a value. It is scoped to `OutpostApp.cpp` alone,
because the same member name means something else one class over — `UniverseView`'s own `m_log` is a
pointer, and checking every file would report that as twenty failures and teach a reader to ignore the
output.

Replanting the exact failure proves it catches both halves:

```
Outpost/OutpostApp.cpp:841: m_view.SimTimeSec() is not public on UniverseView
Outpost/OutpostApp.cpp:841: m_log->PushFormat -- OutpostApp holds m_log by value, use "."
```

**It is deliberately not in CI.** MSVC gates the same mistakes there, five minutes later and more
completely; this is what you run in the two seconds before pushing. `AGENTS.md` §12 says so, which is
where a contributor will meet it. It is a crutch and says so in its own docstring — the real fix is a
composition root that something can compile, and that is not this slice's to build.
