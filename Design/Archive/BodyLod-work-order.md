# Work order — MMO slice 20: body LOD and culling completion

Implements slice 20 of [`MmoScalabilityPlan.md`](MmoScalabilityPlan.md) §6 (finding G5), the last
slice of the plan: three grids baked per body at boot, selection by projected radius per frame, and
distance culling for asteroids on slice 9's frustum pass.

**Layer:** `NeuronClient` (`BodyMeshBuilder` untouched) and `Outpost` (the bake sites and the
per-frame selection live in the view and the root).
**Depends on:** slice 19 (this branch stacks on it). **Blocks:** nothing; it closes the plan.

---

## 1. Why this is a slice

A grid-6 planet is 147,456 unshared vertices — 4.13 MB — drawn at that cost however small it is on
screen, and an asteroid twelve metres wide is rasterised from any distance the frustum admits. The
bake already writes wherever it is pointed (ADR 0020) and `gridPower` has been the door since the
planet design deferred LOD (`Archive/PlanetRenderer.md` §13); this slice walks through it.

## 2. Scope

### 2.1 Three grids per body

`WorldView::BodyView::terrain` becomes `terrainLod[BODY_LOD_COUNT]` (3). The root bakes each body
three times at descending grid powers — the textured world's sphere at {6, 5, 4}, a generated rock
at {5, 4, 3} (`BodyDesc.gridPower` is copied and lowered per level; the field is a coarser sample
of the same seeded noise, which is the source design's own three-LOD scheme). All three ride the
one upload bracket the scene already uses; F5 rebuilds all three.

Memory, stated: a grid-6 world was 4.13 MB and is now 4.13 + 1.03 + 0.26 ≈ **5.42 MB (+31 %)**;
a grid-5 rock was 1.03 MB and is now ≈ 1.35 MB. The trade is memory for draw cost, and the plan's
finding is about what breaks at dozens of bodies: the drawn bytes, not the resident ones.

### 2.2 Selection by projected radius

In the view's per-body visibility loop: `projectedPx = boundingRadius / distance × (heightPx / 2) /
tan(fovY / 2)`. Two thresholds in `ViewTuning.h` — `BODY_LOD1_BELOW_PX`, `BODY_LOD2_BELOW_PX` —
chosen so the switch happens where a silhouette segment is already subpixel; the acceptance
screenshots at the switch distances are what tune them. The chosen level is stored beside
`m_bodyVisible` and both draw loops (terrain and outline) read it, so the outline always cages the
mesh that is actually drawn.

### 2.3 Distance culling for asteroids

The same projected radius, one more threshold: a body below `BODY_CULL_BELOW_PX` is not submitted
at all, riding the frustum pass's existing `m_bodyVisible` flag. Asteroids only — a world is never
distance-culled, because a world on the horizon is the framing.

### 2.4 Back-face culling: investigated, and deferred with its reason

The plan's scope says "enabled once winding is confirmed". The confirmation **failed**:
`BodyMeshBuilder::Build`'s own comment records that three of the six cube faces wind one way and
three the other — the normal is computed outward *whichever way the face's own winding ran* — and
`BuildSphere` and the bake kernel deliberately reproduce that order byte-for-byte. Enabling
`CULL_BACK` today would drop half of every body. Making the winding consistent means changing the
emit order in `Build`, `BuildSphere` **and** `BodyBake.hlsli` together, re-pinning
`BodyMeshTests`' golden vertex hash, and re-running the GPU/CPU readback agreement — a slice of its
own, named here so the next reader does not re-discover it. Cull stays `NONE`.

### 2.5 Sea-floor degenerates: stated, not built

The GPU bake writes a degenerate triangle where the CPU builder emits nothing, and the cheap
compaction the plan gestures at does not exist (it needs a count readback or a compaction
dispatch). With every shipped body a dry rock the degenerate count is zero anyway; stated per the
plan's "else stated".

## 3. What to build on

`BodyMeshBuilder::BuildSphere`/`Build` and `BakeBody` all take a grid power already; the
`place` lambda in `SpawnStartingBodies`; the visibility loop and `CULL_BODY_RADIUS_SCALE`;
`Camera`'s field of view from `ViewTuning.h`'s `CAMERA_FOV_DEG`.

## 4. Acceptance

- Screenshots at two zooms with no visible pop at the switch distances (the far one also shows the
  BC mips from slice 19 — one pair serves both).
- Memory per body at three grids stated against today's 4.13 MB (above).
- F5 reseeds; all four suites green; `CheckProjectFiles.py`, `CheckFormat.py`; Debug|x64 builds;
  the F1 readout's `BODY TRIS` counts the LODs actually drawn.

## 5. Assumptions

- Thresholds are presentation tuning (`ViewTuning.h`), outside any contract.
- A textured world's three spheres share the one map; no per-body texture work (out of scope per
  the plan).
- The plan's status paragraph and slice table close out with this slice; the review's G5 row is
  retired with the winding finding recorded.
