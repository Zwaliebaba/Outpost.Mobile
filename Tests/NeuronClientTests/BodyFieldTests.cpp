#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
// One tile of about fifty-seven degrees round +X on a thousand-metre ocean world. Every test below
// starts here and changes the one thing it is about, so a failure names its own cause.
[[nodiscard]] Neuron::BodyDesc OneContinent()
{
  Neuron::BodyDesc desc;
  desc.seed = 4321u;
  desc.radiusMetres = 1000.0f;
  desc.gridPower = 4;
  desc.outsideHeight = -0.02f;

  Neuron::BodyTile tile;
  tile.centre = XMFLOAT3(1.0f, 0.0f, 0.0f);
  tile.halfWidthRad = 1.0f;
  tile.edgeFraction = 0.25f;
  tile.desiredHeight = 0.05f;
  tile.posY = 0.0f;
  desc.tiles.push_back(tile);

  return desc;
}

[[nodiscard]] XMFLOAT3 Normalised(float _x, float _y, float _z)
{
  XMFLOAT3 direction;
  XMStoreFloat3(&direction, XMVector3Normalize(XMVectorSet(_x, _y, _z, 0.0f)));
  return direction;
}

// The height of OneContinent at one named sample, in metres. **This is the replay key.** If it moves,
// every world the game has ever generated is a different world, and the pull request says why it
// moved -- it is not a number to update until the test goes green.
constexpr float PINNED_HEIGHT_METRES = -2.48367691f;

// Pinned to a ten-thousandth of a metre on a body a kilometre across, rather than to the bit. The
// field is bit-identical between gcc and clang, at every optimisation level, and only moves at all
// -- by 1.5e-5 m -- when the compiler is allowed to contract a multiply and an add into an FMA,
// which /arch bans solution-wide. The remaining unknown is one vendor's pow against another's, and
// they are evaluated in double and narrowed once for exactly that reason (BodyField.cpp). A tenth of
// a millimetre is far tighter than any change to the field could hide inside.
constexpr float HEIGHT_PIN_TOLERANCE = 1e-4f;
} // namespace

TEST_CLASS(BodyFieldTests)
{
public:
  TEST_METHOD(TheSameDescriptionIsTheSameWorld)
  {
    // The whole feature rests on this. A body is a seed and a class; if two fields built from one
    // description could differ, a server could not describe a world in sixteen bytes and a screenshot
    // could not be reproduced (Design/Archive/PlanetRenderer.md 10).
    const Neuron::BodyDesc desc = OneContinent();
    const Neuron::BodyField field(desc);
    const Neuron::BodyField twin(desc);
    const std::uint32_t samples = Neuron::CubeSphere::SamplesPerSide(desc.gridPower);

    for (std::uint32_t i = 0; i < 500; ++i)
    {
      const XMFLOAT3 direction = Neuron::CubeSphere::Direction(static_cast<Neuron::CubeFace>(i % Neuron::CUBE_FACE_COUNT),
                                                               (i * 7u) % samples, (i * 13u) % samples, samples);
      Assert::AreEqual(field.Height(direction), twin.Height(direction), L"two fields built from one description disagree");
    }

    // And the value itself, not merely that two runs agree with each other.
    const XMFLOAT3 pinned = Neuron::CubeSphere::Direction(Neuron::CubeFace::PosX, 3, 5, samples);
    Assert::AreEqual(PINNED_HEIGHT_METRES, field.Height(pinned), HEIGHT_PIN_TOLERANCE,
                     L"the field has changed shape; see PINNED_HEIGHT_METRES");
  }

  TEST_METHOD(OutsideEveryTileTheHeightIsTheOutsideHeight)
  {
    // Below zero it is the sea floor and everything above it is a coastline. It is exact rather than
    // near: the tiles max-merge into it, so a direction no cap reaches has had nothing done to it at
    // all, and any drift here would be a fade leaking past its own rim.
    const Neuron::BodyDesc desc = OneContinent();
    const Neuron::BodyField field(desc);

    const float expected = desc.outsideHeight * desc.radiusMetres;
    Assert::AreEqual(expected, field.Height(XMFLOAT3(0.0f, -1.0f, 0.0f)), L"the height at the south pole is not the outside height");
    Assert::AreEqual(expected, field.Height(XMFLOAT3(-1.0f, 0.0f, 0.0f)), L"the height opposite the continent is not the outside height");
    Assert::AreEqual(expected, field.Height(XMFLOAT3(0.0f, 0.0f, 1.0f)),
                     L"the height a right angle from the continent is not the outside height");
  }

  TEST_METHOD(ATilesMaximumIsItsDesiredHeight)
  {
    // desiredHeight is the one number in a catalogue row that a person will reach for to say "make
    // this world's mountains higher", so it has to mean the height of the mountains and not an
    // amplitude somewhere inside a fractal. The constructor measures the tile and rescales it.
    const Neuron::BodyDesc desc = OneContinent();
    const Neuron::BodyField field(desc);

    const float expected = desc.tiles[0].desiredHeight * desc.radiusMetres;
    Assert::AreEqual(expected, field.MaxHeight(), expected * 0.01f, L"the field's maximum is not the tile's desired height");
  }

  TEST_METHOD(ADescriptionsOwnMaxHeightWins)
  {
    // A class that wants two worlds coloured on one scale sets it, and then the measurement is not
    // wanted -- a taller neighbour would otherwise recolour the shorter one.
    Neuron::BodyDesc desc = OneContinent();
    desc.maxHeight = 0.2f;
    const Neuron::BodyField field(desc);

    Assert::AreEqual(200.0f, field.MaxHeight(), 1e-3f, L"a description that set its own maxHeight did not get it");
  }

  TEST_METHOD(AnAbsoluteFlattenAreaIsFlat)
  {
    // A landing pad. Exactly flat rather than nearly flat, because the thing that will be put on it
    // is a building with a footprint, and "nearly" is what a building sinks into.
    Neuron::BodyDesc desc = OneContinent();
    Neuron::BodyFlatten pad;
    pad.centre = XMFLOAT3(1.0f, 0.0f, 0.0f);
    pad.halfWidthRad = 0.2f;
    pad.mode = Neuron::FlattenMode::Absolute;
    pad.value = 0.03f;
    desc.flatten.push_back(pad);
    const Neuron::BodyField field(desc);

    const float expected = pad.value * desc.radiusMetres;
    for (int i = 0; i < 10; ++i)
    {
      const float away = 0.15f * (i + 1) / 11.0f;
      const float around = XM_2PI * i / 10.0f;
      const XMFLOAT3 inside = Normalised(std::cos(away), std::sin(away) * std::cos(around), std::sin(away) * std::sin(around));
      Assert::AreEqual(expected, field.Height(inside), L"a direction inside an Absolute cap is not at the cap's height");
    }
  }

  TEST_METHOD(ASubtractFlattenAreaIsABowl)
  {
    // A crater: its full depth at the centre, nothing at all at the rim, and nothing outside it. The
    // depth is measured against the same body without the crater rather than against its own rim,
    // which is the only way to separate the bowl from the terrain it was dug into.
    Neuron::BodyDesc desc = OneContinent();
    Neuron::BodyFlatten crater;
    crater.centre = XMFLOAT3(1.0f, 0.0f, 0.0f);
    crater.halfWidthRad = 0.2f;
    crater.mode = Neuron::FlattenMode::Subtract;
    crater.value = 0.03f;
    desc.flatten.push_back(crater);

    const Neuron::BodyField plain(OneContinent());
    const Neuron::BodyField dug(desc);

    const XMFLOAT3 centre(1.0f, 0.0f, 0.0f);
    const float depth = plain.Height(centre) - dug.Height(centre);
    Assert::AreEqual(crater.value * desc.radiusMetres, depth, 1e-2f, L"the crater is not its stated depth at its centre");

    const float beyond = crater.halfWidthRad * 1.05f;
    const XMFLOAT3 outside = Normalised(std::cos(beyond), std::sin(beyond), 0.0f);
    Assert::AreEqual(plain.Height(outside), dug.Height(outside), L"the crater changed the height outside its own cap");
  }

  TEST_METHOD(BothPolesAreAsColdAsASummit)
  {
    // One flat landscape was one hemisphere, equator to pole, so the ramp's top row has to appear at
    // both ends of the axis and the equator has to read as its own altitude. The cap term is even in
    // the latitude, which is what buys the second pole for nothing (Design/Archive/PlanetRenderer.md 5.6).
    Neuron::BodyDesc desc = OneContinent();
    desc.outsideHeight = 0.01f; // dry: a pole under water would be colder than a summit for a duller reason
    desc.tiles[0].centre = XMFLOAT3(0.0f, 1.0f, 0.0f);
    desc.tiles[0].halfWidthRad = 3.2f;
    desc.tiles[0].edgeFraction = 0.05f;
    desc.polarStrength = 1.0f;
    desc.capStart = 0.5f;
    desc.capNoise = 0.1f;
    const Neuron::BodyField field(desc);

    const XMFLOAT3 north(0.0f, 1.0f, 0.0f);
    const XMFLOAT3 south(0.0f, -1.0f, 0.0f);
    const XMFLOAT3 equator(1.0f, 0.0f, 0.0f);

    Assert::IsTrue(field.Climate(north, field.Height(north)) >= field.MaxHeight(), L"the north pole is not as cold as a summit");
    Assert::IsTrue(field.Climate(south, field.Height(south)) >= field.MaxHeight(), L"the south pole is not as cold as a summit");

    // Exactly the height, not nearly it: the cap-edge dither is applied to the latitude inside the
    // smoothstep rather than to the cap value after it, so a body with no cap at its equator has
    // nothing at its equator -- no speckle, and no colour band where there is no cap.
    Assert::AreEqual(field.Height(equator), field.Climate(equator, field.Height(equator)), L"the equator picked up a polar cap");
  }

  TEST_METHOD(ClimateIsTheHeightWhenThereAreNoCaps)
  {
    // Asteroids set polarStrength to zero and the whole term has to disappear rather than round to
    // nothing, because the ramp lookup divides by maxHeight and a rounding here is a colour there.
    const Neuron::BodyDesc desc = OneContinent();
    const Neuron::BodyField field(desc);
    const std::uint32_t samples = Neuron::CubeSphere::SamplesPerSide(desc.gridPower);

    for (std::uint32_t i = 0; i < 60; ++i)
    {
      const XMFLOAT3 direction = Neuron::CubeSphere::Direction(static_cast<Neuron::CubeFace>(i % Neuron::CUBE_FACE_COUNT),
                                                               (i * 3u) % samples, (i * 5u) % samples, samples);
      const float height = field.Height(direction);
      Assert::AreEqual(height, field.Climate(direction, height), L"a body with no polar caps has a climate that is not its height");
    }
  }

  TEST_METHOD(TheEllipsoidReachesTheParameterBlock)
  {
    // BodyField does not place a vertex -- the builder does, at P = d * ellipsoid * (R + h) -- so what
    // this pins is that the ratios survive the flattening and that the rule they are used under is
    // written down somewhere a test can fail on. An oblate body at +Y is at half the radius; at +X it
    // is at all of it. Note that the axis scales the height with the radius, which is what the design
    // formula says and is why the pole is at 0.5 * (R + h) rather than at 0.5 * R + h.
    Neuron::BodyDesc desc = OneContinent();
    desc.ellipsoid = XMFLOAT3(1.0f, 0.5f, 1.0f);
    const Neuron::BodyField field(desc);

    Assert::AreEqual(1.0f, field.Params().radiusEllipsoid.y, L"the ellipsoid's x ratio did not reach the block");
    Assert::AreEqual(0.5f, field.Params().radiusEllipsoid.z, L"the ellipsoid's y ratio did not reach the block");
    Assert::AreEqual(1.0f, field.Params().radiusEllipsoid.w, L"the ellipsoid's z ratio did not reach the block");
    Assert::AreEqual(desc.radiusMetres, field.Params().radiusEllipsoid.x, L"the radius did not reach the block");

    const XMFLOAT3 up(0.0f, 1.0f, 0.0f);
    const XMFLOAT3 east(1.0f, 0.0f, 0.0f);
    const float atPole = 0.5f * (desc.radiusMetres + field.Height(up));
    const float atEquator = 1.0f * (desc.radiusMetres + field.Height(east));
    Assert::IsTrue(atPole < atEquator, L"an oblate body is not flatter at its pole than at its equator");
  }

  TEST_METHOD(TooManyTilesAreClippedAndTraced)
  {
    // Clipped rather than truncated in silence: a catalogue that asks for nine continents has a bug
    // in it, and the ninth going missing without a word is how that bug survives a review. The
    // harness cannot assert on a trace, so what is asserted is the count; the trace is beside it in
    // BodyField.cpp and is read in the debugger's output window.
    Neuron::BodyDesc desc = OneContinent();
    while (desc.tiles.size() < Neuron::BodyParams::MAX_TILES + 1)
    {
      Neuron::BodyTile extra = desc.tiles[0];
      const float turn = 0.4f * desc.tiles.size();
      extra.centre = XMFLOAT3(std::sin(turn), std::cos(turn), 0.2f);
      desc.tiles.push_back(extra);
    }

    const Neuron::BodyField field(desc);
    Assert::AreEqual(Neuron::BodyParams::MAX_TILES, field.Params().tileCount,
                     L"a description with too many tiles was not clipped to the capacity");
  }

  TEST_METHOD(AnOutOfRangeGridPowerIsClamped)
  {
    // The cost of the next grid power up is four times this one, and no body in the game wants it, so
    // a description that asks is clamped and traced rather than being allowed to spend a minute of
    // boot on a mistake.
    Neuron::BodyDesc desc = OneContinent();
    desc.gridPower = 20;
    const Neuron::BodyField field(desc);

    Assert::AreEqual(static_cast<float>(Neuron::BodyField::MAX_GRID_POWER), field.Params().outsideMaxHeightGrid.z,
                     L"an oversized grid power was not clamped");
  }
};
} // namespace NeuronClientTests
