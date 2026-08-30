# Work order — the sky

One slice. Implements the whole of `Design/Archive/Skybox.md`.

## Scope

1. `NeuronClient/SkyVertex.h` — a packed 28-byte vertex carrying a **direction**, a quad corner, an
   rgb color with intensity already folded in, three twinkle bytes, and an angular half-size and
   roll. Reuse `FxVertex`'s `PackUnorm8` / `PackSnorm16` / `PackHalf` rather than restating the
   rounding rules. `static_assert` the size and every offset, because `SkyRenderer` spells them by
   hand.
2. `NeuronClient/SkyField.{h,cpp}` — `SkyField::Build(const Desc&, SkyMesh&)`, device-free and
   deterministic in `Desc::seed` alone. Produces the three layers of `Design/Archive/Skybox.md` 4 in
   `SkyLayer` order and contiguously, following the four rules of section 5.
   `SkyField::TemperatureColour` is public because a test pins it.
3. `NeuronClient/Shaders/Sky.hlsli`, `SkyVS.hlsl`, `SkyPS.hlsl` — the quad is built in the vertex
   stage (section 6). The pixel stage samples **alpha**, not red.
4. `NeuronClient/SkyRenderer.{h,cpp}` — its own root signature (32 vertex DWORDs, one SRV table),
   one static sampler, one additive depth-less pipeline, three SRVs, one static default-heap vertex
   buffer. `Init` and `UploadField` record into the caller's upload bracket, as `BodyRenderer` does;
   `DiscardStaging` releases afterwards.
5. `Outpost/ViewTuning.h` — a sky block: seed, three counts, radius, intensity, twinkle rate.
6. `Outpost/WorldView.{h,cpp}` — `SetSkyRenderer`, a real-time clock for the twinkle, and the draw,
   first, before `BeginScene`.
7. `Outpost/OutpostApp.{h,cpp}` — the three texture names, `BuildSky`, and the sky in the same
   upload bracket as the bodies. F5 reseeds it with them.
8. `Tests/NeuronClientTests/SkyFieldTests.cpp`.
9. Registration in both the `.vcxproj` and the `.filters`, and in `NeuronClient.h`.

## Out of scope

- Anything that makes the sky a light source, or a source of parallax.
- Level of detail, culling, or a cubemap bake.
- Authored constellations. The catalogue is generated.
- Touching the effect or body passes. The sky has its own root signature for the same reason they
  have theirs.

## What to build on

`FxRenderer` is the model for the GPU side — texture upload through `DdsImage::TopMipAsBgra` and
`UploadColourTexture`, a private root signature, `DefaultPipelineDesc`. `BodyRenderer` is the model
for the upload bracket and `DiscardStaging`. `SpriteParticles` and `BodyField` are the model for a
device-free generator decided by tests. `Noise3` cuts the dust lanes. `Pcg32` is the only generator.
`UploadStaticBuffer` puts the vertices in a default heap.

## Acceptance

**Tests** — `SkyFieldTests`, and these are the claims that matter:

- the same seed builds the same sky byte for byte, and a different seed does not;
- the three layers are contiguous, in `SkyLayer` order, and account for every vertex;
- every quad is six vertices of one direction covering all four corners, and the direction is unit;
- the magnitude draw follows the count law: under 2 % of stars are bright, over 80 % are near the
  faint end, and the size ramp has a real range in it;
- more than half the stars lie within 15° of the galactic plane, where a uniform sphere gives a
  quarter;
- a cloud on the band outshines one far from it;
- the average cloud is under half the average star and the narrowest cloud is wider than the widest
  star — *not* "dimmer than the faintest star", which the dust lanes make false;
- every flare sits on a star that exists, is dimmer than it, and the brightest star has one;
- clouds do not twinkle and every star does, with rate and phase in range;
- the blackbody ramp is exactly white at 6 600 K, orange-red at 3 000 K, blue-white at 20 000 K, and
  clamped outside its range;
- asking for more flares than stars caps rather than reads off the end, an empty description builds
  an empty sky, and building twice into one mesh replaces rather than appends.

**Screenshot** — the sky at two window sizes, showing stars of visibly different size and color, a
galactic band with dust structure in it, and at least one flare.

**A code read** — no allocation and no CPU billboard work per frame; the draw is three calls.

## Assumptions the implementer may make

- The three textures are white in rgb with their shape in alpha. Verified; it is why the pixel
  shader samples `.a`.
- `SKY_RADIUS_METRES` only has to sit between the near and far planes. Nothing is depth-tested
  against the sky.
- A missing texture is a diagnostic: `Ready()` stays false, `Draw` draws nothing, and the game boots
  onto the clear color.
