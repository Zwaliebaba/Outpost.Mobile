# 0046 — GpuDevice gains a copy queue, and a render handle gains a generation

Status: accepted
Date: 2026-08-30

## Context

Two findings, one slice, and they turn out to be the same shape twice.

**G3, the uploader.** `GpuDevice::BeginUploads` reset allocator 0 — which is also frame 0's — so it
had to `WaitForGpu()` first, because resetting an allocator the GPU is still reading is undefined
behaviour. That drain is free at boot, where nothing is in flight, and is the cost of every
mid-session load: F5 regenerates every body, and the bracket it opens waits for the whole pipeline
to empty before the first byte moves.

**G3, the store.** `MeshHandle` and `BodyHandle` are `std::uint32_t` array indices, and
`SceneRenderer::UploadMesh` and `BodyRenderer`'s two producers only ever `push_back`. Nothing could
say a mesh was finished with, so nothing was ever released: `OutpostApp.cpp` said so beside the F5
key — "what it costs is the memory of the scene it replaces, since BodyRenderer keeps every handle
for the run". Ten presses is ten scenes on the GPU.

## Decision

**`GpuDevice` gains a `D3D12_COMMAND_LIST_TYPE_COPY` queue** with its own allocator, list and fence,
bracketed by `BeginCopies`/`SubmitCopies`. `SubmitCopies` signals the copy fence and makes the
*graphics* queue wait on it; **the CPU is not blocked**. `UploadStaticBuffer` — every hull, every
body mesh, the sky — records there.

**No resource barrier is written on either side of a copy on that queue, and none may be.** A buffer
created in `COMMON` is promoted to `COPY_DEST` implicitly on first access; everything a copy queue
touches decays back to `COMMON` when its `ExecuteCommandLists` completes; and the first graphics use
promotes it to `VERTEX_AND_CONSTANT_BUFFER` for free. The barrier that used to follow the copy is
not merely unnecessary now, it is illegal: a `COPY` list supports `COMMON`, `COPY_DEST` and
`COPY_SOURCE` and nothing else. This is documented behaviour, not an inference — "Using Resource
Barriers to Synchronize Resource States in Direct3D 12", *Implicit State Transitions*.

**`BeginUploads` keeps its direct-queue bracket and gains an allocator of its own**, so it waits for
the previous upload batch rather than for every frame in flight.

**`HandleStore`** is the bookkeeping behind a render handle: which slots are live, which are free,
and which generation each is on. A handle is a slot and a generation packed into the 32 bits
`MeshHandle` already was — sixteen bits each. `SceneRenderer` and `BodyRenderer` index their payload
arrays *by slot*, resolve *by handle*, and gain `FreeMesh`, `FreeBody` and `FreeAllBodies`. F5 calls
the last of these, so the scene it replaces is released.

## Alternatives considered

- **Move the whole upload bracket to the copy queue**, which is what the plan's slice sentence asks
  for. It does not survive contact with what the bracket carries: `BodyRenderer::BakeBody` dispatches
  a *compute shader*, `UploadCoverageTexture` and `UploadColourTexture` transition to
  `PIXEL_SHADER_RESOURCE`, and `ReadBackBody` transitions to `COPY_SOURCE` from a graphics state. A
  copy queue can do none of the three. So the bracket splits by *what the work is* rather than
  moving whole, and the direct one stays for bakes and readbacks.
- **Move the texture uploads too.** They are entangled with the bake that reads the ramp texture on
  the direct queue, so ordering them correctly means untangling that first — and slice 19 rewrites
  that path anyway for BC formats and mip chains, "via a subresource-walking upload on slice 18's
  copy queue". The queue is here; the textures arrive on it when their upload is rewritten. Named
  here so the gap is a decision rather than an oversight.
- **Widen `MeshHandle` to 64 bits** and carry the generation beside the slot, as `ShipHandle` does.
  Rejected because a `MeshHandle` is a `std::uint32_t` in a dozen places — `ShipView::mesh`, the hull
  table, `INVALID_MESH`, `std::array<MeshHandle, HULL_COUNT>` — and sixteen bits each is 65,535 live
  meshes against a tree that has eleven hulls and eight bodies, and 65,535 reuses of one slot before
  a generation wraps against an F5 key somebody presses by hand.
- **Free the resource where the handle is freed.** The obvious shape, and wrong: the GPU may still be
  drawing with it. Retired buffers go on a list that `DiscardStaging` clears, which is the call that
  already means "the bracket has run" and is already called at exactly the right points.
- **Reference-count the meshes.** Correct and larger. Nothing in this tree shares a mesh between two
  owners: the hull table owns one per hull for the run, and a body is owned by the scene that baked
  it. A free list is the smallest thing that closes the leak, and a count can be added the day two
  owners exist.
- **Let `Free` shrink the payload array.** Rejected for `World`'s reason: a slot that has been used
  once is the cheapest to use again, and shrinking would move every element after it, invalidating
  the slot every live handle resolves to.

## Consequences

- **A load no longer waits for a frame.** `BeginCopies` waits on the copy fence alone; a hull mesh
  can start uploading while the graphics queue is still drawing. `SubmitCopies` does not block the
  CPU at all.
- **Copies must be submitted before the direct bracket executes.** `SubmitCopies` enqueues the
  graphics queue's `Wait`, and work submitted *before* that call is not behind it. The composition
  root's three load sites therefore read `BeginCopies; BeginUploads; …; SubmitCopies;
  ExecuteAndWait;` and say so.
- **`SceneRenderer::Init` waits for its own copy**, because the unit quad is drawn by the first frame
  and `DiscardStaging` releases the staging buffer behind it. Boot is the one place where blocking
  costs nothing.
- **`BodyCount()` means live bodies now**, not slots; `BodySlotCount()` is the high-water mark and is
  the number ten F5 presses must not move.
- **A stale handle draws nothing** instead of drawing whatever took its slot. That was the failure
  mode a bare index had and nothing had hit yet, because nothing ever freed.
- **`HandleStore` holds no D3D12**, which is what let it be tested without a device: eleven rows in
  `NeuronClientTests` cover allocation, reuse, staleness, a fabricated handle on a freed slot, the
  double free, the reserved slot at the store's cap, the generation wrap driven to 70,000 cycles,
  and ten clear-and-refill cycles holding the slot count flat.
- **The frame-time measurement this slice's acceptance asks for is not in this commit.** It needs
  Windows, a GPU and a mid-session bake, and none of the three exists where this was written. What
  is measured is the part that could be: the store's bookkeeping, by test and by mutation.
