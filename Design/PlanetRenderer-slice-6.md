# Work order — Planets and asteroids slice 6: the compute bake

Implements slice 6 of [`PlanetRenderer.md`](PlanetRenderer.md) §15 and §17.4: the same
`FxVertex` stream slice 2 builds, produced by a compute shader into the default-heap buffer
slice 3 draws from, with the CPU builder kept as the reference it is verified against.

**Layer:** `NeuronClient`, plus one constant and one branch in `Outpost`.
**Depends on:** slice 5 (so both producers cover the ocean rules).
**Blocks:** nothing.
**Cut when** (design §17.4): the starting scene outgrows what boot generates in the time a
player will wait, F5 stops feeling instant, or a body first has to appear mid-session. Until one
of those is true this order is not started.

---

## 1. What this slice is for

Design §17 found every stage of the generator GPU-portable and chose a one-time compute bake
over per-frame displacement. The CPU path stays: it is the thing this slice is *checked
against*, byte for byte where the maths is integer and to a tolerance where it is float. The
slice is the first compute pipeline in the tree, and most of this document is the list of ways
a first compute pipeline goes wrong.

---

## 2. Scope

### 2.1 `NeuronClient/Shaders/`

Three files, registered as `FxCompile` items with `ShaderType` **`Compute`** — the first in the
tree — and the `.hlsli` as `None`.

**`BodyBake.hlsli`**: `cbuffer Params : register(b0)` laid out **field for field as
`BodyParams`** (slice 1 §2.4 — that struct was shaped for this); the `Noise3` gradient function
ported from `Noise3.h` with the permutation read from the cbuffer; `CellHash` ported from
`BodyMeshBuilder` (integer, bit-identical); `Direction` ported from `CubeSphere.h`; the height,
climate and colour functions ported from `BodyField.cpp` and `BodyMeshBuilder.cpp` **in the same
operation order**, so float results agree to a few ULPs. `Texture2D Ramp : register(t0)` with a
static bilinear clamp sampler, and `RWStructuredBuffer<FxVertexGpu> Out : register(u0)` where
`FxVertexGpu` is twelve floats in `FxVertex` order.

**`BodyBakeMaxCS.hlsl`**: one thread per sample (`[numthreads(64,1,1)]`, dispatch
`ceil(6·N·N / 64)`), evaluates each tile's raw octaves at that sample and the finished height,
and `InterlockedMax`es the **order-preserving `uint` image** of each float
(`asuint(f) ^ ((asint(f) >> 31) | 0x80000000)`) into `RWBuffer<uint> Maxima : register(u1)`,
`MAX_TILES + 1` entries. This is the reduction design §17.1 named; it replaces the constructor
loop in `BodyField`.

**`BodyBakeCS.hlsl`**: one thread per **cell** (`[numthreads(64,1,1)]`, dispatch
`ceil(6·(N−1)·(N−1) / 64)`), reads `Maxima`, reproduces slice 2's and slice 5's builder for its
two triangles — sample four corners plus the two neighbours, cull to degenerates (three equal
vertices at the origin) when all six are `≤ 0` and wet, shore dip, normal flipped outward,
colour from `Ramp.SampleLevel`, dither from `CellHash`, uv the cell — and writes six
`FxVertexGpu` at `cellIndex × 6`.

### 2.2 `NeuronClient/BodyRenderer`

```cpp
// Records the bake of one body into a new default-heap buffer: two dispatches, a UAV barrier,
// and a transition to VERTEX_AND_CONSTANT_BUFFER. Usable after the list runs, like UploadBody.
[[nodiscard]] BodyHandle BakeBody(GpuDevice& _gpu, const BodyParams& _params, const ColourRamp& _ramp, const DirectX::XMFLOAT3& _oceanColour);

// Debug only: copies a baked body back to the CPU through a READBACK heap and ExecuteAndWait.
// Slow, blocking, and the whole of this slice's acceptance.
void ReadBackBody(GpuDevice& _gpu, BodyHandle _body, std::vector<FxVertex>& _out);
```

- **A compute root signature**: param 0 = CBV (`b0`, the `BodyParams` in an upload-heap
  constant buffer, 256-byte aligned); param 1 = a descriptor table with one SRV (`t0`, the
  ramp as a 64×64 BGRA texture through `UploadColourTexture`) and two UAVs (`u0` the vertex
  buffer, `u1` the maxima); one static sampler. `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` is **not**
  set — this is not a graphics root signature.
- **Two compute PSOs** through `D3D12_COMPUTE_PIPELINE_STATE_DESC`.
- **The vertex buffer** is created in the default heap with
  `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` in state `UNORDERED_ACCESS`, and transitioned to
  `VERTEX_AND_CONSTANT_BUFFER` after the bake. `UploadStaticBuffer`'s buffers do not carry the
  UAV flag; the two paths create their buffers differently and draw them identically.
- **The ocean** still comes from the CPU builder (`MeshVertex`, 3 072 triangles); baking it
  buys nothing.
- **Per-body ramp SRV**: a small heap of `BODY_CLASS_COUNT` slots or one slot rewritten per
  bake with a fence between — the second, at boot, is simpler and the order allows it.

### 2.3 `Outpost`

`ViewTuning.h` gains `inline constexpr bool BODY_BAKE_ON_GPU = true;`. `SpawnStartingBodies`
branches on it: `BakeBody(field.Params(), ramp, oceanColour)` or the slice 4 path. The F1
readout's generation time now shows the CPU time of the branch taken, which for the GPU path is
the record time, not the bake time — the line says "gen ms (record)" so nobody reads it as the
GPU cost.

A second debug key is **not** added; the comparison in §7 runs from a temporary call that is
not committed.

### 2.4 Project files

`NeuronClient.vcxproj` gains the two `Compute` `FxCompile` items and the `.hlsli`; the
per-configuration `FxCompile` `ItemDefinitionGroup`s already set `ShaderModel` 5.1 and are not
touched.

---

## 3. Out of scope

- **Removing the CPU builder.** It is the reference and the test subject. Design §17.2.
- **Baking asynchronously across frames.** The bake is recorded and waited on at boot exactly
  as an upload is. Overlapping it with frames is the slice that follows the day it is wanted,
  and is what `ExecuteAndWait` at boot is standing in for.
- **Baking mid-session.** Enabled by this slice, not done by it; nothing spawns a body after
  boot yet.
- **`ExecuteIndirect`, append buffers, compaction.** Degenerates are free.
- **LOD.** Three bakes per body is a loop, but nothing selects between them yet.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronClient/BodyParams.h` (slice 1) | The cbuffer layout, already 16-byte packed |
| `NeuronClient/Noise3.h`, `CubeSphere.h`, `BodyField.cpp`, `BodyMeshBuilder.cpp` (slices 1, 2, 5) | The functions the HLSL ports, line by line |
| `NeuronClient/BodyRenderer.cpp` (slice 3) | The root-signature and PSO idioms, `UploadStaticBuffer`, the heap |
| `NeuronClient/GpuHelpers.cpp` | `HeapProps`, `BufferDesc`, `Transition` |
| `NeuronClient/GpuDevice.h` | `ExecuteAndWait`, `CommandList` |
| `Design/PlanetRenderer.md` §17 | The analysis, the shape, the acceptance rule |

---

## 5. What will surprise the implementer

### 5.1 Operation order is the tolerance

`a + b + c` and `a + (b + c)` differ in the last bit, and six octaves of that is a few ULPs of
height, which is `1e-4 R` of position at worst. The acceptance tolerance is set for that. If a
readback differs by more, the HLSL reordered something — usually the compiler folding a
`pow(len·10, fd)` that the CPU computes per octave. Match the C++ line for line before
suspecting the tolerance.

### 5.2 `InterlockedMax` on a float is `InterlockedMax` on its ordered `uint`

Positive floats order as their bits; negatives order backwards. The xor trick in §2.1 fixes
both, and the CPU side decodes with the inverse. Getting this wrong makes every tile's maximum
come back as its most negative sample, which rescales the continent upside down and looks like
a noise bug.

### 5.3 The cbuffer must match `BodyParams` to the byte

A `float3` followed by a `float` packs into one `float4` in HLSL; a `float3` followed by a
`float3` does not. `BodyParams` is all `float4` and `uint4` groups for that reason; do not
"tidy" it. `static_assert(sizeof(BodyParams) == <the HLSL size>)` in the renderer with the
number written out.

### 5.4 UAV to vertex buffer is a state transition, and a barrier between the two dispatches

`Maxima` is written by the first dispatch and read by the second: a `UAV` barrier on it between
them, or the second reads zeros on some hardware and the right values on the implementer's.
Then `UNORDERED_ACCESS → VERTEX_AND_CONSTANT_BUFFER` on the vertex buffer before any draw.

### 5.5 The debug layer is the test harness here

Enable it (`GpuDevice` already tries) and treat every message during the bake as a failure.
Compute paths are where missing barriers are silent on one GPU and fatal on another.

---

## 6. Decision records due

One: **the tree gains a compute pipeline**. A new kind of build item (`Compute` `FxCompile`)
and a new resource pattern (UAV buffers) are a library gaining a responsibility (AGENTS.md
§9). Context, the alternatives from design §17.2, and the consequence that the field is no
longer decidable by a `CppUnitTest` alone but by readback against the CPU reference.

---

## 7. Acceptance

**The readback comparison, run from a temporary call in `OutpostApp::Init` that is not
committed, for the terran planet and one asteroid of `BODY_START_SEED`:**

- Triangle count equal (degenerates counted as culled on the GPU side, by the three-equal-
  vertices test).
- Every position and normal within `1e-4 × R` and `1e-4` respectively of the CPU builder's.
- Every colour equal to within `1/255` — the dither hash is integer and the ramp filter is the
  same bilinear, so a larger difference is a bug, not a tolerance.
- Every uv bitwise equal.
- The numbers written into the pull request: max position error, max normal error, max colour
  error, triangle counts both sides.

**Screenshots at two window sizes:** the slice 4/5 starting scene with `BODY_BAKE_ON_GPU`
true and false, indistinguishable by eye.

**Measured:** boot time and F5 latency with each producer, Release|x64; the bake's GPU time from
a timestamp query pair if the implementer adds one locally (not committed), else the wall time
of `ExecuteAndWait`.

**The tree:** the checks pass; Debug|x64 and Release|x64 build; no debug-layer message during
boot; `NeuronClientTests` unchanged; the decision record exists and is indexed; design §15
marks slice 6 `landed`; this file moves to `Design/Archive/`.

---

## 8. Assumptions the implementer may make

- **`BODY_BAKE_ON_GPU` defaults to `true` once the comparison passes**; the CPU path is kept
  buildable and is exercised by the test suite, not by boot.
- **The ramp is uploaded once per class** at boot; runtime biome changes are still out.
- **A 64×64 ramp `SampleLevel` at mip 0 with bilinear clamp** is the same filter `ColourRamp`
  implements; the test in slice 2 pinned that filter's midpoint rule so the two agree.
- **`ExecuteAndWait` at boot is the synchronisation**; no fence work beyond what exists.
