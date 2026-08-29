# Work order — Planets and asteroids slice 2: the mesh

Implements slice 2 of [`PlanetRenderer.md`](PlanetRenderer.md) §15: `ColourRamp` and
`BodyMeshBuilder` — the field of slice 1 turned into an `FxVertex` list with one normal and one
colour per triangle, still with no device in reach, so every rule in §6 and §8.2 of the design
is a test.

**Layer:** `NeuronClient` and `NeuronClientTests`.
**Depends on:** slice 1 (`BodyField`); `SpaceshipExplosion` slice 2 for `FxVertex.h`, or this
slice lands it (§2.1).
**Blocks:** slice 3, which draws what this builds.

---

## 1. Why this is a slice

Colour and geometry are where the source's look actually lives — the amplitude law is what the
terrain *is*, but `GetLandscapeColour` with its slope axis and its dither is what it *looks like*
— and both are pure CPU work that a test can pin. Splitting them from the renderer means the
first screenshot in slice 3 is a screenshot of a vertex list that is already known to be right
by construction: outward normals, integral uvs, the pinned hash. Anything wrong on that screen is
a shader or a pipeline, and the search space is one file.

The ocean rules are **not** here (slice 5): a dry body lands and is looked at first.

---

## 2. Scope

### 2.1 `NeuronClient/FxVertex.h`

If `SpaceshipExplosion` slice 2 has landed, nothing. If not, this slice lands the file **exactly
as `SpaceshipExplosion-slice-2.md` §2.1 specifies it** — same fields, same order, the same
`static_assert(sizeof(FxVertex) == 48)` — and that slice finds it in place. Two designs, one
vertex; the comment in the header names both.

### 2.2 `NeuronClient/ColourRamp.h/.cpp`

```cpp
class ColourRamp
{
public:
  static constexpr std::uint32_t SIDE = 64;

  // Reads _fileName through DdsImage; reports false and traces on anything that is not a 64x64
  // 8-bpc surface. A ramp that fails to load leaves the object unloaded, not half-filled.
  [[nodiscard]] static bool Load(const std::wstring& _fileName, ColourRamp& _outRamp);
  [[nodiscard]] bool FromImage(const DdsImage& _image);

  // Bilinear, clamped. u: 0 flat .. 1 cliff (columns); v: 0 summit .. 1 sea level (rows).
  [[nodiscard]] DirectX::XMFLOAT3 Sample(float _u, float _v) const noexcept;
  [[nodiscard]] bool Loaded() const noexcept;

private:
  float m_rgb[SIDE * SIDE * 3] = {};
  bool m_loaded = false;
};
```

Rules:

- **The ramp is a lookup table, never a texture.** Nothing in this class or its callers uploads
  it; design §6.1.
- **`v = 0` is row 0 and row 0 is the summit** — inspected on every ramp in the tree (design
  §6.1). A test on a synthetic image pins the orientation so a future ramp authored upside down
  is caught by the sampler's test, not by a planet with white beaches.
- **BGRA on disk, RGB in memory**: `TopMipAsBgra` gives `B G R A`; store `R G B` as floats in
  `[0, 1]`.

### 2.3 `NeuronClient/BodyMeshBuilder.h/.cpp`

```cpp
struct BodyBuildStats
{
  std::uint32_t trianglesEmitted = 0;
  std::uint32_t trianglesCulled = 0;   // slice 5; always 0 in this slice
  float maxHeightMetres = 0.0f;
};

class BodyMeshBuilder
{
public:
  // Appends the terrain of _field to _outTerrain: three FxVertex per triangle, one normal and one
  // colour per triangle, uv = grid cell. _ramp may be null, in which case every triangle is
  // BODY_FALLBACK_GREY and one trace says so. Design 8.2.
  static void Build(const BodyField& _field, const ColourRamp* _ramp, std::vector<FxVertex>& _outTerrain, BodyBuildStats& _outStats);

  // The integer hash that seeds one triangle's dither: design 6.1. Public so the test and, later,
  // a shader port can pin it.
  [[nodiscard]] static constexpr std::uint32_t CellHash(std::uint64_t _seed, std::uint32_t _face, std::uint32_t _x, std::uint32_t _z) noexcept;
};
```

Rules, each a test in §5:

- **Sample first, then emit.** Six `N×N` height arrays are filled from `BodyField::Height` over
  `CubeSphere::Direction`, and a `DEBUG_ASSERT` checks that every shared edge sample is equal
  across the two faces — the design's seam guarantee, asserted where a bug would first appear.
- **Per cell, two triangles**, `(x,z) (x,z+1) (x+1,z+1)` and `(x,z) (x+1,z+1) (x+1,z)`, from the
  four corner positions `P = d ⊙ ellipsoid · (R + h)`.
- **Normal from the cross product, flipped to point outward**: `if (dot(n, dCentroid) < 0) n = −n`.
  Written to all three vertices. This is what lets `BodyPS` skip the eye-facing test `ScenePS`
  needs (design §7.2).
- **Colour per triangle**, design §6.1: `gradient = dot(n, dCentroid)`, `u = pow(1 − gradient,
  0.4)`, `climate = field.Climate(dCentroid, hCentroid)`, `v = 1 − climate / maxHeight`,
  dither `v += rng.Signed(0.45 / (climateSourceUnits + 2))` with `rng = Pcg32(CellHash(...))`
  and `climateSourceUnits = climate / R · 2000` (the source-unit rule slice 1 named). `a = 1`.
- **`CellHash` is a PCG-style integer mix**: fold `_seed` to 32 bits by xor of its halves, then
  `h = seed; h ^= face * 0x9E3779B9; h = (h ^ (h >> 16)) * 0x85EBCA6B; h ^= x * 0xC2B2AE35; h = (h ^ (h >> 13)) * 0x27D4EB2F; h ^= z; h ^= h >> 16`.
  Integer only, so HLSL reproduces it bit for bit (design §17.3). The test pins one value.
- **uv** is the cell's corner: `(x, z)`, `(x, z + 1)`, `(x + 1, z + 1)`, and so on — integral at
  corners, one outline tile per cell.
- **`maxHeight`** is `field.MaxHeight()`; `_outStats.maxHeightMetres` reports it.
- **No ocean logic.** No culling, no shore dip; a body with `outsideHeight < 0` simply has its
  floor drawn below `R`. Slice 5 adds both behind that sign.
- **`BODY_FALLBACK_GREY`** is a `static constexpr` on the builder, `{0.5f, 0.5f, 0.5f}`, not a
  `ViewTuning` constant: it is what the engine draws when the game gave it nothing, and the
  engine may name that.
- **One reserve, no reallocation**: `_outTerrain.reserve(current + 6·cells·2·3)` before the
  loop. A test does not check it; a code read does.

### 2.4 The umbrella and the project files

`NeuronClient.h` includes `FxVertex.h` (if new), `ColourRamp.h`, `BodyMeshBuilder.h` after
`BodyField.h`. Project and filters gain the files (`Body` filter); the test project gains the two
test files.

---

## 3. Out of scope

- **Anything with a device.** No upload, no PSO, no shader. Slice 3.
- **The ocean sphere, sea-level culling, the shore dip.** Slice 5, behind `outsideHeight < 0`.
- **Decals, tint overrides, LOD.** Design §13.
- **A generic mesh utility.** `BodyMeshBuilder` builds bodies; it does not become the second
  `MeshData` producer or gain an `.obj` writer.
- **Choosing a ramp.** The builder takes a pointer; which file it came from is slice 4's.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronClient/BodyField.h`, `CubeSphere.h`, `BodyParams.h` (slice 1) | `Height`, `Climate`, `MaxHeight`, `Direction`, `SamplesPerSide` |
| `NeuronClient/DdsImage.h/.cpp` | `Load`, `TopMipAsBgra`, `widthPx`/`heightPx` — the ramp reader is thirty lines over this |
| `NeuronClient/ObjParser.cpp` | A content reader that reports and fails closed; the trace idiom (`Trace`/`Debug.h`) |
| `NeuronClient/MeshData.h` | The bounds and centroid helpers; a body needs neither but the style is the model |
| `NeuronCore/Pcg32.h` | `Signed` for the dither |
| `Tests/NeuronClientTests/DdsImageTests.cpp` | Building a synthetic DDS in memory (`HEADER_FLAGS_BASIC`, `FORMAT_FLAGS_RGBA`, the offsets) — the ramp test builds its image the same way and calls `FromImage` |
| `Design/PlanetRenderer.md` §6.1, §8.2, §17.3 | The lookup, the build steps, the integer-hash rule |

---

## 5. Acceptance

**`ColourRampTests.cpp`**

- **Corners come back.** A synthetic 64×64 with red at `(0,0)`, green at `(63,0)`, blue at
  `(0,63)`, white at `(63,63)`: `Sample(0,0)`, `(1,0)`, `(0,1)`, `(1,1)` return those within
  `1/255`.
- **Row 0 is the summit.** The same image: `Sample(0, 0)` is red and `Sample(0, 1)` is blue —
  the orientation, named.
- **Bilinear midpoint.** `Sample(0.5, 0)` on a row that runs red→green is `(0.5, 0.5, 0)` within
  `1/255`.
- **Clamped.** `Sample(−1, 2)` equals `Sample(0, 1)`.
- **Wrong size fails closed.** A 32×32 image: `FromImage` returns false, `Loaded()` is false.

**`BodyMeshTests.cpp`**, on the slice 1 test desc (`R = 1000`) with `gridPower = 3` and a
synthetic ramp:

- **Count.** `trianglesEmitted == 6·8·8·2 == 768` and `outTerrain.size() == 2304`.
- **Every normal faces outward.** For every triangle, `dot(n, normalize(centroid)) > 0`.
- **Every normal is the triangle's.** `|n − normalize(cross(p1 − p0, p2 − p0))| < 1e-4` up to sign.
- **uv is the cell.** The first triangle's uvs are `(0,0) (0,1) (1,1)`; every uv is integral.
- **The three vertices of a triangle share colour and normal.** Bitwise equal.
- **Flat is left, cliff is right.** A desc with `heightScale = 0` gives `u ≈ 0` for every
  triangle (all sample the ramp's left column); a desc with `heightScale = 0.4` gives at least
  one triangle with `u > 0.5`.
- **The dither is seeded.** `CellHash(0x1234, 2, 5, 7)` equals a pinned literal; the same desc
  built twice gives bitwise-equal vertex lists; changing `seed` changes at least one colour.
- **No ramp, grey.** `_ramp == nullptr`: every colour is `BODY_FALLBACK_GREY`.
- **Determinism of the whole.** A FNV-1a over the bytes of the vertex list equals a pinned
  literal, written by the implementer and explained in the pull request (the slice 1 rule).

A measurement, stated in the pull request: **the wall time of `Build` for `gridPower = 6`** in
Debug|x64 and Release|x64, on the implementer's machine, with the machine named. Design §8.4
estimates tens of milliseconds; if it is hundreds, slice 6's trigger is met earlier than planned
and the pull request says so.

The tree:

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; `NeuronClientTests` runs and passes, slice 1's tests included.
- No screenshot: nothing visual.
- Design §15 marks slice 2 `landed`; this file moves to `Design/Archive/`.

---

## 6. Assumptions the implementer may make

- **`FxVertex` is the 48-byte record in `SpaceshipExplosion-slice-2.md`** whether it arrived
  from there or from here.
- **Bilinear over nearest** for the ramp is settled (design §6.1); do not add a mode.
- **The dither strength constant `0.45`** and the `+ 2` are the source's and stay; the
  `2000` source-unit conversion is the one place the design adapted them, and it is a named
  constant shared with slice 1.
- **Winding is whatever the corner order in §2.3 gives**; nothing culls yet (design §7.2), so
  the outward normal is the only orientation that matters. Slice 3 measures whether the winding
  is consistent enough to turn back-face culling on.
- **The pinned literals** (`CellHash`, the FNV hash) are taken from the first correct build.
