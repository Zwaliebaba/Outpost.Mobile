# Work order — Planets and asteroids slice 4: the catalogue, the scene, and F5

Implements slice 4 of [`PlanetRenderer.md`](PlanetRenderer.md) §15: the game's side — the body
classes and their ramps, the starting scene, the spin and tumble, the two draw loops, F5 to
reseed, every number in `ViewTuning.h`, and the decision record that bodies are presentation.

**Layer:** `Outpost` only, plus one file in `Design/Decisions/`. No `GameLogic` file, no engine
file.
**Depends on:** slices 1–3.
**Blocks:** slice 5.

---

## 1. What this slice is for

Slices 1–3 built the parts and none of them knows what a terran world or an asteroid field is.
This slice does: it is where "terran uses `LandscapeEarth`, caps from 75° latitude" is written
down as a table row, where the fleet's neighbourhood gets its two worlds and six rocks, and where
the owner tunes by pressing a key. It removes slice 3's placeholder and is the first time the
feature is on screen as content rather than as scaffolding.

---

## 2. Scope

### 2.1 `Outpost/ViewTuning.h` — a new section

After "world look", in the file's style. Every number from design §3, §5.4, §6.2 and §9,
**fractions of radius unless the name says metres**:

```cpp
// --- planets and asteroids ---------------------------------------------------------------------
// Bodies are presentation only, placed by the composition root, metre-scale inside the existing
// frustum (Design/PlanetRenderer.md 3, 14). Every length is a fraction of the body's radius unless
// the name carries a unit.
inline constexpr float BODY_PLANET_RADIUS_MIN_METRES = 400.0f;
inline constexpr float BODY_PLANET_RADIUS_MAX_METRES = 1200.0f;
inline constexpr float BODY_ASTEROID_RADIUS_MIN_METRES = 15.0f;
inline constexpr float BODY_ASTEROID_RADIUS_MAX_METRES = 120.0f;
inline constexpr std::uint32_t BODY_PLANET_GRID_POWER = 6;      // 65 samples a side
inline constexpr std::uint32_t BODY_ASTEROID_GRID_POWER = 5;    // 33; 4 for the smallest
inline constexpr float BODY_PLANET_LIFT = 1.15f;                // centre height over the plane, × radius
inline constexpr float BODY_PLANET_SPIN_SEC = 240.0f;           // one turn
inline constexpr float BODY_PLANET_TILT_MAX_DEG = 30.0f;
inline constexpr float BODY_ASTEROID_TUMBLE_MAX_RAD_PER_SEC = 0.15f;
inline constexpr float BODY_ASTEROID_ELLIPSOID_MIN = 0.55f;
inline constexpr float BODY_ASTEROID_LUMPINESS = 0.25f;
inline constexpr float BODY_ASTEROID_HEIGHT_SCALE_MIN = 0.15f;
inline constexpr float BODY_ASTEROID_HEIGHT_SCALE_MAX = 0.40f;
inline constexpr int BODY_ASTEROID_CRATERS_MIN = 6;
inline constexpr int BODY_ASTEROID_CRATERS_MAX = 20;
inline constexpr float BODY_ASTEROID_CRATER_HALF_WIDTH_MIN_RAD = 0.07f;
inline constexpr float BODY_ASTEROID_CRATER_HALF_WIDTH_MAX_RAD = 0.26f;
inline constexpr float BODY_PLANET_HEIGHT_SCALE_MIN = 0.03f;
inline constexpr float BODY_PLANET_HEIGHT_SCALE_MAX = 0.12f;
inline constexpr int BODY_PLANET_TILES_MIN = 1;
inline constexpr int BODY_PLANET_TILES_MAX = 4;
inline constexpr float BODY_PLANET_TILE_HALF_WIDTH_MIN_RAD = 0.44f;
inline constexpr float BODY_PLANET_TILE_HALF_WIDTH_MAX_RAD = 1.22f;
inline constexpr float BODY_PLANET_TILE_EDGE_FRACTION = 0.25f;
inline constexpr float BODY_PLANET_OUTSIDE_HEIGHT_WET = -0.02f;
inline constexpr float BODY_PLANET_OUTSIDE_HEIGHT_DRY = 0.01f;
inline constexpr float BODY_FRACTAL_DIMENSION = 0.8f;
inline constexpr float BODY_LOWLAND_SMOOTHING = 1.2f;
inline constexpr float BODY_CAP_NOISE = 0.1f;
inline constexpr float BODY_POLAR_GEOMETRY = 0.15f;
inline constexpr Neuron::BodyOverlayParams BODY_OVERLAY{1.2f, 4.0f, 0.5f, 40.0f};
// The starting scene, from one seed so the pull request's screenshot reproduces.
inline constexpr std::uint64_t BODY_START_SEED = 0x4F75747031ull;   // "Outp1"
inline constexpr float BODY_START_PLANET_DISTANCE_METRES = 4000.0f;
inline constexpr float BODY_START_MOON_DISTANCE_METRES = 3000.0f;
inline constexpr int BODY_START_ASTEROIDS = 6;
inline constexpr float BODY_START_ASTEROID_RING_MIN_METRES = 150.0f;
inline constexpr float BODY_START_ASTEROID_RING_MAX_METRES = 400.0f;
```

`ViewTuning.h` includes `BodyRenderer.h` for `BodyOverlayParams`, as it already includes
`RenderTypes.h` for `Rgba`.

### 2.2 `Outpost/BodyCatalogue.h`

The class table, in the `HullSpec.h` style — an enum, a `COUNT`, a `constexpr` array with an
annotated column header, a `static_assert` on the count, and a clamping accessor:

```cpp
enum class BodyClass : std::uint8_t { Terran, Classic, Ice, Desert, Barren, Asteroid };
inline constexpr std::uint32_t BODY_CLASS_COUNT = 6;

struct BodyClassSpec
{
  const wchar_t* ramp;          // file under Terrain\, or nullptr for the fallback grey
  float polarStrength;
  float capStart;
  bool wet;                     // outsideHeight below zero; the ocean lands in slice 5
  Neuron::Rgba oceanColour;     // slice 5 reads it; named now so the row is complete
  bool asteroid;                // the asteroid parameter set of design 5.4
};

//                                       ramp                       polar  cap    wet    ocean colour                       asteroid
inline constexpr BodyClassSpec BODY_CLASSES[BODY_CLASS_COUNT] = {
  {L"LandscapeEarth.dds",    0.6f, 0.75f, true,  {0.10f, 0.22f, 0.40f, 1.0f}, false},  // Terran
  {L"LandscapeDefault.dds",  0.6f, 0.75f, true,  {0.08f, 0.12f, 0.35f, 1.0f}, false},  // Classic
  {L"LandscapeIcecaps.dds",  1.0f, 0.35f, true,  {0.30f, 0.34f, 0.42f, 1.0f}, false},  // Ice
  {L"LandscapeDesert.dds",   0.0f, 1.0f,  false, {0.0f, 0.0f, 0.0f, 0.0f},    false},  // Desert
  {L"LandscapeMine.dds",     0.0f, 1.0f,  false, {0.0f, 0.0f, 0.0f, 0.0f},    false},  // Barren
  {L"LandscapeMine2.dds",    0.0f, 1.0f,  false, {0.0f, 0.0f, 0.0f, 0.0f},    true},   // Asteroid
};

[[nodiscard]] constexpr const BodyClassSpec& BodyClassOf(BodyClass _class) noexcept;   // clamps an unknown id to row 0

// Everything else in a BodyDesc, drawn from Pcg32(_seed) within the BODY_* ranges. Design 5.4, 9.
[[nodiscard]] Neuron::BodyDesc RandomBody(std::uint64_t _seed, BodyClass _class, float _radiusMetres);
```

`RandomBody` lives in `BodyCatalogue.cpp`. It draws in a fixed order (ellipsoid, lumpiness,
height scale, tile count then tiles, crater count then craters, tilt) so a seed means one body
forever — the same rule slice 1 gave `BodyField`, one level up.

### 2.3 `Outpost/WorldView` — bodies, spin, tumble, and the draws

```cpp
struct BodyView
{
  Neuron::BodyHandle terrain = Neuron::INVALID_BODY;
  Neuron::MeshHandle ocean = Neuron::INVALID_MESH;      // slice 5 fills it
  Game::WorldPos centre;
  float centreY = 0.0f;
  DirectX::XMFLOAT3 spinAxis{0.0f, 1.0f, 0.0f};
  float spinRadPerSec = 0.0f;
  float spinRad = 0.0f;
  DirectX::XMFLOAT3X3 tumble;                           // identity at add; asteroids only
  DirectX::XMFLOAT3 tumbleRadPerSec{0.0f, 0.0f, 0.0f};
};

void AddBody(const BodyView& _body);
void ClearBodies() noexcept;                            // F5
void SetBodyRenderer(Neuron::BodyRenderer& _bodies) noexcept;
[[nodiscard]] std::size_t BodyCount() const noexcept;   // the F1 readout
```

- **`UpdateFeedback`**: `spinRad += spinRadPerSec × dt` (wrapped); `tumble = tumble ×
  RotationRollPitchYaw(tumbleRadPerSec × dt)` — the explosion's `Tumbler` rule, the same stored
  type.
- **`Render`**, per design §7.3:

```
BeginScene; ground; [ocean spheres -- slice 5]; hulls
if (m_bodies && !m_bodyViews.empty())
    m_bodies->Begin(gpu, viewProj, lightDir, AMBIENT_LEVEL, eye, BODY_OVERLAY)
    for each body: world = tumble · RotationAxis(spinAxis, spinRad) · Translation(ViewX(centre), centreY, ViewZ(centre)); DrawMain
    for each body: DrawOverlay with the same world (recomputed or cached in a scratch vector; no allocation per frame)
DrawFeedback (unchanged)
```

Bodies go through `ViewX/ViewZ` like ships, so the camera-rebase seam (`WorldView.h`, the
`m_viewOrigin` comment) moves them for free the day it lands.

### 2.4 `Outpost/OutpostApp` — boot, the scene, and the key

- `Neuron::BodyRenderer m_bodyRenderer` after `m_textRenderer`; `std::array<Neuron::ColourRamp,
  BODY_CLASS_COUNT> m_ramps`; a `TERRAIN_DIR = L"Terrain\\"` beside the other three directory
  constants.
- `Init`: after `m_textRenderer.Init`, `m_bodyRenderer.Init(m_gpu, {TEXTURE_DIR +
  L"TriangleOutline.dds"})`; load each class's ramp through `ColourRamp::Load(TERRAIN_DIR + ramp)`
  — a failure traces and the class draws grey; `m_view.SetBodyRenderer(m_bodyRenderer)`;
  `SpawnStartingBodies(BODY_START_SEED)`; **then** `m_gpu.ExecuteAndWait()` and
  `m_bodyRenderer.DiscardStaging()` — the one submission slice 3's `Init` comment asks for. If
  `m_textRenderer.Init` already called `ExecuteAndWait`, a second call here is harmless and
  required.
- `SpawnStartingBodies(seed)`: a `Pcg32(seed)`; one `Terran` planet of radius drawn from the
  planet range at `BODY_START_PLANET_DISTANCE_METRES` bearing north-east; one `Barren` moon at
  `BODY_START_MOON_DISTANCE_METRES` north-west, half the planet's radius; `BODY_START_ASTEROIDS`
  asteroids at random bearings and radii in the ring range. For each: `RandomBody`, `BodyField`,
  `BodyMeshBuilder::Build` with the class's ramp (or `nullptr`), `UploadBody`, `AddBody` with
  `centreY = R × BODY_PLANET_LIFT` for planets and `R` for asteroids, spin axis tilted by
  `Signed(BODY_PLANET_TILT_MAX_DEG)` about X then Z, tumble rates `Signed(BODY_ASTEROID_TUMBLE_MAX_RAD_PER_SEC)`
  on two axes for asteroids. Trace each body's `BodyBuildStats` and the total wall time.
- `OnKeyDown`: `case VK_F5:` — `m_view.ClearBodies()`, `m_bodyRerollCount++`,
  `SpawnStartingBodies(BODY_START_SEED + m_bodyRerollCount)`, then `ExecuteAndWait` and
  `DiscardStaging`. **The old buffers are not released**: `BodyRenderer` keeps every handle for
  the run (slice 3 §2.4), so F5 leaks 7 MB a press until a body-release path exists. Stated
  as a debug-key limitation in the `OutpostApp.h` key comment and in this order's §8; a
  `ReleaseBody` is a later slice if the leak matters.
- The F1 readout gains one line: body count, total body triangles, last generation time in ms
  — three `Hud::Frame::stats` fields.
- Remove slice 3's placeholder block entirely.

### 2.5 `Design/Decisions/00NN-bodies-are-presentation.md`

Next free number (0011 is the explosion's). In the README's format, indexed. **Context**: the
first planet arrives in a game whose simulation is a plane of ships; nothing asks the
simulation to know about bodies; the wire carries ships only. **Decision**: a planet or an
asteroid is presentation, placed by the composition root from a seed, drawn by the client, and
absent from `GameLogic` and the snapshot. **Alternatives**: a `Body` entity in `GameLogic` with
a snapshot record (a wire change and a determinism surface for something no rule reads); an
immovable `HullSpec` row per asteroid (the route in when collision is wanted — recorded as such,
not lost). **Consequences**: two clients agree on a body only by agreeing on a seed, so the day
bodies matter to play the seed goes on the wire before the body does; a ship flies through a
rock until then.

### 2.6 Project files and documents

`Outpost.vcxproj` and `.filters` gain `BodyCatalogue.h/.cpp`. No asset change: the ramps and
the outline are `<Image>` items already. `Design/PlanetRenderer.md` §15 marks slices 1–4
`landed`; `AGENTS.md`'s "What is actually here" paragraph gains a clause — "procedurally
generated planets and asteroids as presentation" — because the sentence describing the game
would otherwise be false (AGENTS.md §14's own rule).

---

## 3. Out of scope

- **Any `GameLogic` change.** No entity, no wire record, no collision. `GameLogicTests` is not
  touched.
- **The ocean.** `wet` classes are generated with `outsideHeight = BODY_PLANET_OUTSIDE_HEIGHT_WET`
  and draw their sea floor as terrain until slice 5.
- **Releasing a body's buffer.** F5 leaks by design (§2.4); a `ReleaseBody` is a slice when a
  body first has to go away in play.
- **Ramps beyond the eight.** A converted ramp is a file and a row; not here.
- **Tuning beyond making the scene read.** The ranges land as the design gives them; the
  owner tunes with F5 the day they look. The pull request notes what looked wrong and does not
  silently move a constant.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `GameLogic/HullSpec.h:19–115` | The table pattern: enum, `COUNT`, annotated `constexpr` array, `static_assert`, clamping accessor |
| `Outpost/OutpostApp.cpp:13–15, 36–104, 107–125, 129–158` | The directory constants, boot order, `SpawnStartingFleet`, the key switch |
| `Outpost/WorldView.cpp:206–251` | `UpdateFeedback` and the marker age loop — the model for spin and tumble |
| `Outpost/WorldView.cpp:507–572` | `Render`: where the body loops go, `ViewX/ViewZ` |
| `Outpost/ViewTuning.h:73–90` | The "world look" section, the shape for the new one |
| `Outpost/Hud.cpp`, `Hud.h:54` | The F1 readout and `Frame::stats` |
| `NeuronClient/BodyRenderer.h`, `BodyMeshBuilder.h`, `BodyField.h`, `ColourRamp.h`, `BodyDesc.h` (slices 1–3) | The whole engine surface this slice calls |
| `Design/SpaceshipExplosion-slice-4.md` §2.3 | The tumbler update line and `XMFLOAT3X3` storage |
| `Design/Decisions/README.md` | The record template and the index |

---

## 5. What will surprise the implementer

### 5.1 The catalogue's file names are `wchar_t*` in a `constexpr` table

`HullSpec` holds numbers; this table holds a file name, which is content the executable may name
(AGENTS.md §2, `TextRenderer.h:15–17`). `const wchar_t*` in a `constexpr` aggregate is fine;
`std::wstring` is not. Build the path at load with `TERRAIN_DIR + spec.ramp`.

### 5.2 Generation happens before the first frame and takes real time

Eight bodies at boot is a few hundred milliseconds in Debug. The window is shown after `Init`
returns (`m_window.Show()` is last), so the user sees a delay, not a black frame. State the
measured boot delta in the pull request; if it exceeds a second in Release, slice 6's trigger is
met and the pull request says so.

### 5.3 A planet at 4 km is inside `GRID_FADE_DISTANCE`'s shadow, not the grid's

The grid fades by 900 m; the ground quad still extends 2 km from the camera target and follows
it. A planet's lower limb at `R × (LIFT − 1) = 0.15 R` above the plane clears the quad. If a
planet is placed with `LIFT < 1` the quad slices it: `BODY_PLANET_LIFT` is `≥ 1` and the
comment says why.

### 5.4 Asteroids rest on the plane and the ships hover 4 m above it

An asteroid of radius 15 m centred at `y = 15` has its equator at hull height. A Corvette
ordered through one flies through it (design §14, decision 2). That is expected; it is in the
screenshot; nobody files it as a bug.

### 5.5 F5 and the depth of the reseed

`BODY_START_SEED + n` is a different seed each press, and after a restart the first press is
`+ 1` again — so a scene is reproducible by *press count*, which is what the acceptance below
relies on.

---

## 6. Decision records due

One, §2.5. **If the implementer finds bodies wanting simulation time** or a place on the wire
while building this, that is not a second record here — it is a note in the pull request for a
`GameLogic` design to pick up.

---

## 7. Acceptance

**Screenshots at two window sizes (AGENTS.md §7), from `BODY_START_SEED` at boot:**

- The default view: the fleet, six asteroids among and around it, the terran planet on the
  north-east horizon with white poles and green-to-grey continents, the barren moon north-west.
- Zoomed to 40 m on an asteroid: craters visible as bowls, grey ramp, the outline glowing on
  the near facets; tumbling between two frames five seconds apart.
- Zoomed to 900 m looking at the planet: no outline sparkle; the planet visibly rotated between
  two frames sixty seconds apart.
- F5 pressed once: a different scene; restart and F5 once: the same different scene (press
  count reproducibility, §5.5).
- The F1 readout shows body count 8, triangle total, and generation time.
- A selected, moving ship with rings, markers, thrusters and HUD unchanged from before this
  slice.

**Measured, stated:** boot time before and after this slice, Debug and Release; frame time at
the default view with and without bodies (bodies off by setting `BODY_START_ASTEROIDS = 0` and
skipping the two planets locally; not committed).

**The tree:**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds and runs; all four suites pass unchanged — `GameLogicTests` in particular.
- `git diff --stat` shows no file under `GameLogic/`, `NeuronCore/`, `NeuronClient/`,
  `NeuronServer/`.
- The decision record exists and the index lists it.
- `Design/PlanetRenderer.md` §15 marks slice 4 `landed`; this file and slices 1–3's move to
  `Design/Archive/` if they have not already.
- `AGENTS.md`'s "What is actually here" updated (§2.6).

---

## 8. Assumptions the implementer may make

- **F5 is the key** — settled with the owner (design §14).
- **F5 leaks the previous bodies' buffers** (§2.4); acceptable for a debug key and stated.
- **The six ramps in the catalogue are the six the classes need**; `LandscapeLaunchpad` and
  `LandscapeContainment` stay on disk and unreferenced (design §6.2).
- **Bodies age on real time**, like every other feedback in `WorldView`, unaffected by keys 1/2/3.
- **The starting scene's bearings and radii are whatever `Pcg32(BODY_START_SEED)` gives**; the
  implementer does not hand-place a body to improve the screenshot.
- **`BODY_OVERLAY` is a `constexpr` aggregate of an engine type**, like `Camera::Desc` values in
  the same file; the engine type has no constructor, so that compiles.
