# 0017 — The tree gains a compute pipeline, and the CPU generator stays as its reference

Status: accepted
Date: 2026-08-29

## Context

Every pipeline in this tree until now has been a graphics pipeline: a vertex shader, a pixel shader,
an input assembler and a render target. The body generator asked for something else. Generating a
planet is a few hundred thousand independent evaluations of the same pure function, which is what a
GPU is for, and `Design/Archive/PlanetRenderer.md` §17 found every stage of it portable.

Adding it is not just a shader. It is a new kind of build item (`FxCompile` with `ShaderType`
`Compute`), a root signature with no input assembler in it, UAV resources and the barriers they
need, and a class of bug — a missing barrier — that is silent on the machine it was written on and
fatal on the next one. That is a library gaining a responsibility, and this is the record of it.

## Decision

`NeuronClient` gains a compute pipeline: `BodyBakeMaxCS` and `BodyBakeCS`, dispatched by
`BodyRenderer::BakeBody` into the same default-heap buffer `UploadBody` fills, drawn afterwards by
the same two passes with no knowledge of which producer made them.

**The CPU builder stays, and stays the reference.** It is not a fallback kept out of timidity: it is
the thing the bake is verified against. `BodyMeshBuilder` and `BodyField` are decidable by
`CppUnitTest`s that run on any machine in milliseconds; a kernel is decidable only by a readback
compared to something, and this is the something. `BODY_BAKE_ON_GPU` selects the producer and both
paths are built.

## Alternatives considered

- **Vertex-shader displacement**, evaluating the height per vertex per frame. No generation step at
  all, no per-body memory, and LOD becomes a bind. Rejected in design §17.2: about 450 000 height
  evaluations per body per pass per frame, for a shape that never changes, and it repeats the ramp
  sample and the dither every frame too. It also cannot produce a per-triangle colour without
  carrying each vertex's two mates.
- **Tessellation or mesh shaders** for continuous LOD. Rejected: it redoes the height per frame like
  the above, fights the per-triangle colour, and mesh shaders need shader model 6.5 and DXC, which
  this tree does not have.
- **Leaving the generator on the CPU.** This was the right answer for five slices and its trigger is
  written down (design §17.4): body counts that outgrow boot, an F5 that stops feeling instant, or a
  body that has to appear mid-session. **None of those is true today** — eight bodies generate in
  about a tenth of a second — so this slice was built to the owner's ask rather than to its trigger,
  and `BODY_BAKE_ON_GPU` is false until a readback on real hardware says it should not be.
- **Deleting the CPU path once the bake works.** Rejected, and it is the alternative most likely to
  be proposed again. It would leave the field with no test that runs without a GPU, and it would
  leave the bake with nothing to be wrong against.

## Consequences

- **The field is no longer decidable by a unit test alone.** It is decidable by one for the CPU
  producer, and by a readback against that producer for the GPU one. A change to the generator now
  has two places to make it and a comparison to re-run; the shader header says so at the top.
- **Two implementations of one algorithm have to be kept in step**, which is the cost this record is
  really about. It is paid for by the readback and by the shader being a line-for-line port, and it
  is the reason the operation order in `BodyBake.hlsli` looks clumsy where it matches C++.
- **Barrier discipline is now load-bearing.** A missing UAV barrier between the two reduction passes
  reads zeros on some hardware and the right answer on others. The debug layer is the test harness
  for this and its messages during a bake are failures, not noise.
- **`BodyParams` is now a contract in two languages.** A `static_assert` on its size guards the C++
  side; nothing but review guards the HLSL side, which is why the two are written next to each other
  and why the block is all `float4` and `uint4` groups.
