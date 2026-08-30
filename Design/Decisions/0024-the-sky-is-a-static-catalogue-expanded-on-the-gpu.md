# 0024 — The sky is a static catalogue expanded on the GPU

Status: accepted
Date: 2026-08-29

## Context

The sky is a star field: fourteen thousand stars, a couple of hundred nebula patches and two dozen
flares, all of them camera-facing billboards. This tree already knows how to draw a camera-facing
billboard — `SpriteParticles::Build` walks a pool every frame and writes `FxVertex` quads into a
caller's vector, and `FxRenderer` copies them into a per-frame ring. Doing the same thing here is
the obvious first move, and every reviewer will propose it.

It is also wrong here, and the reason is a property of the sky rather than of the code. A particle
moves; a star does not. Rebuilding the sky's billboards each frame is 97 000 vertices and 2.7 MB of
memcpy per frame to reproduce, exactly, what was there the frame before. `FxRenderer::MAX_FX_VERTS`
is 49 152, so the sky does not fit in the effect's ring at all, and widening it would grow the ring
for every frame in the game to serve one caller that never changes.

The billboard corners are the only part that depends on the camera, and they depend on it only
through its right and up vectors — two values already in `SceneFrame` and in `Camera`.

`FxSpriteVS` anticipated this. Its comment says the sprite and fragment shaders "part company the
day the billboard corners are built on the GPU"; this is that day, for one pass.

## Decision

`SkyVertex` carries a **direction on the celestial sphere** and a quad-corner index, not a world
position. `SkyVS` places the quad's center at `cameraPos + dir * radius` and expands the corner
against the camera's right and up, applying a per-quad roll and a per-star twinkle from numbers
baked into the vertex. `SkyField::Build` runs once, at boot and on F5, and `SkyRenderer` uploads the
result into a default-heap buffer that is never written again.

The per-frame cost of the whole sky is therefore three `DrawInstanced` calls and 32 root constants,
with no CPU work and no allocation. `SkyRenderer` gets its own root signature and pipeline rather
than widening `FxRenderer`'s, for the reason the body pass has its own: the vertex layouts differ,
the blends differ, and the depth states differ.

Nothing else changes. `SpriteParticles` keeps building on the CPU, because its particles move.

## Alternatives considered

- **Build the quads on the CPU, like every other billboard.** One less shader idea to hold in your
  head, and the ring already exists. Lost on the numbers above: 2.7 MB a frame to recompute a
  constant, and a ring that would have to be tripled for one caller.
- **A procedural pixel shader over a fullscreen triangle.** No vertex data at all. Lost because it
  leaves the three authored textures (`Glow`, `Starburst`, `CloudyGlow`) unused and makes per-star
  shape — a flare with rays, a cloud with structure — much harder than sampling a picture of one.
  `Design/Archive/Skybox.md` 2 has the longer argument.
- **Bake a cubemap at boot.** Cheapest steady state. Lost because it adds a render-to-texture path
  the tree has no precedent for, and fixes resolution at bake time.
- **Instancing instead of six vertices per quad.** Would cut the buffer by a third. Lost for now
  because nothing else in this renderer instances, and 2.4 MB uploaded once is not a problem to
  solve.

## Consequences

- The sky's cost does not scale with the frame rate, only with the star count. Doubling the stars
  costs upload memory and fill rate, and no CPU time at all.
- A star's appearance can only depend on things known at generation time or derivable in the vertex
  shader from the frame constants. The twinkle fits because it is a function of time; anything that
  had to know what the camera is looking at would not.
- Reseeding replaces the buffer, so it is only safe inside an upload bracket — `BeginUploads` drains
  the GPU before it opens one, which is what makes that safe. Stated at `SkyRenderer::UploadField`.
- `SkyVertex` reuses `FxVertex`'s three packing rules rather than restating them, so `0019` now
  covers two vertex formats and a change to a rounding rule moves both.
