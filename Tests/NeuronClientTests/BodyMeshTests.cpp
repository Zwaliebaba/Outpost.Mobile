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

const XMFLOAT3 OCEAN_COLOUR(0.10f, 0.22f, 0.40f);

// The same body with its sea drained. Everything about the grid -- how many triangles a cell makes,
// what a uv is -- is the same question on a dry body and is asked there, so that the ocean's own
// rules are the only thing the wet tests are about.
[[nodiscard]] Neuron::BodyDesc OneDryContinent()
{
  Neuron::BodyDesc desc = OneContinent();
  desc.outsideHeight = 0.01f;
  return desc;
}

[[nodiscard]] std::vector<Neuron::FxVertex> BuildTerrain(const Neuron::BodyDesc& _desc, const Neuron::ColourRamp* _ramp,
                                                         Neuron::BodyBuildStats& _outStats)
{
  const Neuron::BodyField field(_desc);
  std::vector<Neuron::FxVertex> terrain;
  std::vector<Neuron::MeshVertex> ocean;
  Neuron::BodyMeshBuilder::Build(field, _ramp, terrain, ocean, OCEAN_COLOUR, _outStats);
  return terrain;
}

[[nodiscard]] std::vector<Neuron::MeshVertex> BuildOcean(const Neuron::BodyDesc& _desc, Neuron::BodyBuildStats& _outStats)
{
  const Neuron::BodyField field(_desc);
  std::vector<Neuron::FxVertex> terrain;
  std::vector<Neuron::MeshVertex> ocean;
  Neuron::BodyMeshBuilder::Build(field, nullptr, terrain, ocean, OCEAN_COLOUR, _outStats);
  return ocean;
}

// The radius a vertex sits at once the ellipsoid is divided back out, which is the number both the
// shore dip and the ocean sphere are stated in.
[[nodiscard]] float RadiusOf(float _x, float _y, float _z, const XMFLOAT3& _ellipsoid)
{
  const float x = _x / _ellipsoid.x;
  const float y = _y / _ellipsoid.y;
  const float z = _z / _ellipsoid.z;
  return std::sqrt(x * x + y * y + z * z);
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

// The two builds of the test description through AxisRamp, byte for byte. **These are the replay key
// for the whole mesh**, the way PINNED_HEIGHT_METRES is for the field. If one moves, every body the
// game has drawn is a different body, and the pull request says why -- they are not numbers to
// update until a test goes green.
//
// The wet one moved once, when the ocean landed and a wet body stopped drawing its sea floor. The
// dry one has never moved, and that is the point of it: it was 0x0a3af4ac1bb89864 built by the
// builder that had no ocean in it, and it is that today.
constexpr std::uint64_t PINNED_WET_VERTEX_HASH = 0x5b93f37b6cd3c306ull;
constexpr std::uint64_t PINNED_DRY_VERTEX_HASH = 0x0a3af4ac1bb89864ull;
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
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneDryContinent(), &ramp, stats);

    constexpr std::uint32_t CELLS_PER_SIDE = 8;
    constexpr std::uint32_t TRIANGLES = Neuron::CUBE_FACE_COUNT * CELLS_PER_SIDE * CELLS_PER_SIDE * 2;
    Assert::AreEqual(TRIANGLES, stats.trianglesEmitted, L"a grid power of three did not emit six faces of eight by eight cells");
    Assert::AreEqual(static_cast<std::size_t>(TRIANGLES) * 3, terrain.size(), L"the vertex count is not three times the triangle count");
    Assert::AreEqual(0u, stats.trianglesCulled, L"a dry body had cells culled at a sea level it does not have");
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
    // On the dry body, so that the first triangle emitted really is the first cell of the first face
    // rather than whichever cell survived the sea-level cull.
    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneDryContinent(), &ramp, stats);

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

    Assert::AreEqual(PINNED_WET_VERTEX_HASH, HashVertices(terrain), L"the mesh has changed; see PINNED_WET_VERTEX_HASH");
    Assert::AreEqual(50.0f, stats.maxHeightMetres, 0.5f, L"the reported maximum height is not the field's");

    // Every cell is either drawn or culled and never both or neither, which is the one thing the
    // cull's early exit could get wrong without any pinned number noticing.
    constexpr std::uint32_t TRIANGLES = Neuron::CUBE_FACE_COUNT * 8 * 8 * 2;
    Assert::AreEqual(TRIANGLES, stats.trianglesEmitted + stats.trianglesCulled, L"the cells drawn and the cells culled do not add up");
  }

  TEST_METHOD(ADryBodyIsUntouchedByTheOcean)
  {
    // The whole of slice 5 is behind one sign test, and this is what says so: OneContinent's own
    // outsideHeight is below zero, so the dry version of it is the same description with that one
    // number moved. Its vertices are what they were before an ocean existed anywhere in the tree,
    // and nothing is culled or dipped.
    Neuron::BodyDesc dry = OneContinent();
    dry.outsideHeight = 0.01f;

    const Neuron::ColourRamp ramp = AxisRamp();
    const Neuron::BodyField field(dry);
    std::vector<Neuron::FxVertex> terrain;
    std::vector<Neuron::MeshVertex> ocean;
    Neuron::BodyBuildStats stats;
    Neuron::BodyMeshBuilder::Build(field, &ramp, terrain, ocean, OCEAN_COLOUR, stats);

    Assert::IsTrue(ocean.empty(), L"a dry body was given an ocean");
    Assert::AreEqual(0u, stats.trianglesCulled, L"a dry body had cells culled at sea level");
    Assert::AreEqual(static_cast<std::size_t>(6 * 8 * 8 * 2 * 3), terrain.size(), L"a dry body lost triangles");

    // Bitwise, not merely the same shape. This literal was produced by the builder that had no ocean
    // in it at all, and a dry body has to go on getting it: the whole of the ocean is behind one
    // sign test, and this is the assertion that says the sign test is where it says it is.
    Assert::AreEqual(PINNED_DRY_VERTEX_HASH, HashVertices(terrain), L"the ocean changed a body that has no ocean");
  }

  TEST_METHOD(AnOceanWorldWithNoLandDrawsNoTerrainAndAWholeOcean)
  {
    // The far end of the sign: nothing above the water anywhere, so every cell is culled and all
    // that is left is the sphere. It is worth pinning because the culling rule reads six samples and
    // a rule that read the wrong six would still cull most of them.
    Neuron::BodyDesc drowned = OneContinent();
    drowned.tiles.clear();

    Neuron::BodyBuildStats stats;
    const Neuron::ColourRamp ramp = AxisRamp();
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(drowned, &ramp, stats);

    constexpr std::uint32_t CELLS = 6 * 8 * 8;
    Assert::AreEqual(0u, stats.trianglesEmitted, L"a body with no land above water emitted terrain");
    Assert::AreEqual(CELLS * 2, stats.trianglesCulled, L"a drowned body did not cull every one of its cells");
    Assert::IsTrue(terrain.empty(), L"a body that emitted no triangles still wrote vertices");

    // gridPower 3 builds its ocean at 1: three samples a side, two cells, twelve triangles a face.
    const std::vector<Neuron::MeshVertex> ocean = BuildOcean(drowned, stats);
    Assert::AreEqual(static_cast<std::size_t>(6 * 2 * 2 * 2 * 3), ocean.size(), L"the ocean sphere is not the expected size");
  }

  TEST_METHOD(TheOceanIsASphereOfOneColourAtTheBodysRadius)
  {
    // Sea level is the radius, exactly: the terrain's heights are measured from it and the coast
    // dips below it, so an ocean at any other radius would put the whole shoreline in the wrong
    // place. The ellipsoid applies to it as it does to the land.
    Neuron::BodyDesc oblate = OneContinent();
    oblate.ellipsoid = XMFLOAT3(1.0f, 0.7f, 0.85f);

    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::MeshVertex> ocean = BuildOcean(oblate, stats);
    Assert::IsFalse(ocean.empty(), L"a wet body built no ocean");

    for (const Neuron::MeshVertex& vertex : ocean)
    {
      Assert::AreEqual(oblate.radiusMetres, RadiusOf(vertex.px, vertex.py, vertex.pz, oblate.ellipsoid), 1e-3f,
                       L"an ocean vertex is not at the body's radius");
      Assert::AreEqual(OCEAN_COLOUR.x, vertex.r, L"an ocean vertex is not the class's ocean colour");
      Assert::AreEqual(OCEAN_COLOUR.y, vertex.g, L"an ocean vertex is not the class's ocean colour");
      Assert::AreEqual(OCEAN_COLOUR.z, vertex.b, L"an ocean vertex is not the class's ocean colour");
    }
  }

  TEST_METHOD(TheShoreDipsWellUnderTheWater)
  {
    // A coast that met the water edge-on would show a seam of sea floor through it at any angle.
    // Every vertex that survived the cull is either land above the threshold or a dipped coast, and
    // there is nothing in between -- which is the property that closes the seam.
    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats stats;
    const std::vector<Neuron::FxVertex> terrain = BuildTerrain(OneContinent(), &ramp, stats);
    Assert::IsFalse(terrain.empty(), L"the wet test body emitted no terrain at all");

    const float radius = OneContinent().radiusMetres;
    const float threshold = radius + Neuron::BodyMeshBuilder::BODY_SHORE_THRESHOLD * radius;
    const float dipped = radius + Neuron::BodyMeshBuilder::BODY_SHORE_DIP * radius;

    const XMFLOAT3 sphere(1.0f, 1.0f, 1.0f);
    std::size_t dippedCount = 0;
    for (const Neuron::FxVertex& vertex : terrain)
    {
      const float atRadius = RadiusOf(vertex.px, vertex.py, vertex.pz, sphere);
      if (atRadius >= threshold)
        continue;

      Assert::AreEqual(dipped, atRadius, 1e-3f, L"a vertex sits between the shore threshold and the dip");
      ++dippedCount;
    }

    Assert::IsTrue(dippedCount > 0, L"no vertex was dipped, so this test proved nothing");
  }

  TEST_METHOD(TheDipMovesGeometryAndOnlyGeometry)
  {
    // What the dip must not do is change how much terrain there is or what it is coloured: it moves
    // a coast vertex down and nothing else. The two builds either side of the sign that turns it on
    // differ by one ten-thousandth of the radius in the field, so every triangle both of them drew
    // has to come out the same colour to well inside a ramp row.
    //
    // The design also asks that the colour be looked up from the *undipped* height, and it is -- but
    // that is not what this test checks, because on any sensible body it cannot be checked at all.
    // At a coastline the climate is already at sea level, so v is already 1 and the ramp's clamp
    // takes both answers to the same bottom row; the largest difference a dipped lookup could make
    // is the shore threshold over the maximum height, 0.3 m in 50, and the dither at sea level is
    // 0.19 of a ramp row, which is thirty times larger. A test written to catch it would pass
    // whichever height the builder used, and a test that cannot fail is worse than none.
    Neuron::BodyDesc barelyWet = OneContinent();
    barelyWet.outsideHeight = -0.0001f;
    Neuron::BodyDesc barelyDry = OneContinent();
    barelyDry.outsideHeight = 0.0001f;

    const Neuron::ColourRamp ramp = AxisRamp();
    Neuron::BodyBuildStats wetStats;
    Neuron::BodyBuildStats dryStats;
    const std::vector<Neuron::FxVertex> wet = BuildTerrain(barelyWet, &ramp, wetStats);
    const std::vector<Neuron::FxVertex> dry = BuildTerrain(barelyDry, &ramp, dryStats);

    // The wet build visits the cells in the same order and only ever leaves one out, so its triangles
    // are a subsequence of the dry build's and one cursor pairs them.
    //
    // Paired by direction and not by uv: a uv is the cell's (x, z) and carries no face, so the six
    // faces repeat every uv six times and a cursor that scanned for one would happily pair a cell
    // with its namesake on the next face. The direction is what a cell actually is, and the dip
    // moves a vertex along it rather than off it, so it survives exactly the change under test.
    const auto sameDirection = [](const Neuron::FxVertex& _a, const Neuron::FxVertex& _b)
    {
      const XMVECTOR a = XMVector3Normalize(XMVectorSet(_a.px, _a.py, _a.pz, 0.0f));
      const XMVECTOR b = XMVector3Normalize(XMVectorSet(_b.px, _b.py, _b.pz, 0.0f));
      return XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(a, b))) < 1e-8f;
    };

    std::size_t dryIndex = 0;
    std::size_t compared = 0;
    for (std::size_t wetIndex = 0; wetIndex < wet.size(); wetIndex += 3)
    {
      while (dryIndex < dry.size() && !(sameDirection(dry[dryIndex], wet[wetIndex]) && sameDirection(dry[dryIndex + 2], wet[wetIndex + 2])))
        dryIndex += 3;

      if (dryIndex >= dry.size())
        break;

      // The green channel is the climate axis of AxisRamp, which is the one the height feeds. Red is
      // the slope, and the slope genuinely does change when a vertex moves -- that is the dip doing
      // its job, not a defect.
      Assert::AreEqual(dry[dryIndex].g, wet[wetIndex].g, 0.02f, L"dipping a coast moved its colour up the climate axis");
      dryIndex += 3;
      ++compared;
    }

    Assert::IsTrue(compared > 100, L"too few triangles paired between the two builds to prove anything");
  }
};
} // namespace NeuronClientTests
