# Work order — Planets and asteroids slice 1: the field

Implements slice 1 of [`PlanetRenderer.md`](PlanetRenderer.md) §15: the seeded height function on
a sphere — `Noise3`, `CubeSphere`, `BodyDesc` and `BodyField` — with no device in reach, so every
rule in §5 of the design is a test.

**Layer:** `NeuronClient` and `NeuronClientTests`.
**Depends on:** `SpaceshipExplosion` slice 1 (`NeuronCore/Pcg32.h`).
**Blocks:** slice 2, which turns the field into vertices.

---

## 1. Why this is a slice

The field is the half of the feature that can be *proved*: same seed, same height, on every
machine, forever — the guarantee that lets a server describe a world with sixteen bytes (design
§10). It has to exist and be pinned by tests before a mesh is built on it, because once vertices
are on screen every argument about the look becomes an argument about a screenshot, and a
screenshot cannot tell a noise bug from a colour bug. This slice lands nothing visible and that
is the point.

---

## 2. Scope

### 2.1 `NeuronClient/Noise3.h`

Header-only, namespace `Neuron`, Perlin-style gradient noise in three dimensions.

```cpp
class Noise3
{
public:
  static constexpr std::uint32_t PERMUTATION_SIZE = 256;

  // Shuffles the permutation from _rng, so the noise itself is seeded: two Noise3 built from
  // generators seeded alike are the same function.
  explicit Noise3(Pcg32& _rng) noexcept;

  // In [-0.5, 0.5]. Continuous, zero at integer lattice points.
  [[nodiscard]] float Sample(float _x, float _y, float _z) const noexcept;

  // The permutation, for a consumer that wants to carry the same function elsewhere (design 17).
  [[nodiscard]] std::span<const std::uint32_t, PERMUTATION_SIZE> Permutation() const noexcept;

private:
  std::uint32_t m_permutation[PERMUTATION_SIZE * 2]; // doubled so the wrap needs no masking in the inner loop
};
```

Rules:

- **Twelve gradient directions** — the edge midpoints of the cube, `(±1, ±1, 0)` and its
  permutations — selected by `hash & 15` with the four extras folded as the reference does.
  Quintic fade `6t⁵ − 15t⁴ + 10t³`.
- **The permutation is a Fisher–Yates shuffle of `0..255` driven by `_rng.Below(i + 1)`.** No
  static table: the tree's rule is that randomness is seeded, and a fixed Perlin table is an
  unseeded constant hiding in a header.
- **No `XM*Est`, no `/fp:fast`** (already banned solution-wide). The maths is scalar `float`;
  the samples per body are few enough that vectorising it is not this slice's problem.
- **Range is `[−0.5, 0.5]`** by scaling the reference's `[−1, 1]` output by `0.5` — the design's
  amplitude law (§5.2) was written against a diamond-square displacement of that range.

### 2.2 `NeuronClient/CubeSphere.h`

Header-only, `constexpr` throughout.

```cpp
enum class CubeFace : std::uint8_t { PosX, NegX, PosY, NegY, PosZ, NegZ };
inline constexpr std::uint32_t CUBE_FACE_COUNT = 6;

// N = 2^gridPower + 1 samples along each side of a face.
[[nodiscard]] constexpr std::uint32_t SamplesPerSide(std::uint32_t _gridPower) noexcept;

// The unit direction of sample (x, z) on a face, 0 <= x, z < SamplesPerSide. Equal-area warp
// (tan) on the two in-face coordinates, then normalised. An edge sample of one face is the same
// direction as the matching edge sample of its neighbour, to the bit -- that is what makes the
// height function seamless (Design/PlanetRenderer.md 5.1).
[[nodiscard]] constexpr DirectX::XMFLOAT3 Direction(CubeFace _face, std::uint32_t _x, std::uint32_t _z, std::uint32_t _samplesPerSide) noexcept;
```

`Direction` is written so that the bit-identity across faces holds by construction: the in-face
coordinate `s ∈ [−1, 1]` is computed as `(2·x − (n − 1)) / (n − 1)` in that order and the warp
`tan(s · π/4)` is applied to the *same* `s` whichever face asks. The test in §5 pins it; if it
fails, the expression order moved.

`constexpr` `tan` and `sqrt` are not in the standard library at this language level; the header
carries a small `constexpr` polynomial `tan` on `[−π/4, π/4]` and a Newton `sqrt`, both private
to the file, both `noexcept`, both exact enough that the `static_assert` in §5 holds. State the
polynomial's degree in a comment so nobody "improves" it into a different function.

### 2.3 `NeuronClient/BodyDesc.h`

The aggregate in design §8.1, plus the three records it holds. Plain `camelCase` fields (R8), no
methods beyond what brace-initialisation needs:

```cpp
enum class FlattenMode : std::uint8_t { Absolute, Add, Subtract, Subtract2, Smooth };

struct BodyTile
{
  DirectX::XMFLOAT3 centre{0.0f, 1.0f, 0.0f}; // unit direction
  float halfWidthRad = 1.0f;                  // angular radius of the cap
  float edgeFraction = 0.25f;                 // share of halfWidthRad over which the cap fades
  float desiredHeight = 0.05f;                // fraction of radius the tile's maximum is rescaled to
  float posY = 0.0f;                          // whole-tile lift, fraction of radius
  float fractalDimension = 0.8f, heightScale = 0.05f, lowlandSmoothing = 1.2f;
  bool ridged = false;
};

struct BodyFlatten
{
  DirectX::XMFLOAT3 centre{0.0f, 1.0f, 0.0f};
  float halfWidthRad = 0.1f;
  FlattenMode mode = FlattenMode::Subtract;
  float value = 0.02f;      // fraction of radius; the pad height, the bowl depth, the add
  float threshold = 0.0f;   // Subtract2: only where h > threshold
};

struct BodyDesc { /* design 8.1 verbatim */ };
```

Every length in a `BodyDesc` is a **fraction of `radiusMetres`** except `radiusMetres` itself.
That is the rule that lets one catalogue row describe a 400 m and a 1 200 m world; the comment
on the struct says so and R6 says the field names carry no unit because there is none.

### 2.4 `NeuronClient/BodyParams.h`

The flattened, fixed-capacity block `BodyField` evaluates from and a compute kernel would read
(design §8.1, §17.3):

```cpp
struct BodyParams
{
  static constexpr std::uint32_t MAX_TILES = 8;
  static constexpr std::uint32_t MAX_FLATTEN = 32;

  DirectX::XMFLOAT4 radiusEllipsoid;              // x radius, yzw ellipsoid  (one float4)
  DirectX::XMFLOAT4 seedOffset;                   // xyz noise offset, w lumpiness
  DirectX::XMFLOAT4 outsideMaxHeightGrid;         // x outsideHeight, y maxHeight, z gridPower, w octaves
  DirectX::XMFLOAT4 polar;                        // x strength, y capStart, z capNoise, w polarGeometry
  DirectX::XMFLOAT4 spinAxis;                     // xyz, w unused
  struct Tile { DirectX::XMFLOAT4 centreHalfWidth; DirectX::XMFLOAT4 edgeDesiredPosYRidged; DirectX::XMFLOAT4 fractal; } tiles[MAX_TILES];
  struct Flatten { DirectX::XMFLOAT4 centreHalfWidth; DirectX::XMFLOAT4 modeValueThreshold; } flatten[MAX_FLATTEN];
  std::uint32_t tileCount, flattenCount, pad0, pad1;
  std::uint32_t permutation[Noise3::PERMUTATION_SIZE];
};
static_assert(sizeof(BodyParams) % 16 == 0, "BodyParams is laid out for a constant buffer");
```

Every field is a `float4` or a `uint4` group: HLSL constant-buffer packing rules, obeyed now so
that slice 6 is a `memcpy`. A `BodyDesc` with more than `MAX_TILES` tiles or `MAX_FLATTEN`
areas is **clipped and traced**, never truncated silently.

### 2.5 `NeuronClient/BodyField.h/.cpp`

```cpp
class BodyField
{
public:
  // Draws every random number the body needs from Pcg32(_desc.seed) -- once, here -- and
  // flattens the description into BodyParams. After this, Height and Climate are pure.
  explicit BodyField(const BodyDesc& _desc);

  // Height above the ellipsoid surface at unit direction _d, in metres.
  [[nodiscard]] float Height(const DirectX::XMFLOAT3& _d) const noexcept;

  // Height plus the polar lift for direction _d (design 5.6), in metres. Colour uses this.
  [[nodiscard]] float Climate(const DirectX::XMFLOAT3& _d, float _height) const noexcept;

  // The field's maximum over the grid, found while constructing; or desc.maxHeight when set.
  [[nodiscard]] float MaxHeight() const noexcept;

  [[nodiscard]] const BodyParams& Params() const noexcept;

private:
  [[nodiscard]] float Octaves(const DirectX::XMFLOAT3& _d, const BodyParams::Tile& _tile) const noexcept;
  BodyParams m_params;
  Noise3 m_noise;
  float m_maxHeight = 0.0f;
  float m_tileScale[BodyParams::MAX_TILES]; // desiredHeight / (the tile's own maximum), found at construction
};
```

Rules, each of which is a test in §5:

- **Construction draws in a fixed order**: seed offset, then per tile in order (nothing — tile
  centres come from the desc, the catalogue drew them), then the noise permutation, then the
  per-tile maxima. Order matters because it is what determinism pins; a comment says so.
- **`Height` is design §5.2 and §5.3 in that order**: lumpiness octaves; then for each tile whose
  cap contains `d`, the tile's octaves scaled by `m_tileScale`, faded by `edgeFraction`, lifted by
  `posY`, max-merged; `outsideHeight` outside every tile; then the flatten areas in list order;
  then the polar geometry term if `polarGeometry > 0`. Multiply by `radiusMetres` last.
- **The tile maximum is measured, not guessed**: the constructor samples the tile's octaves over
  the grid at `gridPower` and keeps the largest, so `desiredHeight` means what it says. That is
  the reduction design §17.1 names; on the CPU it is a loop.
- **`MaxHeight` is the real maximum** of `Height` over the same grid, unless `desc.maxHeight`
  is non-zero.
- **The amplitude law's constants are the source's**, spelled once as `constexpr` at the top
  of the `.cpp` with the source expression in a comment: `30.7 · e^(−6.5·fd)`,
  `15.353 · e^(−3.1·smoothing)`, `len = 256 · 0.5^octave`, `pow(len · 10, fd)`,
  `0.1 + pow(|h|, smoothing) · 0.15`. The `|h|` term is evaluated in the *source's units* —
  `h` as a fraction of radius times `2000` — so the law bends at the same heights it did on a
  2 000-unit map. One constant, `SOURCE_MAP_SIZE = 2000.0f`, with that sentence beside it.
- **`ridged` applies `0.5 − |n|` to the two coarsest octaves only.**
- **A flatten cap's profile**: `Subtract` removes `value · (1 − t²)` where `t` is the angular
  distance over `halfWidthRad` (a bowl); `Absolute` sets `value`; `Add` adds `value`;
  `Subtract2` subtracts `value` only where `h > threshold`; `Smooth` replaces `h` with the mean
  of `h` at four points `halfWidthRad / 4` away — the source's five, over a cap.
- **`Climate`** is §5.6 verbatim; the cap-edge dither uses a `Pcg32` seeded from
  `hash(seed, quantised d)` so it is a pure function of direction.

### 2.6 The umbrella and the project files

`NeuronClient.h` includes `Noise3.h`, `CubeSphere.h`, `BodyDesc.h`, `BodyParams.h`,
`BodyField.h` after `MeshData.h`. `NeuronClient.vcxproj` and `.filters` gain the six files under
a new `Body` filter; `NeuronClientTests.vcxproj` and `.filters` gain the three test files.
`Build/CheckProjectFiles.py` checks.

---

## 3. Out of scope

- **Any vertex.** No `FxVertex`, no builder, no colour. Slice 2.
- **Any content.** No catalogue, no `BODY_*` constant, no ramp. A test builds a `BodyDesc` by
  hand and that is the only place one is built.
- **Diamond-square.** Turned down with the owner (design §14); if the slice 4 screenshot
  reverses that, it swaps inside `Height` and this interface does not change.
- **Moving `Noise3` to `NeuronCore`.** Design §4 says why not yet; it is a record when a
  second consumer appears.
- **Performance.** Nothing here runs at frame rate. A generation time is measured in slice 2
  where the whole cost exists.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronCore/Pcg32.h` (explosion slice 1) | `Below`, `Float01`, `Signed`, and the seeding contract |
| `NeuronCore/Ease.h` | The shape of a header-only maths utility in this tree |
| `NeuronClient/MeshData.h` | A device-free aggregate with `XMFLOAT3` fields and `[[nodiscard]]` helpers — the style for `BodyDesc` |
| `NeuronClient/Camera.h/.cpp` | Device-free `NeuronClient` code that computes in `XMVECTOR` locals and stores `XMFLOAT*` |
| `Tests/NeuronClientTests/CameraTests.cpp` | Sentence-named `TEST_METHOD`s, a why-comment per test, wide-string failure messages |
| `Design/PlanetRenderer.md` §5, §8.1, §10, §17.3 | The formulas, the parameter block, the determinism rule |
| AGENTS.md §1 R6, R8; §5 | Units in names, plain fields on aggregates, DirectXMath rules |

---

## 5. Acceptance

Tests in `Tests/NeuronClientTests/`:

**`Noise3Tests.cpp`**

- **Range.** Over a 32³ lattice of non-integer points, every `Sample` is in `[−0.5, 0.5]`.
- **Zero at the lattice.** `Sample(3, 7, −2) == 0` exactly.
- **Seeded.** Two `Noise3` from `Pcg32(11)` agree on 1 000 samples; `Pcg32(11)` and `Pcg32(12)`
  differ on the first.
- **Continuous.** For 1 000 random points, `|Sample(p + 0.001 x̂) − Sample(p)| < 0.01`.

**`CubeSphereTests.cpp`**

- **Unit length.** Every direction at `gridPower = 4` has `|d| − 1 < 1e-6`.
- **The corners are the cube's.** `Direction(PosY, 0, 0, n)` is `normalize(−1, 1, −1)` — pin the
  face's corner convention, and a `static_assert` on it proves `constexpr`.
- **Edges agree to the bit.** For every pair of adjacent faces and every sample along their
  shared edge at `gridPower = 5`, the two `Direction` calls return equal `x`, `y`, `z` — compared
  with `==`, not a tolerance.
- **Near-equal area.** At `gridPower = 5`, the area of the cell at a face's centre and the one at
  its corner are within 30 % of each other. (Without the warp they differ by ~2.6×.)

**`BodyFieldTests.cpp`**, all on a hand-built desc with `radiusMetres = 1000`, `gridPower = 4`:

- **Determinism.** Two fields from the same desc give equal `Height` at 500 directions, and
  `Height` at `Direction(PosX, 3, 5, 17)` equals a pinned value to six decimals, written into
  the test as a literal. **This is the replay key.** If the literal changes, the pull request
  says why.
- **Outside is outside.** One tile of half-width 0.3 rad at `+Y`; `Height` at `−Y` equals
  `outsideHeight × 1000`.
- **Desired height is the maximum.** `MaxHeight()` equals `desiredHeight × 1000` within 1 %.
- **Absolute flattens.** An `Absolute` cap of half-width 0.2 rad at the tile's centre: ten
  directions inside it return `value × 1000` exactly.
- **A bowl is a bowl.** A `Subtract` cap: `Height` at its centre is below `Height` at its rim by
  `value × 1000` within 5 %.
- **Caps at both poles.** `polarStrength = 1`, `capStart = 0.5`: `Climate` at `+Y` and at `−Y`
  both `≥ MaxHeight()`; at `+X` (equator) `Climate == Height`.
- **Ellipsoid.** `ellipsoid = (1, 0.5, 1)`: the point at `+Y` is at distance `500 + h` from
  the origin, the point at `+X` at `1000 + h` — checked through `Height` and the design's
  `P = d ⊙ ellipsoid · (R + h)` rule stated in the test.
- **Clipping traces, not truncates.** A desc with `MAX_TILES + 1` tiles builds, `Params().tileCount
  == MAX_TILES`, and the test states that the trace was seen (the harness cannot assert on a
  trace; the comment says so).

The tree:

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; `NeuronClientTests` runs and every existing test still passes.
- No screenshot: nothing visual.
- `Design/PlanetRenderer.md` §15 marks slice 1 `landed` in the same pull request, and this
  file moves to `Design/Archive/`.

---

## 6. Assumptions the implementer may make

- **`Pcg32` exists** with the interface `SpaceshipExplosion-slice-1.md` §2.1 gives. If that
  slice has not merged, this one waits; it does not land a second generator.
- **The pinned height literal is whatever the first correct build produces**, written into the
  test by the implementer and explained in the pull request. It pins *stability*, not a value
  derived elsewhere.
- **The `constexpr` `tan` polynomial** is accurate to `1e-6` on `[−π/4, π/4]`; a degree-9 odd
  polynomial is. A `static_assert` in the test compares it to `std::tan` at three points at
  runtime (the `static_assert` is on `Direction` compiling `constexpr`; the accuracy check is a
  `TEST_METHOD`).
- **Nothing here runs at frame rate**, so no performance claim is due.
