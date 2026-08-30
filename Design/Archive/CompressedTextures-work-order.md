# Work order — MMO slice 19: compressed textures and the descriptor allocator

Implements slice 19 of [`MmoScalabilityPlan.md`](MmoScalabilityPlan.md) §6 (finding G4): the
upload path accepts what `DdsImage` already parses — BC formats and mip chains — via a
subresource-walking upload on slice 18's copy queue, and one shared shader-visible descriptor heap
with a free-list allocator replaces the per-pass constant-sized heaps.

**Layer:** `NeuronClient` (+ `Tools/`, one content conversion in `Outpost/Assets`).
**Depends on:** slice 18 (the copy queue). **Blocks:** nothing; slice 20 follows it in the layer.

---

## 1. Why this is a slice

Every texture in the game today is one uncompressed top mip: `TopMipAsBgra` refuses BC input, so
the planet map shimmers at distance (no mips) and costs 4× what BC1 would, and every pass owns a
constant-sized descriptor heap (Text 10, Fx 3, Sky 3, Body 2) where a new texture is a code
change. `DdsImage` has parsed all of it — BC, mip chains, arrays — since it was written; nothing
consumes what it parses. This slice is that consumer.

## 2. Scope

### 2.1 `NeuronClient/DescriptorAllocator.h/.cpp` — one heap, a free list

A shader-visible `CBV_SRV_UAV` heap of `SRV_HEAP_CAPACITY = 256` slots, owned by `GpuDevice` and
handed to passes as a `DescriptorAllocator&`. `Allocate()` returns a slot index (free list, LIFO
reuse — the HandleStore precedent without generations: a descriptor slot is not a lifetime anyone
races); `Free(slot)` returns it; `CpuHandle(slot)`/`GpuHandle(slot)` do the stride arithmetic.
Exhaustion reports and returns `INVALID_SLOT`, and the caller's texture simply is not drawn — a
diagnostic, not a crash, like a missing mesh. The allocation logic is device-free and unit-tested;
the handle arithmetic needs a device and is exercised by the game.

Passes stop creating SRV heaps: `TextRenderer`, `FxRenderer`, `SkyRenderer` and `BodyRenderer`
allocate their slots at Init, bind the shared heap (`GpuDevice::SrvHeap()`), and their root
signatures are untouched — a descriptor table takes a GPU handle at an offset, which is what
`GpuHandle(slot)` is. The bake's transient heap stays its own: it is UAV scratch with a
one-dispatch lifetime, not a texture registry. The RTV/DSV heaps are the device's and stay.

### 2.2 `GpuHelpers` — `UploadDdsTexture`, the subresource walk

```cpp
void UploadDdsTexture(GpuDevice& _gpu, const DdsImage& _image, D3D12_CPU_DESCRIPTOR_HANDLE _srv,
                      GpuPtr<ID3D12Resource>& _outTexture, GpuPtr<ID3D12Resource>& _outStaging);
```

Creates the texture with the image's format, mip count and array size; one staging buffer sized by
`GetCopyableFootprints` over every subresource; rows repacked from the file's pitch to the
footprint's; one `CopyTextureRegion` per subresource, recorded on the **copy queue**
(`BeginCopies`/`SubmitCopies` bracket, no barriers — decay to COMMON, promotion to shader
resource on first sample, the ADR 0044 rules). The SRV states the real mip count, which is the
whole point. Cube maps and volumes are structurally handled by the walk and declared untested —
no asset is one.

`UploadColourTexture` callers move to it: Fx's three, Sky's three, Body's outline, planet map and
ramps. `UploadCoverageTexture` and `TopMipAsBgra` remain for the readers that genuinely need CPU
texels — `CoverageOf` (fonts, icons through `ScreenImage`) inspects channels on the CPU and stays.

### 2.3 `Tools/DdsBake.py` — mips and BC1, offline

Stdlib-only, in `Tools/`'s charter: reads an uncompressed DDS, builds a box-filtered mip chain,
encodes BC1 (no-alpha colour; BC3 where alpha exists), writes a DX10-header DDS. Runtime
compression stays out of scope — this is the content tool that makes the acceptance testable.
`Planet1.dds` is converted with it (1024×526, full chain) and committed; every other texture stays
as authored — 64–128 px surfaces gain nothing from mips at the sizes they draw.

### 2.4 Out of scope

Runtime compression; texture streaming by residency (all-resident continues, now 4× cheaper for
the one converted map); the bake heap; body LOD (slice 20).

## 3. What to build on

`DdsImage`'s subresource array ("already in D3D12 order"); `UploadStaticBuffer`'s copy-queue
pattern and staging-release discipline (`LastCopyFence`); `HandleStore` for the free-list shape;
each pass's `SrvHandle` arithmetic, which becomes the allocator's.

## 4. Acceptance

- `DescriptorAllocatorTests` in `NeuronClientTests`: allocate to capacity, free, LIFO reuse,
  exhaustion reports `INVALID_SLOT`, double-free asserts in debug.
- The BC1 planet map renders: screenshots near and far — the far one is what mips fix.
- `TopMipAsBgra` calls remain only in `CoverageOf` and the tools' test fixtures.
- All four suites green; `CheckProjectFiles.py`, `CheckFormat.py`; Debug|x64 builds; F5 reseeds.
- `Tools/DdsBake.py` has a stdlib self-test beside the NMO ones (encode → parse with `DdsImage`'s
  rules via a golden byte check).
- Memory stated in the PR: the planet map's bytes before and after.

## 5. Assumptions

- BC1's quality is judged by the two screenshots, not by a metric; the encoder is a simple
  min/max-endpoint one and says so.
- A cube map or volume upload is structurally supported and untested — no asset exercises it.
- Descriptor capacity 256 is a constant, not a budget: today's count is 18 slots.
