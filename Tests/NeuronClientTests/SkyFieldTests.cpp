#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
constexpr std::uint32_t VERTS_PER_QUAD = 6;

// One UNORM8 step. A color and the three twinkle numbers all go through the packing (SkyVertex.h),
// which moves a value by at most half of one, and a tolerance sitting exactly on that half fails on
// the rounding of the comparison itself.
constexpr float UNORM8_STEP = 1.0f / 255.0f;

[[nodiscard]] Neuron::SkyField::Desc SmallSky(std::uint64_t _seed)
{
  Neuron::SkyField::Desc desc;
  desc.seed = _seed;
  desc.starCount = 400;
  desc.brightStarCount = 5;
  desc.nebulaCount = 20;
  return desc;
}

[[nodiscard]] std::uint32_t QuadCount(const Neuron::SkyMesh& _sky, Neuron::SkyLayer _layer)
{
  return _sky.layerVertexCount[static_cast<std::uint32_t>(_layer)] / VERTS_PER_QUAD;
}

// The direction of quad _quad of _layer. Every vertex of a quad carries the same one, which is
// what the vertex shader relies on when it expands the corners around it.
[[nodiscard]] XMFLOAT3 QuadDirection(const Neuron::SkyMesh& _sky, Neuron::SkyLayer _layer, std::uint32_t _quad)
{
  return _sky.verts[_sky.LayerFirstVertex(_layer) + _quad * VERTS_PER_QUAD].Direction();
}

// The peak channel of a quad's color, which is what its brightness on screen is proportional to:
// the pixel shader multiplies the texture's alpha by this and adds it.
[[nodiscard]] float QuadPeak(const Neuron::SkyMesh& _sky, Neuron::SkyLayer _layer, std::uint32_t _quad)
{
  const XMFLOAT3 colour = _sky.verts[_sky.LayerFirstVertex(_layer) + _quad * VERTS_PER_QUAD].Colour();
  return std::max(colour.x, std::max(colour.y, colour.z));
}

// |galactic latitude| of a direction, given the pole the description named.
[[nodiscard]] float LatitudeOf(const XMFLOAT3& _direction, const XMFLOAT3& _pole)
{
  const XMVECTOR pole = XMVector3Normalize(XMLoadFloat3(&_pole));
  const float sinLatitude = std::clamp(XMVectorGetX(XMVector3Dot(pole, XMLoadFloat3(&_direction))), -1.0f, 1.0f);
  return std::abs(std::asin(sinLatitude));
}
} // namespace

TEST_CLASS(SkyFieldTests)
{
public:
  TEST_METHOD(TheSameSeedBuildsTheSameSkyByteForByte)
  {
    // The whole point of a seed. A sky that reproduced only approximately would make every
    // screenshot in a pull request unrepeatable and every one of the tests below a coin toss.
    Neuron::SkyMesh first;
    Neuron::SkyMesh second;
    Neuron::SkyField::Build(SmallSky(7u), first);
    Neuron::SkyField::Build(SmallSky(7u), second);

    Assert::AreEqual(first.verts.size(), second.verts.size(), L"two builds of one seed made different vertex counts");
    Assert::AreEqual(0, std::memcmp(first.verts.data(), second.verts.data(), first.verts.size() * sizeof(Neuron::SkyVertex)),
                     L"two builds of one seed differ somewhere in their bytes");
  }

  TEST_METHOD(ADifferentSeedBuildsADifferentSky)
  {
    Neuron::SkyMesh first;
    Neuron::SkyMesh second;
    Neuron::SkyField::Build(SmallSky(7u), first);
    Neuron::SkyField::Build(SmallSky(8u), second);

    Assert::AreEqual(first.verts.size(), second.verts.size(), L"the counts are the description's, not the seed's");
    Assert::AreNotEqual(0, std::memcmp(first.verts.data(), second.verts.data(), first.verts.size() * sizeof(Neuron::SkyVertex)),
                        L"two seeds produced the same sky");
  }

  TEST_METHOD(TheLayersAreContiguousAndInDrawOrder)
  {
    // SkyRenderer draws three ranges out of one buffer and computes where each begins by adding up
    // the ones before it. If the layers were not laid out in this order and back to back, it would
    // draw stars with the nebula texture and never say so.
    Neuron::SkyMesh sky;
    const Neuron::SkyField::Desc desc = SmallSky(11u);
    Neuron::SkyField::Build(desc, sky);

    Assert::AreEqual(desc.nebulaCount, QuadCount(sky, Neuron::SkyLayer::Nebula), L"the nebula count is not what was asked for");
    Assert::AreEqual(desc.starCount, QuadCount(sky, Neuron::SkyLayer::Star), L"the star count is not what was asked for");
    Assert::AreEqual(desc.brightStarCount, QuadCount(sky, Neuron::SkyLayer::Burst), L"the flare count is not what was asked for");

    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < Neuron::SKY_LAYER_COUNT; ++i)
      total += sky.layerVertexCount[i];
    Assert::AreEqual(static_cast<std::size_t>(total), sky.verts.size(), L"the layers do not account for every vertex");

    Assert::AreEqual(0u, sky.LayerFirstVertex(Neuron::SkyLayer::Nebula), L"the nebulae do not start at zero");
    Assert::AreEqual(desc.nebulaCount * VERTS_PER_QUAD, sky.LayerFirstVertex(Neuron::SkyLayer::Star),
                     L"the stars do not follow the nebulae");
    Assert::AreEqual((desc.nebulaCount + desc.starCount) * VERTS_PER_QUAD, sky.LayerFirstVertex(Neuron::SkyLayer::Burst),
                     L"the flares do not follow the stars");
  }

  TEST_METHOD(EveryQuadIsSixVerticesOfOneDirectionAndFourCorners)
  {
    // The vertex shader expands a quad around the direction every one of its six vertices carries,
    // and derives the texture coordinate from the corner. A quad whose vertices disagreed about
    // either would be a triangle pair somewhere other than where the star is.
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(SmallSky(3u), sky);

    for (std::size_t quad = 0; quad * VERTS_PER_QUAD < sky.verts.size(); ++quad)
    {
      const Neuron::SkyVertex& first = sky.verts[quad * VERTS_PER_QUAD];
      int corners = 0;
      for (std::uint32_t i = 0; i < VERTS_PER_QUAD; ++i)
      {
        const Neuron::SkyVertex& vertex = sky.verts[quad * VERTS_PER_QUAD + i];
        Assert::AreEqual(first.dirX, vertex.dirX, 0.0f, L"a quad's vertices point in different directions");
        Assert::AreEqual(first.dirY, vertex.dirY, 0.0f, L"a quad's vertices point in different directions");
        Assert::AreEqual(first.dirZ, vertex.dirZ, 0.0f, L"a quad's vertices point in different directions");
        Assert::AreEqual(first.HalfAngleRad(), vertex.HalfAngleRad(), 0.0f, L"a quad's vertices are different sizes");
        Assert::AreEqual(first.RollRad(), vertex.RollRad(), 0.0f, L"a quad's vertices are rolled differently");

        const XMFLOAT2 corner = vertex.Corner();
        Assert::AreEqual(1.0f, std::abs(corner.x), 1e-4f, L"a corner is not +-1, so SNORM16 was not exact");
        Assert::AreEqual(1.0f, std::abs(corner.y), 1e-4f, L"a corner is not +-1, so SNORM16 was not exact");
        // A bit per distinct corner: the two triangles must between them touch all four.
        corners |= 1 << ((corner.x > 0.0f ? 1 : 0) | (corner.y > 0.0f ? 2 : 0));
      }
      Assert::AreEqual(0xF, corners, L"a quad does not cover all four corners");

      // A direction the shader normalizes anyway, but one that was not unit here would mean the
      // generator produced it by adding vectors rather than by naming an angle.
      const XMFLOAT3 direction = first.Direction();
      const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
      Assert::AreEqual(1.0f, length, 1e-4f, L"a billboard's direction is not a unit vector");
    }
  }

  TEST_METHOD(FaintStarsVastlyOutnumberBrightOnes)
  {
    // The count law is the difference between a sky and a scattering of identical dots. Over the
    // magnitude range the generator uses, the brightest tenth of the *intensity* range should hold a
    // small minority of the stars -- a uniform draw would put roughly half of them there.
    Neuron::SkyField::Desc desc = SmallSky(21u);
    desc.starCount = 8000;
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(desc, sky);

    std::uint32_t bright = 0;
    std::uint32_t nearTheFloor = 0;
    float widest = 0.0f;
    const std::uint32_t first = sky.LayerFirstVertex(Neuron::SkyLayer::Star);
    for (std::uint32_t i = 0; i < QuadCount(sky, Neuron::SkyLayer::Star); ++i)
    {
      const float peak = QuadPeak(sky, Neuron::SkyLayer::Star, i);
      bright += (peak > 0.55f) ? 1u : 0u;
      nearTheFloor += (peak < 0.45f) ? 1u : 0u;
      widest = std::max(widest, sky.verts[first + i * VERTS_PER_QUAD].HalfAngleRad());
    }

    // Worked through against the law rather than guessed: over magnitudes -1 to 6.5 with the tone
    // curve the generator uses, a peak above 0.55 needs a star brighter than magnitude 2.0, which is
    // 0.19 % of the draw -- about fifteen of these eight thousand. A uniform draw would put a third of
    // them there.
    const float share = static_cast<float>(bright) / static_cast<float>(desc.starCount);
    Assert::IsTrue(share < 0.02f, L"too many stars are bright; the magnitude draw is not following the count law");
    Assert::IsTrue(bright >= 4u, L"no star is bright at all, so the sky has no structure in it");
    // And the other end: the same law puts nineteen stars in twenty near the faint end of the ramp.
    Assert::IsTrue(nearTheFloor > desc.starCount * 8 / 10, L"the faint end of the sky is not populated");
    // Magnitude is carried by size as much as by brightness, so the range has to be a real one: the
    // biggest star is several times the smallest, or every star is the same dot.
    Assert::IsTrue(widest > 0.005f, L"no star is large, so magnitude is not reaching the size ramp");
  }

  TEST_METHOD(TheStarsClusterTowardsTheGalacticPlane)
  {
    // The band. Without it the sky is uniform noise, which no real sky is.
    Neuron::SkyField::Desc desc = SmallSky(33u);
    desc.starCount = 4000;
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(desc, sky);

    // A uniformly spread sphere puts sin|b| under 0.25 for a quarter of its stars, so a quarter is
    // the number to beat and the band should beat it by a distance.
    const float nearPlaneRad = std::asin(0.25f);
    std::uint32_t nearPlane = 0;
    const std::uint32_t stars = QuadCount(sky, Neuron::SkyLayer::Star);
    for (std::uint32_t i = 0; i < stars; ++i)
      nearPlane += (LatitudeOf(QuadDirection(sky, Neuron::SkyLayer::Star, i), desc.galacticPole) < nearPlaneRad) ? 1u : 0u;

    const float share = static_cast<float>(nearPlane) / static_cast<float>(stars);
    Assert::IsTrue(share > 0.50f, L"the stars are not clustered towards the galactic plane");
  }

  TEST_METHOD(ANebulaOnTheBandOutshinesOneFarFromIt)
  {
    // The band's glow is unresolved starlight, so it is brightest where the stars are. A cloud far
    // off the plane is dust catching light from somewhere else and is much dimmer.
    Neuron::SkyField::Desc desc = SmallSky(45u);
    desc.nebulaCount = 400;
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(desc, sky);

    float onBandTotal = 0.0f;
    float offBandTotal = 0.0f;
    std::uint32_t onBand = 0;
    std::uint32_t offBand = 0;
    for (std::uint32_t i = 0; i < QuadCount(sky, Neuron::SkyLayer::Nebula); ++i)
    {
      const float latitude = LatitudeOf(QuadDirection(sky, Neuron::SkyLayer::Nebula, i), desc.galacticPole);
      const float peak = QuadPeak(sky, Neuron::SkyLayer::Nebula, i);
      if (latitude < 0.1f)
      {
        onBandTotal += peak;
        ++onBand;
      }
      else if (latitude > 0.8f)
      {
        offBandTotal += peak;
        ++offBand;
      }
    }

    Assert::IsTrue(onBand > 0u && offBand > 0u, L"the sample found no clouds on one side or the other");
    Assert::IsTrue(onBandTotal / static_cast<float>(onBand) > offBandTotal / static_cast<float>(offBand) * 1.25f,
                   L"a cloud on the band is not meaningfully brighter than one off it");
  }

  TEST_METHOD(ANebulaIsDimmerAndWiderThanAnyStar)
  {
    // If these two ever cross, the nebulosity stops being a background and starts being a foreground
    // of enormous blurry stars, which is what a sky looks like when its cloud intensity is tuned by
    // eye on one monitor.
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(SmallSky(57u), sky);

    const std::uint32_t clouds = QuadCount(sky, Neuron::SkyLayer::Nebula);
    const std::uint32_t stars = QuadCount(sky, Neuron::SkyLayer::Star);
    const std::uint32_t cloudFirst = sky.LayerFirstVertex(Neuron::SkyLayer::Nebula);
    const std::uint32_t starFirst = sky.LayerFirstVertex(Neuron::SkyLayer::Star);

    float brightestCloud = 0.0f;
    float cloudTotal = 0.0f;
    float narrowestCloud = 1e9f;
    for (std::uint32_t i = 0; i < clouds; ++i)
    {
      const float peak = QuadPeak(sky, Neuron::SkyLayer::Nebula, i);
      brightestCloud = std::max(brightestCloud, peak);
      cloudTotal += peak;
      narrowestCloud = std::min(narrowestCloud, sky.verts[cloudFirst + i * VERTS_PER_QUAD].HalfAngleRad());
    }

    float brightestStar = 0.0f;
    float starTotal = 0.0f;
    float widestStar = 0.0f;
    for (std::uint32_t i = 0; i < stars; ++i)
    {
      const float peak = QuadPeak(sky, Neuron::SkyLayer::Star, i);
      brightestStar = std::max(brightestStar, peak);
      starTotal += peak;
      widestStar = std::max(widestStar, sky.verts[starFirst + i * VERTS_PER_QUAD].HalfAngleRad());
    }

    // Not "dimmer than the faintest star": a star behind a dust lane is dimmer than anything, and an
    // assertion that said otherwise would be pinning the absence of the dust. The claim is that the
    // typical cloud is well under the typical star and that the brightest cloud is nowhere near the
    // brightest star -- which is what keeps the layer a background.
    Assert::IsTrue(cloudTotal / static_cast<float>(clouds) < starTotal / static_cast<float>(stars) * 0.5f,
                   L"the average cloud is not clearly dimmer than the average star");
    Assert::IsTrue(brightestCloud < brightestStar * 0.5f, L"the brightest cloud is competing with the brightest star");
    Assert::IsTrue(narrowestCloud > widestStar, L"the narrowest cloud is smaller than the widest star");
    // And a cloud has to survive the 8-bit packing at all, or the whole layer is black.
    Assert::IsTrue(brightestCloud > UNORM8_STEP, L"every cloud quantized to zero");
  }

  TEST_METHOD(TheFlaresSitOnTheBrightestStarsAndNowhereElse)
  {
    // A flare is drawn over a star that is already there, not instead of one and not beside one. A
    // sky whose standout stars sat somewhere other than its brightest stars would have two skies.
    Neuron::SkyMesh sky;
    const Neuron::SkyField::Desc desc = SmallSky(63u);
    Neuron::SkyField::Build(desc, sky);

    const std::uint32_t stars = QuadCount(sky, Neuron::SkyLayer::Star);
    for (std::uint32_t burst = 0; burst < QuadCount(sky, Neuron::SkyLayer::Burst); ++burst)
    {
      const XMFLOAT3 direction = QuadDirection(sky, Neuron::SkyLayer::Burst, burst);
      const float peak = QuadPeak(sky, Neuron::SkyLayer::Burst, burst);

      bool matched = false;
      for (std::uint32_t star = 0; star < stars; ++star)
      {
        const XMFLOAT3 starDirection = QuadDirection(sky, Neuron::SkyLayer::Star, star);
        const float starPeak = QuadPeak(sky, Neuron::SkyLayer::Star, star);
        if (starDirection.x == direction.x && starDirection.y == direction.y && starDirection.z == direction.z)
        {
          matched = true;
          // The flare is a fraction of its star's brightness, and never more than it: a flare that
          // outshone the star under it would read as the star and the star as a smudge.
          Assert::IsTrue(peak < starPeak + UNORM8_STEP, L"a flare is brighter than the star it sits on");
        }
      }
      Assert::IsTrue(matched, L"a flare sits where no star is");
    }

    // And they are a *selection*: the brightest star in the sky has one.
    float brightest = 0.0f;
    XMFLOAT3 brightestDirection(0.0f, 0.0f, 0.0f);
    for (std::uint32_t star = 0; star < stars; ++star)
    {
      const float peak = QuadPeak(sky, Neuron::SkyLayer::Star, star);
      if (peak > brightest)
      {
        brightest = peak;
        brightestDirection = QuadDirection(sky, Neuron::SkyLayer::Star, star);
      }
    }

    bool covered = false;
    for (std::uint32_t burst = 0; burst < QuadCount(sky, Neuron::SkyLayer::Burst); ++burst)
    {
      const XMFLOAT3 direction = QuadDirection(sky, Neuron::SkyLayer::Burst, burst);
      covered =
        covered || (direction.x == brightestDirection.x && direction.y == brightestDirection.y && direction.z == brightestDirection.z);
    }
    Assert::IsTrue(covered, L"the brightest star in the sky carries no flare");
  }

  TEST_METHOD(OnlyTheStarsTwinkle)
  {
    // A cloud that scintillated would read as a monitor problem. The shader has no branch for it:
    // a zero amount makes the sine term the identity, so the generator has to write the zero.
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(SmallSky(71u), sky);

    for (std::uint32_t i = 0; i < sky.layerVertexCount[static_cast<std::uint32_t>(Neuron::SkyLayer::Nebula)]; ++i)
      Assert::AreEqual(0.0f, sky.verts[i].TwinkleAmount(), 0.0f, L"a cloud twinkles");

    std::uint32_t twinkling = 0;
    const std::uint32_t first = sky.LayerFirstVertex(Neuron::SkyLayer::Star);
    for (std::uint32_t i = 0; i < QuadCount(sky, Neuron::SkyLayer::Star); ++i)
    {
      const Neuron::SkyVertex& vertex = sky.verts[first + i * VERTS_PER_QUAD];
      twinkling += (vertex.TwinkleAmount() > UNORM8_STEP) ? 1u : 0u;
      // The rate is a fraction of the frame's maximum and the shader multiplies by it, so a rate
      // outside 0..1 would be a star ticking at a speed the tuning constant does not bound.
      Assert::IsTrue(vertex.TwinkleRateFraction() > 0.0f && vertex.TwinkleRateFraction() <= 1.0f, L"a twinkle rate is out of range");
      Assert::IsTrue(vertex.TwinklePhaseFraction() >= 0.0f && vertex.TwinklePhaseFraction() <= 1.0f, L"a twinkle phase is out of range");
    }
    Assert::AreEqual(QuadCount(sky, Neuron::SkyLayer::Star), twinkling, L"a star does not twinkle at all");
  }

  TEST_METHOD(TheBlackbodyRampRunsRedThroughWhiteToBlue)
  {
    // 6 600 K is where the fit's two branches meet and is exactly white; either side of it the ramp
    // has to move the right way, or every star's color is the mirror of what its class says.
    const XMFLOAT3 white = Neuron::SkyField::TemperatureColour(6600.0f);
    Assert::AreEqual(1.0f, white.x, 0.02f, L"6 600 K is not white in red");
    Assert::AreEqual(1.0f, white.y, 0.02f, L"6 600 K is not white in green");
    Assert::AreEqual(1.0f, white.z, 0.02f, L"6 600 K is not white in blue");

    const XMFLOAT3 cool = Neuron::SkyField::TemperatureColour(3000.0f);
    Assert::AreEqual(1.0f, cool.x, 1e-4f, L"a cool star's peak channel is not red");
    Assert::IsTrue(cool.z < cool.y && cool.y < cool.x, L"a 3 000 K star is not orange-red");

    const XMFLOAT3 hot = Neuron::SkyField::TemperatureColour(20000.0f);
    Assert::AreEqual(1.0f, hot.z, 1e-4f, L"a hot star's peak channel is not blue");
    Assert::IsTrue(hot.x < hot.y && hot.y < hot.z, L"a 20 000 K star is not blue-white");

    // Clamped, not extrapolated: the fit is meaningless outside its range and a NaN in a vertex
    // color is a black star nobody can find.
    const XMFLOAT3 absurd = Neuron::SkyField::TemperatureColour(0.0f);
    Assert::IsTrue(absurd.x >= 0.0f && absurd.x <= 1.0f, L"a temperature below the fit's range escaped the clamp");
  }

  TEST_METHOD(AskingForMoreFlaresThanStarsIsNotADefect)
  {
    // The description is content, and content is allowed to be wrong: the flares are a selection out
    // of the stars, so asking for more of them than there are stars gives every star one rather than
    // reading off the end of the list.
    Neuron::SkyField::Desc desc = SmallSky(83u);
    desc.starCount = 4;
    desc.brightStarCount = 50;
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(desc, sky);

    Assert::AreEqual(4u, QuadCount(sky, Neuron::SkyLayer::Burst), L"the flare count was not capped at the star count");
  }

  TEST_METHOD(AnEmptyDescriptionBuildsAnEmptySkyAndSaysSo)
  {
    // SkyRenderer::UploadField traces and draws nothing on an empty field. It has to be reachable
    // without a crash, because a zero in a tuning constant is a plausible mistake.
    Neuron::SkyField::Desc desc;
    desc.starCount = 0;
    desc.brightStarCount = 0;
    desc.nebulaCount = 0;
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(desc, sky);

    Assert::IsTrue(sky.verts.empty(), L"an empty description produced vertices");
    for (std::uint32_t i = 0; i < Neuron::SKY_LAYER_COUNT; ++i)
      Assert::AreEqual(0u, sky.layerVertexCount[i], L"an empty description left a layer with a count");
  }

  TEST_METHOD(BuildingTwiceIntoOneMeshReplacesItRatherThanAppending)
  {
    // The composition root reuses one SkyMesh across an F5, and a Build that appended would grow the
    // buffer without bound while every layer offset after the first went quietly wrong.
    Neuron::SkyMesh sky;
    Neuron::SkyField::Build(SmallSky(91u), sky);
    const std::size_t once = sky.verts.size();
    Neuron::SkyField::Build(SmallSky(91u), sky);

    Assert::AreEqual(once, sky.verts.size(), L"a second Build into the same mesh did not replace the first");
  }
};
} // namespace NeuronClientTests
