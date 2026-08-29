#include "BodyBake.hlsli"

// The reduction of Design/PlanetRenderer.md 17.1: the two maxima BodyField's constructor finds with
// a loop. One thread per grid sample, dispatched twice -- pass 0 for each tile's own peak, pass 1
// for the field's peak once those are known. A UAV barrier separates them, or pass 1 reads whatever
// pass 0 happened to have finished.
//
// The maxima buffer is seeded by the CPU with the same starting values the constructor uses: zero
// for a tile, outsideHeight for the field.

[numthreads(64, 1, 1)]
void main(uint3 _id : SV_DispatchThreadID)
{
  const uint samplesPerSide = SamplesPerSide();
  const uint perFace = samplesPerSide * samplesPerSide;
  if (_id.x >= 6u * perFace)
    return;

  const uint face = _id.x / perFace;
  const uint within = _id.x - face * perFace;
  const uint x = within / samplesPerSide;
  const uint z = within - x * samplesPerSide;
  const float3 direction = Direction(face, x, z, samplesPerSide);

  if (control.x == 0u)
  {
    // The tile's maximum is measured with its edge fade already applied, because the fade is part of
    // what the tile contributes -- the same reasoning MeasureTiles carries.
    for (uint i = 0u; i < counts.x; ++i)
    {
      const float fade = CapFade(direction, tiles[i]);
      if (fade <= 0.0)
        continue;

      InterlockedMax(Maxima[i], OrderedBits(Octaves(direction, i) * fade));
    }
    return;
  }

  float tileScale[BAKE_MAX_TILES];
  LoadTileScales(tileScale);

  // Before the polar lift, deliberately: the lift is a fraction of this number, and measuring it
  // after would define the maximum in terms of itself.
  InterlockedMax(Maxima[BAKE_MAX_TILES], OrderedBits(Flattened(direction, tileScale)));
}
