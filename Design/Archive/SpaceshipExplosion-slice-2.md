# Work order — Spaceship explosion slice 2: the effect simulation

Implements slice 2 of [`SpaceshipExplosion.md`](SpaceshipExplosion.md) §14: `FxVertex`,
`MeshShatter` and `SpriteParticles` — everything the effect computes, with no device in reach, so
every rule in §5–§7 of the design is a test.

**Layer:** `NeuronClient` and `NeuronClientTests`.
**Depends on:** slice 1 (`Pcg32`).
**Blocks:** slice 3, which draws what this builds.

---

## 1. What this slice is for

The effect has two halves: the arithmetic that moves fragments and particles and turns them into
vertices, and the D3D12 that draws the vertices. The first half is where every number in the
design lives and where every subtle rule — the shared tumblers, the deferred emission, the
darkening alpha — can be got wrong quietly. So it lands alone, decided by tests, before a single
pipeline exists. `Camera`, `ObjParser` and `DdsImage::Parse` are already in `NeuronClientTests`
on exactly these terms (`Tests/NeuronClientTests/pch.h`: the library under test and its umbrella,
nothing more).

Nothing in this slice includes `<d3d12.h>`. `FxRenderer` is slice 3.

---

## 2. Scope

### 2.1 `NeuronClient/FxVertex.h`

```cpp
struct FxVertex
{
  float px, py, pz;   // world position
  float nx, ny, nz;   // world normal; a sprite writes zero and the sprite shader never reads it
  float r, g, b, a;   // colour and alpha already curved on the CPU; the shaders do no fading
  float u, v;
};
static_assert(sizeof(FxVertex) == 48);
```

A public aggregate, so plain `camelCase` fields (AGENTS.md R8), like `MeshVertex` and
`TextVertex`. The `static_assert` is there because slice 3's input layout spells the offsets.

### 2.2 `NeuronClient/MeshShatter.h/.cpp`

One shattered mesh. Design §5 is the specification; this section is the interface.

```cpp
class MeshShatter
{
public:
  static constexpr int TUMBLER_COUNT = 5;

  struct Desc
  {
    float lifetimeSec = 5.0f;
    float radialSpeedPerSec = 3.0f;        // vel = (centroid - centre) * this
    float maxAngVelRadPerSec = 4.0f;       // per axis, uniform in +-
    float frictionCoef = 0.05f;
    float rotFrictionCoef = 0.2f;
    float minCircumferenceMetres = 6.0f;   // smaller triangles are skipped
    float fraction = 1.0f;                 // keep each triangle with this probability
    std::uint32_t maxFragments = 500;
    DirectX::XMFLOAT3 gravityMetresPerSec2{0.0f, 0.0f, 0.0f};
    // Fragment colour = lerp(tintColour, vertexColour, tintMix). The defaults leave the vertex
    // colour untouched; slice 4 passes SHIP_COLOUR / SHIP_MATERIAL_MIX so a shard is the colour
    // the hull was drawn in (design 13).
    DirectX::XMFLOAT3 tintColour{0.0f, 0.0f, 0.0f};
    float tintMix = 1.0f;
  };

  // Returns how many triangles were dropped at maxFragments, so the caller can trace it.
  std::uint32_t Spawn(const MeshData& _mesh, const DirectX::XMFLOAT4X4& _world, const DirectX::XMFLOAT3& _inheritedVelMetresPerSec,
                      const Desc& _desc, Pcg32& _rng);

  // True once the shatter has reached its lifetime and should be dropped.
  bool Advance(float _dtSec);

  void Build(std::vector<FxVertex>& _out) const;

  [[nodiscard]] std::uint32_t FragmentCount() const noexcept;
  [[nodiscard]] float AgeSec() const noexcept;
  [[nodiscard]] const Tumbler& TumblerAt(int _index) const noexcept;    // tests read the rotation
  [[nodiscard]] const Fragment& FragmentAt(std::uint32_t _index) const noexcept;
  ...
};
```

`Tumbler` and `Fragment` are the records in design §5.1, public nested structs of `MeshShatter`
(they are the class's own state, exposed read-only for tests). The `Desc` is the default
source values; slice 4 fills it from `ViewTuning.h`.

Rules to get exactly right, because a test checks each:

- **The mesh centre is `_mesh.BoundsCentre()` transformed by `_world`**, not the origin. A hull's
  origin sits at its base (`MeshData::RestY`), and shattering about it would throw the whole
  wreck upward.
- **Triangle rejection**: two coincident vertices, or `|p0−p1| + |p1−p2| + |p2−p0| <
  minCircumferenceMetres`. The circumference is measured **after** the world transform, so a
  scaled hull is measured in metres.
- **`fraction < 1` skips triangles with `rng.Float01() >= fraction`** before the cap is counted,
  so `fraction = 0.5` on a 1000-triangle mesh yields about 500 fragments and drops none at the
  cap.
- **The cap is `maxFragments` kept fragments**, and the return value is the number of triangles
  that passed rejection but arrived after the cap.
- **Tumblers are seeded from `_rng` at spawn**, each axis `rng.Signed(maxAngVelRadPerSec)`; the
  rotation is identity. The fragment's tumbler is `rng.Below(TUMBLER_COUNT)`.
- **Advance** is design §5.3 verbatim. The rotation update is `rot = rot ×
  XMMatrixRotationRollPitchYaw(angVel.x·dt, angVel.y·dt, angVel.z·dt)`, computed in `XMMATRIX`
  locals and stored back to `XMFLOAT3X3` — never a stored `XMVECTOR`/`XMMATRIX`.
- **Build** emits three vertices per fragment with `uv = (0,0), (0,1), (1,1)`, normal
  `rot × normal`, colour from the fragment, and `a = 1 − ageSec / lifetimeSec` clamped to
  `[0, 1]`. No allocation inside the loop; the caller's vector is `reserve`d by
  `3 × FragmentCount()` before the loop.

### 2.3 `NeuronClient/SpriteParticles.h/.cpp`

The fixed-capacity billboard pool. Design §6 and §7 are the specification.

```cpp
enum class SpriteType : std::uint8_t { Core, Debris, Smoke };
enum class SpriteBlend : std::uint8_t { Dark, Additive };

struct SpriteTypeSpec
{
  float lifeSec;
  float size;                    // quad half-size is size / 16
  float friction;
  DirectX::XMFLOAT3 colourA;     // 0..1
  DirectX::XMFLOAT3 colourB;
  SpriteBlend blend;
};
inline constexpr SpriteTypeSpec SPRITE_TYPES[] = { /* design §6.1, colours / 255 */ };

class SpriteParticles
{
public:
  static constexpr float SMOKE_RATE_PER_SEC = 5.0f;
  static constexpr float PEAK_ALPHA = 90.0f / 255.0f;
  static constexpr float FADE_START_FRACTION = 0.75f;

  struct Desc { std::uint32_t capacity = 4096; };

  void Init(const Desc& _desc);
  void Emit(SpriteType _type, const DirectX::XMFLOAT3& _pos, const DirectX::XMFLOAT3& _velMetresPerSec, float _size, Pcg32& _rng);
  void Advance(float _dtSec, Pcg32& _rng);
  void Build(SpriteBlend _blend, const DirectX::XMFLOAT3& _cameraRight, const DirectX::XMFLOAT3& _cameraUp, std::vector<FxVertex>& _out) const;
  void Clear() noexcept;

  [[nodiscard]] std::uint32_t Count() const noexcept;
  [[nodiscard]] std::uint32_t Dropped() const noexcept;      // emissions refused since Init or Clear
  [[nodiscard]] const Particle& At(std::uint32_t _index) const noexcept;
  ...
};
```

Rules a test checks:

- **Emission picks the colour once**: `lerp(colourA, colourB, rng.Float01())`, stored, never
  updated.
- **Capacity is a hard cap**: `Emit` into a full pool increments `Dropped()` and returns; nothing
  is overwritten and nothing grows.
- **Emissions during `Advance` are deferred**: a `Debris` particle's smoke goes into a member
  scratch vector and is appended after the sweep. The scratch is a member, not a local, so a
  frame allocates nothing once warm.
- **Death is swap-remove**, so the pool stays dense and order is not promised.
- **The alpha curve** is `PEAK_ALPHA` for `age < 0.75·life`, then linear to 0 at `life`.
- **`Build(Dark, …)`** writes `rgb = colour × (alpha / PEAK_ALPHA)`, `a = 0`.
  **`Build(Additive, …)`** writes `rgb = colour`, `a = alpha`. Design §7 says why the dark pass
  carries zero alpha; the comment at the site says so too, because someone will try to fix it.
- **Corners** are `pos + half × (−up)`, `(+right)`, `(+up)`, `(−right)` in that order, with
  `uv = (0,0), (1,0), (1,1), (0,1)`, split into two triangles `(0,1,2)`, `(0,2,3)`.
  `half = size / 16`.
- **`Build` only emits particles whose type's blend matches `_blend`**, so the renderer makes two
  calls and gets two disjoint sets.

### 2.4 The umbrella and the project files

`NeuronClient.h` includes `FxVertex.h`, `MeshShatter.h`, `SpriteParticles.h` after `MeshData.h`
(they consume it). `MeshShatter.h` includes `MeshData.h`, `FxVertex.h` and `Pcg32.h` itself
(AGENTS.md §3: a header includes what it declares members of). `NeuronClient.vcxproj` and
`.filters` gain the five files under the `Render` filter; `NeuronClientTests.vcxproj` and
`.filters` gain `MeshShatterTests.cpp` and `SpriteParticlesTests.cpp`.

---

## 3. Out of scope

- **Anything that touches the GPU.** No PSO, no buffer, no texture, no `<d3d12.h>` include in any
  file this slice adds. If `FxVertex` turns out to want a `D3D12_INPUT_ELEMENT_DESC` beside it,
  that goes in slice 3's `FxRenderer.cpp`, not here.
- **The recipe.** No "three copies", no core counts, no ring of debris. Those are Outpost
  (slice 4). This slice provides `Spawn` and `Emit` and nothing that knows what a ship is.
- **Tuning constants in `ViewTuning.h`.** The `Desc` defaults carry the source values so the
  types are usable alone; the named constants arrive in slice 4.
- **Frustum culling, sorting, LOD.** Design §12.
- **`MeshData` changes.** It already has everything the shatter reads.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronClient/MeshData.h` | `MeshVertex` (position + colour, three per triangle), `BoundsCentre()`, `HalfExtents()` |
| `NeuronCore/Pcg32.h` (slice 1) | `Below`, `Float01`, `Signed` |
| `NeuronCore/Ease.h` | The house style for small numeric utilities |
| `NeuronClient/Camera.h` | `Right()` / `Up()` — the vectors `Build` will be handed |
| `Tests/NeuronClientTests/ObjParserTests.cpp` | A test that builds a `MeshData` by hand and asserts on it |
| Design §5–§7 | The algorithms, the constants, and the one blend rule that must not be "fixed" |
| `InterstellarOutpost.dx12/GameRenderer/explosion.cpp`, `particle_system.cpp` | The originals, for a second opinion on any step the design leaves ambiguous |

---

## 5. What will surprise the implementer

### 5.1 A hull's bounds centre is not its origin

`ObjParser` leaves the mesh where the file put it, and `WorldView` lifts it by `RestY()` so the
lowest vertex sits on the ground. Shatter about the origin and every fragment gets a velocity with
an upward component it should not have. Use `BoundsCentre()`, transformed by the same `_world`
as the vertices.

### 5.2 `XMFLOAT3X3` rotation order

`rot = rot × delta`, post-multiplied, so an accumulated tumble keeps spinning about the same
body axes. Pre-multiplying spins about world axes and the fragments visibly precess. The test in
§7 pins one case.

### 5.3 The cap counts kept fragments, not visited triangles

The source's `if (j > 500) break;` counts *kept* fragments after rejection. A Structure with
1784 faces and `fraction = 1` yields 500 fragments and reports 1284 dropped, assuming none are
rejected for size. Slice 4 traces the number; make it correct here.

### 5.4 The dark pass really carries zero alpha

A reviewer reading `a = 0` on a visible sprite will call it a bug. The comment at the site cites
design §7 and says "the destination term is what draws; the source term is meant to vanish".

---

## 6. Decision records due

None. No type moves, no rule changes, no dependency is added. `NeuronClient` gaining three
headers that consume `MeshData` and `Pcg32` is what the library is for.

---

## 7. Acceptance

**`MeshShatterTests.cpp`** — every test builds its `MeshData` by hand; none loads a file:

- **One triangle, identity transform**: a mesh of one triangle at `(0,0,0) (2,0,0) (0,2,0)` with
  `boundsMin/Max` set around it gives one fragment; `pos` equals the centroid `(2/3, 2/3, 0)`
  within `1e-5`; `v0 + v1 + v2 == 0` within `1e-5`; the normal has unit length; `Build` emits
  exactly 3 vertices with `uv` `(0,0) (0,1) (1,1)` in order and `a == 1` at age 0.
- **Radial velocity**: with `radialSpeedPerSec = 3`, the same triangle with bounds centre at
  `(0,0,0)` gets `vel == centroid × 3 + inheritedVel` within `1e-5`, for a non-zero
  `inheritedVel`.
- **The world transform applies to the centre too**: translate the mesh by `(100, 0, 0)` through
  `_world` and the fragment velocity is unchanged from the untranslated case.
- **Degenerate and small triangles are skipped**: a triangle with two equal vertices, and a
  triangle with circumference 5.9 at `minCircumferenceMetres = 6`, each give 0 fragments; the
  same small triangle at `minCircumferenceMetres = 5` gives 1.
- **The cap and the drop count**: 20 valid triangles with `maxFragments = 5` give
  `FragmentCount() == 5` and `Spawn` returns 15.
- **`fraction`**: 2000 valid triangles at `fraction = 0.5` and `maxFragments = 100000` give
  between 900 and 1100 fragments; `Spawn` returns 0.
- **Every fragment's tumbler index is below `TUMBLER_COUNT`**, over 1000 fragments.
- **Tint**: a triangle with vertex colour `(1, 0, 0)` and the default `Desc` builds
  `rgb == (1, 0, 0)`; with `tintColour = (0, 0, 1)`, `tintMix = 0.25` it builds
  `(0.25, 0, 0.75)` within `1e-5` — `lerp(tint, vertex, mix)`, the same operand order as
  `ScenePS`'s `lerp(baseColour, col, baseColour.w)`.
- **Tumble is post-multiplied**: a single tumbler with `angVel = (0, π/2, 0)` advanced by `dt = 1`
  rotates a fragment's `v0 = (1, 0, 0)` to `(0, 0, −1)` within `1e-4` in the built vertex
  (`XMMatrixRotationRollPitchYaw` about Y, LH). Advance again by 1 s: `(−1, 0, 0)` — post-
  multiplication keeps composing about the same axis.
- **Friction slows, and never reverses**: a fragment with `|vel| = 100` at `frictionCoef = 0.05`,
  advanced by `dt = 1`, has `|vel| < 100` and the same direction; advanced with `dt = 100` the
  friction clamps at 1 and `vel == 0`, not negative.
- **Angular friction**: `angVel` shrinks by exactly `1 − 0.2·dt` per step.
- **Lifetime**: `Advance` returns false at `4.99 s` and true at `5.0 s` cumulative; at `2.5 s`
  `Build` writes `a == 0.5` within `1e-5`.
- **Gravity**: with `gravityMetresPerSec2 = (0, −10, 0)` a resting fragment gains `vel.y == −10`
  after `dt = 1`; with the default zero it stays at rest.
- **Determinism**: two `Spawn`s of the same mesh with `Pcg32(7)` each produce fragments whose
  `tumbler` indices and tumbler `angVel`s compare bit-identical.
- **No per-frame allocation**: a code read, stated in the pull request — `Advance` allocates
  nothing; `Build` reserves once before its loop.

**`SpriteParticlesTests.cpp`**:

- **Emit and count**: `Init({capacity = 8})`, 8 emits give `Count() == 8`, `Dropped() == 0`; a
  ninth gives `Count() == 8`, `Dropped() == 1`.
- **Colour is fixed at emission**: emit one `Core`, record `At(0).colour`, advance 1 s, the
  colour is unchanged and lies component-wise between `colourA` and `colourB`.
- **Life and death**: a `Core` (2 s) is alive after `1.99 s` of cumulative `Advance` and gone at
  `2.0 s`; `Count()` falls to 0.
- **Friction**: a `Core` at `vel = (10,0,0)` after `dt = 1` has `vel.x == 10 × (1 − 0.2)`; a
  `Smoke` (friction 0) keeps `vel` exactly.
- **Debris smokes at the rate**: one `Debris` particle, `Advance(1/60)` 300 times (5 s) with a
  fixed seed, yields between 15 and 35 `Smoke` particles born (count `Smoke` in the pool plus
  any that died — or emit with capacity 64 and assert `Count()` in `[16, 36]` including the
  debris). Each smoke's `vel` is the debris's `vel / 5` and its `size` is `debrisSize / 1.5`
  within `1e-4`.
- **Deferred emission does not disturb the sweep**: with capacity 4 and one `Debris`, advance
  until dropped smoke is counted; `Count()` never exceeds 4 and no particle is overwritten
  (the debris is still `Debris` at index 0 or wherever swap-remove put it, by type count).
- **Alpha curve**: `Core` at age 0 builds `a == 90/255`; at `1.5 s` (75 %) still `90/255`; at
  `1.75 s` (87.5 %) `45/255` within `1e-5`; at `1.999 s` near 0.
- **The dark pass carries zero alpha and scaled colour**: emit one `Smoke`, build with
  `Blend::Dark`: six vertices, every `a == 0`, every `rgb == colour` at age 0 (alpha fraction 1);
  at 95 % of life `rgb == colour × 0.2` within `1e-4`. Build with `Blend::Additive`: zero
  vertices.
- **The additive pass**: emit one `Core`, build `Additive`: six vertices, `a == 90/255`,
  `rgb == colour`. Build `Dark`: zero vertices.
- **Billboard corners**: with `right = (1,0,0)`, `up = (0,1,0)`, `pos = (0,0,0)`, `size = 16`
  (`half = 1`), the six vertices are, in order, `(0,−1,0) (1,0,0) (0,1,0)` and `(0,−1,0) (0,1,0)
  (−1,0,0)` — the 45° diamond — with `uv` `(0,0) (1,0) (1,1)` and `(0,0) (1,1) (0,1)`.
- **`Clear` empties and resets `Dropped()`.**
- **Determinism**: two pools fed the same emits and advances with `Pcg32(3)` each build
  bit-identical vertex lists.

**The tree:**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; `NeuronClientTests` runs, every existing test still passes, and the pull
  request states the new count.
- No screenshot: nothing draws yet, and the work order says so rather than asking for a picture
  of nothing.
- No `GameLogic` file is touched, so the replay test is not in question — say so.

---

## 8. Assumptions the implementer may make

- **Source constants are metres and seconds 1:1** (design §3). The `Desc` defaults are the
  source's numbers unconverted.
- **`XMMatrixRotationRollPitchYaw(x, y, z)` is the port of the original `Matrix33(ax·dt, ay·dt,
  az·dt)`.** The original's Euler order is not the point; the test pins the LH single-axis case
  and the rest is a look, tuned in slice 4.
- **Particle `size` follows the source's convention** (half-size = `size / 16`), so the `Desc`
  and the table read like the original and the tuning file in slice 4 can be checked against it
  line by line.
- **The `SpriteType` table is `constexpr` data in the header.** Loading it from a file is not on
  any roadmap (AGENTS.md: no configuration file).
- **Tests may construct a `MeshData` by hand** and set `boundsMin`/`boundsMax` directly; the
  parser is not involved.
