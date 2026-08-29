# 0020 — The compute bake is the producer, and three silent defects had to go first

Status: accepted
Date: 2026-08-29

## Context

`0017` landed the compute pipeline and left `BODY_BAKE_ON_GPU` false, because the readback
comparison its own work order asks for "needs a GPU and has not happened". It has now happened, on
an RTX 3070 Ti Laptop. The kernels did not agree with the builder, and what they produced was not
subtly wrong: the wet world baked 49 152 cells and every one of them was a degenerate, so the
planet was not drawn at all, and the dry bodies came out with positions up to 0.38 of a radius from
the builder's.

Three defects, and the shape of each is worth keeping, because all three were invisible to the
compiler, to the debug layer, and to `NeuronClientTests`:

1. **`BodyField::ParamsFor` left `octaveAmplitude` zero.** The amplitudes are a pure function of a
   tile's own numbers, but they were filled in `MeasureTiles`, which runs in `BodyField`'s
   constructor. The bake takes what `ParamsFor` returns and never builds a `BodyField`, so every
   octave was multiplied by zero: no terrain, every tile peaking at exactly its seeded zero, every
   tile scaling to nothing, and a smooth sphere at `outsideHeight` whose entire surface the
   sea-level rule then culled. The reduction and the kernels were innocent; the block handed to
   them was empty.
2. **`BakeBody` seeded the maxima through `UploadStaticBuffer`.** That helper ends by transitioning
   what it filled to `VERTEX_AND_CONSTANT_BUFFER` — correct for the vertices it was written for,
   wrong for a buffer whose next use is as a copy *source*. The debug layer rejected the copy once
   per body and the maxima kept whatever the fresh default-heap buffer held.
3. **The kernel reseeded its dither generator per triangle.** `BodyMeshBuilder` seeds one `Pcg32`
   per *cell* and draws from it once per triangle, so the two halves of a cell take the first and
   second draws of one stream. `TriangleColour` seeded from the cell hash itself, giving both
   halves the first draw — a different grain on every second triangle, and a fifth of all vertices
   more than a colour step from the builder's.

## Decision

Fix all three, at their causes: `ParamsFor` fills the octave amplitudes and `MeasureTiles` only
measures; the maxima are seeded from an upload buffer, which is `GENERIC_READ` and so already a
legal copy source, in one buffer where the helper made two; and the cell kernel seeds one generator
and passes it through both triangles. `BODY_BAKE_ON_GPU` becomes true.

The CPU builder stays as the reference and as the fallback where `Int64ShaderOps` or `WaveOps` are
missing. It remains what `NeuronClientTests` decides; the bake is decided by readback against it.

## Alternatives considered

- **Leave the switch false and the defects in place.** Honest, and what the previous state
  amounted to. Lost because the bake was already written and paid for, and an untested producer
  that nobody can turn on is worse than no producer at all.
- **Replace the kernels with the three-dispatch bake written alongside them** (a separate branch,
  verified to the same tolerances before slice 5 landed). Lost because it predates the ocean, so
  the shore dip and the sea-level cull would have had to be ported into it; because it recomputes
  the octave amplitudes with six `pow` calls per sample where `0017`'s block carries them; and
  because the defects above turned out to be three contained mistakes rather than a wrong design.
- **Filter the ramp from a buffer with `ColourRamp::Sample`'s own arithmetic**, rather than through
  a texture and a sampler whose bilinear weights are quantised. Tried and measured: it changed
  nothing, because the colour error was the dither and not the filter. Reverted, and `0017`'s
  half-texel correction stands.

## Consequences

- Boot generates bodies on the GPU. The CPU path is one constant away and is still built and
  tested.
- The acceptance numbers are in `ViewTuning.h` beside the switch, where the next person to doubt
  them will be standing.
- `ParamsFor` now returns a block that is complete on its own. Anything else that comes to want
  one — a second kernel, a tool — gets the amplitudes without knowing they were ever measured.
- A change to `BodyField.cpp` or `BodyMeshBuilder.cpp` is a change to `BodyBake.hlsli`, and the
  readback is how the two are shown to still agree. Nothing automated does that yet: it is a
  temporary harness in `OutpostApp` against `ReadBackBody`, written and removed each time.
