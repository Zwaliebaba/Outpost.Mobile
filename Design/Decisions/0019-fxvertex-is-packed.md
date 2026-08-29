# 0019 — FxVertex is packed: float position, SNORM16 normal, UNORM8 colour, half uv

Status: accepted
Date: 2026-08-29

## Context

`FxVertex` is the one vertex format the effect passes and the bodies share: twelve floats, 48
bytes. A 65-grid planet is 147 456 of them — 7.1 MB the input assembler reads twice a frame — and
the effect ring writes up to 49 152 of them a frame, 2.25 MB. On the mobile-class targets this
tree is for, vertex fetch bandwidth is where a frame's time goes before anything in a pixel
shader does.

None of the nine non-position floats carries what a float holds. The normal is one per triangle
and only feeds a Lambert term and a specular power; the colour was read off an eight-bit ramp
or an eight-bit hull material and then dithered; the uv is a cell index below 2 048 or a 0/1
corner. The position stays float: a planet is kilometres across and its facets are metres.

The input assembler converts `R16G16B16A16_SNORM`, `R8G8B8A8_UNORM` and `R16G16_FLOAT` to
floats for free, so the vertex shaders do not change.

## Decision

`FxVertex` is 28 bytes: `float px, py, pz`, `int16 normalSnorm[4]`, `uint8 colourUnorm[4]`,
`uint16 uvHalf[2]`. `FxVertex::Make` packs and `Position()`/`Normal()`/`Colour()`/`Uv()` unpack;
producers call `Make`, tests read the accessors, and the raw fields exist so a test can assert
"these three vertices carry the same bytes". The packing is written out in integer arithmetic
with one stated rounding rule per format — UNORM half-up, SNORM halves away from zero, half
nearest-even — and not through DirectXMath, whose conversion takes the F16C path on one machine
and a software path on another. `BodyBake.hlsli`'s `MakeVertex` mirrors the three rules with `round()`
and `f32tof16()`, so the bake of `0017` writes the same bytes, and `FxVertexTests` pins them.

## Alternatives considered

- **Keep 48 bytes.** Nothing was wrong. Lost because halving the largest buffers in the game is
  a measurable win with no visible cost, and the change is contained: three producers, two
  input layouts, the tests.
- **Pack at upload instead of in the struct.** The builders keep writing floats; `UploadBody`
  and the effect ring pack on the way in. Lost because the ring would pack 49 152 vertices a
  frame on the CPU that it now memcpys, and because two vertex formats — one in memory, one on
  the GPU — is one more thing to keep equal than one.
- **8-bit normals (`R8G8B8A8_SNORM`, 24 bytes).** Four bytes fewer. Lost because a step of
  1/127 is 0.45° and visible in the overlay's specular term; a step of 1/32767 is not visible
  anywhere. 28 bytes is the honest number.
- **10:10:10:2 normals.** No SNORM variant in DXGI; the shader would decode. Lost for the same
  precision reason and for touching the vertex shaders.

## Consequences

- A 65-grid planet is 4.1 MB, a 33-grid asteroid 1.0 MB; the effect ring is 1.3 MB a frame.
- `BodyMeshTests`' two pinned vertex hashes moved once each, and colour assertions in the effect
  tests compare to one UNORM8 step rather than `1e-5`.
- A colour is now quantised at the vertex rather than at the render target; both are eight bits,
  so nothing on screen changes.
- The compute bake (`0017`) writes this layout directly, through `MakeVertex`; a change to the
  packing is a change in two places, and `BodyRenderer.cpp` static_asserts the size so one moving
  without the other stops the build.
