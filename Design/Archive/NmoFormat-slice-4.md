# Work order — NMO slice 4: the marker consumers

Implements the **marker consumers** of [`NmoFormat.md`](NmoFormat.md) §9 and §14 — that document's
slice 3, moved behind the content swap; [slice 3](NmoFormat-slice-3.md) §2.7 says why. This slice
is the visible one: every exhaust burns the colour its author gave it, at the radius its author
gave it, and the hulls grow navigation lights that blink on periods authored per light.

**Layer:** `Outpost`, plus the `NeuronClient` field slice 2 marked temporary and the reader loop
that fills it.
**Depends on:** slice 3 (hulls ship as `.nmo` and load through `NmoReader`).
**Blocks:** slice 5 (liveries), which multiplies the exhaust colours this slice puts in the view.
Slice 6 (articulated parts) is independent.

---

## 1. Why this is a slice

Two constants have been standing in for authored content since the hulls arrived: every plume in
the game is `SELECTED_COLOUR` or `HOSTILE_ACCENT_COLOUR`, and no hull has a running light at all.
The data to replace both has been in the files since slice 3 and has been read into `MeshData`
since slice 2, drawn by nothing. This slice connects them, and it is decided by screenshots
because that is the only thing that can decide it.

It is also where `attachPoints` — the last trace of the clustering heuristic, kept alive across
two slices purely so the swap could be invisible — finally goes.

---

## 2. Scope

### 2.1 `NeuronClient/MeshData.h` — `attachPoints` is deleted

The field, its comment, and the loop in `NmoReader` that fills it (slice 2 §2.2). Its one consumer
moves to the marker list in §2.2. Nothing else in `MeshData` changes.

### 2.2 `Outpost/WorldView.h` — the view holds exhausts, not points

`ShipView::thrusterLocals` (`std::vector<XMFLOAT3>`, "one point per exhaust nozzle") becomes:

```cpp
struct ExhaustView
{
  DirectX::XMFLOAT3 local{0.0f, 0.0f, 0.0f}; // nozzle position in mesh space
  Neuron::Rgba colour{1.0f, 1.0f, 1.0f, 1.0f};
  float radiusMetres = 0.0f;                 // the marker's scale, already in metres
};
std::vector<ExhaustView> exhausts;
```

and a parallel `std::vector<NavLightView> navLights` carrying `local`, `colour`, `periodSec`
(`param0`) and `phase` (`param1`).

The trail ring buffer is indexed nozzle-major off `thrusterLocals.size()` in three places
(`ApplySnapshot`'s `assign`, the sampler, the draw). It becomes `exhausts.size()`; the ring's
shape, its head and its count are unchanged — the trail is per nozzle and there are exactly as many
nozzles as before.

**Copy the colour and radius into the view rather than reaching into `MeshLibrary` per frame.**
`ShipView` already copies `restY`, `pickCentre` and `halfExtents` out of `MeshData` for exactly
this reason, and the draw loop runs per billboard.

### 2.3 `Outpost/WorldView.cpp` — `ApplySnapshot`, the plume, the lights

**`ApplySnapshot`** (the branch that first sees a ship) fills `exhausts` and `navLights` by walking
`data.markers` once and switching on `MarkerKind`. `MarkerKind::Gun`, `Point` and `Unknown` are
skipped — carried by the file, consumed by nobody, exactly as §9 says.

`radiusMetres` is the marker's `scale`. It is the nozzle radius in metres and it is authored per
hull (0.911 on the Corvette's two), so it replaces `THRUSTER_GLOW_RADIUS` as the *size* of the
glow — see §2.4.

**The plume** (`Render`, the thruster glow and trail block). Three changes and no more:

- `view.thrusterLocals[nozzle]` becomes `view.exhausts[nozzle].local`.
- The per-ship `const Rgba accent = IsOwn(i) ? SELECTED_COLOUR : HOSTILE_ACCENT_COLOUR;` hoist is
  deleted. The colour is now per nozzle, from `view.exhausts[nozzle].colour`, hoisted out of the
  step loop instead — one lookup per ribbon rather than one per billboard, which is what the
  existing comment on that hoist is actually protecting.
- `glowRadius` becomes per nozzle: `std::max(0.1f, exhaust.radiusMetres * THRUSTER_GLOW_SCALE) *
  SHIP_SCALE`. The alpha ramp, the taper, the trail walk, `THRUSTER_GLOW_FALLOFF` and the
  intensity animation are untouched — the marker colours the effect, the view still animates it
  (§9).

**A faction signal is lost here, and that is the decision, not an oversight.** A hostile's plume is
currently red because `HOSTILE_ACCENT_COLOUR` tints it; after this slice it is whatever the hull's
author chose, and friend and foe burn the same colour if they fly the same hull. Faction stays
readable through the selection ring, the minimap and the contact count, all of which are
untouched. The screenshots of §4 must show a hostile and a friendly together so a reviewer sees
the change rather than discovering it later. If it reads badly on screen, the fix is a design
question — a faction tint blended over the marker colour, a different hull for hostiles — and it
is not decided inside this slice.

**Navigation lights**, new, drawn through the same path the plume uses: `m_glowSamples` filled with
`Neuron::GlowSample`, `BuildGlowBillboards`, `m_fx->DrawGlows`. (Design §9 says `DrawGlow`; the
plume moved to the FX pass's batched glow after that sentence was written, and the nav lights
follow the code, not the citation. Note it in the pull request.)

Per visible ship, per nav light: transform `local` into world the way the trail sampler already
does — heading rotation, `SHIP_SCALE`, `restY`, `SHIP_HOVER_HEIGHT` — and push one `GlowSample` at
`radiusMetres = NAV_LIGHT_RADIUS * SHIP_SCALE`, colour the marker's, alpha
`NAV_LIGHT_INTENSITY × blink`.

`blink` is `1.0f` when `periodSec <= 0.0f` — a steady light, which is what a zero period means
(§5.10) and what most of them are. Otherwise, on the real-time clock:

```
phase01 = frac(navTimeSec / periodSec + light.phase)
blink   = phase01 < NAV_LIGHT_DUTY ? 1.0f : NAV_LIGHT_OFF_LEVEL
```

Ships blink independently only because their authors gave them different phases; two ships of one
hull blink together, which is correct and is what an author controls with `param1`.

**The clock is the interesting part.** Use a counter wrapped the way `m_skyTimeSec` is, and for the
same reason its comment gives: a float second counter left running loses enough precision after a
few hours that consecutive frames land on the same argument and the blink freezes. `m_skyTimeSec`
wraps at a period that is seamless for the sky's quantized rates and means nothing here, so add
`m_navTimeSec` beside it, advanced by the same `dt` in `UpdateFeedback`, wrapped at a whole
multiple of the longest period a marker may carry — `NAV_LIGHT_MAX_PERIOD_SEC`, with periods above
it clamped at load and traced. That makes the wrap seamless for every legal period and puts the
one number a content author could break in a named constant.

This is presentation state on a real clock, so it lives in `WorldView` and nowhere near a tick
(AGENTS.md §5).

A ship the frustum rejected gets no lights, on the same `view.visible` test the plume uses.

### 2.4 `Outpost/ViewTuning.h` — the new constants

Beside the existing `THRUSTER_*` block:

| Constant | Value | What it is |
|---|---|---|
| `THRUSTER_GLOW_SCALE` | `6.0f` | Multiplies the marker's nozzle radius into a glow radius. Replaces `THRUSTER_GLOW_RADIUS` as the tunable; the size now comes from content and this is how loudly it is drawn |
| `NAV_LIGHT_RADIUS` | `1.2f` | Metres, before `SHIP_SCALE` |
| `NAV_LIGHT_INTENSITY` | `0.85f` | Alpha at full on |
| `NAV_LIGHT_DUTY` | `0.35f` | Fraction of the period a blinking light is lit |
| `NAV_LIGHT_OFF_LEVEL` | `0.12f` | Alpha between blinks — a beacon dims, it does not vanish |
| `NAV_LIGHT_MAX_PERIOD_SEC` | `30.0f` | The clamp, and the wrap the clock is a multiple of |

`THRUSTER_GLOW_RADIUS` is deleted; every other `THRUSTER_*` constant stays exactly as it is.

The values above are a starting point chosen against the Corvette's authored numbers (nozzle
radius 0.911, nav light scale 1.28, the beacon's 1.8 s period and 0.15 phase). **Tune them from the
screenshots and state the final numbers in the pull request** — that is what this block is for.

### 2.5 The sentences this makes false

- **`AGENTS.md`**, opening section: the fleet description gains navigation lights, and the
  thruster-glow sentence stops implying one colour.
- **`Design/NmoFormat.md`** §9's `WorldView.cpp:100`/`728` citations, and its `DrawGlow` claim for
  nav lights. Per Design/README.md the design is not rewritten to match what was built; §14 gains
  the landed mark and the pull request notes the drift.
- Any comment in `WorldView` that calls a thruster position an "attach point" or a "cluster".

### 2.6 What this slice deliberately does **not** do

- **No `Gun` consumer.** The markers are read into `MeshData` and skipped by the view. Combat has
  no muzzle to flash yet, and simulation truth stays authored in `GameLogic`
  ([ADR 0002](Decisions/0002-content-readers-live-with-their-consumer.md), design §9).
- **No marker direction.** Nothing reads a marker's orientation. The plume is drawn along the path
  the nozzle travelled, as it is today, not along the marker's `+Z` — and the hulls' exhaust
  markers carry an identity rotation that lands on `+Y` anyway ([slice 3](NmoFormat-slice-3.md) §5).
  Slice 5's converter turns them aft when it regenerates the corpus; aiming a plume along one is
  slice 6's.
- **No emissive materials.** Still carried, still unread.
- **No livery.** An `Exhaust` marker's colour is drawn exactly as authored here. The corpus this
  slice runs on is slice 3's — the GLB's green hues, no `RaceTinted` bit set anywhere — so every
  plume burns the green its author gave it, on friend and foe alike. That is an interim look, one
  slice long: [slice 5](NmoFormat-slice-5.md) regenerates the corpus as shades with the flags set
  and multiplies them by the flying faction's colour. Do not "fix" it here by reading a faction
  colour early — the plume and the hull must start being liveried in the same commit or the two
  disagree on screen. `MeshMarker::raceTinted` is copied into `ExhaustView` now so slice 5's
  change is one multiply and not a second walk of the markers.
- **No `GameLogic`, `NeuronServer` or `NeuronCore` file.** None of this crosses the seam.
- **No new pipeline.** Nav lights reuse the FX glow the plume already batches into.
- **No `SceneRenderer` change.**
- **No bone or clip evaluation.** Markers are placed at bind pose.

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| `NeuronClient/MeshData.h` (slice 2) | `MeshMarker`, `MarkerKind`, `MeshData::markers` |
| `Outpost/WorldView.cpp` `ApplySnapshot` | The branch that first sees a ship and copies from `MeshData`; the trail `assign` |
| `Outpost/WorldView.cpp`, trail sampler (~`:343`) | Mesh-space → world for a nozzle: heading, `SHIP_SCALE`, `restY`, `SHIP_HOVER_HEIGHT`. The nav lights need the same transform |
| `Outpost/WorldView.cpp`, glow block (~`:1124`) | `m_glowSamples`, `GlowSample`, `BuildGlowBillboards`, `m_fx->DrawGlows`, the `view.visible` gate |
| `Outpost/WorldView.cpp` `UpdateFeedback` | `m_skyTimeSec` and the float-precision comment the nav clock repeats |
| `Outpost/ViewTuning.h` | The `THRUSTER_*` block and the file's constant conventions |
| `Art/Meshes/*.glb` | The authored markers, if a value needs checking at source |
| `Design/NmoFormat.md` §5.10, §9 | What each kind consumes, and who never sees one |

---

## 4. Acceptance

Visual, at two window sizes — 1600×900 and one other — in the pull request:

- **Authored plumes.** A friendly and a hostile Interceptor in one frame, both under way: the
  plumes are the hull's authored colour, not blue and red. State in the caption that the faction
  tint is gone on purpose (§2.3) and that the ring and minimap still carry it.
- **Per-nozzle radius.** The Bomber (three nozzles) beside the Corvette (two): glow sizes differ
  because the markers do, not because a constant does.
- **Nav lights, steady.** The Corvette at rest: red to port, green to starboard, at the positions
  the markers give. A second shot from the other side, showing the sides swap — which is the check
  that the mesh-space transform is right and not mirrored.
- **Nav lights, blinking.** Two shots ~1 s apart of the Corvette's `NavBeacon` (period 1.8 s,
  phase 0.15): lit in one, at `NAV_LIGHT_OFF_LEVEL` in the other.
- **The Structure and the Stargate.** Twenty-five and ten nav lights respectively, no exhausts:
  the station reads as lit architecture rather than a dark block. This is the shot that shows the
  slice was worth landing.
- **Nothing else moved.** Selection rings, order markers, shock rings, the explosion and the HUD
  are unchanged; one before/after pair at the same seed says so.

Not visual:

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; all four suites green. `NeuronClientTests` loses the `attachPoints`
  assertions from slice 2 §2.5 and keeps every other test; say which changed and why.
- `git diff --stat` shows no path under `GameLogic/`, `NeuronCore/` or `NeuronServer/`.
- A code read, stated: no allocation in the per-frame glow path — `exhausts` and `navLights` are
  filled in `ApplySnapshot`, and `m_glowSamples` is `clear()`ed and refilled as it is today.
- The final `ViewTuning.h` values, with a sentence on what each screenshot made you change.
- No decision record is due: the faction-tint loss is design §9's stated consequence and the
  reorder was recorded in slice 3. Say so — or, if the tint reads badly enough that you kept one,
  that *is* a record, and it is due here.
- `Design/NmoFormat.md` §14 marks the last slice landed, so **`NmoFormat.md` itself moves to
  `Design/Archive/`** with every citation of it retargeted in the same commit (Design/README.md),
  and this file moves with it.

---

## 5. Assumptions the implementer may make

- **Every hull has markers, and some have only one kind.** `Structure` and `Stargate` have nav
  lights and no exhausts; `Hauler` and `Miner` have five markers each. A ship with an empty
  `exhausts` list draws no plume, which is the behaviour today for a hull with no attach points.
- **Marker colours are linear RGBA and go straight to `Rgba`.** No conversion: the file states
  linear (§5.10), the codec wrote what Blender held, and `Rgba` is what the glow takes.
- **A marker's alpha is an intensity** (`NmoFormat.md` §5.10), on exhausts and nav lights alike:
  the glow alpha is `NAV_LIGHT_INTENSITY × blink × colour.a`, and the plume's is
  `thrusterIntensity × taper × colour.a`. Every shipped marker has `a = 1`, so nothing visible
  changes; the multiply is there so an author who dims one gets what they asked for.
- **Blink is not synchronised to anything.** It is free-running real time, so it drifts against the
  simulation and against a recording. That is correct for a running light.
- **The clamp on `periodSec` is a diagnostic, not a rejection.** A marker asking for 500 s is
  clamped to `NAV_LIGHT_MAX_PERIOD_SEC` and traced; the file is still valid and the ship still
  draws.
- **Twenty-five lights on the Structure is not a performance question.** They are billboards in a
  batch that already carries every plume sample in the scene; if a profile ever says otherwise it
  says so with a number.
