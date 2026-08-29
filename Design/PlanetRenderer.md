# Planets and asteroids

**Status: proposed. No slice has landed.** §15 lists the slices; §16 is the implementation plan.
Every open question was put to the owner on 2026-08-29 and settled (§14); nothing in this design
is open.

This document proposes how a planet or an asteroid is drawn: a low-poly heightfield wrapped round a
sphere, one flat colour per triangle taken from a 64×64 colour-ramp bitmap indexed by (slope,
climate), and an additive wire-frame outline over the top. It is a port of the Darwinia-style
landscape renderer from Interstellar Outpost — `LandscapeTile::Generate`, `GetLandscapeColour`,
`RenderMainSlow`/`RenderOverlaySlow` — onto a body in space, under this tree's rules. The source
analysis it starts from is `InterstellarOutpost.dx12/Docs/PlanetRenderer_Design.md`; that document
describes the original technique and a port to a Rust bindless renderer that does not exist here.
This one says what the port looks like *in this tree*: which layer holds what, what already exists
to build on, what the numbers are in metres, and what is deliberately left out.

Nothing about a planet or an asteroid exists today — not in `GameLogic`, not on the wire, not in
the client. Everything below is new, and all of it is presentation: the simulation does not learn
that bodies exist in this design (§2, §13).

Five decisions were taken with the owner before writing (§14): bodies are metre-scale objects
inside the existing frustum; they are presentation only and do not collide; the height field is
3-D noise under the source's amplitude law rather than a diamond-square port; the eight ramps
already in the tree are the palette and the ocean is a flat colour; and F5 reseeds the scene.

---

## 1. What is being built

Two kinds of body, one code path, different numbers:

1. **Planets.** A sphere of radius `R` (hundreds of metres, §3) whose surface is a seeded
   heightfield: continents that rise out of an ocean sphere, smooth lowlands, rugged peaks, polar
   caps that show the ramp's white top rows at both poles. Spins slowly about a tilted axis.
2. **Asteroids.** The same generator on an ellipsoid a few tens of metres across, with a coarser
   grid, much taller relief, cratered by subtractive flatten areas, coloured from a grey ramp, no
   ocean, tumbling on two axes.

Both are drawn in two passes per body — opaque faceted colour, then the additive outline — by a
new `BodyRenderer` in `NeuronClient`, from a mesh built once on the CPU from a seed. The seed is
the whole description: two clients with the same seed draw the same body, so a server will never
need to send geometry.

Deliberately not in this design: walking on the surface, atmosphere scattering, texture splatting,
shadows between bodies, and any effect of a body on the simulation (§13).

---

## 2. What the tree already guarantees

Constraints, not preferences. Each one shaped what follows.

| Constraint | Where it comes from | What it does to this design |
|---|---|---|
| Presentation state does not live in the simulation | AGENTS.md §5 | Bodies are client-side objects placed by the composition root. `GameLogic` and the wire format do not change. |
| `NeuronClient` names no game type | AGENTS.md §2 | The generator, mesh builder, ramp lookup and renderer are engine and take plain numbers. "A terran world uses `LandscapeEarth.dds` with 60 % polar caps" is content and lives in `Outpost` (§4). |
| One seeded PCG32, no `<random>`, no OS entropy | AGENTS.md §5; `SpaceshipExplosion.md` §10 | The source's private LCG (`holdrand * 214013 + 2531011`) is **not** ported. All randomness — noise permutation, tile placement, colour dither — draws from `Pcg32` (explosion slice 1). The look does not depend on which uniform generator produced it, only on it being seeded. |
| Math is DirectXMath, stored as `XMFLOAT*` | AGENTS.md §5 | Records store `XMFLOAT3`; the generator computes in `XMVECTOR` locals. `/fp:precise` and no `/arch` solution-wide is what makes the same seed give the same mesh on every machine. |
| Left-handed, `LH` everywhere; render space is (east, up, north) | AGENTS.md §5 | A body's spin axis is +Y unless tilted; latitude is measured from it. No `RH` call anywhere. |
| Cull mode is `NONE`, and the scene shades flat from `ddx/ddy` | `GpuHelpers.cpp:83`, `ScenePS.hlsl` | Faceted shading is already the house look. The body passes carry a real per-triangle normal instead (§7), because the colour lookup needs it anyway and it is free from the builder. Cull stays `NONE`; the depth buffer sorts the sphere's far side out. |
| No index buffer anywhere; a mesh is triangle soup | `MeshData.h`, `SceneRenderer::UploadMesh` | Unshared vertices, three per triangle, which is also the only way to get one colour and one normal per triangle without a provoking-vertex buffer. The source document's option 1, and the same choice for the same reason. |
| The only textured pipeline is `TextRenderer`'s (R8 coverage, point sampler); a BGRA upload helper is specified but not landed | `GpuHelpers.cpp:132`; `SpaceshipExplosion.md` §8.1 | `BodyRenderer` needs `UploadColourTexture` (explosion slice 3). If that slice has not landed, this design's renderer slice lands it, exactly as specified there, and slice 3 finds it in place (§15). |
| `FxVertex` — position, normal, colour+alpha, uv, 48 bytes — is the effect vertex format | `SpaceshipExplosion.md` §5.1 | It is exactly the body vertex too. One format, one input layout; same landing rule as above. |
| DDS only, kept as on disk; `TopMipAsBgra` reads 8-bpc texels on the CPU | `DdsImage.h:93` | The colour ramps are read on the CPU through `TopMipAsBgra` and never reach the GPU (§6). No BMP reader is written. |
| The ramp and outline textures are already in the tree, as 32-bit BGRA DDS, one mip | `Outpost/Assets/Terrain/Landscape*.dds` (8), `Textures/TriangleOutline.dds` | No conversion step. **Eight** ramps rather than the source's twenty-three, and **no `water_*` or `waves_*`** — the ocean is a flat colour in v1 (§6.3). |
| No threads; expensive work happens at `Init` and blocks | AGENTS.md §5; `GpuDevice::ExecuteAndWait` | Bodies are generated at boot on the main thread. A 65-grid planet is tens of milliseconds (§8.4); a background generator is a later slice and an ADR. |
| Camera: orbit over a ground plane, zoom 40–900 m, far plane 8 000 m, `D32_FLOAT` `LESS` | `Camera.h`, `ViewTuning.h:14-19` | Bodies are placed **inside the existing frustum at metre scale** (§3). No camera rebase, no reversed-Z, no second projection. What that rules out is in §13 and §14. |
| A missing asset is a diagnostic, not a crash | AGENTS.md §5 | A ramp that fails to load leaves the body drawn in a flat grey; a missing outline texture leaves the overlay off. Both trace. |

---

## 3. Scale: what "a planet" is in a game measured in metres

The source terrain was a 2 000-unit map with 12-unit cells and 200-unit peaks, seen from a camera
a few hundred units up. This game's hulls are 10 m across, the camera is 40–900 m from its target
and can see 8 km. A body has to read as *big* against a Frigate and still fit in that frustum with
usable depth precision, and the ground quad — 4 km wide, following the camera — has to be
somewhere sensible relative to it.

| | Planet | Asteroid |
|---|---|---|
| Radius `R` | 400–1 200 m | 15–120 m |
| Grid `N` per cube face | 65 (64 cells) | 17 or 33 |
| Cell at the equator | `πR / (2·64)` ≈ 10–30 m | 1.5–11 m |
| Relief (`heightScale`, as a fraction of `R`) | 3–12 % | 15–40 % |
| Centre height above the plane | `R · BODY_PLANET_LIFT` (default 1.15 — floats clear of the grid) | `R` (rests on the plane, as a Structure does) |
| Where | 2.5–5 km from the fleet's start, one or two per scene | among the ships, several |
| Motion | spin about a tilted axis, one turn per `BODY_PLANET_SPIN_SEC` | tumble on two axes |

A 1 000 m planet 4 km away subtends about 28° at the camera's 45° field of view — a backdrop that
fills a third of the screen at the edge of the play area, under a grid that has already faded
(`GRID_FADE_DISTANCE = 900`). A 60 m asteroid beside a 10 m Corvette reads as a rock you could
park behind. Both are ordinary meshes at ordinary depths; nothing about the camera changes.

What this deliberately does not do is put a 6 000 km planet in the sky. That takes a
camera-relative origin, a second projection or reversed-Z, and a decision about what the ground
plane means when the game is in orbit — three things this tree has not decided, and the owner
chose not to decide them here (§14). The renderer does not care: it takes a world matrix, so
either later shape is additive.

---

## 4. Where each piece lives

```
NeuronCore
└── Pcg32                       (explosion slice 1 — a dependency, not part of this design)

NeuronClient
├── Noise3                      seeded 3-D gradient noise, header-only, constexpr permutation      (slice 1)
├── CubeSphere                  (face, x, z, N) -> unit direction; the six-face grid              (slice 1)
├── BodyDesc                    every number a body is generated from, plain aggregate           (slice 1)
├── BodyField                   height h(d) on the sphere: octaves, tiles, flatten areas, caps    (slice 1)
├── ColourRamp                  a 64×64 BGRA lookup with bilinear sampling, from a DdsImage       (slice 2)
├── BodyMeshBuilder             BodyField -> std::vector<FxVertex>, normals, colours, uvs, culling (slice 2)
├── FxVertex                    (explosion slice 2, or landed here identically)
├── UploadColourTexture         (explosion slice 3, or landed here identically)
├── BodyRenderer                outline texture, two PSOs, static body meshes, Draw              (slice 3)
└── Shaders/Body*.hlsl          BodyVS, BodyPS, BodyOverlayPS, Body.hlsli                         (slice 3)

Outpost
├── BodyCatalogue               body classes: which ramp, which parameter ranges; Random(seed)    (slice 4)
├── WorldView                   owns the placed bodies, spins them, draws them                    (slice 4)
├── OutpostApp                  loads ramps, generates the starting bodies, F5 reseeds            (slice 4)
└── ViewTuning.h                every number in §3, §5–§7 as BODY_* constants                     (slice 4)
```

The split follows the explosion design's test: *would a second game want it unchanged?* A cube-
sphere, a fractal height function with tiles and flatten areas, a ramp lookup and a two-pass
renderer are engine. "Six classes of world, terran ones get `LandscapeEarth`, ice ones get caps
from 35° latitude" is Outpost.

`Noise3`, `CubeSphere`, `BodyField`, `ColourRamp` and `BodyMeshBuilder` are **device-free**: they
take a `BodyDesc` and a `Pcg32`, and write `FxVertex` into a caller's `std::vector`. That is what
makes them testable in `NeuronClientTests` the way `Camera` and `ObjParser` are. `BodyRenderer` is
the only file in the feature that includes `<d3d12.h>`.

`Noise3` goes in `NeuronClient` rather than `NeuronCore` on the content-reader rule's reasoning
(ADR 0002): the test is who calls it, and today only the renderer does. The day the simulation
needs a planet's surface — a colony pad that must be flat, a ray-hit against terrain — the height
function moves to `NeuronCore` with a record, and `BodyField` is already written to make that a
move rather than a rewrite: it depends on `Pcg32`, `Noise3` and DirectXMath and nothing else.

---

## 5. The generator — `BodyField`

### 5.1 Topology: a cube-sphere

A body is six `N×N` grids, one per cube face, `N = 2^k + 1`. Sample `(face, x, z)` maps to a
point on the unit cube, is warped with the usual `tan`-based correction so cells are close to
equal area, and normalised to a direction `d`. The vertex is at

```
P = d ⊙ ellipsoid · (R + h(d))          // ellipsoid = (1,1,1) for a planet
```

Why a cube-sphere and not a latitude/longitude sphere: the grid stays regular, so every piece of
the source's grid-based code — the neighbour rule for sea-level culling, one outline tile per
cell, the per-cell dither seed — ports face by face with no pole special-case. Faces share their
edge samples, and because `h` is evaluated from `d` and not from `(x, z)`, the shared samples are
bit-identical and there are no seams. That is a test (§16, slice 1).

`CubeSphere::Direction(face, x, z, N)` is the whole of it: a pure function, `constexpr`-capable,
that the builder and the tests both call.

### 5.2 Height: the source's amplitude law, sampled in 3-D

Diamond-square works on one square and cannot be made seamless across six faces without
special-casing every edge. Replace the *sampling* with 3-D gradient noise and keep the source's
**amplitude law**, because the law — noise grows with height, so lowlands are smooth and peaks are
rugged — is what makes the terrain look the way it does:

```
h = 0
amp = heightScale · 30.7·e^(−6.5·fractalDimension) · 15.353·e^(−3.1·lowlandSmoothing)   // compensatedHeightScale, verbatim
for octave in 0 .. octaves−1:                                      // octaves = k, one per diamond-square level
    len = 256 · 0.5^octave                                         // the "halfSize" of that level
    n   = Noise3(d · frequency(octave) + seedOffset)               // in [−0.5, 0.5]
    if ridged and octave < 2:  n = 0.5 − |n|                       // stands in for generationMethod 1/2
    n  *= pow(len · 10, fractalDimension) · amp
    n  *= 0.1 + pow(|h|, lowlandSmoothing) · 0.15                  // the height-dependent term from GenerateNoise
    h  += n
h += posY                                                          // LandscapeTile::m_posY, a whole-body lift
```

`seedOffset` is three floats from the body's `Pcg32`, so two bodies with different seeds sample
different regions of the same noise. `Noise3` is Perlin-style gradient noise with a 256-entry
permutation shuffled by a `Pcg32` at construction — no static table, so the noise itself is
seeded and reproducible. It is written once in this tree, by hand, in a header; no library.

### 5.3 Continents are tiles; craters and pads are flatten areas

- **`LandscapeTile` → continent.** A body has `tileCount` tiles. A tile is a spherical cap round a
  random direction `c` with angular half-width `σ`; inside it the height is generated with the
  tile's own parameters, rescaled so its maximum is `desiredHeight`, and **max-merged** into the
  body — `if (h1 > h2)`, exactly `MergeTileIntoLandscape`. A cap's edge is softened over
  `edgeFraction` of `σ`, or continents end in a vertical wall. Outside every tile the height is
  `outsideHeight`: below zero for an ocean world, at or above zero for a dry one.
- **`LandscapeFlattenArea` → crater, basin, landing pad.** The source's five modes — Absolute,
  Add, Subtract, Subtract2, Smooth — over a spherical cap instead of a square. `Subtract` with a
  bowl profile is a crater and is what asteroids are covered in; `Absolute` is a flat pad, kept
  because a colony site will want one; `Smooth` is erosion. Applied in list order after the tiles.
- **`BurnPatch` → decal list.** Left out of v1 (§13); the field has the hook (`decals`, empty).

### 5.4 Asteroids are parameters, not a code path

| Parameter | Planet | Asteroid |
|---|---|---|
| `ellipsoid` | (1, 1, 1) | ratios in 0.55–1.0 on two axes |
| lumpiness | none | 1–3 extra octaves at `frequency ≈ 1–3 / R`, amplitude ±25 % `R`, applied before the tiles |
| `N` | 65 | 17 or 33 |
| `heightScale / R` | 0.03–0.12 | 0.15–0.40 |
| `outsideHeight` | −0.02 `R` (ocean) | +0.01 `R` (dry) |
| tiles | 1–4, `σ` 25–70° | 1, whole body |
| flatten areas | 0–3 Absolute pads | 6–20 Subtract bowls, `σ` 4–15° |
| `polarStrength` | by class (§6.2) | 0 |
| motion | spin | tumble |

### 5.5 Ocean

The source drops any grid quad whose six surrounding samples are all at or below sea level and
pushes any vertex with `h < 0.3` down to `h = −10` so the coast dips under the water plane. Both
rules are kept, in metres scaled by `R` (`BODY_SHORE_DIP = −0.01 R`, `BODY_SHORE_THRESHOLD =
0.0003 R`). The ocean is an inner smooth sphere at radius `R`, **drawn through the existing
`SceneRenderer::DrawMesh` as `MeshVertex` soup with a flat colour** — it needs no texture, no
new pipeline, and the scene pass's derivative shading gives it the same facets as everything else.
Drawn opaque before the terrain; terrain above sea level occludes it and the coast dips hide
behind it. A textured, drifting ocean needs a water texture this tree does not have (§6.3).

### 5.6 Latitude: the ramp's top row must appear at both poles

On the flat map the ramp's Y axis was height alone: white on summits, green at sea level. On a
globe the same white must also form polar caps, and the equator must read as the warm bottom rows
whatever its altitude. One flat landscape is one hemisphere, equator to pole; the other hemisphere
is its mirror. So the ramp lookup takes a *climate height*, not the raw height:

```
lat      = asin(dot(d, spinAxis))                      // object space, so a tilted axis works
polar    = smoothstep(capStart, 1, |sin lat|)          // even in lat -> both poles for free
polar   += rng.Signed(capNoise)                        // ragged cap edge, per triangle
climate  = h + polarStrength · maxHeight · polar       // "as cold here as on a summit"
h       += polarGeometry · polarStrength · maxHeight · polar   // optional: caps with thickness
```

`v = 1 − climate / maxHeight` then goes into the ramp exactly as `GetLandscapeColour` did. Asteroids
set `polarStrength = 0` and skip the whole term.

---

## 6. Colour — `ColourRamp` and the port of `GetLandscapeColour`

### 6.1 The lookup, verbatim with the sphere substitutions

```
gradient = dot(faceNormal, d)                       // was norm.y: 1 = flat, 0 = cliff; d is radial "up"
u        = pow(1 − gradient, 0.4)                   // 0 flat … 1 cliff       -> ramp X
v        = 1 − climate / maxHeight                  // 0 summit … 1 sea level -> ramp Y
v       += rng.Signed(0.45 / (climate + 2))         // dither: strong near sea level, weak on peaks
colour   = ramp.Sample(u, v)                        // bilinear, clamped
```

`maxHeight` is `BodyDesc::maxHeight` when set (the source's `LandscapeDef::m_maxHeight`, used for
colour scaling only), else the field's real maximum. The dither's generator is a `Pcg32` seeded
from `hash(bodySeed, face, cellX, cellZ)` per triangle — the source seeded it from an expression
that was effectively unseeded, and here it has to be deterministic. `hash` is an **integer** mix
(a PCG-style `uint32` hash of the four values), not a float expression, so the identical grain is
reproducible in HLSL, where integer arithmetic is exact on every GPU and float arithmetic is not
(§17). The height in the dither term
is in the source's units (a 2 000-unit map), so it is taken as `climate / R · 2000` to keep the
same strength; that is a constant with a comment, not a formula the reader has to rediscover.

The ramps were inspected: every one is 64×64 BGRA with the **summit on row 0** and sea level on
row 63, flat terrain in column 0 and cliffs in column 63 — the orientation `GetLandscapeColour`
assumes. `ColourRamp::Sample` reads bilinear where the source's `GetPixel` read nearest; the ramps
are soft gradients and bilinear is the smoother of two indistinguishable choices. Stated so a
later diff of a ramp is not blamed on the sampler.

Colour is **baked into the vertex** on the CPU, one colour per triangle written to its three
vertices. A pixel-shader ramp lookup would allow swapping a biome at runtime but loses the
per-triangle dither that gives the surface its grain; kept as a later option (§13).

### 6.2 Body classes — the eight ramps this tree has

| Class | Ramp | `polarStrength` | `capStart` | Ocean | Notes |
|---|---|---|---|---|---|
| Terran | `LandscapeEarth` | 0.6 | 0.75 | yes | the garden world |
| Classic | `LandscapeDefault` | 0.6 | 0.75 | yes | the source's green/blue |
| Ice | `LandscapeIcecaps` | 1.0 | 0.35 | yes, pale | |
| Desert | `LandscapeDesert` | 0 | — | no | ramp is sand top to bottom |
| Barren | `LandscapeMine`, `LandscapeMine2` | 0 | — | no | asteroids, dead moons |
| Structure sites | `LandscapeLaunchpad`, `LandscapeContainment` | — | — | — | not whole-body ramps; reserved for flatten-area tints (§13) |

The source document maps fifteen more ramps (`landscape_dark`, `_silver`, `_marble`, `_volcanic`,
…) that were not copied into this tree. Adding one is an asset plus a catalogue row; converting
them is `texconv -f B8G8R8A8_UNORM` from the `.bmp` files in `InterstellarOutpost/Assets/Terrain/`.

### 6.3 Textures

| File | Size | Format in tree | Used by | Sampler |
|---|---|---|---|---|
| `Terrain/Landscape*.dds` × 8 | 64×64 | BGRA8, one mip | `ColourRamp`, **CPU only** | — |
| `Textures/TriangleOutline.dds` | 128×128 | BGRA8, one mip; **RGB all 255, the lines are in alpha** (0 off, 255 on the diagonals and edges) | overlay pass | linear min/mag, wrap |
| `water_*`, `waves_*` | — | **not in the tree** | — | — |

The outline's line mask is `tex.a`, not `tex.r` as in the source document — that document
described an 8-bit indexed BMP, and this file was converted with the ink moved into alpha. The
overlay shader reads alpha (§7.2).

**No mips.** The tree generates none, and the file has one. At one tile per grid cell a planet at
4 km puts many cells under one pixel, and a mipless linear sample of a 128-texel line pattern
sparkles. Rather than add mip generation, the overlay shader **fades the outline out as the cell
shrinks**: `gain · saturate(1 − length(fwidth(uv)) · BODY_OUTLINE_FADE)` — the same `fwidth`
device the ground grid already uses to anti-alias itself. The outline is a near-field effect and
should vanish at distance anyway. A CPU box-filter mip chain is the fallback if the fade reads
wrong in the screenshot (slice 3's stated assumption).

---

## 7. `BodyRenderer` — the GPU side

One object in `NeuronClient`, initialised after `SceneRenderer`, that owns the outline texture,
two pipelines and the uploaded body meshes.

### 7.1 Resources

- **Vertex format.** `FxVertex` (§2): object-space position, per-triangle normal, baked colour
  with `a = 1`, uv `= (cellX, cellZ)` so one outline tile covers one grid cell. 48 bytes; a
  65-grid planet is `6·64·64·2 = 49 152` triangles → 147 456 vertices → **7.1 MB**, before
  sea-level culling removes the ocean floor. An asteroid at `N = 33` is 1.8 MB, at 17 it is 0.44 MB.
  Acceptable for the handful of bodies §3 describes; LOD is a later slice (§13).
- **Meshes.** Static, uploaded once — but **not** into an upload heap as `SceneRenderer::UploadMesh`
  does. That helper's argument ("a few thousand triangles do not justify a staging copy") was made
  for hulls; a body is 7 MB that the input assembler reads twice a frame, and on a discrete GPU an
  upload-heap buffer is system memory read across PCIe at every draw — two planets are ≈ 28 MB
  per frame, ≈ 1.7 GB/s at 60 Hz, on a bus that is also carrying everything else. `UploadBody`
  stages through an upload buffer into a **`D3D12_HEAP_TYPE_DEFAULT`** resource, transitions it
  to `VERTEX_AND_CONSTANT_BUFFER`, and finishes the copy through the `ExecuteAndWait` path the
  tree already uses for initialisation. It is also the resource a GPU-side generator would write
  (§17). `UploadBody` returns a `BodyHandle` (`std::uint32_t`, index into a vector, `INVALID_BODY`),
  the `MeshHandle` idiom.
- **Texture.** One `CBV_SRV_UAV` heap slot, `TriangleOutline.dds` through
  `DdsImage::Load → TopMipAsBgra → UploadColourTexture`, and one static sampler, linear, wrap.
  The composition root passes the file name in `BodyRenderer::Desc`, as it does for fonts.
- **Root signature.** 32 DWORDs of `world` + `viewProj` for the vertex stage (the scene layout,
  because bodies spin and so are object-space, unlike fx vertices), 12 DWORDs for the pixel stage
  (`lightDirAmbient`, `cameraPos`, `overlayParams`), one SRV descriptor table. The scene root
  signature is not widened — the explosion design's §8.1 reasoning, unchanged.

### 7.2 Pipelines

| PSO | Shaders | Blend | Depth | Purpose |
|---|---|---|---|---|
| `m_mainPso` | `BodyVS` / `BodyPS` | opaque | test **and write**, `LESS` | the coloured facets |
| `m_overlayPso` | `BodyVS` / `BodyOverlayPS` | `SRC_ALPHA / ONE` (additive) | test, **no write**, `LESS_EQUAL` | the outline |

Both from `DefaultPipelineDesc()`: cull none, back-buffer format. Cull none means the sphere's far
side is rasterised and depth-rejected; at 50 k triangles that is a cost worth measuring, not
assuming, and switching to back-face culling is one line once the builder's winding is confirmed
(slice 3 acceptance).

```hlsl
// Body.hlsli
cbuffer Matrices : register(b0) { row_major float4x4 world; row_major float4x4 viewProj; }
cbuffer Shading  : register(b1) { float4 lightDirAmbient; float4 cameraPos; float4 overlayParams; } // gain, fade, spec, shininess
Texture2D    OutlineTex : register(t0);
SamplerState Wrap       : register(s0);
struct VsIn  { float3 pos : POSITION; float3 normal : NORMAL; float4 col : COLOR; float2 uv : TEXCOORD0; };
struct VsOut { float4 clip : SV_Position; float3 worldPos : POSITION; nointerpolation float3 normal : NORMAL;
               nointerpolation float4 col : COLOR; float2 uv : TEXCOORD0; };
```

`BodyVS` transforms position and normal by `world` and passes colour and uv through.

`BodyPS`, the source's main pass — lit, colour material, no specular:

```hlsl
float3 n = normalize(i.normal);
float lambert = saturate(dot(n, normalize(lightDirAmbient.xyz)));
float3 lit = i.col.rgb * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);
return float4(lit, 1.0);
```

`BodyOverlayPS`, the source's overlay material — diffuse 1.2, specular 0.5, shininess 40, additive:

```hlsl
float4 tex = OutlineTex.Sample(Wrap, i.uv);
float3 n = normalize(i.normal);
float3 l = normalize(lightDirAmbient.xyz);
float3 v = normalize(cameraPos.xyz - i.worldPos);
float lambert = saturate(dot(n, l));
float spec = pow(saturate(dot(n, normalize(l + v))), overlayParams.w) * overlayParams.z;
float fade = saturate(1.0 - length(fwidth(i.uv)) * overlayParams.y);           // §6.3: gone when a cell is sub-pixel
float3 rgb = (i.col.rgb * overlayParams.x * (lightDirAmbient.w + lambert) + spec) * tex.a;
return float4(rgb * fade, tex.a * fade);
```

The normal is not faced towards the eye as `ScenePS` does: the builder guarantees it points
outward (`dot(n, d) > 0`, flipped if not), so a back-facing triangle is correctly dark rather than
lit twice. That is the one place the body passes differ from the scene's lighting convention, and
it is a test (slice 2).

### 7.3 Draw

```cpp
struct Desc { std::wstring outlineTexture; };
void Init(GpuDevice&, const Desc&);
[[nodiscard]] BodyHandle UploadBody(GpuDevice&, std::span<const FxVertex>);
void Begin(GpuDevice&, const XMFLOAT4X4& viewProj, const XMFLOAT3& lightDir, float ambient, const XMFLOAT3& cameraPos, const OverlayParams&);
void DrawMain(GpuDevice&, BodyHandle, const XMFLOAT4X4& world);       // main PSO
void DrawOverlay(GpuDevice&, BodyHandle, const XMFLOAT4X4& world);    // overlay PSO
```

Two entry points rather than one `Draw` that does both, so the caller can draw every body's main
pass, then every body's overlay — one PSO switch per pass rather than two per body, and the
overlay of body A tests against the depth of body B. The order in `WorldView::Render` becomes:

```
BeginScene → ground → ocean spheres (DrawMesh, flat colour) → hulls      (opaque, depth write)
bodies.Begin → DrawMain × bodies                                          (opaque, depth write)
bodies.DrawOverlay × bodies                                               (additive, depth test only)
DrawFeedback                                                              (decals, glow)
[fx passes, when the explosion lands]
TextRenderer.Flush
```

Bodies draw after the hulls only so the ocean, which goes through the scene pass, is already in
the depth buffer; the order between hulls and bodies is otherwise free. `DrawFeedback` rebinds the
scene root signature in `BeginDecals`, so nothing else changes.

---

## 8. `BodyDesc` and `BodyMeshBuilder`

### 8.1 The description — every number a body is made from

```cpp
struct BodyDesc                                  // NeuronClient/BodyDesc.h — R8: plain fields
{
  std::uint64_t seed = 0;
  float radiusMetres = 500.0f;
  DirectX::XMFLOAT3 ellipsoid{1.0f, 1.0f, 1.0f};
  std::uint32_t gridPower = 6;                   // N = 2^gridPower + 1
  float heightScale = 0.05f;                     // fraction of radius
  float fractalDimension = 0.8f;                 // source: m_fractalDimension
  float lowlandSmoothing = 1.2f;                 // source: m_lowlandSmoothingFactor
  bool ridged = false;                           // source: m_generationMethod != 0
  float outsideHeight = -0.02f;                  // fraction of radius; < 0 is ocean
  float maxHeight = 0.0f;                        // colour scaling only; 0 = use the field's maximum
  float lumpiness = 0.0f;                        // asteroids: ±fraction of radius at 1–3 cycles
  std::vector<BodyTile> tiles;                   // centre dir, halfWidthRad, edgeFraction, desiredHeight, posY, own fractal params
  std::vector<BodyFlatten> flatten;              // centre dir, halfWidthRad, mode, value, threshold
  float polarStrength = 0.0f, capStart = 0.75f, capNoise = 0.1f, polarGeometry = 0.0f;
  DirectX::XMFLOAT3 spinAxis{0.0f, 1.0f, 0.0f};
};
```

Everything but `seed`, `radiusMetres` and the class's ramp is derived by `Outpost`'s catalogue
(`BodyCatalogue::Random(seed, class)`), so a future server sends a class and a seed — or nothing,
if the seed is a system id.

`BodyField` does not evaluate from the `BodyDesc` directly. Its constructor draws every random
number once and flattens the result into a **`BodyParams`** block: fixed capacity
(`MAX_TILES = 8`, `MAX_FLATTEN = 32`), 16-byte-aligned `float4`-shaped fields, the noise
permutation as 256 `uint32`, no pointers and no vectors. `Height` and `Climate` read only that
block. The reason is §17: the block is already the constant buffer a compute kernel would take,
so the day the evaluation moves to the GPU, the CPU side that draws the random numbers — the part
the `Pcg32` rule and the determinism tests are about — is unchanged.

### 8.2 The builder

`BodyMeshBuilder::Build(const BodyDesc&, const ColourRamp*, std::vector<FxVertex>& outTerrain,
std::vector<MeshVertex>& outOcean, BuildStats&)`:

1. Evaluate `BodyField` at every `(face, x, z)` sample into six `N×N` height arrays.
   Neighbouring faces share edge samples, which is a property of evaluating from `d` and is
   asserted in debug (`DEBUG_ASSERT`) and tested in release.
2. For each cell, skip it if all six surrounding samples are `≤ 0` and the body has an ocean
   (§5.5). Otherwise emit two triangles with unshared vertices; a vertex with `h < shoreThreshold`
   is pushed to `shoreDip`.
3. Per triangle: normal from the cross product, flipped to face outward; centroid height and
   `gradient = dot(n, d)`; climate from §5.6; colour from §6.1 with a `Pcg32` seeded per cell;
   uv `= (x, z)` of the cell corner plus `(0|1, 0|1)`.
4. Ocean: a `MeshVertex` sphere at radius `R` with the ellipsoid applied, from the same cube-
   sphere at `gridPower − 2`, colour `BODY_OCEAN_COLOUR` of the class. Empty when `outsideHeight ≥ 0`.
5. `BuildStats` reports triangle counts before and after culling and the real `maxHeight`, so the
   composition root can trace what it got and a test can pin it.

### 8.3 Object → world

`world = scale(1) · rotate(spinAxis, angle) · translate(centre)`, with `angle` advanced in
`WorldView::UpdateFeedback` on real time (`BODY_PLANET_SPIN_SEC` per turn). Asteroids accumulate a
`XMFLOAT3X3` from two angular velocities exactly as the explosion's `Tumbler` does — the same
stored type, the same reason. A body's `centre` is a `Game::WorldPos` converted through
`ViewX/ViewZ`, so it is placed in the same coordinate the ships are and moves with the camera
rebase the day that lands.

### 8.4 Cost

Per vertex, `octaves` noise evaluations (six for `N = 65`) plus the tile and flatten loops; per
triangle one ramp sample and a `Pcg32` seed. A 65-grid planet is ~150 k vertex evaluations and
~50 k triangle finishes: **tens of milliseconds** in Debug, less in Release, once, at boot. Two
planets and eight asteroids at boot is well under a second, on the main thread, before the window
shows — which is the pattern the tree has (`ExecuteAndWait` at init) and the one this design uses.
A body arriving mid-session would stall a frame; that is stated, not solved (§13).

---

## 9. What `Outpost` adds — the catalogue, the placement, the key

- **`BodyCatalogue.h`**: `enum class BodyClass { Terran, Classic, Ice, Desert, Barren, Asteroid }`,
  a `constexpr` table of `{ ramp file, polarStrength, capStart, oceanColour, heightScale range,
  tile count range, … }` in the `HULL_SPECS` style — a row per class, an annotated column header,
  a `static_assert` on the count — and `BodyDesc Random(std::uint64_t seed, BodyClass)`, which
  draws every range from a `Pcg32(seed)`.
- **`WorldView`** gains `std::vector<BodyView>` — `{ BodyHandle terrain; MeshHandle ocean;
  WorldPos centre; float centreY; XMFLOAT3 spinAxis; float spinRadPerSec; XMFLOAT3X3 tumble;
  XMFLOAT3 tumbleRadPerSec; }` — an `AddBody(...)` the composition root calls, the spin/tumble
  update in `UpdateFeedback`, and the two draw loops in `Render` (§7.3).
- **`OutpostApp::Init`** loads the ramps it needs through `ColourRamp::Load`, generates the
  starting bodies from `BODY_START_*` constants, uploads them, and hands them to the view.
  **F5 reseeds and regenerates every body** — beside F1/F3, and clear of the F4 the explosion
  design reserves — so tuning is a key press and a screenshot rather than a rebuild.
- **`ViewTuning.h`**: `BODY_*` for every number in §3, §5 and §6; nothing in `NeuronClient`
  holds a default that is really content.

The starting scene: one Terran planet at 4 km bearing north-east, one Barren moon at 3 km
north-west, and six asteroids scattered 150–400 m from the fleet, all from a fixed seed so the
screenshot in the pull request is reproducible.

---

## 10. Determinism

The same `BodyDesc` produces the same `std::vector<FxVertex>` byte for byte, on every machine, on
every run. What guarantees it: `Pcg32` for every random draw; `Noise3`'s permutation built from
`Pcg32` rather than a static table; `/fp:precise` and no `/arch` solution-wide; no `XM*Est`
functions in the generator (the ban is written for `GameLogic` and adopted here for the same
reason); dense-array iteration order only. It is the first test in slice 1 and it is what will let
a server describe a world with sixteen bytes.

---

## 11. Tests

All device-free, all in `NeuronClientTests`, in the tree's sentence-named style:

| File | What it pins |
|---|---|
| `Noise3Tests.cpp` | output in `[−0.5, 0.5]`; same seed same value; different seeds differ; continuous (neighbouring samples within a bound) |
| `CubeSphereTests.cpp` | every direction is unit length; the four corners of a face are the cube's; an edge sample of face A equals the same edge sample of face B; cell areas at a face centre and a face corner are within 30 % |
| `BodyFieldTests.cpp` | determinism; `outsideHeight` outside every tile; a tile's maximum is `desiredHeight`; an `Absolute` flatten area is flat inside; a `Subtract` bowl is lower at its centre than its rim; `polarStrength = 1` gives climate `≥ maxHeight` at the pole and `≈ h` at the equator; ellipsoid scales radii |
| `ColourRampTests.cpp` | a synthetic 64×64 gradient samples back its corners; clamping at the edges; bilinear midpoint is the mean |
| `BodyMeshTests.cpp` | vertex count is `3 × triangles` and a multiple of 3; every normal faces outward; an all-ocean body emits no terrain and a full ocean; a dry body emits a full terrain and no ocean; shore vertices are at `shoreDip`; uv of a cell corner is integral; determinism of the whole vertex list |

Slice 3 has no test that can decide it and is accepted by screenshot; slice 4 by screenshot and
by what it does not touch (`GameLogicTests` unchanged, no `GameLogic` file in the diff).

---

## 12. Design choices

The choices made inside the design, as opposed to the ones put to the owner (§14). Each is
reversible in the slice that would first notice.

| Question | Decision | What lost |
|---|---|---|
| Which RNG? | `Pcg32`, the tree's one (§2) | Porting the source LCG verbatim — a second generator the rulebook forbids, for a look that does not depend on it |
| Diamond-square or 3-D noise? | 3-D gradient noise under the source's amplitude law (§5.2) | Porting diamond-square per face — seam handling on twelve edges, and a ridged option that then needs a second mechanism |
| Vertex format? | `FxVertex`, unshared, one normal and colour per triangle (§7.1) | Shared vertices with a provoking-vertex colour buffer — a third of the memory, three times the moving parts; `MeshVertex` — no uv, so no outline |
| Where do colours come from? | Baked on the CPU from the ramp, with dither (§6.1) | A ramp texture and a PS lookup — runtime biome swap, but no per-triangle grain |
| Where is the noise? | `NeuronClient` (§4) | `NeuronCore` now — an ADR for a consumer that does not exist yet |
| Ocean texture? | Flat colour through the scene pass (§5.5) | The source's `water_*` caustic — the texture is not in the tree |
| Outline at distance? | `fwidth` fade in the shader (§6.3) | Mip generation — new upload machinery for a pattern that should vanish at distance anyway |
| Two draw entry points or one? | `DrawMain` and `DrawOverlay` (§7.3) | One `Draw` per body — two PSO switches per body, and body A's outline could not test against body B's depth |

---

## 13. Deliberately left out

- **Bodies in the simulation.** No `GameLogic` type, nothing on the wire, no collision. A ship
  flies through an asteroid — the owner's choice (§14). When gameplay wants otherwise, `HullSpec`
  already has `immovable` and `collidable` and the `Structure` row is the precedent for a large
  static body: an asteroid
  becomes a hull row with a capsule and the view draws the body for that hull id. That is a
  `GameLogic` slice and an ADR, and this design must not pre-empt it.
- **Astronomical scale and a backdrop pass.** §3, §14. The renderer takes a world matrix and a
  view-projection and would draw a planet through a rotation-only view with depth writes off as
  readily as through the scene's; that is two more PSOs and a decision about the ground plane.
- **LOD.** One grid per body. The source document's three-LOD-by-screen-radius scheme needs a
  frustum-free projected-radius estimate and three uploads per body; the counts in §3 do not
  need it. `gridPower` in `BodyDesc` is the door.
- **Background generation and a cache.** No threads in the tree (§2). A generator on a worker
  that enqueues finished vertex lists to the main thread is the shape the rulebook's single-writer
  rule allows, and an ADR.
- **Decals** (`BurnPatch` → bombardment scars, colony footprints, lava glow). The field carries an
  empty list; the colour step has the hook.
- **Flatten-area tint overrides** using `LandscapeLaunchpad`/`LandscapeContainment` — a colony
  pad coloured from a second ramp. Needs decals first.
- **Textured, drifting ocean and the shoreline `waves` band.** No water texture in the tree, and
  the owner chose the flat colour for v1 (§14). Converting `water_default.bmp` and adding a third
  PSO is the whole of the change if it is wanted later.
- **The other fifteen source ramps.** Each is a `texconv` and a catalogue row (§6.2), deferred
  until a class needs one (§14).
- **Atmosphere rim, clouds, rings, moons shadowing planets.** Effects, each a shader term or a
  second body; none needed for the surface to read.
- **Ray-hit against the terrain** (`Landscape::RayHit`). Picking a body is a sphere test the day
  something needs to pick one.
- **Runtime biome switching** through a PS ramp lookup (§6.1).
- **Back-face culling.** Measured in slice 3, switched on if it pays.

---

## 14. Decisions taken before writing

Put to the owner on 2026-08-29 and answered as follows; each was the recommended option.

| Question | Decision | What lost |
|---|---|---|
| Where do bodies live relative to the camera and the ground plane? | **Metre scale, inside the existing frustum** (§3): planets of 400–1 200 m at the edge of the play area, asteroids of 15–120 m among the ships | A rotation-only backdrop pass for planets — two more PSOs and a decision about the ground plane; true astronomical scale — a camera-relative origin and a depth strategy the tree has not decided on |
| Do asteroids block ships? | **Presentation only.** `GameLogic` and the wire are untouched; a ship flies through a rock | A `HullSpec` row per asteroid so the fleet routes round it — a `GameLogic` slice and an ADR for a behaviour no gameplay yet asks for; the ADR in slice 4 records it as the route in |
| Look-alike, or a faithful diamond-square port? | **3-D gradient noise under the source's amplitude law** (§5.2), judged by the slice 4 screenshot against the source game | Diamond-square per cube face — the exact algorithm, but seam handling on twelve edges and a second mechanism for the ridged option; kept in reserve, and it would swap inside `BodyField` only |
| Which ramps, and what ocean? | **The eight ramps in the tree and a flat-colour ocean sphere** (§5.5, §6.2) | Converting the other fifteen ramps now — an asset commit for classes nothing places yet; a textured drifting ocean — a water texture, one more PSO and a drift constant, for a surface seen from a kilometre away |
| Which key reseeds the scene? | **F5**, beside F1/F3 and clear of the explosion's F4 | F6; no key at all, which makes tuning a rebuild |

---

## 15. Slices

Five, in dependency order. Slices 1–3 are all `NeuronClient` and cannot run in parallel with each
other or with the explosion's slices 2 and 3 (same project file, same umbrella header); the order
below interleaves with the explosion's where the two share a file.

| # | Slice | Layer | Depends on | Work order |
|---|---|---|---|---|
| 1 | `Noise3`, `CubeSphere`, `BodyDesc`, `BodyField`, tests | `NeuronClient` | explosion slice 1 (`Pcg32`) | [slice 1](PlanetRenderer-slice-1.md) |
| 2 | `ColourRamp`, `BodyMeshBuilder`, `FxVertex` if not landed, tests | `NeuronClient` | 1 | [slice 2](PlanetRenderer-slice-2.md) |
| 3 | `BodyRenderer`, `UploadColourTexture` if not landed, three shaders and one `.hlsli` | `NeuronClient` | 2 | [slice 3](PlanetRenderer-slice-3.md) |
| 4 | `BodyCatalogue`, `BodyView` in `WorldView`, starting bodies, F5, `BODY_*` tuning, ADR | `Outpost` | 3 | [slice 4](PlanetRenderer-slice-4.md) |
| 5 | Ocean sphere through the scene pass, shore dip and sea-level culling wired to a class | `NeuronClient` + `Outpost` | 4 | [slice 5](PlanetRenderer-slice-5.md) |
| 6 | Compute-shader bake: `BodyBakeCS`, the reductions, readback test against the CPU builder (§17) | `NeuronClient` | 5 | [slice 6](PlanetRenderer-slice-6.md) — cut when body counts or reseed latency ask for it |

Slice 5 is split from 2 and 4 so that a dry body — every asteroid, the desert world — can land
and be looked at before the ocean rules are argued over a screenshot. It touches two layers and
is the one exception to one-layer-per-slice; it is small enough to be one pull request.

One decision record is due, in slice 4: *bodies are presentation, placed by the composition root,
and not simulation entities* — an approach (a `Planet` entity in `GameLogic`) that a reasonable
person will propose again, with the `HullSpec` route recorded as the way in when gameplay asks.
It takes the next free number; the explosion's slice 1 claims 0011.

---

## 16. Implementation plan

What each slice builds, in the order it is built, with the acceptance that decides it. The work
orders carry the exact file lists, the "what to build on" tables and the assumptions; this is the
plan they are cut from, and it is written to the decisions in §14.

### Slice 1 — the field (`NeuronClient`, device-free)

| Step | File | What |
|---|---|---|
| 1.1 | `Noise3.h` | Header-only gradient noise. `explicit Noise3(Pcg32&)` shuffles a 256-entry permutation; `float Sample(float x, float y, float z) const noexcept` in `[−0.5, 0.5]`. Twelve fixed gradient directions, quintic fade. No statics. |
| 1.2 | `CubeSphere.h` | `enum class CubeFace : std::uint8_t { PosX, NegX, PosY, NegY, PosZ, NegZ }`; `constexpr XMFLOAT3 Direction(CubeFace, std::uint32_t x, std::uint32_t z, std::uint32_t n)`; `constexpr std::uint32_t SamplesPerSide(std::uint32_t gridPower)`. Equal-area warp `tan(θ·π/4)` applied to the two in-face coordinates. |
| 1.3 | `BodyDesc.h` | The aggregate in §8.1, plus `BodyTile`, `BodyFlatten`, `enum class FlattenMode { Absolute, Add, Subtract, Subtract2, Smooth }`. |
| 1.4 | `BodyField.h/.cpp` | `class BodyField { explicit BodyField(const BodyDesc&); float Height(const XMFLOAT3& d) const; float Climate(const XMFLOAT3& d, float h) const; float MaxHeight() const; }`. Construction draws every random number (seed offsets, tile centres, cap noise) from `Pcg32(desc.seed)` once, so `Height` is pure and can be sampled in any order. |
| 1.5 | umbrella, `.vcxproj`, `.filters` | `NeuronClient.h` gains the four headers; both project files gain every file. |
| 1.6 | tests | `Noise3Tests`, `CubeSphereTests`, `BodyFieldTests` (§11). |

**Accepted when:** all three suites pass; `Build/CheckProjectFiles.py` and `CheckFormat.py`
pass; a `static_assert` pins `CubeSphere::Direction(PosY, 0, 0, 3)`; a test pins the first
height of a fixed `BodyDesc` to six decimals, which is the replay-key guarantee (§10).

### Slice 2 — the mesh (`NeuronClient`, device-free)

| Step | File | What |
|---|---|---|
| 2.1 | `FxVertex.h` | Landed exactly as `SpaceshipExplosion-slice-2.md` §2.1 specifies it, if not already there. |
| 2.2 | `ColourRamp.h/.cpp` | `class ColourRamp { static bool Load(const std::wstring&, ColourRamp&); bool FromImage(const DdsImage&); XMFLOAT3 Sample(float u, float v) const noexcept; bool Loaded() const; }`. Holds 64×64×3 floats; `FromImage` accepts any `TopMipAsBgra`-readable 64×64 and reports on anything else. |
| 2.3 | `BodyMeshBuilder.h/.cpp` | §8.2. A `nullptr` ramp bakes `BODY_FALLBACK_GREY` and traces once. |
| 2.4 | tests | `ColourRampTests`, `BodyMeshTests` (§11). |

**Accepted when:** the suites pass; a test builds a `gridPower = 3` dry body and pins its vertex
count at `6·8·8·2·3 = 2 304`; every normal satisfies `dot(n, d) > 0`; the vertex list of a fixed
desc hashes to a pinned value.

### Slice 3 — the renderer (`NeuronClient`, device)

| Step | File | What |
|---|---|---|
| 3.1 | `GpuHelpers.h/.cpp` | `UploadColourTexture`, as `SpaceshipExplosion-slice-3.md` specifies, if not landed. |
| 3.2 | `Shaders/Body.hlsli`, `BodyVS.hlsl`, `BodyPS.hlsl`, `BodyOverlayPS.hlsl` | §7.2. Three `FxCompile` items in `NeuronClient.vcxproj` with `HeaderFileOutput` and `VariableName` in the house pattern; the `.hlsli` as `<None>`. |
| 3.3 | `BodyRenderer.h/.cpp` | §7.1, §7.3. Descriptor heap of one, one static sampler, two PSOs, `std::vector<GpuMesh>` of bodies. |
| 3.4 | `OutpostApp` (temporary) | A debug body from a hard-coded `BodyDesc`, drawn in `WorldView::Render`, so the slice has a screen to be accepted on. Removed by slice 4, which replaces it with the catalogue. Stated in the work order as the placeholder it is. |

**Accepted when:** a screenshot at two window sizes shows a `gridPower = 6` body with facets,
ramp colours and the outline; the outline fades rather than sparkles when the camera zooms to
900 m; the frame time with two planets and six asteroids is measured and written in the PR, with
and without back-face culling; nothing under `CompiledShaders/` is committed.

### Slice 4 — the game (`Outpost`)

| Step | File | What |
|---|---|---|
| 4.1 | `ViewTuning.h` | `BODY_*` for every number in §3, §5, §6 and §9. |
| 4.2 | `BodyCatalogue.h` | The class table and `Random` (§9), in the `HullSpec` style. |
| 4.3 | `WorldView.h/.cpp` | `BodyView`, `AddBody`, spin and tumble in `UpdateFeedback`, the two draw loops in `Render` (§7.3). |
| 4.4 | `OutpostApp.cpp` | Ramp loading through `FileSys` with a `TERRAIN_DIR`, the starting scene from a fixed seed, `F5` reseed, the `BodyRenderer::Desc` with the outline file name. Removes slice 3's placeholder. |
| 4.5 | `Design/Decisions/00NN-bodies-are-presentation.md` | The ADR (§15), and its index row. |

**Accepted when:** screenshots at two sizes of the starting scene; F5 produces a different scene
and F5 after a restart produces the same first scene; `GameLogicTests` unchanged and no
`GameLogic` file in the diff; every number in the PR's screenshots is a named constant.

### Slice 5 — the ocean (`NeuronClient` + `Outpost`)

| Step | File | What |
|---|---|---|
| 5.1 | `BodyMeshBuilder` | Sea-level quad culling, shore dip, and the `MeshVertex` ocean sphere output (§5.5, §8.2 steps 2 and 4), each behind `outsideHeight < 0`. |
| 5.2 | tests | The ocean rows of `BodyMeshTests` (§11). |
| 5.3 | `WorldView` | `MeshHandle ocean` on `BodyView`, drawn through `DrawMesh` in the opaque pass before the terrain. |
| 5.4 | `BodyCatalogue` | `oceanColour` per class; Terran, Classic and Ice gain an ocean. |

**Accepted when:** the terrain suite passes with the ocean rows; a screenshot of the Terran world
shows coastlines with no gap and no z-fighting at the shore at both 40 m and 900 m zoom.

### What runs in parallel

Nothing within this design: every slice is in `NeuronClient` or depends on one that is. Against
the explosion design, this design's slice 1 can proceed the moment the explosion's slice 1
(`Pcg32`) merges, and its slices 2 and 3 should be sequenced with the explosion's 2 and 3 by
whoever holds the `NeuronClient` project file that week — whichever lands second rebases and
finds `FxVertex.h` or `UploadColourTexture` already there.

---

## 17. Moving generation to the GPU — analysis and the slice it becomes

Written after the design was settled, as a Direct3D 12 review of §5–§8: what of the CPU
pipeline could run in a shader, what it would buy, and what it would cost under this tree's
toolchain — FXC, shader model 5.1, no DXC, no mesh shaders, and no compute pipeline yet.

### 17.1 Stage by stage

| Stage | GPU-portable | What has to change |
|---|---|---|
| Cube-sphere directions (§5.1) | yes | a static unit grid, or `SV_VertexID` → `(face, x, z)` |
| Noise octaves and the amplitude law (§5.2) | yes | nothing; float noise is not bit-exact across vendors, which stopped mattering when bodies became presentation only (§14) |
| Tiles, max-merge, rescale to `desiredHeight` (§5.3) | with a reduction | the rescale needs each tile's maximum **before** evaluation — a first dispatch with `InterlockedMax` on order-preserving float bits, then the real pass |
| Flatten areas, polar caps (§5.3, §5.6) | yes | nothing |
| Per-triangle normal, gradient, centroid climate (§6.1) | yes | one compute thread per triangle owns its three vertices; in a vertex shader it would mean carrying the two mates' directions per vertex and three height evaluations |
| Ramp lookup and dither (§6.1) | yes | `SampleLevel` on the ramp; the dither hash is integer (§6.1), so the grain matches the CPU to the bit |
| `maxHeight` from the field (§6.1) | with a reduction | the same mechanism as the tile maximum |
| Sea-level quad culling (§5.5) | yes, differently | a shader cannot delete a triangle; a culled quad is written as two **degenerate** triangles — zero area, no raster cost, no `ExecuteIndirect` |
| Shore dip, uv (§5.5, §8.2) | yes | nothing |
| Ocean sphere, spin, the two passes (§5.5, §7, §8.3) | already GPU-side | nothing |

Everything moves, and only the two maxima need a mechanism. Because `BodyField` already draws
every random number once into `BodyParams` (§8.1), the split is clean: **the CPU keeps the
`Pcg32` and produces the parameter block; the GPU evaluates pure mathematics on it.** The
"one seeded PCG32" rule is untouched by the move.

### 17.2 Three shapes, one chosen

| Shape | What it is | Wins | Why it is not the one |
|---|---|---|---|
| A — vertex-shader displacement | one shared grid per `gridPower`; `BodyVS` evaluates `h(d)` for its vertex and its two mates every frame | no generation, no per-body memory, LOD is a bind | ~450 k height evaluations per body per pass, per frame, for a shape that never changes; the ramp sample and dither repeat every frame too |
| **B — compute bake at load** | `BodyBakeCS` (`cs_5_1`, compiled by FXC through an `FxCompile` item of type `Compute`) writes the identical `FxVertex` stream into the default-heap buffer of §7.1 through a UAV; one barrier to `VERTEX_AND_CONSTANT_BUFFER`; two small reduction dispatches first | same look, boot generation ≈ 0, **a body generated mid-session no longer stalls a frame** — this tree's route to many systems without introducing a thread, since a GPU timeline is not a second CPU writer; LOD is three dispatches; F5 is instant | the first compute pipeline in the tree (a root signature with a CBV, an SRV table for the ramp and a UAV — about 150 lines); the field stops being decidable by a `CppUnitTest` on its own |
| C — tessellation / mesh shaders | hull/domain shaders for continuous LOD | continuous LOD | redoes the height per frame like A and fights the per-triangle colour; mesh shaders need SM 6.5 and DXC, which the tree does not have |

**B is the shape**, and **not yet**. Tens of milliseconds at boot is invisible, the tests in §11
are what make slices 1 and 2 decidable, and a CPU reference is the thing a compute port is
*verified against* — a readback of the baked buffer compared to `BodyMeshBuilder`'s output for a
fixed seed, positions and normals within a few ULPs, colours exact (the hash is integer, the ramp
sample is the same bilinear filter). Porting first would leave the design with no reference.

### 17.3 What the CPU slices do now so that slice 6 is a drop-in

Three preparations, none visible on screen, already folded into §6.1, §7.1 and §8.1:

1. **Body vertex buffers live in a default heap**, staged once (§7.1). A correctness-of-intent
   change on its own — 7 MB read twice a frame does not belong in system memory — and it is the
   resource the kernel writes.
2. **The dither hash is integer** (§6.1), so the CPU and the GPU produce the same grain.
3. **`BodyField` evaluates from a flat `BodyParams` block** (§8.1) that is already a constant
   buffer in shape.

### 17.4 Slice 6 — the bake (`NeuronClient`)

| Step | File | What |
|---|---|---|
| 6.1 | `Shaders/BodyBake.hlsli`, `BodyBakeMaxCS.hlsl`, `BodyBakeCS.hlsl` | `BodyParams` as a cbuffer mirroring the C++ block; the reduction kernel (one thread per sample, `InterlockedMax` into a `MAX_TILES + 1` `uint` buffer); the bake kernel (one thread per triangle, 64 per group, writes three `FxVertex` per thread, degenerate when culled) |
| 6.2 | `BodyRenderer` | a compute root signature and two PSOs; `BakeBody(GpuDevice&, const BodyParams&, const ColourRamp&, std::uint32_t gridPower)` returning a `BodyHandle` over the same default-heap buffer `UploadBody` fills; the ramp uploaded once as a 64×64 BGRA SRV through `UploadColourTexture` |
| 6.3 | `NeuronClient.vcxproj` | two `FxCompile` items with `ShaderType` `Compute`, in the house pattern |
| 6.4 | `OutpostApp` | a `BODY_BAKE_ON_GPU` constant in `ViewTuning.h` selecting the producer, so both paths stay alive and comparable |
| 6.5 | acceptance | a debug readback (`READBACK` heap, `ExecuteAndWait`) of one baked body compared to the CPU builder for the starting seed: positions and normals within `1e-4 R`, colours and uvs exact, triangle count equal; a screenshot at two sizes; boot time and F5 latency measured with each producer and written in the PR |

Cut when one of three things happens: the starting scene wants more bodies than boot can generate
in the time a player will wait, F5 stops feeling instant, or a body first has to appear
mid-session. Until then the CPU path is the reference and the only producer.
