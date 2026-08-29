# Work order — Planets and asteroids slice 5: the ocean

Implements slice 5 of [`PlanetRenderer.md`](PlanetRenderer.md) §15: sea-level culling, the
shore dip, and the flat-colour ocean sphere — the three rules of design §5.5 — for every class
whose `outsideHeight` is below zero.

**Layer:** `NeuronClient` and `NeuronClientTests`, plus `Outpost`. Two layers in one slice, the
design's stated exception (§15): the engine half is a few dozen lines behind one sign test and
the game half is one draw call, and neither is worth looking at without the other.
**Depends on:** slice 4.
**Blocks:** nothing; slice 6 is independent of it.

---

## 1. What this slice is for

Design §15 split the ocean out so a dry body could be judged first. Now it is: the terran and
ice worlds of slice 4 draw their sea floor as green terrain below `R`, which is wrong, and this
slice makes them wet. Three rules from the source, one inner sphere through a pipeline that
already exists, and no new texture (owner's decision, design §14).

---

## 2. Scope

### 2.1 `NeuronClient/BodyMeshBuilder` — the two terrain rules

Both behind `field.Params().outsideHeight < 0`; a dry body's output is bitwise unchanged (a test).

- **Sea-level culling.** A cell is skipped when all six samples the source looked at — its four
  corners and the two that complete the strip's neighbours, `(x−1, z)` and `(x, z−1)` clamped
  to the face — are `≤ 0`. `_outStats.trianglesCulled += 2`.
- **Shore dip.** A corner with `h < BODY_SHORE_THRESHOLD × R` is placed at `h =
  BODY_SHORE_DIP × R` before the position is formed. Both constants are `static constexpr` on
  the builder — `0.0003f` and `−0.01f`, design §5.5 — because the ratio to the source's `0.3`
  and `−10` on a 2 000-unit map is the engine's port, not a game tuning. The colour is looked up
  from the **undipped** centroid height, as the source did (its colour pass read the height
  before the strip builder pushed the vertex down).

`Build` gains a second output:

```cpp
static void Build(const BodyField& _field, const ColourRamp* _ramp, std::vector<FxVertex>& _outTerrain,
                  std::vector<MeshVertex>& _outOcean, const DirectX::XMFLOAT3& _oceanColour, BodyBuildStats& _outStats);
```

- **The ocean sphere.** When wet: a `MeshVertex` cube-sphere at `gridPower − 2` (17 samples a
  side for a planet — 3 072 triangles), radius `R`, the ellipsoid applied, every vertex
  `_oceanColour`, unshared vertices in the same corner order as the terrain. When dry:
  `_outOcean` untouched. It is `MeshVertex` because it goes through `SceneRenderer::DrawMesh`
  and gets the scene's derivative flat shading for free (design §5.5).

### 2.2 `Outpost/WorldView` — one draw

`BodyView::ocean` is filled by `AddBody`'s caller; `Render` draws it in the **opaque pass,
before the hulls**, through `_renderer.DrawMesh(gpu, ocean, world, oceanColour, 0.0f, false)`
with the same `world` the terrain uses — spin included, so the sea turns with the land. Hulls
and bodies follow; the terrain's depth then occludes the sea where land rises and the dip hides
the coast behind it.

`BodyView` gains `Neuron::Rgba oceanColour`, from the class row.

### 2.3 `Outpost/OutpostApp` — upload

`SpawnStartingBodies` passes `BodyClassOf(cls).oceanColour` to `Build`, uploads `_outOcean`
through `m_sceneRenderer.UploadMesh` when non-empty, and stores the handle. F5 leaks it as it
leaks the terrain (slice 4 §2.4).

### 2.4 Tests

`BodyMeshTests.cpp` gains the ocean rows of design §11.

---

## 3. Out of scope

- **A textured, drifting ocean; the `waves` shore band.** Design §13, §14.
- **Lava and acid.** A class with a red `oceanColour` is a row; no emissive term.
- **Releasing buffers on F5.** Slice 4's stated leak, unchanged.
- **Changing the asteroid path.** Every asteroid is dry and its output is bitwise unchanged.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronClient/BodyMeshBuilder.cpp` (slice 2) | The sample-then-emit loop the two rules slot into |
| `NeuronClient/CubeSphere.h` (slice 1) | The sphere the ocean is built on |
| `NeuronClient/SceneRenderer.h` | `UploadMesh`, `DrawMesh` with a base colour and `materialMix = 0` |
| `Outpost/WorldView.cpp` `Render` (slice 4) | The body loop; the ground draw is the model for one flat-colour `DrawMesh` |
| `Outpost/BodyCatalogue.h` (slice 4) | `wet`, `oceanColour` already on the row |
| Design §5.5, §8.2 steps 2 and 4 | The rules |

---

## 5. What will surprise the implementer

### 5.1 The six-sample rule is the source's strip artefact, kept on purpose

Four corners would be the obvious test. The source checked six because its strip joined
neighbouring rows; keeping six means a one-sample island keeps one ring of sea-floor cells
around it, which is what stops coastlines from looking punched out. Do not simplify to four.

### 5.2 Depth at the shore

The dipped vertex sits `0.01 R` below the sea — 8 m on an 800 m world. At 900 m zoom with
`D32_FLOAT` and a 0.5 m near plane, that is many depth units; no z-fighting is expected. If the
screenshot shows any, the dip is the knob, and the pull request says what value fixed it
rather than changing the constant silently.

### 5.3 The ocean sphere is drawn through a pipeline that faces normals to the eye

`ScenePS` flips the derivative normal towards the camera. On a closed sphere with cull none that
is right for the near side and harmless for the far side (depth-rejected). Nothing to do; noted
so nobody adds a flag.

---

## 6. Decision records due

None.

---

## 7. Acceptance

**`BodyMeshTests.cpp`:**

- **All ocean, no land.** A desc with `outsideHeight = −0.02` and no tiles: `trianglesEmitted
  == 0`, `trianglesCulled == 6·8·8·2`, `_outOcean.size() == 6·2·2·2·3` at `gridPower = 3`
  (ocean at `gridPower − 2 = 1`, two cells a side).
- **Dry is untouched.** The slice 2 desc built with the new signature hashes to the same pinned
  literal as before, and `_outOcean` is empty.
- **The shore dips.** A wet desc with one tile: every vertex whose undipped height was below
  `BODY_SHORE_THRESHOLD × R` sits at distance `R + BODY_SHORE_DIP × R` from the origin (along
  its direction, ellipsoid applied), within `1e-3`.
- **The dip does not change the colour.** The same triangle's colour equals the colour it had
  with the dip disabled (a `BodyMeshBuilder` test hook is **not** added for this; the test
  builds the same desc at `outsideHeight = +0.0001` and `−0.0001` and compares the colours of
  triangles present in both).
- **The ocean is a sphere.** Every ocean vertex is at distance `R` along its direction, ellipsoid
  applied, within `1e-3`, and every ocean colour is `_oceanColour`.

**Screenshots at two window sizes:**

- The terran planet at the default view: blue sea, green continents with no gap and no dark
  line at the coast; the ice world's pale sea.
- Zoomed to 40 m on a coastline: the land meets the water with the outline running to the
  water's edge and no sea floor visible.
- Zoomed to 900 m: no z-fighting at any shore.
- The barren moon and every asteroid unchanged.

**The tree:** the checks pass; Debug|x64 builds; `NeuronClientTests` passes with the new rows;
`GameLogicTests` unchanged; design §15 marks slice 5 `landed`; this file moves to
`Design/Archive/`.

---

## 8. Assumptions the implementer may make

- **`gridPower − 2` for the ocean** is enough: a 17-sample sphere at 800 m has 75 m facets,
  which read as the source's flat water plane, not as polygons, from the distances the camera
  reaches.
- **The ocean's colour is flat** and the class row's; no lighting change, no material mix.
- **The dip and threshold constants are the engine's**, not `ViewTuning`'s (§2.1).
