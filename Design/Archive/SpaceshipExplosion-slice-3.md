# Work order — Spaceship explosion slice 3: `FxRenderer`

Implements slice 3 of [`SpaceshipExplosion.md`](SpaceshipExplosion.md) §14: the D3D12 side of the
effect — three textures, two static samplers, three pipelines, a per-frame vertex ring, and three
draw calls that take the `FxVertex` lists slice 2 builds.

**Layer:** `NeuronClient`. No test suite change: nothing in it can run without a device, and
the tree has no headless renderer test (AGENTS.md §2, "Nothing depends on the executable").
**Depends on:** slice 2 (`FxVertex`).
**Blocks:** slice 4, which is the first caller.

---

## 1. What this slice is for

Everything the effect needs a GPU for, in one object, so that slice 4 in the executable is a
`Build` into a vector and a `Draw` of the result. Design §8 is the specification; this document
is the interface and the list of places a D3D12 slice in this tree goes wrong.

The one thing this slice cannot do is prove itself: no test suite creates a device. Its
acceptance is a screenshot, taken through slice 4's debug key, of each pipeline drawing a known
list. That is stated in §7 rather than discovered at review.

---

## 2. Scope

### 2.1 `UploadColourTexture` in `GpuHelpers`

Beside `UploadCoverageTexture`, the same function for `DXGI_FORMAT_B8G8R8A8_UNORM` — four bytes
per texel, the row copy sized accordingly, the SRV format matching. Same signature shape, same
staging-outlives-the-copy contract, same comment. **Do not generalise the two into one function
with a format parameter**: the coverage one exists because the overlay draws coverage and says so
at the call site; the colour one exists because a sprite is colour. Two names, two intents.

### 2.2 `NeuronClient/Shaders/`

One `.hlsli` and two VS/PS pairs, all registered as `FxCompile`/`None` items with `ShaderType`,
`HeaderFileOutput` and `VariableName` exactly as the six existing entries in
`NeuronClient.vcxproj:256–285`, plus the `Shaders` filter entries.

**`Fx.hlsli`** — the contract both pairs share:

```hlsl
cbuffer VsConstants : register(b0) { row_major float4x4 viewProj; };
cbuffer PsConstants : register(b1)
{
  float4 lightDirAmbient;   // xyz towards the light, w ambient level -- same meaning as Scene.hlsli
  float4 cameraPos;         // xyz eye
};
Texture2D FxTex : register(t0);
SamplerState WrapLinear : register(s0);
SamplerState ClampPoint : register(s1);

struct VsIn  { float3 pos : POSITION; float3 normal : NORMAL; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct VsOut { float4 clip : SV_Position; float3 worldPos : TEXCOORD0; float3 normal : TEXCOORD1; float4 col : COLOR0; float2 uv : TEXCOORD2; };
```

`row_major` for the same reason `Scene.hlsli` gives at its line 4: it matches `XMFLOAT4X4`
storage.

**`FxFragmentVS.hlsl`**: `clip = mul(float4(pos, 1), viewProj)`; pass the rest through. There is
no world matrix — the vertices are already in world space, that is the whole point of the CPU
build.

**`FxFragmentPS.hlsl`**: design §8.2 verbatim — the decal `lerp(col.rgb, tex.rgb, tex.a)`, the
normal faced to the eye as `ScenePS` does, the same `ambient + (1 − ambient) × lambert`, and
`a = col.a`. Sampled through `WrapLinear`.

**`FxSpriteVS.hlsl`**: identical to the fragment VS. It exists as its own file because the tree
names a shader for the stage it is and the pass it serves, and because the two will diverge the
day sprites move to the GPU; it is ten lines.

**`FxSpritePS.hlsl`**: `return FxTex.Sample(ClampPoint, i.uv) * i.col;` and nothing else. The
fade and the dark-pass zero alpha were baked on the CPU (slice 2, design §7).

### 2.3 `NeuronClient/FxRenderer.h/.cpp`

```cpp
class FxRenderer
{
public:
  static constexpr std::uint32_t MAX_FX_VERTS = 49152;   // design 8.1: five deaths with the pool full

  struct Desc
  {
    std::wstring fragmentTexture;   // ShapeWireframe.dds
    std::wstring spriteTexture;     // Particle.dds
    std::wstring flashTexture;      // Starburst.dds -- loaded and slotted, drawn by nothing yet
  };

  void Init(GpuDevice& _gpu, const Desc& _desc);

  // Binds the heap, the root signature and the frame constants; resets the ring offset. Call once
  // per pass the caller wants -- the scene and decal passes rebind their own state after.
  void Begin(GpuDevice& _gpu, const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _lightDir, float _ambient,
             const DirectX::XMFLOAT3& _cameraPos);

  void DrawFragments(GpuDevice& _gpu, std::span<const FxVertex> _verts);
  void DrawSpritesDark(GpuDevice& _gpu, std::span<const FxVertex> _verts);
  void DrawSpritesAdd(GpuDevice& _gpu, std::span<const FxVertex> _verts);

  [[nodiscard]] bool Ready() const noexcept;            // every texture loaded
  [[nodiscard]] std::uint32_t DroppedVerts() const noexcept;   // since the last Begin on frame 0 of the ring

private:
  void CreatePipelines(GpuDevice& _gpu);
  void LoadTexture(GpuDevice& _gpu, std::uint32_t _slot, const std::wstring& _fileName);
  void Draw(GpuDevice& _gpu, ID3D12PipelineState* _pso, std::uint32_t _srvSlot, std::span<const FxVertex> _verts);

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_fragmentPso, m_spriteDarkPso, m_spriteAddPso;
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap;             // slot 0 fragment, 1 sprite, 2 flash
  GpuPtr<ID3D12Resource> m_textures[3];
  GpuPtr<ID3D12Resource> m_vb[GpuDevice::FRAME_COUNT];
  std::uint8_t* m_vbCpu[GpuDevice::FRAME_COUNT] = {};
  std::uint32_t m_ringOffsetVerts = 0;
  std::uint32_t m_droppedVerts = 0;
  std::uint32_t m_srvStride = 0;
  bool m_ready = false;
};
```

Rules:

- **Init order.** `Init` records three texture uploads into the device's command list and calls
  `_gpu.ExecuteAndWait()` once, then releases the staging buffers — exactly `TextRenderer::Init`'s
  shape (`TextRenderer.cpp:27–36`). It therefore has the same constraint: it runs at boot, before
  the first `BeginFrame`, and the composition root calls it where it calls `m_textRenderer.Init`.
- **A missing texture is a diagnostic, not a throw** (AGENTS.md §5). `LoadTexture` traces and
  leaves the slot empty; `Ready()` is false; every `Draw*` returns without drawing. The game
  boots without the effect rather than not at all.
- **Root signature**: param 0 = 16 root constants at `b0`, VS; param 1 = 8 root constants at
  `b1`, PS; param 2 = one SRV descriptor table (`t0`, one descriptor), PS; two static samplers
  (`s0` `MIN_MAG_MIP_LINEAR`, wrap; `s1` `MIN_LINEAR_MAG_POINT_MIP_POINT`, clamp);
  `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT`. Through `CreateRootSignature(...)` with a name.
- **Input layout** for `FxVertex`: `POSITION` `R32G32B32_FLOAT` @0, `NORMAL` `R32G32B32_FLOAT`
  @12, `COLOR` `R32G32B32A32_FLOAT` @24, `TEXCOORD` `R32G32_FLOAT` @40. The `static_assert` on
  `sizeof(FxVertex) == 48` in slice 2 is what keeps these honest.
- **Three PSOs from `DefaultPipelineDesc()`** with the blend and depth states in design §8.2's
  table, `DSVFormat = DEPTH_FORMAT` on all three (they draw into the scene's depth, unlike the
  overlay). Cull none comes from the default and is right: fragments are double-sided.
- **The ring.** `FRAME_COUNT` upload buffers of `MAX_FX_VERTS × 48` bytes, persistently mapped
  in `Init`. `Begin` sets `m_ringOffsetVerts = 0`. Each `Draw` copies at the offset, advances
  it, and builds its `D3D12_VERTEX_BUFFER_VIEW` from the offset — one buffer, several views.
  A draw that would exceed `MAX_FX_VERTS` is **clipped to whole triangles, the remainder counted
  in `m_droppedVerts`, and traced once per frame** — never silently truncated.
- **`Begin` twice per frame is legal and expected** (design §8.3): it rebinds after the decal
  pass and does *not* reset the ring offset on the second call within a frame. Track the frame
  index: reset the offset when `_gpu.FrameIndex()` differs from the last `Begin`'s.
- **`Draw` sets `OMSetRenderTargets` with the depth view** — the overlay's `Flush` drops it
  (`TextRenderer.cpp:256`) and if the text pass ever moves before the effect, the effect must not
  inherit that. `GpuDevice::BeginFrame` binds both, so today this is a no-op restatement; it is
  there for the day it is not.

### 2.4 The umbrella and the project files

`NeuronClient.h` includes `FxRenderer.h` after `SceneRenderer.h`. `FxRenderer.h` includes
`RenderTypes.h`, `GpuDevice.h`, `FxVertex.h`, `<d3d12.h>`, `<span>`, `<string>` itself. Project
and filters gain `FxRenderer.h/.cpp` (`Render`), the four `.hlsl` (`Shaders`, as `FxCompile`),
and `Fx.hlsli` (`Shaders`, as `None`). Nothing under `CompiledShaders/` is committed.

---

## 3. Out of scope

- **Calling it.** No file in `Outpost` changes. Slice 4 wires `Init`, `Begin` and the three
  draws.
- **Mip generation.** The three textures have one mip and the tree generates none; the linear
  sampler's `MIP` mode is irrelevant with one level. If wire lines shimmer at distance, that is
  a slice of its own, and the pull request says the assumption was made.
- **Drawing `Starburst.dds`.** Loaded into slot 2 so the flash in design §12 is a `Build` away;
  no entry point draws it.
- **Touching the scene root signature or any existing PSO.** Design §8.1 says why the fx pass
  has its own; changing `SceneRenderer` here would be the drive-by AGENTS.md §7 forbids.
- **A general texture library or SRV allocator.** Three fixed slots, like `TextRenderer`'s ten.
  The day a fourth renderer wants a heap is the day a shared one is designed; not this one.
- **Sorting, culling, instancing, indexed quads.** The tree draws unindexed triangle lists
  everywhere and the counts here do not argue for a change.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronClient/TextRenderer.h/.cpp` | The per-frame persistently-mapped ring (`Init` 16–24, `Flush` 236–275), the SRV heap and `SrvHandle`, `ExecuteAndWait` then `DiscardStaging` at boot, the static-sampler root signature (`CreatePipeline`) |
| `NeuronClient/SceneRenderer.cpp` | `CreateScenePipeline` / `CreateDecalPipelines` — the PSO idiom from `DefaultPipelineDesc()`, the blend and depth fields per pass, `check_hresult` + `IID_PPV_ARGS(x.put())` |
| `NeuronClient/GpuHelpers.h/.cpp` | `HeapProps`, `BufferDesc`, `Transition`, `CreateRootSignature`, `DefaultPipelineDesc`, and `UploadCoverageTexture`, which `UploadColourTexture` is a copy of with the format changed |
| `NeuronClient/DdsImage.h` | `Load`, `TopMipAsBgra` — the three files are BGRA8 with one mip and load through the existing reader unchanged (design §11) |
| `NeuronClient/ScreenImage.cpp` | The 26-line load-and-upload sequence, which `LoadTexture` mirrors with the colour helper |
| `NeuronClient/Shaders/Scene.hlsli`, `ScenePS.hlsl` | `row_major`, the cbuffer layout, the eye-facing normal and the lighting line the fragment shader reproduces |
| `NeuronClient/Shaders/Text.hlsli` | A shader that declares a texture and a sampler register |
| `NeuronClient.vcxproj:256–291` | The `FxCompile` item shape, per-file settings only; per-config settings are already in the `ItemDefinitionGroup`s |
| AGENTS.md §3 (Shaders), §5 (COM, errors) | `<Name>VS.hlsl` / `<Name>PS.hlsl` / `<Name>.hlsli`, `g_p<Name>`, `winrt::com_ptr` via `GpuPtr`, one error path |

---

## 5. What will surprise the implementer

### 5.1 `put()` asserts the pointer is empty

`GpuPtr` is `winrt::com_ptr`; refilling one that is not null asserts rather than releasing
(AGENTS.md §5). `UploadCoverageTexture` nulls its outputs first for that reason; keep the line in
the colour copy, and never reuse one `GpuPtr` across a loop of `CreateCommittedResource` calls.

### 5.2 The SRV table is per-draw, and heaps are per-command-list

`SetDescriptorHeaps` must be called before `SetGraphicsRootDescriptorTable`, and the overlay pass
sets its *own* heap later in the frame. `Begin` sets ours; each `Draw` sets the table for its
slot. Do not assume the heap survives from the previous frame's `Begin`.

### 5.3 Two `Begin`s, one ring

The offset must not reset on the second `Begin` of a frame or the sprites overwrite the
fragments the GPU has not yet read. Keying the reset on `FrameIndex()` changing is the rule in
§2.3; a test cannot check it, so a comment at the site says why.

### 5.4 The dark blend is `INV_SRC_COLOR`, not `INV_SRC_ALPHA`

`D3D12_BLEND_INV_SRC_COLOR` on `DestBlend`, with `SrcBlend = SRC_ALPHA`. With the vertex alpha at
zero the source contribution vanishes and the frame is darkened by `1 − src.rgb`. Writing
`INV_SRC_ALPHA` here gives a transparent sprite that draws nothing at all, which looks like a
missing texture and sends the implementer to the wrong file. Design §7.

### 5.5 `DepthWriteMask` differs between the fragment PSO and the sprite PSOs

Fragments write depth so a shard can hide the smoke behind it; sprites do not, so overlapping
smoke blends. Both test. Copying one PSO to make the other and forgetting the mask gives smoke
that pops when it crosses a shard.

---

## 6. Decision records due

None, if the fx pass keeps its own root signature as the design says. **If the implementer
finds a reason to widen the scene root signature instead** — a real one, measured — that is a
record, because someone will propose it again and the design already turned it down.

---

## 7. Acceptance

Nothing here can be decided by a test; say so in the pull request and give the evidence that
can decide it instead.

**Builds and boots:**

- Debug|x64 and Release|x64 build clean, FXC included, and the four new `CompiledShaders/*.h`
  are generated and **not committed**.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- The game boots with the three textures present and `Ready()` is true — checked by a trace line
  at `Init`, or by the debug readout (F1) once slice 4 exists.
- The game boots with one texture renamed away; `Ready()` is false, a trace names the file, and
  nothing else changes. Restore the file afterwards; do not commit the rename.

**Draws, screenshot at two window sizes (AGENTS.md §7), taken through slice 4's F4 key if slice 4
is already on a branch, or through a temporary call in `WorldView::Render` that is not
committed:**

- The fragment pipeline drawing a hand-built list — one large triangle in front of the camera
  with `a = 1`, `uv` covering the wireframe texture, panel colour grey — shows the white wire
  lines *replacing* the grey where the texture's alpha is 1, and lit grey elsewhere. At
  `a = 0.5` the whole triangle is half-transparent against the ground.
- The dark pipeline drawing one light-grey sprite over the ground grid visibly **darkens** the
  grid behind it; the additive pipeline drawing one red sprite visibly brightens it.
- A sprite drawn at the hull's depth is hidden by a hull in front of it (depth test on) and two
  overlapping sprites do not z-fight (depth write off).
- The overlay (HUD, event log) still draws on top of everything after the fx pass: the text pass
  was not disturbed.

**A code read, stated in the pull request:**

- Nothing allocates per frame: the ring is mapped once, `Draw` is a `memcpy` and a view.
- The drop path is exercised once by hand (temporarily set `MAX_FX_VERTS` to 64, draw a bigger
  list, see the trace, restore) and the result stated.

---

## 8. Assumptions the implementer may make

- **The three textures are what the tree holds today**: 32-bit BGRA, one mip, alpha channel
  present, sizes 16×16 / 128×128 / 128×128 (design §11, inspected). No colour-key, no palette
  expansion, no format conversion beyond `TopMipAsBgra`.
- **No mips** (see §3).
- **`MAX_FX_VERTS = 49152`** is settled by the budget in design §8.1; do not resize it to fit a
  screenshot.
- **`GpuDevice::BeginFrame` has bound the render target and depth view** before any `Begin`;
  the restatement in `Draw` costs nothing and is allowed to be redundant.
- **The file names arrive from the composition root** in `Desc`, resolved by `FileSys` the way
  fonts and icons are; `FxRenderer` reads no path of its own (AGENTS.md §5: no argv, no
  environment, config structs only).
