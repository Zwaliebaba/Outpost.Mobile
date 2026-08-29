#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
constexpr std::size_t PIXELS_OFFSET = 128;

void Put32(Neuron::ByteBuffer& _bytes, std::size_t _offset, std::uint32_t _value)
{
  _bytes[_offset + 0] = static_cast<std::uint8_t>(_value);
  _bytes[_offset + 1] = static_cast<std::uint8_t>(_value >> 8);
  _bytes[_offset + 2] = static_cast<std::uint8_t>(_value >> 16);
  _bytes[_offset + 3] = static_cast<std::uint8_t>(_value >> 24);
}

// A ramp whose red channel is the slope axis and whose green channel is the climate axis, so a
// baked triangle colour reads back as the (u, v) that was used to look it up. Linear in both, so the
// bilinear filter reproduces it exactly and the test measures the builder rather than the sampler.
[[nodiscard]] Neuron::ColourRamp AxisRamp()
{
  constexpr std::uint32_t SIDE = Neuron::ColourRamp::SIDE;
  Neuron::ByteBuffer bytes(PIXELS_OFFSET + static_cast<std::size_t>(SIDE) * SIDE * 4, 0);
  Put32(bytes, 0, 0x20534444u); // 'DDS '
  Put32(bytes, 4, 124);
  Put32(bytes, 8, 0x0002100fu);
  Put32(bytes, 12, SIDE);
  Put32(bytes, 16, SIDE);
  Put32(bytes, 20, SIDE * 4);
  Put32(bytes, 28, 1);
  Put32(bytes, 76, 32);
  Put32(bytes, 80, 0x41u);
  Put32(bytes, 88, 32);
  Put32(bytes, 92, 0x00ff0000u);
  Put32(bytes, 96, 0x0000ff00u);
  Put32(bytes, 100, 0x000000ffu);
  Put32(bytes, 104, 0xff000000u);

  const float last = static_cast<float>(SIDE - 1);
  for (std::uint32_t row = 0; row < SIDE; ++row)
  {
    for (std::uint32_t column = 0; column < SIDE; ++column)
    {
      const std::size_t texel = PIXELS_OFFSET + (static_cast<std::size_t>(row) * SIDE + column) * 4;
      bytes[texel + 0] = 0;
      bytes[texel + 1] = static_cast<std::uint8_t>(std::lround(row / last * 255.0f));
      bytes[texel + 2] = static_cast<std::uint8_t>(std::lround(column / last * 255.0f));
      bytes[texel + 3] = 255;
    }
  }

  Neuron::DdsImage image;
  Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"the synthetic ramp did not parse");
  Neuron::ColourRamp ramp;
  Assert::IsTrue(ramp.FromImage(image), L"the synthetic ramp did not load");
  return ramp;
}

// The slice 1 description at the grid power the counting tests are written against: eight cells a
// side, which is 768 triangles -- small enough to walk every one of them in a test.
[[nodiscard]] Neuron::BodyDesc OneContinent()
{
  Neuron::BodyDesc desc;
  desc.seed = 4321u;
  desc.radiusMetres = 1000.0f;
  desc.gridPower = 3;
  desc.outsideHeight = -0.02f;

  Neuron::BodyTile tile;
  tile.centre = XMFLOAT3(1.0f, 0.0f, 0.0f);
  tile.halfWidthRad = 1.0f;
  tile.edgeFraction = 0.25f;
  tile.desiredHeight = 0.05f;
  desc.tiles.push_back(tile);

  return desc;
}

[[nodiscard]] std::vector<Neuron::FxVertex> BuildTerrain(const Neuron::BodyDesc& _desc, const Neuron::ColourRamp* _ramp,
                                                         Neuron::BodyBuildStats& _outStats)
{
  const Neuron::BodyField field(_desc);
  std::vector<Neuron::FxVertex> terrain;
  Neuron::BodyMeshBuilder::Build(field, _ramp, terrain, _outStats);
  return terrain;
}

[[nodiscard]] XMVECTOR Position(const Neuron::FxVertex& _vertex)
{
  return XMVectorSet(_vertex.px, _vertex.py, _vertex.pz, 0.0f);
}

[[nodiscard]] XMVECTOR Normal(const Neuron::FxVertex& _vertex)
{
  return XMVectorSet(_vertex.nx, _vertex.ny, _vertex.nz, 0.0f);
}

// FNV-1a over the raw bytes of the vertex list. It is the whole build in one number: a change to any
// position, normal, colour or uv of any of the 2 304 vertices moves it.
[[nodiscard]] std::uint64_t HashVertices(const std::vector<Neuron::FxVertex>& _vertices)
{
  const std::uint8_t* const bytes = reinterpret_cast<const std::uint8_t*>(_vertices.data());
  const std::size_t count = _vertices.size() * sizeof(Neuron::FxVertex);

  std::uint64_t hash = 0xcbf29ce484222325ull;
  for (std::size_t i = 0; i < count; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

// The build of OneContinent through AxisRamp, byte for byte. **This is the replay key for the whole
// mesh**, the way PINNED_HEIGHT_METRES is for the field. If it moves, every body the game has drawn
// is a different body, and the pull request says why -- it is not a number to update until the test
// goes green.
constexpr std::uint64_t PINNED_VERTEX_HASH = 0x732268fa67202a0eull;
constexpr std::uint32_t PINNED_CELL_HASH = 0x1cf48a38u;
} // namespace

TEST_CLASS(BodyMeshTests)
{
public:
  TEST_METHOD(EveryCellBecomesTwoTrianglesOfThreeVerticesEach)
  {
    // Unshared vertices, three per triangle: the only way to get one colour and one normal per
    // triangle without a provoking-vertex buffer, and the reason a 65-grid planet is 7.1 MB.
    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneContinent(), &ramp, stats);

    constexpr std::uint32_t CELLS_PER_SIDE = 8;
    constexpr std::uint32_t TRIANGLES = Neuron::CUBE_FACE_COUNT * CELLS_PER_SIDE * CELLS_PER_SIDE * 2;
    Assert::AreEqual(TRIANGLES, stats.trianglesEmitted, L"a grid power of three did not emit six faces of eight by eight cells");
    Assert::AreEqual(static_cast<std::size_t>(TRIANGLES) * 3, terrain.size(), L"the vertex count is not three times the triangle count");
    Assert::AreEqual(0u, stats.trianglesCulled, L"something was culled before slice 5 landed the ocean");
  }

  TEST_METHOD(EveryNormalIsTheTrianglesOwnAndFacesOutward)
  {
    // Outward, so that the pixel shader can trust the normal it is given instead of testing it
    // against the eye the way ScenePS has to. The normal is also the input to the slope axis of the
    // colour lookup, so one flipped triangle is a cliff-coloured patch of flat ground.
    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneContinent(), &ramp, stats);

    for (std::size_t base = 0; base < terrain.size(); base += 3)
    {
      const XMVECTOR a = Position(terrain[base]);
      const XMVECTOR b = Position(terrain[base + 1]);
      const XMVECTOR c = Position(terrain[base + 2]);
      const XMVECTOR outward = XMVector3Normalize(XMVectorAdd(XMVectorAdd(a, b), c));
      const XMVECTOR normal = Normal(terrain[base]);

      Assert::IsTrue(XMVectorGetX(XMVector3Dot(normal, outward)) > 0.0f, L"a triangle's normal points into the body");
      Assert::AreEqual(1.0f, XMVectorGetX(XMVector3Length(normal)), 1e-4f, L"a triangle's normal is not unit length");

      const XMVECTOR fromCross = XMVector3Normalize(XMVector3Cross(XMVectorSubtract(b, a), XMVectorSubtract(c, a)));
      const float alignment = std::fabs(XMVectorGetX(XMVector3Dot(normal, fromCross)));
      Assert::AreEqual(1.0f, alignment, 1e-4f, L"a triangle's normal is not the normal of that triangle");
    }
  }

  TEST_METHOD(TheThreeVerticesOfATriangleShareItsNormalAndItsColour)
  {
    // Flat shading is the house look, and it is bought here rather than in a shader: one colour and
    // one normal written to three vertices. Bitwise, because an interpolator will find any drift.
    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneContinent(), &ramp, stats);

    for (std::size_t base = 0; base < terrain.size(); base += 3)
    {
      for (std::size_t corner = 1; corner < 3; ++corner)
      {
        const Neuron::FxVertex& first = terrain[base];
        const Neuron::FxVertex& other = terrain[base + corner];
        Assert::IsTrue(first.nx == other.nx && first.ny == other.ny && first.nz == other.nz,
                       L"two vertices of one triangle carry different normals");
        Assert::IsTrue(first.r == other.r && first.g == other.g && first.b == other.b,
                       L"two vertices of one triangle carry different colours");
        Assert::AreEqual(1.0f, other.a, L"a terrain vertex is not opaque");
      }
    }
  }

  TEST_METHOD(TheUvOfAVertexIsItsCornerOfTheCell)
  {
    // One outline tile per grid cell, whatever the grid power, which is what makes the overlay a
    // wire frame rather than a texture that stretches as the body's resolution changes.
    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneContinent(), &ramp, stats);

    Assert::AreEqual(0.0f, terrain[0].u, L"the first triangle does not start at the cell's corner");
    Assert::AreEqual(0.0f, terrain[0].v, L"the first triangle does not start at the cell's corner");
    Assert::AreEqual(0.0f, terrain[1].u, L"the first triangle's second vertex is not the next cell corner");
    Assert::AreEqual(1.0f, terrain[1].v, L"the first triangle's second vertex is not the next cell corner");
    Assert::AreEqual(1.0f, terrain[2].u, L"the first triangle's third vertex is not the far cell corner");
    Assert::AreEqual(1.0f, terrain[2].v, L"the first triangle's third vertex is not the far cell corner");

    for (const Neuron::FxVertex& vertex : terrain)
    {
      Assert::AreEqual(vertex.u, std::floor(vertex.u), L"a uv is not on a cell corner");
      Assert::AreEqual(vertex.v, std::floor(vertex.v), L"a uv is not on a cell corner");
    }
  }

  TEST_METHOD(FlatGroundReadsTheRampsLeftColumnAndCliffsReadItsRight)
  {
    // u = pow(1 - dot(n, d), 0.4): the slope axis of GetLandscapeColour, with the radial direction
    // standing in for the flat map's up. The ramp's red channel is u, so the colour reads it back.
    //
    // The relief knob is desiredHeight and not heightScale, because the tile is rescaled to its
    // desired height after generation -- heightScale sets what the roughness law measures against,
    // and the rescale then divides it back out (Design/PlanetRenderer.md 5.2, 5.3).
    const Neuron::ColourRamp ramp = AxisRamp();

    Neuron::BodyDesc flat = OneContinent();
    flat.outsideHeight = 0.0f;
    flat.tiles[0].heightScale = 0.0f;
    flat.tiles[0].desiredHeight = 0.0f;

    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> sphere = BuildTerrain(flat, &ramp, stats);
    for (const Neuron::FxVertex& vertex : sphere)
      Assert::IsTrue(vertex.r < 0.2f, L"a facet of a smooth sphere read the ramp's cliff end");

    Neuron::BodyDesc steep = OneContinent();
    steep.tiles[0].desiredHeight = 0.4f;
    const std::vector<Neuron::FxVertex> mountains = BuildTerrain(steep, &ramp, stats);

    float steepest = 0.0f;
    for (const Neuron::FxVertex& vertex : mountains)
      steepest = std::max(steepest, vertex.r);
    Assert::IsTrue(steepest > 0.5f, L"a body with four hundred metres of relief has no cliff on it");
  }

  TEST_METHOD(WithoutARampEverythingIsTheFallbackGrey)
  {
    // A missing asset is a diagnostic and a grey planet, not a crash and not a black one: grey reads
    // as "the ramp did not arrive" where black reads as a lighting bug.
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneContinent(), nullptr, stats);

    for (const Neuron::FxVertex& vertex : terrain)
    {
      Assert::AreEqual(Neuron::BodyMeshBuilder::BODY_FALLBACK_GREY.x, vertex.r,
                       L"a triangle built without a ramp is not the fallback grey");
      Assert::AreEqual(Neuron::BodyMeshBuilder::BODY_FALLBACK_GREY.y, vertex.g,
                       L"a triangle built without a ramp is not the fallback grey");
      Assert::AreEqual(Neuron::BodyMeshBuilder::BODY_FALLBACK_GREY.z, vertex.b,
                       L"a triangle built without a ramp is not the fallback grey");
    }
  }

  TEST_METHOD(TheDitherIsSeededByTheCellAndTheBody)
  {
    // The source seeded its dither from an expression that was effectively unseeded. Here it has to
    // be a pure function of the body and the cell, so that the grain is the same on two machines and
    // -- one day -- the same in a compute shader, which is why the hash is integer throughout.
    Assert::AreEqual(PINNED_CELL_HASH, Neuron::BodyMeshBuilder::CellHash(0x1234u, 2, 5, 7),
                     L"the cell hash has changed; the grain of every body moves with it");

    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> once = BuildTerrain(OneContinent(), &ramp, stats);
    const std::vector<Neuron::FxVertex> twice = BuildTerrain(OneContinent(), &ramp, stats);
    Assert::AreEqual(HashVertices(once), HashVertices(twice), L"one description built two different meshes");

    Neuron::BodyDesc reseeded = OneContinent();
    reseeded.seed = 4322u;
    const std::vector<Neuron::FxVertex> different = BuildTerrain(reseeded, &ramp, stats);
    Assert::AreNotEqual(HashVertices(once), HashVertices(different), L"two seeds built the same mesh");
  }

  TEST_METHOD(TheWholeVertexListIsPinned)
  {
    // Determinism of the build, end to end and byte for byte: the field, the placement, the normals,
    // the ramp lookup and the dither, in one number (Design/PlanetRenderer.md 10).
    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneContinent(), &ramp, stats);

    Assert::AreEqual(PINNED_VERTEX_HASH, HashVertices(terrain), L"the mesh has changed; see PINNED_VERTEX_HASH");
    Assert::AreEqual(50.0f, stats.maxHeightMetres, 0.5f, L"the reported maximum height is not the field's");
  }
};
} // namespace NeuronClientTests
