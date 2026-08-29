#include "BodyBake.hlsli"

// The bake: one thread per grid cell, writing the six FxVertex of its two triangles straight into
// the buffer the input assembler will read. It is BodyMeshBuilder's inner loop, with slice 5's ocean
// rules, and nothing else.
//
// A thread evaluates the field at six samples of its own -- four corners and the two neighbours the
// cull reads -- where the CPU builder samples the grid once and shares each corner between four
// cells. That is six evaluations where the CPU does one, and it is still the cheaper arrangement
// here: sharing would need a second dispatch and a buffer to share through, and the GPU has threads
// to spare where it does not have bandwidth to spare.
//
// A culled cell writes six degenerate vertices at the origin rather than being skipped. A shader
// cannot delete a triangle, and a degenerate has no area, rasterises nothing, and costs no
// ExecuteIndirect (Design/PlanetRenderer.md 17.1).

[numthreads(64, 1, 1)]
void main(uint3 _id : SV_DispatchThreadID)
{
  const uint samplesPerSide = SamplesPerSide();
  const uint cells = samplesPerSide - 1u;
  const uint perFace = cells * cells;
  if (_id.x >= 6u * perFace)
    return;

  const uint face = _id.x / perFace;
  const uint within = _id.x - face * perFace;
  const uint x = within / cells;
  const uint z = within - x * cells;
  const uint base = _id.x * 6u;

  float tileScale[BAKE_MAX_TILES];
  LoadTileScales(tileScale);
  const float maxHeight = MaxHeightMetres();

  const bool wet = outsideMaxHeightGrid.x < 0.0;
  const float shoreThreshold = BODY_SHORE_THRESHOLD * radiusEllipsoid.x;
  const float shoreDip = BODY_SHORE_DIP * radiusEllipsoid.x;

  // The four corners, in the builder's order: (x, z), (x, z+1), (x+1, z+1), (x+1, z).
  float3 directions[4];
  float heights[4];
  float3 positions[4];
  [unroll] for (uint corner = 0u; corner < 4u; ++corner)
  {
    const uint cornerX = x + ((corner == 2u || corner == 3u) ? 1u : 0u);
    const uint cornerZ = z + ((corner == 1u || corner == 2u) ? 1u : 0u);
    directions[corner] = Direction(face, cornerX, cornerZ, samplesPerSide);
    heights[corner] = Height(directions[corner], tileScale, maxHeight);

    float placed = heights[corner];
    if (wet && placed < shoreThreshold)
      placed = shoreDip;

    positions[corner] = Place(directions[corner], placed);
  }

  if (wet)
  {
    // Six samples, as the source tested six: the four corners and the two neighbours before this
    // cell, clamped at a face edge. BodyMeshBuilder.cpp says why it is not four.
    const uint beforeX = (x > 0u) ? x - 1u : 0u;
    const uint beforeZ = (z > 0u) ? z - 1u : 0u;
    const float westward = Height(Direction(face, beforeX, z, samplesPerSide), tileScale, maxHeight);
    const float southward = Height(Direction(face, x, beforeZ, samplesPerSide), tileScale, maxHeight);

    if (heights[0] <= 0.0 && heights[1] <= 0.0 && heights[2] <= 0.0 && heights[3] <= 0.0 && westward <= 0.0 && southward <= 0.0)
    {
      FxVertexGpu degenerate;
      degenerate.px = 0.0;
      degenerate.py = 0.0;
      degenerate.pz = 0.0;
      degenerate.nx = 0.0;
      degenerate.ny = 1.0;
      degenerate.nz = 0.0;
      degenerate.r = 0.0;
      degenerate.g = 0.0;
      degenerate.b = 0.0;
      degenerate.a = 1.0;
      degenerate.u = 0.0;
      degenerate.v = 0.0;

      [unroll] for (uint slot = 0u; slot < 6u; ++slot)
        Out[base + slot] = degenerate;
      return;
    }
  }

  // One draw per cell and not per triangle: the two halves of a cell are one patch of ground, and a
  // dither that split them would draw the diagonal in.
  const uint cellHash = CellHash(uint2(counts.z, counts.w), face, x, z);

  const uint order[6] = {0u, 1u, 2u, 0u, 2u, 3u};
  const float2 cellUvs[4] = {float2(0.0, 0.0), float2(0.0, 1.0), float2(1.0, 1.0), float2(1.0, 0.0)};

  [unroll] for (uint triangle = 0u; triangle < 2u; ++triangle)
  {
    const uint i0 = order[triangle * 3u + 0u];
    const uint i1 = order[triangle * 3u + 1u];
    const uint i2 = order[triangle * 3u + 2u];

    const float3 centroid = normalize(directions[i0] + directions[i1] + directions[i2]);
    float3 normal = normalize(cross(positions[i1] - positions[i0], positions[i2] - positions[i0]));

    // Outward, whichever way this face's winding ran. BodyPS trusts the normal it is given rather
    // than facing it to the eye, so a triangle that arrived inward would be lit as the far side.
    if (dot(normal, centroid) < 0.0)
      normal = -normal;

    // The undipped heights, as the colour pass reads them.
    const float heightMetres = (heights[i0] + heights[i1] + heights[i2]) * (1.0 / 3.0);
    const float3 colour = TriangleColour(normal, centroid, heightMetres, maxHeight, cellHash);

    [unroll] for (uint corner = 0u; corner < 3u; ++corner)
    {
      const uint source = order[triangle * 3u + corner];

      FxVertexGpu vertex;
      vertex.px = positions[source].x;
      vertex.py = positions[source].y;
      vertex.pz = positions[source].z;
      vertex.nx = normal.x;
      vertex.ny = normal.y;
      vertex.nz = normal.z;
      vertex.r = colour.x;
      vertex.g = colour.y;
      vertex.b = colour.z;
      vertex.a = 1.0;
      vertex.u = float(x) + cellUvs[source].x;
      vertex.v = float(z) + cellUvs[source].y;
      Out[base + triangle * 3u + corner] = vertex;
    }
  }
}
