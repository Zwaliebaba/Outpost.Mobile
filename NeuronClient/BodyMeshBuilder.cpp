#include "pch.h"
#include "BodyMeshBuilder.h"

#include "CubeSphere.h"

using namespace DirectX;

namespace Neuron
{
namespace
{
// The source's slope curve: u = pow(1 - dot(n, d), 0.4). The exponent pushes the flat end of the
// ramp out, so that a surface only a few degrees off level still reads as level and only a real
// cliff reaches the ramp's right-hand column.
constexpr float SLOPE_EXPONENT = 0.4f;

// The source's dither: 0.45 / (climate + 2), in the units SOURCE_MAP_SIZE converts to. Strong at sea
// level, weak on peaks, which is what stops a soft gradient from banding across a whole hemisphere.
constexpr float DITHER_STRENGTH = 0.45f;
constexpr float DITHER_SOFTENING = 2.0f;

// Every sample of one body: its direction, its height in metres, and where the vertex goes.
struct Sample
{
  XMFLOAT3 direction{0.0f, 1.0f, 0.0f};
  XMFLOAT3 position{0.0f, 0.0f, 0.0f};
  float heightMetres = 0.0f;
};

// The one transcendental in the build, taken in double and narrowed once -- the rule BodyField.cpp
// states at more length. Two vendors' float pow differ in the last bit, and a last bit here is a
// triangle that reads one texel further along the ramp on one machine than on another. Measured:
// with the float overload the whole-mesh hash of BodyMeshTests disagreed between MSVC and gcc.
[[nodiscard]] float Pow(float _base, float _exponent) noexcept
{
  return static_cast<float>(std::pow(static_cast<double>(_base), static_cast<double>(_exponent)));
}

[[nodiscard]] float Dot(const XMFLOAT3& _a, const XMFLOAT3& _b) noexcept
{
  return _a.x * _b.x + _a.y * _b.y + _a.z * _b.z;
}

// P = d * ellipsoid * (R + h), the design's placement rule (5.1). The ellipsoid scales the height
// with the radius, which is what makes an asteroid's relief squash with its axes rather than stand
// off a squashed core.
[[nodiscard]] XMFLOAT3 Place(const XMFLOAT3& _direction, const XMFLOAT4& _radiusEllipsoid, float _heightMetres) noexcept
{
  const float radius = _radiusEllipsoid.x + _heightMetres;
  return XMFLOAT3(_direction.x * _radiusEllipsoid.y * radius, _direction.y * _radiusEllipsoid.z * radius,
                  _direction.z * _radiusEllipsoid.w * radius);
}

#if defined(_DEBUG)
// The seam guarantee, asserted where a bug in it would first appear: two faces that meet share their
// edge samples, so the same direction must have produced the same height on both. Keyed on the bits
// of the direction rather than on a face-adjacency table, because the table is the thing most likely
// to be written wrong and the property does not need it. One pass over the boundary samples.
void AssertSharedEdgesAgree(const std::vector<Sample>& _samples, std::uint32_t _samplesPerSide)
{
  std::unordered_map<std::uint64_t, float> seen;
  for (std::uint32_t face = 0; face < CUBE_FACE_COUNT; ++face)
  {
    for (std::uint32_t x = 0; x < _samplesPerSide; ++x)
    {
      for (std::uint32_t z = 0; z < _samplesPerSide; ++z)
      {
        if (x != 0 && z != 0 && x != _samplesPerSide - 1 && z != _samplesPerSide - 1)
          continue;

        const Sample& sample = _samples[(face * _samplesPerSide + x) * _samplesPerSide + z];
        std::uint32_t bits[3];
        std::memcpy(bits, &sample.direction, sizeof(bits));
        const std::uint64_t key =
          (static_cast<std::uint64_t>(bits[0]) << 32) ^ (static_cast<std::uint64_t>(bits[1]) * 0x9E3779B97F4A7C15ull) ^ bits[2];

        const auto found = seen.find(key);
        if (found == seen.end())
          seen.emplace(key, sample.heightMetres);
        else
          DEBUG_ASSERT_TEXT(found->second == sample.heightMetres, L"two faces disagree on the height of a shared edge sample");
      }
    }
  }
}
#endif
} // namespace

void BodyMeshBuilder::Build(const BodyField& _field, const ColourRamp* _ramp, std::vector<FxVertex>& _outTerrain,
                            std::vector<MeshVertex>& _outOcean, const XMFLOAT3& _oceanColour, BodyBuildStats& _outStats)
{
  const BodyParams& params = _field.Params();
  const std::uint32_t gridPower = static_cast<std::uint32_t>(params.outsideMaxHeightGrid.z);
  const std::uint32_t samplesPerSide = CubeSphere::SamplesPerSide(gridPower);
  const std::uint32_t cells = samplesPerSide - 1u;
  const std::uint64_t seed = (static_cast<std::uint64_t>(params.seedHigh) << 32) | params.seedLow;

  _outStats = BodyBuildStats{};
  _outStats.maxHeightMetres = _field.MaxHeight();

  if (_ramp == nullptr || !_ramp->Loaded())
    DebugTrace("body {}: no colour ramp; the terrain is drawn in the fallback grey\n", seed);

  // Every rule below is behind this one sign. A body with an ocean has its sea floor culled, its
  // coast dipped and a sphere of water inside it; a dry one takes none of the three and its
  // vertices are bitwise what they were before this existed (Design/PlanetRenderer.md 5.5).
  const bool wet = params.outsideMaxHeightGrid.x < 0.0f;
  const float shoreThreshold = BODY_SHORE_THRESHOLD * params.radiusEllipsoid.x;
  const float shoreDip = BODY_SHORE_DIP * params.radiusEllipsoid.x;

  // Sample first, then emit. Every corner of every cell is shared by four cells and read three or
  // four times, and the field is the expensive half of the work -- evaluating it per triangle would
  // be six evaluations where one will do.
  std::vector<Sample> samples(static_cast<std::size_t>(CUBE_FACE_COUNT) * samplesPerSide * samplesPerSide);
  for (std::uint32_t face = 0; face < CUBE_FACE_COUNT; ++face)
  {
    for (std::uint32_t x = 0; x < samplesPerSide; ++x)
    {
      for (std::uint32_t z = 0; z < samplesPerSide; ++z)
      {
        Sample& sample = samples[(face * samplesPerSide + x) * samplesPerSide + z];
        sample.direction = CubeSphere::Direction(static_cast<CubeFace>(face), x, z, samplesPerSide);
        sample.heightMetres = _field.Height(sample.direction);

        // The shore dip: a corner barely above the water is pushed well under it, so the coast
        // disappears beneath the ocean sphere instead of meeting it edge-on and showing a seam of
        // sea floor. The height the colour is looked up from stays the undipped one, as the
        // source's colour pass read it before its strip builder pushed the vertex down.
        float placedHeight = sample.heightMetres;
        if (wet && placedHeight < shoreThreshold)
          placedHeight = shoreDip;

        sample.position = Place(sample.direction, params.radiusEllipsoid, placedHeight);
      }
    }
  }

#if defined(_DEBUG)
  AssertSharedEdgesAgree(samples, samplesPerSide);
#endif

  const float maxHeight = _field.MaxHeight();

  // A body whose field never rose above zero has no scale to colour against; every triangle then
  // reads the ramp's bottom row, which is sea level, and is the right answer for a drowned world.
  const float climateScale = (maxHeight > 0.0f) ? (1.0f / maxHeight) : 0.0f;
  const float sourceUnits = (params.radiusEllipsoid.x > 0.0f) ? (SOURCE_MAP_SIZE / params.radiusEllipsoid.x) : 0.0f;

  _outTerrain.reserve(_outTerrain.size() + static_cast<std::size_t>(CUBE_FACE_COUNT) * cells * cells * 2u * 3u);

  for (std::uint32_t face = 0; face < CUBE_FACE_COUNT; ++face)
  {
    for (std::uint32_t x = 0; x < cells; ++x)
    {
      for (std::uint32_t z = 0; z < cells; ++z)
      {
        const Sample& corner00 = samples[(face * samplesPerSide + x) * samplesPerSide + z];
        const Sample& corner01 = samples[(face * samplesPerSide + x) * samplesPerSide + z + 1u];
        const Sample& corner11 = samples[(face * samplesPerSide + x + 1u) * samplesPerSide + z + 1u];
        const Sample& corner10 = samples[(face * samplesPerSide + x + 1u) * samplesPerSide + z];

        // Sea-level culling: a cell entirely at or under the water is not drawn at all, which is
        // most of the triangles of an ocean world.
        //
        // Six samples, not the four corners it obviously wants to be. The source tested six because
        // its triangle strip joined neighbouring rows, and keeping the artefact keeps its effect: a
        // one-sample island holds a ring of sea-floor cells around it, and coastlines do not come
        // out punched. The two extras are the neighbours before this cell, clamped at a face edge --
        // the clamp is this port's, because a flat map had no edge to fall off.
        if (wet)
        {
          const std::uint32_t beforeX = (x > 0u) ? x - 1u : 0u;
          const std::uint32_t beforeZ = (z > 0u) ? z - 1u : 0u;
          const Sample& westward = samples[(face * samplesPerSide + beforeX) * samplesPerSide + z];
          const Sample& southward = samples[(face * samplesPerSide + x) * samplesPerSide + beforeZ];

          if (corner00.heightMetres <= 0.0f && corner01.heightMetres <= 0.0f && corner11.heightMetres <= 0.0f &&
              corner10.heightMetres <= 0.0f && westward.heightMetres <= 0.0f && southward.heightMetres <= 0.0f)
          {
            _outStats.trianglesCulled += 2;
            continue;
          }
        }

        // One cell, two triangles, sharing the (x, z) to (x+1, z+1) diagonal. The uv of a vertex is
        // its own corner of the cell, so the outline texture tiles once per cell whatever the grid.
        const Sample* const triangles[2][3] = {{&corner00, &corner01, &corner11}, {&corner00, &corner11, &corner10}};
        const float uvs[2][3][2] = {{{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}}, {{0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}}};

        // One draw per cell, not per triangle: the two halves of a cell are one patch of ground and
        // a dither that split them would draw the diagonal in.
        Pcg32 rng(CellHash(seed, face, x, z));

        for (std::uint32_t triangle = 0; triangle < 2; ++triangle)
        {
          const Sample& a = *triangles[triangle][0];
          const Sample& b = *triangles[triangle][1];
          const Sample& c = *triangles[triangle][2];

          const XMVECTOR positionA = XMLoadFloat3(&a.position);
          const XMVECTOR edgeAB = XMVectorSubtract(XMLoadFloat3(&b.position), positionA);
          const XMVECTOR edgeAC = XMVectorSubtract(XMLoadFloat3(&c.position), positionA);

          XMFLOAT3 centroid;
          XMStoreFloat3(&centroid, XMVector3Normalize(XMVectorAdd(XMVectorAdd(XMLoadFloat3(&a.direction), XMLoadFloat3(&b.direction)),
                                                                  XMLoadFloat3(&c.direction))));

          XMFLOAT3 normal;
          XMStoreFloat3(&normal, XMVector3Normalize(XMVector3Cross(edgeAB, edgeAC)));

          // Outward, whichever way the face's own winding ran. Three of the six faces wind the other
          // way round (CubeSphere.h says why that was the trade), and the pixel shader is written to
          // trust the normal it is given rather than to test it against the eye.
          if (Dot(normal, centroid) < 0.0f)
            normal = XMFLOAT3(-normal.x, -normal.y, -normal.z);

          const float heightMetres = (a.heightMetres + b.heightMetres + c.heightMetres) * (1.0f / 3.0f);
          const float climate = _field.Climate(centroid, heightMetres);

          // Slope on the ramp's x axis, climate height on its y: GetLandscapeColour, with the radial
          // direction standing in for the flat map's up.
          const float gradient = std::clamp(Dot(normal, centroid), 0.0f, 1.0f);
          const float u = Pow(1.0f - gradient, SLOPE_EXPONENT);
          float v = 1.0f - climate * climateScale;

          // The source's expression is 0.45 / (h + 2) on a map whose heights were never negative. A
          // body has an ocean floor, and at minus two source units that expression divides by zero.
          // The magnitude of the height keeps the shape the sentence describes -- strongest at sea
          // level, weaker away from it -- and is the same expression everywhere the source could go.
          v += rng.Signed(DITHER_STRENGTH / (std::fabs(climate * sourceUnits) + DITHER_SOFTENING));

          const XMFLOAT3 colour = (_ramp != nullptr && _ramp->Loaded()) ? _ramp->Sample(u, v) : BODY_FALLBACK_GREY;

          for (std::uint32_t corner = 0; corner < 3; ++corner)
          {
            const Sample& vertex = *triangles[triangle][corner];
            _outTerrain.push_back(FxVertex{vertex.position.x, vertex.position.y, vertex.position.z, normal.x, normal.y, normal.z, colour.x,
                                           colour.y, colour.z, 1.0f, static_cast<float>(x) + uvs[triangle][corner][0],
                                           static_cast<float>(z) + uvs[triangle][corner][1]});
          }

          ++_outStats.trianglesEmitted;
        }
      }
    }
  }

  if (!wet)
    return;

  // The ocean: a smooth sphere at the body's radius, two grid powers coarser than the terrain --
  // seventeen samples a side on a planet, whose facets are seventy-five metres across and read as
  // water rather than as polygons from anywhere the camera can get to. It carries a flat colour and
  // no normal, because the scene pass it goes through takes its normal from screen-space
  // derivatives and its colour from the draw.
  const std::uint32_t oceanPower = std::max(gridPower, BodyField::MIN_GRID_POWER + 2u) - 2u;
  const std::uint32_t oceanSamples = CubeSphere::SamplesPerSide(oceanPower);
  const std::uint32_t oceanCells = oceanSamples - 1u;
  _outOcean.reserve(_outOcean.size() + static_cast<std::size_t>(CUBE_FACE_COUNT) * oceanCells * oceanCells * 2u * 3u);

  for (std::uint32_t face = 0; face < CUBE_FACE_COUNT; ++face)
  {
    for (std::uint32_t x = 0; x < oceanCells; ++x)
    {
      for (std::uint32_t z = 0; z < oceanCells; ++z)
      {
        const XMFLOAT3 corners[4] = {CubeSphere::Direction(static_cast<CubeFace>(face), x, z, oceanSamples),
                                     CubeSphere::Direction(static_cast<CubeFace>(face), x, z + 1u, oceanSamples),
                                     CubeSphere::Direction(static_cast<CubeFace>(face), x + 1u, z + 1u, oceanSamples),
                                     CubeSphere::Direction(static_cast<CubeFace>(face), x + 1u, z, oceanSamples)};

        // The same corner order the terrain emits, so the two agree about which way a cell winds.
        const std::uint32_t order[2][3] = {{0u, 1u, 2u}, {0u, 2u, 3u}};
        for (std::uint32_t triangle = 0; triangle < 2; ++triangle)
        {
          for (std::uint32_t corner = 0; corner < 3; ++corner)
          {
            const XMFLOAT3 position = Place(corners[order[triangle][corner]], params.radiusEllipsoid, 0.0f);
            _outOcean.push_back(MeshVertex{position.x, position.y, position.z, _oceanColour.x, _oceanColour.y, _oceanColour.z});
          }
        }
      }
    }
  }
}
} // namespace Neuron
