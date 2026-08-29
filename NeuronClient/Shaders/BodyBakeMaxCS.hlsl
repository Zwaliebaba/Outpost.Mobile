#include "BodyBake.hlsli"

// The reduction of Design/PlanetRenderer.md 17.1: the two maxima BodyField's constructor finds with
// a loop. One thread per grid sample, dispatched twice -- pass 0 for each tile's own peak, pass 1
// for the field's peak once those are known. A UAV barrier separates them, or pass 1 reads whatever
// pass 0 happened to have finished.
//
// The maxima buffer is seeded by the CPU with the same starting values the constructor uses: zero
// for a tile, outsideHeight for the field.
//
// The wave does the reducing and memory sees one atomic per wave, not one per sample: sixty-four
// lanes agree on their maximum with WaveActiveMax and the first lane merges it in. At a 65-grid
// planet that is 150 000 InterlockedMax calls on one address turned into 2 400, and contention on
// one address is the whole cost of this kernel.
//
// A lane past the last sample therefore cannot return early -- a wave operation reads every lane in
// the wave, and leaving before one is undefined. It stays, contributes the image of a value below
// every real one, and the maximum is undisturbed.

[numthreads(64, 1, 1)]
void main(uint3 _id : SV_DispatchThreadID)
{
  const uint samplesPerSide = SamplesPerSide();
  const uint perFace = samplesPerSide * samplesPerSide;
  const bool live = _id.x < 6u * perFace;

  float3 direction = float3(0.0, 1.0, 0.0);
  if (live)
  {
    const uint face = _id.x / perFace;
    const uint within = _id.x - face * perFace;
    const uint x = within / samplesPerSide;
    const uint z = within - x * samplesPerSide;
    direction = Direction(face, x, z, samplesPerSide);
  }

  if (control.x == 0u)
  {
    // The tile's maximum is measured with its edge fade already applied, because the fade is part of
    // what the tile contributes -- the same reasoning MeasureTiles carries. counts.x is a constant
    // buffer value, so every lane runs the same number of turns and the wave stays whole.
    for (uint i = 0u; i < counts.x; ++i)
    {
      // Zero is where MeasureTiles starts each peak, so a sample outside the cap -- or a lane past
      // the end of the grid -- contributes exactly what the CPU's loop would have skipped.
      uint image = OrderedBits(0.0);
      if (live)
      {
        const float fade = CapFade(direction, tiles[i]);
        if (fade > 0.0)
          image = OrderedBits(Octaves(direction, i) * fade);
      }

      const uint waveMaximum = WaveActiveMax(image);
      if (WaveIsFirstLane())
        InterlockedMax(Maxima[i], waveMaximum);
    }
    return;
  }

  float tileScale[BAKE_MAX_TILES];
  LoadTileScales(tileScale);

  // Before the polar lift, deliberately: the lift is a fraction of this number, and measuring it
  // after would define the maximum in terms of itself. outsideHeight is the field's own floor and
  // the value the CPU seeds this maximum with, so it is what a dead lane offers.
  const uint image = live ? OrderedBits(Flattened(direction, tileScale)) : OrderedBits(outsideMaxHeightGrid.x);
  const uint waveMaximum = WaveActiveMax(image);
  if (WaveIsFirstLane())
    InterlockedMax(Maxima[BAKE_MAX_TILES], waveMaximum);
}
