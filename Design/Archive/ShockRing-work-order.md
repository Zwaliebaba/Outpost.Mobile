# Work order — the shock ring

A station that blows up leaves a blast front: one ring on the ground plane, expanding out from where
it stood, thinning and fading as it goes. Small enough that it is a work order rather than a design.

**Layer:** `Outpost` only.
**Depends on:** the ship explosion (`Design/Archive/SpaceshipExplosion.md`, all four slices landed).
**Blocks:** nothing.

---

## 1. What this is, and the three questions the owner settled

The explosion that landed is fire and debris. What it has no answer for is scale: a station going up
should read as an event the map notices, and more particles is not that. A front travelling outward
is, and this tree can already draw one.

Three things were open, and were settled before this was written:

| Question | Decision | What lost |
|---|---|---|
| A flat ground ring, or an expanding sphere? | **Flat ring** | The sphere: it wants a pipeline, depth-aware blending and probably a screen-space distortion, against a ring that reuses a decal the game already draws |
| What decides which deaths get one? | **A parameter on the spawn**, set by the caller | A `hullScale` threshold guessing on the caller's behalf. Nothing is a station yet; when one exists it is the thing that knows |
| What is it called? | **Shock ring** | "Pressure wave": there is no medium in space, and the name would be the only part claiming there is |

## 2. Scope

### 2.1 `ViewTuning.h` — a new section

`SHOCK_RING_LIFETIME_SEC` (0.9), `SHOCK_RING_MAX_RADIUS` (90, scaled by `hullScale` like the rest of
the effect), `SHOCK_RING_WIDTH_METRES` (2.5), `SHOCK_RING_COLOUR`, and `SHOCK_RING_ON_EVERY_DEATH`
— the last a placeholder the station wave deletes (§5).

### 2.2 `ShipExplosion` — the ring is part of the death

`Spawn` gains `bool shockRing = false`: the parameter, not a rule the effect infers.

`Start` records the centre and `SHOCK_RING_MAX_RADIUS * hullScale` when it is set; `Advance` ages it;
`HasShockRing`, `ShockRingCentre`, `ShockRingRadiusMetres` and `ShockRingAlpha` report it. The class
still holds no device — it reports the ring exactly as it reports fragments, and something else draws.

`Finished` accounts for the ring, so the object is never dropped while it is still drawing. At the
tuned lifetimes the ring always passes first, which is why that line is a guard and not a mechanism.

- **Radius eases out**: `maxRadius × (1 − (1 − t)²)`. A front is fastest at the blast and slows as it
  widens; a linear ring reads as a circle being drawn rather than something thrown.
- **Alpha fades linearly** from `SHOCK_RING_COLOUR.a` to zero.

### 2.3 `WorldView::DrawFeedback` — drawing it

Inside the existing decal pass, after the order markers: one `DrawDecal` per live ring on the unit
quad, scaled to `radius × 2`, lifted by `DECAL_LIFT_Y`, `fill = 0`.

**The thickness argument is a fraction of the decal's own half-extent, not a width.** Holding the
front to a width in metres therefore means passing `SHOCK_RING_WIDTH_METRES / radius`, shrinking as
the ring grows. A constant fraction draws a band that thickens as it expands, which reads as a
spreading stain rather than a wave.

`WorldView::ExplodeTheLost` sets `spawn.shockRing` from the placeholder constant.

## 3. Out of scope

- **A sphere, a distortion, a light flash, or anything that is not this ring.**
- **Knocking anything about.** It is presentation: no push on ships, no damage, nothing that reaches
  a tick. The simulation does not learn that a ring exists.
- **A second ring, a rebound, or a vertical component.**
- **Any new pipeline, shader or texture.** If the ring needs one, that is a different work order.
- **Deciding what a station is.** That is the station wave's, and §5 is the seam.

## 4. What to build on

| File | What it gives you |
|---|---|
| `NeuronClient/Shaders/DecalPS.hlsl` | The antialiased ring, already screen-space correct at any zoom |
| `SceneRenderer::DrawDecal` | `(mesh, world, colour, thickness, fill)` — the whole draw |
| `WorldView::DrawFeedback`, the order-marker block | An expanding ring with a fading alpha, done once already |
| `Outpost/ShipExplosion.h` | `Spawn`, and where `hullScale` is worked out |
| `ViewTuning.h:DECAL_LIFT_Y` | Clear of the ground quad, so the two cannot z-fight |

## 5. The seam the station wave takes over

`SHOCK_RING_ON_EVERY_DEATH` exists only so the effect can be looked at before a station exists. The
station wave sets `Spawn::shockRing` from the station itself and **deletes the constant** — it is a
placeholder, and it says so at its definition.

## 6. Decision records due

None. No type moves, no dependency rule changes, nothing is added that a reasonable person will
propose removing.

## 7. Acceptance

Nothing here can be decided by a test: it is one decal, and no suite creates a device.

**Measured** (a Corvette, `hullScale` 0.86, so a 77.4 m ring):

| t | radius | alpha | front width |
|---|---|---|---|
| 0.1 s | 16.2 m | 0.53 | 2.5 m |
| 0.3 s | 43.0 m | 0.40 | 2.5 m |
| 0.6 s | 68.8 m | 0.20 | 2.5 m |
| 0.9 s | 77.4 m | 0.00 | 2.5 m, passed |

The width holding at 2.5 m across a ring that grew from 16 m to 77 m is the §2.3 rule working. A
death with `shockRing = false` reports no ring.

**Screenshots at two window sizes** (AGENTS.md §7), which only a GPU can take:

- F4 on a selected Corvette: a ring leaves the hull, outruns the debris, and is gone before the
  shards have finished falling.
- The ring passes *under* the hulls and over the ground grid, and does not z-fight the grid.
- Two deaths close together: two rings, each on its own ship, neither snapping to the other.
- With `SHOCK_RING_ON_EVERY_DEATH` set false, no ring is drawn and nothing else changes.

**The tree**: `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass; Debug|x64
builds; all four suites still pass; no file under `GameLogic/`, `NeuronCore/`, `NeuronClient/` or
`NeuronServer/` is touched.

## 8. Assumptions the implementer may make

- **The ring is flat and always will be** until a work order says otherwise (§1).
- **0.9 s and 90 m are first passes.** They are single constants and are meant to be nudged once
  someone has watched one.
- **The ring outlives nothing.** Its lifetime is shorter than the fragments', so the explosion object
  is never held open for it in practice.
