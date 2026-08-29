#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
const XMFLOAT3 ORIGIN(0.0f, 0.0f, 0.0f);
const XMFLOAT3 RIGHT(1.0f, 0.0f, 0.0f);
const XMFLOAT3 UP(0.0f, 1.0f, 0.0f);

[[nodiscard]] const Neuron::SpriteTypeSpec& Spec(Neuron::SpriteType _type)
{
  return Neuron::SPRITE_TYPES[static_cast<std::size_t>(_type)];
}

[[nodiscard]] std::uint32_t CountOfType(const Neuron::SpriteParticles& _pool, Neuron::SpriteType _type)
{
  std::uint32_t found = 0;
  for (std::uint32_t i = 0; i < _pool.Count(); ++i)
  {
    if (_pool.At(i).type == _type)
      ++found;
  }
  return found;
}

void AssertVertex(const Neuron::FxVertex& _vertex, const XMFLOAT3& _pos, float _u, float _v, const wchar_t* _what)
{
  Assert::AreEqual(_pos.x, _vertex.px, 1e-5f, _what);
  Assert::AreEqual(_pos.y, _vertex.py, 1e-5f, _what);
  Assert::AreEqual(_pos.z, _vertex.pz, 1e-5f, _what);
  Assert::AreEqual(_u, _vertex.u, 1e-5f, _what);
  Assert::AreEqual(_v, _vertex.v, 1e-5f, _what);
}
} // namespace

TEST_CLASS(SpriteParticlesTests)
{
public:
  TEST_METHOD(TheCapacityIsHardAndARefusedEmitIsCounted)
  {
    // A pool that grew would turn a bad frame into a worse one, and a pool that overwrote would
    // lose a live particle in silence. It refuses, and says how often.
    Neuron::Pcg32 rng(1u);
    Neuron::SpriteParticles pool;
    pool.Init({8u});

    for (int i = 0; i < 8; ++i)
      pool.Emit(Neuron::SpriteType::Core, ORIGIN, ORIGIN, 100.0f, rng);
    Assert::AreEqual(8u, pool.Count(), L"eight emits did not fill the pool");
    Assert::AreEqual(0u, pool.Dropped(), L"a pool with room reported a drop");

    pool.Emit(Neuron::SpriteType::Core, ORIGIN, ORIGIN, 100.0f, rng);
    Assert::AreEqual(8u, pool.Count(), L"a full pool grew");
    Assert::AreEqual(1u, pool.Dropped(), L"a refused emit was not counted");
  }

  TEST_METHOD(TheColourIsPickedOnceAndNeverMoves)
  {
    Neuron::Pcg32 rng(2u);
    Neuron::SpriteParticles pool;
    pool.Init({8u});
    pool.Emit(Neuron::SpriteType::Core, ORIGIN, ORIGIN, 100.0f, rng);

    const XMFLOAT3 atBirth = pool.At(0).colour;
    const Neuron::SpriteTypeSpec& spec = Spec(Neuron::SpriteType::Core);
    Assert::IsTrue(atBirth.x >= spec.colourA.x && atBirth.x <= spec.colourB.x, L"the colour is not between the type's two ends");
    Assert::IsTrue(atBirth.y >= spec.colourA.y && atBirth.y <= spec.colourB.y, L"the colour is not between the type's two ends");
    Assert::IsTrue(atBirth.z >= spec.colourA.z && atBirth.z <= spec.colourB.z, L"the colour is not between the type's two ends");

    pool.Advance(1.0f, rng);
    const XMFLOAT3 later = pool.At(0).colour;
    Assert::IsTrue(atBirth.x == later.x && atBirth.y == later.y && atBirth.z == later.z, L"the colour shifted over the particle's life");
  }

  TEST_METHOD(AParticleDiesAtItsLifetime)
  {
    Neuron::Pcg32 rng(3u);
    Neuron::SpriteParticles pool;
    pool.Init({8u});
    pool.Emit(Neuron::SpriteType::Core, ORIGIN, ORIGIN, 100.0f, rng);

    pool.Advance(1.99f, rng);
    Assert::AreEqual(1u, pool.Count(), L"a core died before its two seconds were up");
    pool.Advance(0.01f, rng);
    Assert::AreEqual(0u, pool.Count(), L"a core outlived its two seconds");
  }

  TEST_METHOD(FrictionIsPerTypeAndZeroMeansZero)
  {
    Neuron::Pcg32 rng(4u);
    Neuron::SpriteParticles pool;
    pool.Init({8u});
    pool.Emit(Neuron::SpriteType::Core, ORIGIN, XMFLOAT3(10.0f, 0.0f, 0.0f), 100.0f, rng);
    pool.Emit(Neuron::SpriteType::Smoke, ORIGIN, XMFLOAT3(10.0f, 0.0f, 0.0f), 100.0f, rng);
    pool.Advance(1.0f, rng);

    for (std::uint32_t i = 0; i < pool.Count(); ++i)
    {
      const Neuron::SpriteParticles::Particle& particle = pool.At(i);
      if (particle.type == Neuron::SpriteType::Core)
        Assert::AreEqual(10.0f * (1.0f - 0.2f), particle.velMetresPerSec.x, 1e-5f, L"a core did not slow by its type's friction");
      else
        Assert::AreEqual(10.0f, particle.velMetresPerSec.x, 1e-6f, L"smoke has no friction and slowed anyway");
    }
  }

  TEST_METHOD(DebrisShedsSmokeAtTheTunedRate)
  {
    Neuron::Pcg32 rng(5u);
    Neuron::SpriteParticles pool;
    pool.Init({64u});
    pool.Emit(Neuron::SpriteType::Debris, ORIGIN, XMFLOAT3(10.0f, 0.0f, 0.0f), 40.0f, rng);

    for (int i = 0; i < 300; ++i) // five seconds at sixty frames
      pool.Advance(1.0f / 60.0f, rng);

    // Five a second over five seconds is twenty-five, and the earliest few have expired by now.
    const std::uint32_t smoke = CountOfType(pool, Neuron::SpriteType::Smoke);
    Assert::IsTrue(smoke >= 15u && smoke <= 35u, L"debris did not shed smoke at about five puffs a second");
    Assert::AreEqual(0u, pool.Dropped(), L"the pool was too small for this test to mean anything");
  }

  TEST_METHOD(SmokeInheritsTheDebrisSlowedAndSmaller)
  {
    Neuron::Pcg32 rng(6u);
    Neuron::SpriteParticles pool;
    pool.Init({64u});
    pool.Emit(Neuron::SpriteType::Debris, ORIGIN, XMFLOAT3(10.0f, 5.0f, -2.0f), 40.0f, rng);

    int steps = 0;
    while (pool.Count() == 1u && steps < 600)
    {
      pool.Advance(1.0f / 60.0f, rng);
      ++steps;
    }
    Assert::AreEqual(2u, pool.Count(), L"no smoke was ever emitted");

    const Neuron::SpriteParticles::Particle& debris = pool.At(0);
    const Neuron::SpriteParticles::Particle& smoke = pool.At(1);
    Assert::IsTrue(debris.type == Neuron::SpriteType::Debris && smoke.type == Neuron::SpriteType::Smoke,
                   L"the pool is not laid out as expected");
    Assert::AreEqual(debris.velMetresPerSec.x / 5.0f, smoke.velMetresPerSec.x, 1e-4f, L"smoke did not take a fifth of the debris velocity");
    Assert::AreEqual(debris.velMetresPerSec.y / 5.0f, smoke.velMetresPerSec.y, 1e-4f, L"smoke did not take a fifth of the debris velocity");
    Assert::AreEqual(debris.velMetresPerSec.z / 5.0f, smoke.velMetresPerSec.z, 1e-4f, L"smoke did not take a fifth of the debris velocity");
    Assert::AreEqual(debris.size / 1.5f, smoke.size, 1e-4f, L"smoke is not two thirds of the debris size");

    // Appended after the sweep, so it is not aged or moved in the frame it was born in. A pool that
    // emitted into itself mid-iteration would have advanced it once already.
    Assert::AreEqual(0.0f, smoke.ageSec, 1e-6f, L"the smoke was advanced in the frame it was emitted");
    Assert::AreEqual(debris.pos.x, smoke.pos.x, 1e-5f, L"the smoke moved before it existed");
  }

  TEST_METHOD(EmissionDuringTheSweepIsDeferredAndBounded)
  {
    // The source's own comment warns that emitting mid-iteration invalidates the iterator. A fixed
    // pool does not make that safe, it only turns the crash into a silent overwrite.
    Neuron::Pcg32 rng(7u);
    Neuron::SpriteParticles pool;
    pool.Init({4u});
    pool.Emit(Neuron::SpriteType::Debris, ORIGIN, XMFLOAT3(10.0f, 0.0f, 0.0f), 40.0f, rng);

    int steps = 0;
    while (pool.Dropped() == 0u && steps < 300)
    {
      pool.Advance(1.0f / 60.0f, rng);
      Assert::IsTrue(pool.Count() <= 4u, L"the pool overran its capacity during the sweep");
      ++steps;
    }

    Assert::IsTrue(pool.Dropped() > 0u, L"the pool never filled, so nothing about overflow was tested");
    Assert::AreEqual(1u, CountOfType(pool, Neuron::SpriteType::Debris), L"the debris was overwritten by the smoke it emitted");
  }

  TEST_METHOD(AlphaHoldsThenFadesOverTheLastQuarter)
  {
    Neuron::Pcg32 rng(8u);
    Neuron::SpriteParticles pool;
    pool.Init({8u});
    pool.Emit(Neuron::SpriteType::Core, ORIGIN, ORIGIN, 16.0f, rng);

    std::vector<Neuron::FxVertex> verts;
    pool.Build(Neuron::SpriteBlend::Additive, RIGHT, UP, verts);
    Assert::AreEqual(Neuron::SpriteParticles::PEAK_ALPHA, verts[0].a, 1e-5f, L"a new particle is not at peak alpha");

    verts.clear();
    pool.Advance(1.5f, rng); // exactly three quarters of a core's two seconds
    pool.Build(Neuron::SpriteBlend::Additive, RIGHT, UP, verts);
    Assert::AreEqual(Neuron::SpriteParticles::PEAK_ALPHA, verts[0].a, 1e-5f, L"the fade started before the last quarter");

    verts.clear();
    pool.Advance(0.25f, rng); // seven eighths: halfway through the fade
    pool.Build(Neuron::SpriteBlend::Additive, RIGHT, UP, verts);
    Assert::AreEqual(45.0f / 255.0f, verts[0].a, 1e-5f, L"the fade is not linear across the last quarter");

    verts.clear();
    pool.Advance(0.249f, rng);
    pool.Build(Neuron::SpriteBlend::Additive, RIGHT, UP, verts);
    Assert::IsTrue(verts[0].a < 0.001f, L"a particle about to expire is still visible");
  }

  TEST_METHOD(TheDarkPassCarriesZeroAlphaAndAPreScaledColour)
  {
    // This looks like a bug and is not: with a source alpha of zero the source term vanishes and
    // the frame becomes dest * (1 - src.rgb), which is what makes a light sprite read as smoke.
    Neuron::Pcg32 rng(9u);
    Neuron::SpriteParticles pool;
    pool.Init({8u});
    pool.Emit(Neuron::SpriteType::Smoke, ORIGIN, ORIGIN, 16.0f, rng);
    const XMFLOAT3 colour = pool.At(0).colour;

    std::vector<Neuron::FxVertex> verts;
    pool.Build(Neuron::SpriteBlend::Dark, RIGHT, UP, verts);
    Assert::AreEqual(static_cast<std::size_t>(6), verts.size(), L"a sprite did not build two triangles");
    for (const Neuron::FxVertex& vertex : verts)
    {
      Assert::AreEqual(0.0f, vertex.a, 1e-6f, L"the dark pass carried an alpha, so its source term would not vanish");
      Assert::AreEqual(colour.x, vertex.r, 1e-5f, L"a new dark sprite is not at its full colour");
      Assert::AreEqual(colour.y, vertex.g, 1e-5f, L"a new dark sprite is not at its full colour");
      Assert::AreEqual(colour.z, vertex.b, 1e-5f, L"a new dark sprite is not at its full colour");
    }

    verts.clear();
    pool.Build(Neuron::SpriteBlend::Additive, RIGHT, UP, verts);
    Assert::AreEqual(static_cast<std::size_t>(0), verts.size(), L"a dark type was built into the additive pass");

    verts.clear();
    pool.Advance(4.75f, rng); // 95 per cent of a five second life: one fifth of the way left
    pool.Build(Neuron::SpriteBlend::Dark, RIGHT, UP, verts);
    Assert::AreEqual(colour.x * 0.2f, verts[0].r, 1e-4f, L"the dark pass does not fade through its colour");
    Assert::AreEqual(colour.y * 0.2f, verts[0].g, 1e-4f, L"the dark pass does not fade through its colour");
    Assert::AreEqual(colour.z * 0.2f, verts[0].b, 1e-4f, L"the dark pass does not fade through its colour");
    Assert::AreEqual(0.0f, verts[0].a, 1e-6f, L"the dark pass gained an alpha as it faded");
  }

  TEST_METHOD(TheAdditivePassCarriesTheAlphaAndTheColourUntouched)
  {
    Neuron::Pcg32 rng(10u);
    Neuron::SpriteParticles pool;
    pool.Init({8u});
    pool.Emit(Neuron::SpriteType::Core, ORIGIN, ORIGIN, 16.0f, rng);
    const XMFLOAT3 colour = pool.At(0).colour;

    std::vector<Neuron::FxVertex> verts;
    pool.Build(Neuron::SpriteBlend::Additive, RIGHT, UP, verts);
    Assert::AreEqual(static_cast<std::size_t>(6), verts.size(), L"a sprite did not build two triangles");
    for (const Neuron::FxVertex& vertex : verts)
    {
      Assert::AreEqual(Neuron::SpriteParticles::PEAK_ALPHA, vertex.a, 1e-5f, L"the additive pass lost its alpha");
      Assert::AreEqual(colour.x, vertex.r, 1e-5f, L"the additive pass scaled its colour");
      Assert::AreEqual(colour.y, vertex.g, 1e-5f, L"the additive pass scaled its colour");
      Assert::AreEqual(colour.z, vertex.b, 1e-5f, L"the additive pass scaled its colour");
    }

    verts.clear();
    pool.Build(Neuron::SpriteBlend::Dark, RIGHT, UP, verts);
    Assert::AreEqual(static_cast<std::size_t>(0), verts.size(), L"an additive type was built into the dark pass");
  }

  TEST_METHOD(TheBillboardIsA45DegreeDiamond)
  {
    // Not an axis-aligned quad: the diamond is what hides the square silhouette of a 16x16 sprite.
    Neuron::Pcg32 rng(11u);
    Neuron::SpriteParticles pool;
    pool.Init({8u});
    pool.Emit(Neuron::SpriteType::Core, ORIGIN, ORIGIN, 16.0f, rng); // half size 16 / 16 = 1

    std::vector<Neuron::FxVertex> verts;
    pool.Build(Neuron::SpriteBlend::Additive, RIGHT, UP, verts);
    Assert::AreEqual(static_cast<std::size_t>(6), verts.size(), L"a sprite did not build two triangles");

    AssertVertex(verts[0], XMFLOAT3(0.0f, -1.0f, 0.0f), 0.0f, 0.0f, L"first triangle, bottom corner");
    AssertVertex(verts[1], XMFLOAT3(1.0f, 0.0f, 0.0f), 1.0f, 0.0f, L"first triangle, right corner");
    AssertVertex(verts[2], XMFLOAT3(0.0f, 1.0f, 0.0f), 1.0f, 1.0f, L"first triangle, top corner");
    AssertVertex(verts[3], XMFLOAT3(0.0f, -1.0f, 0.0f), 0.0f, 0.0f, L"second triangle, bottom corner");
    AssertVertex(verts[4], XMFLOAT3(0.0f, 1.0f, 0.0f), 1.0f, 1.0f, L"second triangle, top corner");
    AssertVertex(verts[5], XMFLOAT3(-1.0f, 0.0f, 0.0f), 0.0f, 1.0f, L"second triangle, left corner");

    for (const Neuron::FxVertex& vertex : verts)
      Assert::IsTrue(vertex.nx == 0.0f && vertex.ny == 0.0f && vertex.nz == 0.0f,
                     L"a sprite wrote a normal the sprite shader will not read");
  }

  TEST_METHOD(ClearEmptiesThePoolAndForgetsWhatItRefused)
  {
    Neuron::Pcg32 rng(12u);
    Neuron::SpriteParticles pool;
    pool.Init({2u});
    for (int i = 0; i < 5; ++i)
      pool.Emit(Neuron::SpriteType::Core, ORIGIN, ORIGIN, 100.0f, rng);
    Assert::AreEqual(3u, pool.Dropped(), L"three emits past a pool of two were not counted");

    pool.Clear();
    Assert::AreEqual(0u, pool.Count(), L"Clear left particles behind");
    Assert::AreEqual(0u, pool.Dropped(), L"Clear left the drop count behind");
  }

  TEST_METHOD(TheSameSeedBuildsTheSameVertices)
  {
    const auto run = [](std::vector<Neuron::FxVertex>& _out)
    {
      Neuron::Pcg32 rng(3u);
      Neuron::SpriteParticles pool;
      pool.Init({256u});
      for (int i = 0; i < 20; ++i)
        pool.Emit(Neuron::SpriteType::Debris, XMFLOAT3(static_cast<float>(i), 0.0f, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), 40.0f, rng);
      for (int i = 0; i < 120; ++i)
        pool.Advance(1.0f / 60.0f, rng);
      pool.Build(Neuron::SpriteBlend::Dark, RIGHT, UP, _out);
    };

    std::vector<Neuron::FxVertex> first;
    std::vector<Neuron::FxVertex> second;
    run(first);
    run(second);

    Assert::AreEqual(first.size(), second.size(), L"the same seed built a different number of vertices");
    Assert::IsTrue(std::memcmp(first.data(), second.data(), first.size() * sizeof(Neuron::FxVertex)) == 0,
                   L"the same seed built different vertices");
  }
};
} // namespace NeuronClientTests
