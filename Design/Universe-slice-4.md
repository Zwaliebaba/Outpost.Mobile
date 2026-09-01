# Universe slice 4 — the client crosses

Work order for slice 4 of [`Universe.md`](Universe.md). Depends on slices 1–3.

## 1. Scope

A player can order a jump, watch it happen, and still know where they are afterwards.

- **`SHIP_FLAG_GATE`** on the ship record. A gate is a Structure, so a client cannot tell one from a
  station or from scenery by the hull table — it reads the record's own flag, which is the rule
  `SHIP_FLAG_STATION` already set (`UniverseView::PickStation`).
- **`JUMP` on the fleet sheet**, fifth of five, beside `DOCK` — the command it is shaped like. It
  arms the next universe tap; `PickGate` resolves it; `IssueJumpOrder` sends it.
- **A jumped ship leaves without dying.** The `Jumped()` run is drained on the docked run's terms:
  no explosion, no shake, no `SHIP LOST`.
- **The camera crosses with the fleet**, through the focus machinery the fleet button already uses —
  and that focus now **snaps** past `CAMERA_SNAP_METRES` instead of easing.

## 2. Out of scope

- **The galaxy map screen.** A fleet can cross without one; a map is UI work that should not gate
  the mechanism (design §9).
- **A wink-out effect.** The removal is silent and plain. The effect is a look rather than a
  mechanism and is left out on purpose, said at the code so nobody mistakes the placeholder for the
  finished thing.
- **Gate marks on the minimap, and bodies that follow the camera's system.** See §7 — this is the
  one piece of §9 the slice does not deliver, and why.
- The sky stays one sky. The save file (5) and the island-scoped replan (6).

## 3. What to build on

- `UniverseView::PickStation`/`IssueDockOrder` — the shape `PickGate`/`IssueJumpOrder` copy.
- `UniverseView::FocusFleet`/`UpdateFocus` and `m_focusSlot` — the fleet button's fly-to.
- The `Docked()` drain loop — the `Jumped()` loop is that loop with a different verb.
- `FleetSheet`'s `COMMAND_LABELS`/`COMMAND_ARMS` pair, whose dispatch keys off `ArmedOrder::None`
  rather than an index, so a fifth command needs no change to the dispatch.

## 4. How it must behave

1. **A gate is picked by its flag**, never by `immovable` or by hull id.
2. **No standing refusal before a jump.** A gate takes anyone this phase, so an affordance that
   refused would be telling a truth the simulation does not hold (design §6.1).
3. **A jumped ship is removed silently.** Anything else and a fleet crossing a gate explodes on
   every screen that watched it leave.
4. **The camera snaps rather than flies** when the gap is beyond `CAMERA_SNAP_METRES` = 20 000 m —
   wider than any system (2 × 7 000 m of gate ring) and far inside the layout's guaranteed 56 926 m
   between stars, so it can only ever trigger on a crossing.
5. **Only a selected fleet takes the camera.** A fleet crossing somewhere else is not a reason to
   move what the player is looking at.

## 5. Acceptance

- `SnapshotTests::AGateIsFlaggedOnTheWire` — a gate's record carries `SHIP_FLAG_GATE`; a station
  carries only `SHIP_FLAG_STATION`; scenery carries nothing.
- The whole `GameLogicTests` suite green.
- **Screenshots at two window sizes**, one showing the `JUMP` prompt armed and one on the far side
  of a crossing. **Owed and not supplied — see §8.**
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy over GameLogic.

## 6. Assumptions

- The wire flag is a new bit in a byte that already exists, so no format size changes and the ALPN
  does not move again — slice 2 already bumped it to `outpost-5` in this same pull request.

---

## 7. What changed on contact, and what is deliberately not here

**The marks and the bodies do not follow the camera yet, and §9 of the design says they should.**
Stated as an omission rather than left to be found: after crossing a gate, the minimap still holds
the *home* system's station marks, which will draw clamped to its edge, and the worlds and asteroids
on screen are still home's. The records are right — ships, stations and gates all arrive over the
wire correctly — but the static scenery is a system behind.

It is not in this slice because it is not a small change and it is the one part of slice 4 that
cannot be checked here at all: re-placing bodies means re-running the generate-and-upload bracket
mid-session (the F5 path), which is GPU work, and re-marking means the view learning which system
the camera is in. Writing that blind, against no compiler and no screen, is how a slice arrives
red twice. **It should be its own slice, ordered next, and `Universe.md` §9 now says so.**

**The camera focus gained a snap, which the design did not ask for.** `Follow` eases
asymptotically — right across a system, useless across a galaxy: the ease spends seconds crossing
interstellar space with nothing on screen and the half-life never lands. `Camera::SnapGoal` is the
one new primitive in `NeuronClient`, and the threshold sits in the gap the layout guarantees so it
can only fire on a crossing. This also repairs a claim `README.md` already makes — that a fleet
button "flies the camera to it, wherever in the universe it is" — which stopped being true the
moment a fleet could be 130 km away.

**`CheckProjectFiles.py` caught a real MSVC break before CI did.** A local named `far` in the new
test: `<windows.h>` defines `far` as a macro, so the declaration would have expanded to something
else on the only platform that builds this tree. Renamed to `farSide`. That guard exists for
exactly this and it earned its keep.

## 8. What was verified, and how — and the honest gap

**This is the slice the design owed screenshots for, and they are not here.** No Windows, no D3D12,
no way to put a frame on a screen in this container. That is the same reason slices 8, 9 and 10 of
`MmoScalabilityPlan.md` owed theirs, and it is stated the same way rather than implied.

**More than that: `UniverseView.cpp`, `FleetSheet.cpp` and `Camera.cpp` were read, not compiled.**
Slice 4 is mostly client code, and the client is the half no compiler here can reach. CI is first
contact for all of it.

What *was* done:

- `SnapshotTests::AGateIsFlaggedOnTheWire` written, run, and **measured against itself**: dropping
  the gate bit from the writer turns it red.
- The whole `GameLogicTests` suite: 287 methods, **580 701 assertions**, green — which covers the
  wire half of this slice and proves the flag change broke nothing that reads a record.
- `CheckProjectFiles.py` (which caught the `far` macro), `CheckFormat.py`, and clang-tidy clean over
  `UniverseSnapshot.cpp` under LLVM 22.

**A reviewer on Windows should build `Debug|x64`, then: select a fleet, hold its button, press
`JUMP`, tap a gate, and confirm the fleet flies to it, crosses, and the camera arrives with it —
and take the two screenshots this slice owes.**
