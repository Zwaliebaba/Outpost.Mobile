# Work order — Planets and asteroids slice 3: `BodyRenderer`

Implements slice 3 of [`PlanetRenderer.md`](PlanetRenderer.md) §15: the D3D12 side — the
outline texture, one root signature, two pipelines, static body meshes in a default heap, and
the two draw calls that take the `FxVertex` lists slice 2 builds.

**Layer:** `NeuronClient`. No test suite change: nothing in it runs without a device, and the
tree has no headless renderer test.
**Depends on:** slice 2 (`BodyMeshBuilder`, `FxVertex`); `SpaceshipExplosion` slice 3 for
`UploadColourTexture`, or this slice lands it (§2.1).
**Blocks:** slice 4, the first real caller.

---

## 1. What this slice is for

Everything a body needs a GPU for, in one object, so that slice 4 in the executable is a
`Build` into a vector, an `UploadBody`, and two draws a frame. Design §7 is the specification;
this document is the interface and the list of places a D3D12 slice in this tree goes wrong.

It cannot prove itself: its acceptance is a screenshot of a hard-coded body, taken through a
temporary hook this slice adds and slice 4 removes (§2.5).

---

## 2. Scope

### 2.1 `UploadColourTexture` in `GpuHelpers`

If `SpaceshipExplosion` slice 3 has landed, nothing. If not, this slice lands it **exactly as
`SpaceshipExplosion-slice-3.md` §2.1 specifies** — beside `UploadCoverageTexture`, `B8G8R8A8_UNORM`,
four bytes a texel, same staging contract, two names for two intents — and that slice finds it.

### 2.2 `UploadStaticBuffer` in `GpuHelpers`

New, and this design's own:

```cpp
// Records the copy of _bytes into a new DEFAULT-heap buffer and its transition to
// VERTEX_AND_CONSTANT_BUFFER. _outStaging has to outlive the copy, which has only been recorded:
// release it after the list has run. This is the path for a buffer the GPU reads every frame and
// the CPU never touches again; SceneRenderer::UploadMesh's upload-heap shortcut is for a few
// thousand triangles, not seven megabytes read twice a frame (Design/PlanetRenderer.md 7.1).
void UploadStaticBuffer(GpuDevice& _gpu, std::span<const std::uint8_t> _bytes, GpuPtr<ID3D12Resource>& _outBuffer,
                        GpuPtr<ID3D12Resource>& _outStaging);
```

`SceneRenderer::UploadMesh` is **not** changed to use it — that would be the drive-by AGENTS.md §7
forbids, and its comment still argues correctly for hulls.

### 2.3 `NeuronClient/Shaders/`

One `.hlsli`, one VS, two PS, registered as `FxCompile`/`None` items with `ShaderType`,
`HeaderFileOutput` and `VariableName` exactly as the existing entries in `NeuronClient.vcxproj`,
plus the `Shaders` filter entries.

**`Body.hlsli`** — design §7.2's contract, verbatim, with `row_major` for the reason
`Scene.hlsli` gives: it matches `XMFLOAT4X4` storage.

**`BodyVS.hlsl`**: `worldPos = mul(float4(pos, 1), world)`, `clip = mul(worldPos, viewProj)`,
`normal = mul(normal, (float3x3)world)` — bodies spin, so unlike `FxFragmentVS` there is a world
matrix, and the normal goes through its rotation. No inverse-transpose: the world matrix is a
rotation and a translation and slice 4 promises no non-uniform scale (the ellipsoid is baked
into the vertices, design §5.1).

**`BodyPS.hlsl`**: design §7.2 verbatim — Lambert on the *given* normal, no eye-facing flip
(the builder guarantees outward), `ambient + (1 − ambient) × lambert`, alpha 1. Note it uses
`(1 − ambient)` as `ScenePS` does; design §7.2's sketch wrote `ambient + lambert`, and the
scene's form is the one to match so a body and a hull under the same light agree.

**`BodyOverlayPS.hlsl`**: design §7.2 verbatim — `tex.a` is the line mask (not `tex.r`; the
file was inspected), Blinn-Phong with `overlayParams.z` strength and `.w` shininess, the
`fwidth` fade with `overlayParams.y`, gain `overlayParams.x`. Output alpha is `tex.a × fade` so
the additive blend adds nothing where there is no line.

### 2.4 `NeuronClient/BodyRenderer.h/.cpp`

```cpp
using BodyHandle = std::uint32_t;
inline constexpr BodyHandle INVALID_BODY = 0xFFFFFFFFu;

struct BodyOverlayParams
{
  float gain = 1.2f;        // the source overlay's diffuse 1.2
  float fade = 4.0f;        // multiplies length(fwidth(uv)); 4 means the outline is gone once a cell is under a quarter pixel
  float specular = 0.5f;    // the source's 0.5
  float shininess = 40.0f;  // the source's 40
};

class BodyRenderer
{
public:
  struct Desc
  {
    std::wstring outlineTexture;   // TriangleOutline.dds
  };

  void Init(GpuDevice& _gpu, const Desc& _desc);

  // Records the upload; the buffer is usable after the next ExecuteAndWait or EndFrame. Returns
  // INVALID_BODY on an empty list. Handles are indices and stay valid for the run.
  [[nodiscard]] BodyHandle UploadBody(GpuDevice& _gpu, std::span<const FxVertex> _verts);

  // Releases the staging buffers of every UploadBody recorded since the last call. Call after the
  // list that carried the copies has run -- at boot, after ExecuteAndWait.
  void DiscardStaging() noexcept;

  // Binds the heap, the root signature and the frame constants for both passes.
  void Begin(GpuDevice& _gpu, const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _lightDir, float _ambient,
             const DirectX::XMFLOAT3& _cameraPos, const BodyOverlayParams& _overlay);

  void DrawMain(GpuDevice& _gpu, BodyHandle _body, const DirectX::XMFLOAT4X4& _world);
  void DrawOverlay(GpuDevice& _gpu, BodyHandle _body, const DirectX::XMFLOAT4X4& _world);

  [[nodiscard]] bool OutlineReady() const noexcept;   // false: DrawOverlay draws nothing

private:
  void CreatePipelines(GpuDevice& _gpu);
  void Draw(GpuDevice& _gpu, ID3D12PipelineState* _pso, BodyHandle _body, const DirectX::XMFLOAT4X4& _world);

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_mainPso, m_overlayPso;
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap;             // one slot: the outline
  GpuPtr<ID3D12Resource> m_outline, m_outlineStaging;
  std::vector<GpuMesh> m_bodies;                      // GpuMesh: vb, vbv, vertexCount -- RenderTypes.h
  std::vector<GpuPtr<ID3D12Resource>> m_staging;      // until DiscardStaging
  bool m_outlineReady = false;
};
```

Rules:

- **Init order.** `Init` records the outline upload into the device's command list and does
  **not** call `ExecuteAndWait` itself: the composition root calls `UploadBody` for every
  starting body first, then one `ExecuteAndWait`, then `DiscardStaging` — one submission for all
  the copies, which is `TextRenderer::Init`'s shape stretched over two objects. The header
  comment says so; slice 4 follows it.
- **A missing outline is a diagnostic, not a throw.** Trace, `m_outlineReady = false`,
  `DrawOverlay` returns early. The terrain still draws.
- **Root signature**: param 0 = 32 root constants at `b0`, VS (`world` DWORDs 0–15, `viewProj`
  16–31 — the scene's layout, so `SetGraphicsRoot32BitConstants(0, 16, &world, 0)` reads the
  same as `SceneRenderer::DrawMesh`); param 1 = 12 root constants at `b1`, PS; param 2 = one
  SRV descriptor table (`t0`), PS; one static sampler `s0` `MIN_MAG_MIP_LINEAR`, wrap/wrap;
  `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT`. Through `CreateRootSignature(...)` with a name.
- **Input layout** for `FxVertex`: `POSITION` `R32G32B32_FLOAT` @0, `NORMAL` @12, `COLOR`
  `R32G32B32A32_FLOAT` @24, `TEXCOORD` `R32G32_FLOAT` @40. Spelled here even if `FxRenderer`
  spells it too: the two renderers share a vertex, not an array (the day they share one, that
  is a `RenderTypes.h` change with a reason).
- **Two PSOs from `DefaultPipelineDesc()`** with design §7.2's table: main opaque, depth test
  and write, `LESS`; overlay `SRC_ALPHA / ONE`, `SrcBlendAlpha = ONE`, depth test, **no write**,
  `LESS_EQUAL`. `DSVFormat = DEPTH_FORMAT` on both. Cull none from the default (§5.3 below).
- **`UploadBody`** goes through `UploadStaticBuffer`; `GpuMesh::vbv` is built from the default-
  heap buffer with `StrideInBytes = sizeof(FxVertex)`.
- **`Begin`** sets the heap, the root signature, the topology, `viewProj` (DWORDs 16–31 of
  param 0), and the 12 PS DWORDs: `lightDir, ambient | cameraPos, 0 | gain, fade, specular,
  shininess`. Each `Draw` sets `world` (DWORDs 0–15), the PSO, the table, the view, and
  `DrawInstanced`.
- **`Draw` sets `OMSetRenderTargets` with the depth view**, for the reason
  `SpaceshipExplosion-slice-3.md` §2.3 gives: the overlay's `Flush` drops it.

### 2.5 A temporary body, for the screenshot

`OutpostApp` and `WorldView` gain — **every line marked `// slice 3 placeholder, removed by slice 4`**
— a `BodyRenderer` member on `OutpostApp`, its `Init` after `m_textRenderer.Init`, one hard-coded
`BodyDesc` (`seed = 1`, `R = 800`, `gridPower = 6`, one tile of half-width 1.2 rad at `+Y`,
`heightScale = 0.08`, `polarStrength = 0.6`) built with `LandscapeEarth.dds` loaded through
`ColourRamp::Load`, uploaded at boot, and drawn at `(0, 920, 3000)` after the hulls in
`WorldView::Render` with a spin of one turn per 120 s. Every number is a literal in that block,
not a `ViewTuning` constant — slice 4 names them; this is scaffolding and the pull request says
so.

### 2.6 The umbrella and the project files

`NeuronClient.h` includes `BodyRenderer.h` after `SceneRenderer.h`. Project and filters gain
`BodyRenderer.h/.cpp` (`Render`), the three `.hlsl` (`Shaders`, `FxCompile`) and `Body.hlsli`
(`Shaders`, `None`). Nothing under `CompiledShaders/` is committed.

---

## 3. Out of scope

- **Content.** The placeholder in §2.5 is the only body and it is removed by slice 4. No
  catalogue, no `BODY_*` constant.
- **The ocean.** Slice 5; the placeholder body is dry (`outsideHeight = 0.01`).
- **Mip generation.** Design §6.3: the `fwidth` fade stands in. If the screenshot shows the
  outline sparkling before it fades, that is the stated fallback and a slice of its own.
- **Touching `SceneRenderer`, its root signature, or `UploadMesh`.**
- **Instancing, indexing, LOD, culling by frustum.** Design §13.
- **The compute bake.** Slice 6; `UploadStaticBuffer` is the buffer it will write and nothing
  more is prepared here.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronClient/SceneRenderer.cpp` | `CreateScenePipeline` / `CreateDecalPipelines` — the PSO idiom, the 32-DWORD VS constant layout, `DrawMesh`'s `SetGraphicsRoot32BitConstants(0, 16, &world, 0)` |
| `NeuronClient/TextRenderer.h/.cpp` | The SRV heap, the static-sampler root signature, `ExecuteAndWait` then `DiscardStaging` at boot |
| `NeuronClient/GpuHelpers.h/.cpp` | `HeapProps`, `BufferDesc`, `Transition`, `CreateRootSignature`, `DefaultPipelineDesc`, `UploadCoverageTexture` (the model for both new helpers) |
| `NeuronClient/ScreenImage.cpp` | The load-and-upload sequence for one texture |
| `NeuronClient/Shaders/Scene.hlsli`, `ScenePS.hlsl` | `row_major`, the cbuffer layout, the lighting line `BodyPS` reproduces |
| `Design/SpaceshipExplosion-slice-3.md` §5 | The five surprises — `put()` asserts, heaps per list, the depth view — all apply here unchanged |
| `Design/PlanetRenderer.md` §7, §6.3 | The passes, the shaders, the alpha-mask rule, the fade |

---

## 5. What will surprise the implementer

### 5.1 The normal is not flipped to the eye, and that is deliberate

`ScenePS` faces the derivative normal to the camera because OBJ import reverses winding.
`BodyPS` must not: the builder's normal points outward, and flipping it to the eye would light
the far side of the sphere as if it were the near side. With cull none the far side is
rasterised and depth-rejected; the frames where it is not (the first pixel of a triangle
before the near side is drawn) are invisible.

### 5.2 The overlay's alpha is the line

`tex.rgb` is white everywhere. Multiply by `tex.a` or the whole body glows.

### 5.3 Cull none costs a full second sphere

At 49 k triangles per planet, the far side is 25 k triangles rasterised for nothing. Measure
with `D3D12_CULL_MODE_BACK` and with `NONE` on the placeholder and write both numbers in the
pull request. If back-face culling is correct (nothing disappears — the builder's winding is
consistent by construction, §2.3 of slice 2), switch both PSOs to it and say so; if a face
vanishes, leave `NONE` and file the winding as slice 5's first job.

### 5.4 `viewProj` is set in `Begin`, `world` per draw, on the same root parameter

Exactly as `SceneRenderer` does: offsets 16 and 0 of parameter 0. Setting `world` with the
wrong offset overwrites `viewProj` and the body vanishes without an error.

### 5.5 The staging buffers must outlive the copy

`UploadBody` records; nothing has run. Releasing the staging buffer before `ExecuteAndWait` is
a use-after-free the debug layer reports as a removed device, at `Present`, in the next frame.
`DiscardStaging` exists so the composition root cannot get this wrong by accident.

---

## 6. Decision records due

None. The body pass has its own root signature for the reason the fx pass does, already turned
down in the explosion design. **If the implementer finds a reason to widen the scene root
signature instead** — measured — that is a record.

---

## 7. Acceptance

Nothing here can be decided by a test; say so in the pull request and give the evidence that
can.

**Builds and boots:**

- Debug|x64 and Release|x64 build clean, FXC included; the four `CompiledShaders/Body*.h` are
  generated and **not committed**.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- `NeuronClientTests` still passes (slices 1–2 untouched).
- Boot with `TriangleOutline.dds` renamed away: `OutlineReady()` false, a trace names the file,
  the terrain still draws. Restore the file; do not commit the rename.

**Screenshots at two window sizes (AGENTS.md §7) of the §2.5 placeholder:**

- From the default camera: a faceted sphere over the grid, green lowlands, grey slopes, white
  at the summits and at both poles, visibly spinning between two frames ten seconds apart.
- Zoomed to 40 m at the body's nearest point: the triangle outline visible as glowing edges,
  one tile per facet, brighter where the sun catches it (the specular term).
- Zoomed to 900 m: the outline has faded out and the facets do not sparkle.
- The terrain occludes a hull ordered behind it; a selection ring on the ground is not drawn
  through the body; the HUD draws over everything.

**Measured, stated in the pull request:** frame time with the placeholder present and absent,
and with cull `NONE` and `BACK` (§5.3), Release|x64, window size named.

---

## 8. Assumptions the implementer may make

- **`TriangleOutline.dds` is 128×128 BGRA, one mip, lines in alpha** (inspected). No mips
  (design §6.3).
- **The world matrix is rotation and translation only** — the ellipsoid is in the vertices —
  so the normal transform is the matrix's upper 3×3.
- **The placeholder's numbers** are literals and are not tuned; they exist to put something on
  screen.
- **`FRAME_COUNT`-deep ring buffers are not needed**: body meshes are static. Nothing here is
  written per frame.
- **The file name arrives from the composition root** in `Desc`, resolved by `FileSys`;
  `BodyRenderer` reads no path of its own.
