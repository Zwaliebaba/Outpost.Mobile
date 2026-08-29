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
//
// Two locals are named differently from the C++ they mirror: `triangle` and `centroid` are HLSL
// keywords -- the geometry-shader input primitive and the interpolation modifier -- so the loop
// index is `triangleIndex` and the face-centre direction is `centreDirection`.

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
      const FxVertexGpu degenerate =
        MakeVertex(float3(0.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float4(0.0, 0.0, 0.0, 1.0), float2(0.0, 0.0));

      [unroll] for (uint slot = 0u; slot < 6u; ++slot)
        Out[base + slot] = degenerate;
      return;
    }
  }

  // One draw per cell and not per triangle: the two halves of a cell are one patch of ground, and a
  // dither that split them would draw the diagonal in.
  const uint cellHash = CellHash(BodySeed(), face, x, z);

  const uint order[6] = {0u, 1u, 2u, 0u, 2u, 3u};
  const float2 cellUvs[4] = {float2(0.0, 0.0), float2(0.0, 1.0), float2(1.0, 1.0), float2(1.0, 0.0)};

  [unroll] for (uint triangleIndex = 0u; triangleIndex < 2u; ++triangleIndex)
  {
    const uint i0 = order[triangleIndex * 3u + 0u];
    const uint i1 = order[triangleIndex * 3u + 1u];
    const uint i2 = order[triangleIndex * 3u + 2u];

    const float3 centreDirection = normalize(directions[i0] + directions[i1] + directions[i2]);
    float3 normal = normalize(cross(positions[i1] - positions[i0], positions[i2] - positions[i0]));

    // Outward, whichever way this face's winding ran. BodyPS trusts the normal it is given rather
    // than facing it to the eye, so a triangle that arrived inward would be lit as the far side.
    if (dot(normal, centreDirection) < 0.0)
      normal = -normal;

    // The undipped heights, as the colour pass reads them.
    const float heightMetres = (heights[i0] + heights[i1] + heights[i2]) * (1.0 / 3.0);
    const float3 colour = TriangleColour(normal, centreDirection, heightMetres, maxHeight, cellHash);

    [unroll] for (uint corner = 0u; corner < 3u; ++corner)
    {
      const uint source = order[triangleIndex * 3u + corner];

      Out[base + triangleIndex * 3u + corner] =
        MakeVertex(positions[source], normal, float4(colour, 1.0),
                   float2(float(x) + cellUvs[source].x, float(z) + cellUvs[source].y));
    }
  }
}
