# 0026 — A world is a picture, a rock is generated, and the sea is gone

Status: accepted
Date: 2026-08-30

## Context

Every body in the scene was generated: a cube-sphere height field from seeded Perlin noise, coloured
per triangle off a ramp, with a wire-frame outline over it and — for a body whose height outside its
continents fell below zero — a sphere of water inside it, a dipped coastline and a culled sea floor.
`Design/PlanetRenderer.md` is the whole of it and it is a lot of machinery: `BodyField`,
`BodyMeshBuilder`, `ColourRamp`, a catalogue of six world classes, and a compute bake in
`BodyBake.hlsli` that mirrors `BodyField` term for term so a GPU can produce the same bytes (`0017`,
`0020`).

It works, and at asteroid scale it looks right: a lumpy, cratered, flat-shaded rock a hundred metres
across reads as a rock. At planet scale it did not. A 900 m sphere of the same flat facets, seen from
five kilometres, reads as a low-poly ball rather than as a world — and the owner said so after
looking at it.

An authored equirectangular map does read as a world, and one arrived: `Assets/Terrain/Planet1.dds`,
2048×1024 BGRA8.

## Decision

**There are two kinds of body, and which one you get is what you are.**

A *world* is a smooth sphere from `BodyMeshBuilder::BuildSphere` wearing a picture.
`Shaders/PlanetPS.hlsl` derives the equirectangular lookup per pixel from the object-space direction,
so there is no seam and no uv on the vertex; the normal is per vertex and interpolated, so the
terminator is a curve. It takes no field, no ramp and no outline. `BODY_PLANET_TEXTURED` selects it.

A *rock* is what it always was. `BodyField`, `BodyMeshBuilder::Build`, the ramp, the dither, the bake
and the outline are all untouched and all still run for the six asteroids.

**The sea is deleted.** It only ever existed for worlds, and worlds do not generate a surface any
more. `BuildOcean`, the ocean mesh path through `SceneRenderer`, the shore dip, the sea-floor cull,
`BodyBuildStats::trianglesCulled`, the `wet` and ocean-colour columns and the wet/dry height pair are
all gone.

**Five of the six world classes go with it** — terran, classic, ice, desert and the dead moon — and
the moon that used one. `BodyClass` is one row, `Asteroid`. `BodyClassSpec` is one field, the ramp.
`RandomBody` no longer takes a class and no longer has a planet branch.

**The polar caps stay**, dead. `BodyDesc` still declares `polarStrength`, `capStart`, `capNoise` and
`polarGeometry`; `RandomBody` leaves all four at their defaults, which is zero strength, and the term
`BodyField` and `BodyBake.hlsli` compute from them multiplies out to nothing. They are not removed
because those two are *mirrored implementations pinned to each other byte for byte*, and `BodyParams`
is a constant-buffer layout both sides spell independently — cutting a `float4` out of the middle of
it is a slice of its own, with its own equivalence run, not a tidy-up to fold into this one.

## Alternatives considered

- **Keep generating the planet and just add a texture over it.** The map would drape over mountains
  the map does not know about, and every coastline would fall in the wrong place. The point of an
  authored map is that the surface *is* the picture.
- **Delete the generator entirely and texture the asteroids too.** Considered and put to the owner,
  who chose against it. There is one map, so six asteroids would be six copies of the same world, and
  the generated rock is the thing in the scene that has never looked wrong.
- **Keep the ocean for a future generated world.** Lost because a feature kept against a future that
  has no date is a feature nobody is testing. It is recoverable from this record and from the history
  the moment a generated world comes back.
- **Keep the five unused class rows.** Lost for the same reason, and they are three lines each to
  restore. Their ramps are still in `Assets/Terrain`.

## Consequences

- **The rock path is provably unchanged.** `BodyMeshTests` pins a dry body's whole vertex list to
  `0xc5a1168f27d54c8f`, and that literal did not move — not when the ocean arrived, and not now that
  it has gone. A dry body never took the shore dip or the cull, so the whole of that feature and the
  whole of its removal are outside the path that draws an asteroid. Five ocean tests went with the
  ocean; 239 remain and pass.
- **The scene is a different scene.** Removing the moon removed its draws from the seeded stream, so
  every asteroid after it has a different seed, bearing and distance. Reproducible as before, just
  not the same arrangement.
- **The map has no mip chain**, because this tree generates none. A world small on screen will
  sparkle. That is the same debt `TriangleOutline.dds` carries, and the outline works around it with
  an `fwidth` fade that a surface map has no equivalent for.
- `Design/PlanetRenderer.md` now describes one of the two kinds of body. It stays as the document
  the rock path is reviewed against.
