# The sky

What is behind everything else, and why it is made the way it is.

## 1. The problem

Until this landed, the sky was one number: `SKY_COLOUR`, the value `GpuDevice::BeginFrame` clears
the back buffer to. The game is set in space and the camera can be pitched until most of the frame
is sky, so most of the frame was a flat dark gray. Two planets and six asteroids float in it and had
nothing to float against.

What is wanted is a star field that reads as a real sky: not a scattering of identical dots, which
is what a first attempt produces, but something with the structure a person recognizes without being
able to say why — a few stars that stand out among thousands that do not, color that means
something, and a galaxy running through it.

Three textures already ship with the game and have no consumer:
`Outpost/Assets/Textures/Glow.dds`, `Starburst.dds` and `CloudyGlow.dds`. All three are 128×128
BGRA8 with one mip, **white everywhere in rgb with their shape in the alpha channel** — the same
convention `TriangleOutline.dds` uses, and the same trap: sample `rgb` and every billboard is a
square.

## 2. What was considered

**A) A procedural pixel shader.** One fullscreen triangle; hash the view ray direction into stars
and noise into nebulosity. No vertex data and no textures at all. Rejected: it leaves the three
authored textures unused, and per-star shape — a flare with rays, a cloud with structure — becomes
much harder than sampling a picture of one. It is also the option that most easily produces the
scattering-of-identical-dots failure, because every star is the same function evaluated at a
different point.

**B) Bake a cubemap at boot.** Generate the sky once into six faces and sample one texture per
frame. The cheapest steady state, but it adds a render-to-texture path this tree has no precedent
for, and it fixes the resolution at bake time.

**C) A generated catalogue drawn as billboards.** Chosen. It uses all three textures, it puts the
interesting decisions in device-free code that tests can pin, and it costs three draw calls.

The one thing (C) has to get right is that the CPU must not rebuild the billboards every frame the
way `SpriteParticles` does. That is `Design/Decisions/0024`.

## 3. Shape

Three files, mirroring how the planets are built (`Design/PlanetRenderer.md` 7):

| | |
|---|---|
| `NeuronClient/SkyVertex.h` | the packed vertex, 28 bytes, reusing `FxVertex`'s three packing rules |
| `NeuronClient/SkyField.{h,cpp}` | the generator: a seed in, a `SkyMesh` out, no device anywhere in it |
| `NeuronClient/SkyRenderer.{h,cpp}` | the GPU side: three textures, one pipeline, one static buffer |
| `NeuronClient/Shaders/Sky{.hlsli,VS.hlsl,PS.hlsl}` | one vertex stage that builds the quads, one pixel stage that samples alpha |

The composition root names the three files, generates the sky from `SKY_SEED`, and hands `WorldView`
the tuning it should draw with. `WorldView::Render` draws it first, before `BeginScene`.

## 4. Three layers, one buffer

A `SkyMesh` holds one vertex list with three contiguous ranges, in `SkyLayer` order:

| Layer | Texture | What it is |
|---|---|---|
| `Nebula` | `CloudyGlow.dds` | the galactic band's diffuse glow and the dust clouds off it |
| `Star` | `Glow.dds` | every star |
| `Burst` | `Starburst.dds` | the flare over the two dozen brightest stars |

`SkyRenderer::Draw` issues one `DrawInstanced` per range and changes one descriptor between them.
The order is the order they are laid out in and the order they are drawn in; additive blending is
commutative so it buys nothing today, and it is what stays right the day one of them stops being
additive.

## 5. What makes it look like a sky

This is the whole substance of the feature, and every number below lives in `SkyField.cpp` with the
reason beside it.

### 5.1 Magnitudes follow the real count law

The number of stars brighter than magnitude *m* goes as 10^(0.6m). The generator draws a uniform
number and inverts that over magnitudes −1 (Sirius) to 6.5 (the naked-eye limit), so faint stars are
ordinary and a first-magnitude star is an event: over eight thousand stars, about fifteen are bright
enough to stand out. A uniform draw over the same range puts a third of them there, and that is
exactly what a generated field looks like when it is wrong.

### 5.2 Size carries the magnitude, brightness mostly does not

Getting this the wrong way round is what a first attempt looks like, and it was the first attempt
here. Mapping magnitude straight onto intensity gives a sky of gray smudges: the faint stars, which
are almost all of them, land in a narrow band near the intensity floor, and are then
indistinguishable from each other and from noise.

So intensity starts at 0.34 — well clear of the floor, so every star is a crisp dot — and magnitude
is spent on **size** instead, from a half-angle of 0.0012 rad to 0.0105. At a 45° vertical field of
view over 900 rows that is under three pixels for the faintest and about twenty-four for the
brightest. They are angles, so a star does not change size when the window does.

### 5.3 Color is a blackbody hue, correlated with brightness

A star's temperature is drawn from a weighted table of spectral classes — the mix a person can
actually *see*, which is not the mix that exists: the sky is full of M dwarfs and none of them is
visible, while O and B stars are a rounding error by count and half of what stands out. The draw is
squeezed towards the hot end in proportion to how bright the star already is, which is the real
luminosity bias, and is what puts the blue-white stars among the ones the eye picks out.

Temperature becomes rgb through Tanner Helland's piecewise fit to the Planckian locus — three
logarithms rather than a spectral integral, accurate to a few percent from 1 000 K to 40 000 K, and
exactly white at 6 600 K where its two branches meet. A test pins that.

Saturation rises with brightness, because a faint star has less color to the eye — but it rises
from a floor of 0.45, not from nothing. A sky whose stars are all white is the other half of the
mistake in 5.2, and the two together are what make a generated field read as gray dust.

### 5.4 A galactic band, a core, and dust lanes

A share of the stars belong to a band: galactic latitude drawn from a two-sided exponential with a
scale height of 0.13 rad, longitude squeezed towards a galactic center so the band is thickest in
one direction and thinnest opposite it. The rest are spread uniformly over the whole sphere, which
is what keeps the sky away from the band populated.

The pole is tilted well off vertical on purpose. A band running level with the horizon is the least
visible arrangement there is; tilted, it arcs up across the sky where it can be seen.

Dust is cut out of the band with `Noise3` — the same gradient noise the planets are made of —
sampled on the direction itself, so a lane is a shape on the sky rather than a shape in a texture.
Extinction only bites near the plane, because that is where the dust is, and it is applied to the
stars *and* the nebulosity, which is what makes a lane read as one shape crossing both rather than
as two coincidences.

### 5.5 The band's own glow

Unresolved stars are a glow; resolved ones are a count. So the same distribution that places band
stars places `CloudyGlow` patches, dim and large and numerous — one cloud at this brightness is
invisible and a hundred overlapping ones are the Milky Way. Each is rolled by a random angle, which
is the whole reason a hundred copies of one 128-pixel picture read as weather rather than as a
hundred copies of one 128-pixel picture. A cloud on the band is whitened towards the color of
unresolved starlight; off the band it keeps the color dust takes.

### 5.6 Flares

The two dozen brightest stars **of the draw** get a second, larger, rolled quad from
`Starburst.dds`. Of the draw, not a separate population: a sky whose standout stars sit somewhere
other than its brightest stars has two skies in it.

## 6. Why the CPU does no work per frame

A star does not move when the camera turns. So a `SkyVertex` carries a **direction**, not a
position, plus which corner of the quad it is; the vertex shader places the quad at
`cameraPos + dir * radius` and expands it against the camera's own right and up. The buffer is
static, uploaded once, and the per-frame cost of the whole sky is three draw calls and 32 root
constants. Measured at 165 FPS / 6.09 ms with the sky on, which is what the game ran at without it.

The twinkle rides the same arrangement. Each star carries an amount, a rate and a phase, packed into
three bytes, and the shader computes `1 + amount * sin(time * rate + phase)`. A cloud's amount is
zero, which makes it the identity rather than a branch. Scintillation is an atmosphere doing it
rather than the star, so a faint star twinkles more than a bright one — the same wobble moves a
larger fraction of a smaller signal.

The clock wraps, and the wrap is seamless for a reason worth keeping: a rate is stored as a fraction
of the frame's maximum quantized to eight bits, so every rate in the sky is *n*/255 × max for a
whole *n*, and after 255 × 2π / max seconds every star has completed exactly *n* whole cycles
whatever its *n*. Nothing jumps and there is nothing to tune.

## 7. Depth, and where it is drawn

The sky pass neither tests nor writes depth. It is drawn first and is behind everything by
construction, so there is nothing for a test to decide — and a depth *write* would be actively
wrong, because the sphere sits at five kilometers and would occlude a planet meant to be further
away than the sky is. `DSVFormat` still names the depth buffer, because the frame has one bound and
a pipeline that disagreed with the bound target is a debug-layer error.

The blend is `ONE, ONE` on color and `ZERO, ONE` on alpha. A star is light arriving, never light
removed, and two stars whose glows overlap are brighter where they do. Source alpha is deliberately
not in it — the intensity is in the color the vertex carries — so the blend says exactly what
happens and nothing else can be read into it.

## 8. What this deliberately does not do

- **No horizon, no atmosphere, no sun.** The light direction the scene is lit by has no visible
  source in the sky, and nothing here casts light on anything.
- **No parallax.** The sky is at infinity; the radius exists only to sit between the near and far
  planes.
- **No constellations and no named stars.** The catalogue is generated, not authored.
- **No level of detail.** Every billboard is drawn every frame. At 14 000 stars that is 2.4 MB of
  vertices and no measurable cost; the day it is one it will be a bounding-cone cull per layer.

## 9. Slices

One. It landed whole because the generator and the renderer are useless apart, and because the
tuning that makes it look like a sky cannot be judged until it is on screen.
