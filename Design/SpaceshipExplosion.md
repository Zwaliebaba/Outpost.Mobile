# Spaceship explosion

**Status: landed. All four slices are in the tree.** §14 lists them.

This document proposes how a ship dies on screen: its hull shatters into tumbling triangles, a
fireball blooms where it was, and a ring of smoking debris flies out and fades. It is a port of the
`Building::Destroy` effect from Interstellar Outpost — hull shatter (`ExplosionManager`), billboard
particles (`ParticleSystem`), and the burst recipe (`Location::Bang`) — into this tree, under this
tree's rules. The source analysis it starts from is
`InterstellarOutpost.dx12/Docs/SpaceshipDestruction/DESIGN.md`; that document describes the
original effect and a generic DX12 port. This one says what the port looks like *here*: which
layer holds what, what already exists to build on, what the numbers are in metres, and what is
deliberately left out.

Four decisions were taken with the owner before writing (§13): the trigger is client-side
detection of a despawn plus a debug key; debris is zero-g; particles get a real textured pipeline
rather than reusing the procedural glow; and randomness arrives as a seeded PCG32 in `NeuronCore`.

---

## 1. What is being built

Three layers of effect spawned in the same frame, all presentation state, none of it simulated:

1. **Hull fragments.** Every triangle of the ship's mesh detaches, flies outward from the mesh
   centre, tumbles, and fades over 5 s. Each fragment carries a wire-frame decal
   (`ShapeWireframe.dds`) over its panel colour, so it reads as a glowing shard rather than a grey
   sliver. Spawned **three times over** with different tumbles, which triples debris density for
   nothing.
2. **Fireball cores.** 50–75 large reddish sprites (`Particle.dds`), additive, 2 s.
3. **Smoke debris.** 25 mid-size sprites launched in a ring, 6 s, each shedding grey smoke puffs
   behind it five times a second. Smoke draws with a *darkening* blend that is the whole reason the
   original looks like smoke rather than fog; §7 reproduces it exactly.

Plus one thing the original did not have and this tree already does: a **camera shake** on death,
through `Camera::Shake()`, which is wired and tuned and bound to F3 today.

Sound and area damage, which the original couples in, are out (§12).

---

## 2. What the tree already guarantees

Constraints, not preferences. The design is shaped by each of them.

| Constraint | Where it comes from | What it does to this design |
|---|---|---|
| Presentation state does not live in the simulation | AGENTS.md §5 | The effect is entirely client-side. `GameLogic` does not change. |
| `NeuronClient` names no game type | AGENTS.md §2 | The generic pieces (shatter, particles, renderer) go in `NeuronClient`; the *recipe* — how many cores, what colour, three copies — is game and goes in `Outpost`. |
| No wall clock outside `FrameClock`; no OS entropy anywhere | AGENTS.md §5 | Effects age by `dtSec` handed in, never by a timestamp. Randomness is a seeded generator. |
| One seeded PCG32 when randomness arrives | AGENTS.md §5, `GameLogic.h:11` | This is where it arrives. It goes in `NeuronCore` so `GameLogic` can adopt the same one later (§10, ADR due). |
| Math is DirectXMath, stored as `XMFLOAT*`, never a stored `XMVECTOR` | AGENTS.md §5 | Fragment and particle records store `XMFLOAT3`/`XMFLOAT3X3`. |
| Left-handed, `LH` everywhere | AGENTS.md §5 | Billboards are built from `Camera::Right()`/`Up()`, which are already LH. |
| Shaders are FXC-compiled `<Name>VS/PS.hlsl` | AGENTS.md §3 | Two new pairs and one `.hlsli` (§8). |
| The client already retains every hull's object-space triangle soup after upload | `MeshLibrary::Load`, `MeshData::verts` | The shatter needs **no new content and no mesh adapter**: `MeshData` is triangle soup with per-vertex colour, three vertices per triangle, no index buffer to flatten. |
| The only per-frame dynamic vertex path is `TextRenderer`'s | `TextRenderer.cpp:16–24`, `:236–275` | The fx renderer copies that pattern: `FRAME_COUNT` persistently-mapped upload buffers, one `memcpy` per frame. |
| The only texture path is R8 coverage with a point sampler | `GpuHelpers.cpp:132`, `TextRenderer.cpp:92–107` | A BGRA upload helper and a linear sampler are new (§8). |
| A ship's presentation state is carried across snapshots by handle | `WorldView::ApplySnapshot` | The carry loop already computes the set of ships that vanished; the trigger falls out of it (§9). |

---

## 3. The source effect, and the units it was written in

`Building::Destroy(intensity = 100)` in the original does, in one frame:

| Step | What | Numbers at intensity 100 |
|---|---|---|
| A | `AddExplosion(shape, transform)` × 3 | every triangle, ≤ 500 per copy, 5 s life |
| B | `Location::Bang(pos, range = 100, damage = 25)` | 25–50 cores, size 120–240; 25 debris in a ring, size 20–40 |
| D | extra cores | 25 more cores, size 100, velocity uniform in [−100, 100]³ |

The original's units are Darwinia's: a building is 30–50 units across; a "size 150" sprite is a
camera-facing quad of half-size `150 / 16 ≈ 9.4` units. **Those numbers were written for a
hull roughly 10 units across, and this game's hulls are roughly 10 metres across** — Corvette
`r = 8.6 m`, Bomber `8.7 m`, Frigate `10.4 m` (`HullSpec.h:103–107`). So the port keeps the
source constants **1:1 as metres and seconds** and scales them by one number per ship,
`hullScale = max(halfExtents) / EXPLOSION_REFERENCE_HALF_SIZE` with the reference at 10 m — so a
Carrier (`r = 40 m`) gets a burst four times wider, and the day a fighter dies it gets a small one.
`halfExtents` is already on `ShipView`, taken from the mesh, so no simulation value is needed.

One number does *not* transfer: the shatter's radial speed is `(triCentre − meshCentre) × 3.0`
per second, which on a 10 m hull throws the outermost pieces at 30 m/s and on a Carrier at 120 m/s.
That is the source's behaviour and it is kept — a bigger ship blowing apart harder is right.

---

## 4. Where each piece lives

```
NeuronCore
└── Pcg32                       seeded generator (slice 1)

NeuronClient
├── FxVertex                    one vertex format for both effect passes (slice 2)
├── MeshShatter                 one shattered mesh: tumblers + fragments, Advance + Build (slice 2)
├── SpriteParticles             fixed-capacity billboard pool, Advance + Build (slice 2)
└── FxRenderer                  textures, three PSOs, per-frame ring buffer, Draw (slice 3)

Outpost
├── ShipExplosion               the recipe: three shatters + cores + debris, per dead ship (slice 4)
├── WorldView                   detects the despawn, owns the live explosions, drives the renderer (slice 4)
└── ViewTuning.h                every number in §5–§7 as EXPLOSION_* constants (slice 4)
```

The split follows one test: *would a second game want it unchanged?* A mesh that shatters into
triangles and a pool of camera-facing sprites are engine; "twenty-five smoke puffs in a ring and
three copies of the hull" is Outpost. The recipe reads no engine internals — it calls
`MeshShatter::Spawn` and `SpriteParticles::Emit` with numbers from `ViewTuning.h`.

`MeshShatter` and `SpriteParticles` are **device-free**: they hold `XMFLOAT*` records, take
`dtSec`, and write `FxVertex` into a caller's `std::vector`. That is what makes them testable in
`NeuronClientTests` the way `Camera` and `ObjParser` are, with no `ID3D12Device` in sight.
`FxRenderer` is the only file in the effect that includes `<d3d12.h>`.

---

## 5. `MeshShatter` — the hull fragments

### 5.1 Records

```cpp
struct FxVertex          // NeuronClient/FxVertex.h — one layout for both effect passes
{
  float px, py, pz;      // world position
  float nx, ny, nz;      // world normal; sprites leave it zero and the sprite shader ignores it
  float r, g, b, a;      // colour and alpha, already curved -- the shaders do no fading
  float u, v;
};                       // 48 bytes

struct Tumbler
{
  DirectX::XMFLOAT3X3 rot;       // accumulated rotation, identity at spawn
  DirectX::XMFLOAT3 angVelRadPerSec;
};

struct Fragment
{
  DirectX::XMFLOAT3 pos;         // world-space centroid
  DirectX::XMFLOAT3 velMetresPerSec;
  DirectX::XMFLOAT3 v0, v1, v2;  // vertices relative to the centroid, world orientation, untumbled
  DirectX::XMFLOAT3 normal;
  DirectX::XMFLOAT3 colour;      // lerp(desc.tintColour, vertexColour, desc.tintMix) -- the hull's own draw mix (§13)
  std::uint8_t tumbler;          // 0..TUMBLER_COUNT-1
};
```

`XMFLOAT3X3` is a stored DirectXMath type and is allowed; a stored `XMVECTOR` is not.

### 5.2 Spawn

`MeshShatter::Spawn(const MeshData&, const XMFLOAT4X4& world, const XMFLOAT3& inheritedVel,
const Desc&, Pcg32&)`. For each triangle `verts[i..i+2]` (skipped with probability
`1 − desc.fraction` when `fraction < 1`):

1. Transform the three positions and the mesh's `BoundsCentre()` by `world`.
2. Reject degenerate triangles and those with circumference under `desc.minCircumferenceMetres`
   (source: 6 units; scaled by `hullScale` by the caller).
3. `centroid = (p0 + p1 + p2) / 3`; store `v0..v2 = p − centroid`.
4. `normal = normalize((v0 − v1) × (v1 − v2))`. The source flips odd triangles as a strip
   artefact; the OBJ soup has no such artefact and **the sign does not matter here anyway**: the
   fragment shader faces the normal towards the eye exactly as `ScenePS` does, because the OBJ
   import already reverses winding (`GpuHelpers.cpp:81–83`).
5. `vel = (centroid − worldCentre) × desc.radialSpeedPerSec + inheritedVel`. `inheritedVel` is
   the ship's velocity at death so the wreck keeps drifting — new against the source, whose
   buildings stood still.
6. `tumbler = rng.Below(TUMBLER_COUNT)`.
7. Stop at `desc.maxFragments` (source: 500) and **report how many were dropped** through the
   return value, so the caller can trace it — a Structure has 1784 faces and the cap is real.

Five tumblers per shatter, each with `angVel` uniform in `±desc.maxAngVelRadPerSec` per axis
(source: 4 rad/s). Every fragment shares one of five rotations; that is the original's trick and
it is what keeps a 1500-fragment shatter at one `XMMatrixRotationRollPitchYaw` per tumbler per
frame rather than per fragment.

### 5.3 Advance and Build

```
Advance(dtSec):
  ageSec += dtSec
  each tumbler:  rot = rot * RotationRollPitchYaw(angVel * dtSec);  angVel *= 1 − dtSec * ROT_FRICTION
  each fragment: pos += vel * dtSec;  vel *= 1 − min(1, |vel| * FRICTION * dtSec);  vel += gravity * dtSec
                 [landed without the |vel| factor: vel *= 1 − min(1, FRICTION * dtSec). Slice 2's
                  own acceptance test rules the speed-dependent form out — a fragment at 100 m/s
                  and FRICTION = 0.05 stops dead in one second under it, where the test requires it
                  merely slowed and still pointing the same way, and pins the clamp separately.]
  return ageSec >= lifetimeSec

Build(std::vector<FxVertex>& out):
  alpha = 1 − ageSec / lifetimeSec
  each fragment: R = tumblers[t].rot;  emit (R·v0 + pos), (R·v1 + pos), (R·v2 + pos)
                 with normal R·n, colour, alpha, uv = (0,0), (0,1), (1,1)
```

`gravity` is a `Desc` field defaulting to zero. Zero-g was the owner's choice (§13); the field
stays so a planetary variant is a constant, not a slice. Fragments that drift below `y = 0` are
clipped by the ground quad — it draws first and writes depth — and that is accepted.

### 5.4 Constants (source values, in metres and seconds, scaled by `hullScale` where marked)

| Name | Value | Scaled | Meaning |
|---|---|---|---|
| `EXPLOSION_FRAGMENT_LIFETIME_SEC` | 5.0 | | whole shatter is dropped at this age |
| `EXPLOSION_FRAGMENT_RADIAL_SPEED` | 3.0 /s | | `vel = offset × 3` |
| `EXPLOSION_FRAGMENT_MAX_ANG_VEL` | 4.0 rad/s | | per axis |
| `EXPLOSION_FRAGMENT_FRICTION` | 0.05 | | linear drag |
| `EXPLOSION_FRAGMENT_ROT_FRICTION` | 0.2 | | angular drag |
| `EXPLOSION_FRAGMENT_MIN_CIRCUMFERENCE` | 6.0 m | ✓ | smaller triangles are skipped |
| `EXPLOSION_FRAGMENT_CAP` | 500 | | per copy |
| `EXPLOSION_HULL_COPIES` | 3 | | see §3 |
| `TUMBLER_COUNT` | 5 | | `static constexpr` on `MeshShatter`, not tuning |

---

## 6. `SpriteParticles` — the billboards

### 6.1 Types

Only the three types the recipe uses. Gravity is zero for all (zero-g); the column is kept because
the source's per-type gravity is what a planetary variant would restore.

| Type | life s | size | friction | colour A → colour B | blend |
|---|---|---|---|---|---|
| `Core` | 2.0 | 150 | 0.2 | (200,100,100) → (255,120,120) | additive |
| `Debris` | 6.0 | 40 | 0.2 | (200,128,128) → (250,200,200) | dark; **emits `Smoke`** |
| `Smoke` | 5.0 | 200 | 0.0 | (100,100,100) → (200,200,200) | dark |

A particle's colour is `lerp(A, B, rng.Float01())` at emission and never changes; what changes over
life is alpha (§6.3). `SpriteType` is a `struct { float lifeSec, size, friction; XMFLOAT3 colourA,
colourB; SpriteBlend blend; }` and the table is `constexpr` data in `SpriteParticles.h`, so a fourth
type is a row.

### 6.2 Pool

Fixed capacity, `SpriteParticles::Desc::capacity` (default 4096), swap-remove on death, and
**emissions during `Advance` go to a scratch list appended after the loop** — the source's own
comment warns that emitting mid-iteration invalidates the iterator, and a fixed pool does not make
that go away, it just changes the failure to a silent overwrite. An `Emit` that finds the pool full
drops the particle and counts it; the count is readable and the recipe traces it, because a silent
cap reads as "covered everything".

Budget for one intensity-100 death: 75 cores + 25 debris, and each debris emits ≈ 5 puffs/s for
6 s ≈ **750 smoke puffs** over its life, of which ≈ 625 are alive at once at the peak. 4096 holds
four simultaneous deaths with room; more than that starts dropping smoke first, which is the
right thing to lose.

### 6.3 Advance and Build

```
Advance(dtSec, rng):
  each particle: ageSec += dtSec; dead if ageSec >= type.lifeSec
                 pos += vel * dtSec;  vel *= 1 − dtSec * type.friction
                 if type == Debris and rng.Float01() < SMOKE_RATE_PER_SEC * dtSec:
                     scratch.emit(Smoke at pos, vel / 5, size / 1.5)
  append scratch

Build(cameraRight, cameraUp, out):
  each particle: alpha = ageSec < 0.75 * life ? 90/255 : (90/255) × (1 − (age − 0.75 life) / (0.25 life))
                 half = size / 16
                 corners = pos + half × (−up, +right, +up, −right)         // 45° diamond, as the source
                 six FxVertex, uv (0,0) (1,0) (1,1) (0,1), colour type-specific (§7)
```

The 45° corner order is deliberate: it hides the square silhouette of a 16×16 sprite.

Two `Build` calls, one per blend, so the renderer issues one draw per pass; particles are not
sorted. Frustum culling is **not** done — the tree has no frustum type, the counts are hundreds,
and the original culled only to save CPU on a 2005 machine.

`SMOKE_RATE_PER_SEC = 5.0` reproduces the source's `frand() < 0.5 × dt × 10`.

---

## 7. The two particle blends, reproduced exactly

The look of the original depends on a blend that is easy to "fix" into something worse.

| Pass | Types | D3D12 blend | What the vertex carries | Effect |
|---|---|---|---|---|
| dark | `Debris`, `Smoke` | `Src = SRC_ALPHA, Dest = INV_SRC_COLOR` | `rgb = colour × alphaFraction`, **`a = 0`** | with `a = 0` the source term vanishes and the frame becomes `dest × (1 − src.rgb)`: a light sprite **darkens** what is behind it, which is how a pink blob reads as smoke |
| additive | `Core` | `Src = SRC_ALPHA, Dest = ONE` | `rgb = colour`, `a = alpha` | fireball |

`alphaFraction = alpha / (90/255)`, i.e. 1 during the first three quarters of life. Depth test on,
**depth write off**, cull none, for both. `Particle.dds` is sampled with **point** magnification
— it is a 16×16 flat grey tile with a lighter rim and the softness is the blend's, not the
texture's (the tree's copy was inspected: every texel opaque, `9c9c9c` inside, `ededed` at the
edge).

---

## 8. `FxRenderer` — the GPU side

One object in `NeuronClient`, initialised after `SceneRenderer`, that owns everything the effect
needs the device for and nothing the simulation does.

### 8.1 Resources

- **Textures.** A shader-visible `CBV_SRV_UAV` heap with three slots, filled at `Init` from
  `Desc::fragmentTexture` (`ShapeWireframe.dds`), `Desc::spriteTexture` (`Particle.dds`), and
  `Desc::flashTexture` (`Starburst.dds`, loaded and slotted but not drawn — §11). Loading goes
  `DdsImage::Load → TopMipAsBgra → UploadColourTexture`, where **`UploadColourTexture` is a new
  helper beside `UploadCoverageTexture`** in `GpuHelpers`, identical but for
  `DXGI_FORMAT_B8G8R8A8_UNORM` and a 4-byte texel. All three files are already 32-bit BGRA with a
  real alpha channel and one mip (inspected), so no colour-key and no mip generation is needed;
  the source document's palette/colour-key notes describe files this tree does not have.
- **Samplers.** Two static: `s0` linear min/mag, wrap (fragments); `s1` point mag / linear min,
  clamp (sprites). Point-vs-linear for sprites is the source's choice and is kept.
- **Vertex ring.** `FRAME_COUNT` upload buffers of `MAX_FX_VERTS × sizeof(FxVertex)`, persistently
  mapped — the `TextRenderer` pattern verbatim. `MAX_FX_VERTS = 49152` (2.25 MB per frame): three
  copies of a 500-fragment shatter is 4500 vertices per dead ship, and a full 4096-particle pool is
  24576, so the ring holds five simultaneous deaths with the pool full. Anything over is dropped
  and traced, never silently truncated.
- **Root signature.** 16 DWORDs of `viewProj` (VS) + 8 DWORDs of `lightDirAmbient`, `cameraPos`
  (PS) as root constants, and one SRV descriptor table (PS). The scene root signature is not
  reused: it has no descriptor table and no sampler, and widening it would touch every draw in
  the game to save one object.

### 8.2 Pipelines

| PSO | Shaders | Blend | Depth | Sampler |
|---|---|---|---|---|
| `m_fragmentPso` | `FxFragmentVS/PS` | src-alpha / inv-src-alpha | test **and write**, `LESS` | `s0` |
| `m_spriteDarkPso` | `FxSpriteVS/PS` | src-alpha / **inv-src-colour** | test, no write, `LESS_EQUAL` | `s1` |
| `m_spriteAddPso` | `FxSpriteVS/PS` | src-alpha / one | test, no write, `LESS_EQUAL` | `s1` |

All from `DefaultPipelineDesc()`, so cull none and the back-buffer format come for free.

**`FxFragmentPS`**, the port of GL_DECAL plus the scene's own lighting:

```hlsl
float4 tex = FragmentTex.Sample(Wrap, i.uv);
float3 rgb = lerp(i.col.rgb, tex.rgb, tex.a);            // decal: wire lines replace, else panel colour
float3 n = normalize(i.normal);
if (dot(n, cameraPos.xyz - i.worldPos) < 0.0) n = -n;    // faced to the eye, as ScenePS
float lambert = saturate(dot(n, normalize(lightDirAmbient.xyz)));
float3 lit = rgb * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);
return float4(lit, i.col.a);
```

**`FxSpritePS`**: `return SpriteTex.Sample(Clamp, i.uv) * i.col;` — colour and alpha were curved on
the CPU (§7), so the shader is the same for both blends.

The fragment shader lights from a real vertex normal rather than `ddx/ddy` because a tumbling
triangle's derivatives are exact anyway and the normal is free — it was computed for the tumble.

### 8.3 Draw

```cpp
void Begin(GpuDevice&, const XMFLOAT4X4& viewProj, const XMFLOAT3& lightDir, float ambient, const XMFLOAT3& cameraPos);
void DrawFragments(GpuDevice&, std::span<const FxVertex>);   // fragment PSO
void DrawSpritesDark(GpuDevice&, std::span<const FxVertex>); // dark PSO
void DrawSpritesAdd(GpuDevice&, std::span<const FxVertex>);  // additive PSO
```

Each `Draw*` copies its span into the current frame's ring at a running offset and issues one
`DrawInstanced`. `Begin` binds the heap, root signature and `viewProj` once. The scene and the
decal pass rebind their own root signature after (`BeginDecals` already does), so the order in
`WorldView::Render` becomes:

```
BeginScene → ground → hulls          (opaque, depth write)
fx.Begin → DrawFragments             (blended, depth write, so a shard occludes the smoke behind it)
DrawFeedback                          (decals and thruster glow, depth test only)
fx.Begin → DrawSpritesDark → DrawSpritesAdd   (depth test only; dark before additive, as the source)
TextRenderer.Flush                    (overlay, no depth)
```

`fx.Begin` twice is one root-signature bind and 24 DWORDs; not worth a second entry point.

---

## 9. The trigger

`GameLogic` has no death: no health, no damage, and `World::DespawnShip` has no production caller.
That is not this design's problem to solve, and the owner chose not to solve it here (§13). What
the client *can* know is that a ship it was drawing is no longer in the snapshot, and
`WorldView::ApplySnapshot` already computes exactly that set — after the carry loop, whatever is
left in `m_carryScratch` whose handle was not matched is a ship that vanished.

So the trigger is: **a `ShipView` that is not carried explodes at the last place it was drawn.**
For that `ShipView` needs two fields it does not have — the last world matrix it was drawn with
(bank, hover, interpolation and all, so the shards start exactly where the hull was) and its
velocity, `(sin h, 0, cos h) × speed`, which the snapshot gives — both written in `Render`.

A ship that despawns before it was ever drawn, or whose mesh never loaded, does not explode. The
seam stays where it is: nothing in `GameLogic` changes, nothing new goes on the wire, and a real
kill later is a producer of the same despawn this already consumes.

**The debug key.** Something has to despawn a ship, and nothing does. `OutpostApp::OnKeyDown`
gets **F4: despawn the selected ships**, beside F3's camera shake, calling `m_world.DespawnShip`
for each selected handle. The composition root touching `m_world` directly is the one place that
is allowed to, and this is a debug hook stated as one; it does not go over the wire because there
is no such order and this design must not invent one for a tuning aid.

The effect and the removal are one frame apart on purpose: the ship is drawn on frame N, removed
on tick N+1, and the explosion appears in the snapshot that lacks it. There is no frame where the
hull and its fragments are both on screen.

---

## 10. Randomness

The tree has none, and both the rulebook and `GameLogic.h` say it arrives as **one seeded PCG32**.
This effect is where it arrives, so it arrives properly: `NeuronCore/Pcg32.h`, the standard
`pcg32` (64-bit state, 64-bit odd increment, XSH-RR output), header-only, `constexpr`-constructible,
with `Next()`, `Below(n)`, `Float01()`, `Signed(x)` (uniform in `±x`), and nothing else. It is in
`NeuronCore` because it has no game semantics and because putting it in the executable would
mean a second one when `GameLogic` needs it — which is the situation the rule was written to
prevent. That is a library gaining a responsibility and takes a decision record (§14, slice 1).

The effect seeds a generator **per explosion** from the dead ship's handle and the snapshot tick.
The same ship dying at the same tick shatters the same way, which is what a replay of a recorded
match will want and costs nothing now. The effect's generator is not the simulation's and never
will be: presentation randomness is allowed to differ between two clients watching one match.

---

## 11. Textures

| File | Size | Format in tree | Used by | Sampler |
|---|---|---|---|---|
| `Particle.dds` | 16×16 | BGRA8, alpha all 255 | every sprite | point mag, clamp |
| `ShapeWireframe.dds` | 128×128 | BGRA8, alpha 0 except white lines | fragment decal | linear, wrap |
| `Starburst.dds` | 128×128 | BGRA8, white with alpha gradient | **not drawn** — loaded and slotted for the flash in §12 | linear, clamp |

All three are already in `Outpost/Assets/Textures/`, already deployed as `<Image>` items in
`Outpost.vcxproj`, and referenced by no code. No asset work is due. The composition root passes
the three file names in `FxRenderer::Desc`, the same way it passes fonts to `TextRenderer`.

No mips: the files have one, the tree generates none, and at 128 texels across a fragment a few
metres wide there is nothing to alias. Stated as an assumption in slice 3; if the wire lines
shimmer at distance, mip generation is a slice of its own.

---

## 12. Deliberately left out

- **Sound, damage, and any simulation of death.** Out of scope by the owner's decision and the
  rulebook; the trigger in §9 is what stands in.
- **A destroy event on the wire.** The client infers the despawn. If a later design carries a
  cause of death, the same code consumes it.
- **The `Starburst.dds` one-frame flash.** The source document's §8 lists it as an improvement,
  not part of the effect. The texture is loaded so adding it is a `Build` of six vertices, but
  it is not in any slice.
- **Frustum culling of particles.** No frustum type exists and the counts do not need one.
- **Fragments keeping the ship's texture.** Hulls are not textured; the vertex colour *is* the
  panel colour. Option B in the source document has nothing to apply to.
- **GPU simulation.** Five simultaneous deaths is a few thousand vertices a frame; the CPU
  build is the right size for it.
- **Gravity, ground rest, bounce.** Zero-g by decision; the `gravity` field is the door.
- **The `Fire` type** (lingering flames). The source marks it optional; it is a table row if
  wanted.
- **Time-scale coupling.** Keys 1/2/3 scale the simulation, not the frame, and the effect ages
  in real time like rings and markers do. An explosion in slow motion is a later choice.

---

## 13. Decisions taken before writing

Put to the owner on 2026-08-29 and answered as follows; each was the recommended option.

| Question | Decision | What lost |
|---|---|---|
| What triggers the effect, when nothing can kill a ship? | Client-side despawn detection plus a debug key (§9) | A real destroy event on the wire — touches `GameLogic`, the format and an ADR for a visual slice; and a purely visual key that leaves the ship alive — proves nothing about the real path |
| Zero-g or the ground plane's gravity? | Zero-g, as the source (§5.3) | Light gravity and ground rest — more code, and the game has not decided whether it is in space |
| A textured particle pipeline, or reuse `DrawGlow`? | New pipeline in `NeuronClient` (§8) | Reusing the glow — no darkening blend, so no smoke, and the supplied textures unused |
| Where does the RNG live? | Seeded PCG32 in `NeuronCore`, with a record (§10) | A private generator in the executable — a second one the day `GameLogic` needs its own |

Two smaller ones were settled the same day, so nothing in this design is open:

| Question | Decision | What lost |
|---|---|---|
| Which key despawns the selected ships? | **F4**, beside F3's camera shake | Delete (an editing key), F5 |
| What colour is a hull fragment? | **The drawn hull's mix**: `lerp(SHIP_COLOUR, vertexColour, SHIP_MATERIAL_MIX)` at spawn, so a shard is exactly the colour of the panel it came from | The raw panel colour (reads more saturated than the hull did); a hot tint constant (a look the owner did not ask for) |

---

## 14. Slices

Four, in dependency order. Each is one layer, so slices 2 and 3 — both `NeuronClient` — cannot run
in parallel with each other; 1 and 4 are in other layers and could overlap their neighbours if
the interfaces are agreed first, but nothing forces it and the tree is small enough to go in
order.

| # | Slice | Layer | Depends on | Status | Work order |
|---|---|---|---|---|---|
| 1 | `Pcg32`, tests, ADR 0012 | `NeuronCore` | — | landed | [slice 1](Archive/SpaceshipExplosion-slice-1.md) |
| 2 | `FxVertex`, `MeshShatter`, `SpriteParticles`, tests | `NeuronClient` | 1 | landed | [slice 2](Archive/SpaceshipExplosion-slice-2.md) |
| 3 | `FxRenderer`, `UploadColourTexture`, two shader pairs | `NeuronClient` | 2 | landed | [slice 3](Archive/SpaceshipExplosion-slice-3.md) |
| 4 | `ShipExplosion`, the trigger, F4, `EXPLOSION_*` tuning | `Outpost` | 3 | landed | [slice 4](Archive/SpaceshipExplosion-slice-4.md) |

The record slice 1 wrote is **0012**, not the 0011 this table asked for: 0011 was taken by the NMO
record between this document being written and slice 1 landing, and records are never renumbered.

Slice 3 is the only one without a test that can decide it; its acceptance is a screenshot of a
known vertex list drawn through each pipeline, from slice 4's debug key. Slices 1 and 2 are
decided by tests. Slice 4 is decided by a screenshot sequence and by what it does not change:
`GameLogicTests` unchanged, no `GameLogic` file touched.

One decision record is due, in slice 1. If slice 3 finds the scene root signature is worth
widening after all, or slice 4 finds the effect wants to age on simulation time, each is a record
in that slice.
