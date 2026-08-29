#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
constexpr float PI = 3.14159265358979323846f;

// Every test here builds its mesh by hand. The parser has its own suite, and a shatter test that
// loaded a file would be measuring both.
Neuron::MeshData OneTriangle(const XMFLOAT3& _a, const XMFLOAT3& _b, const XMFLOAT3& _c,
                             const XMFLOAT3& _colour = XMFLOAT3(1.0f, 1.0f, 1.0f))
{
  Neuron::MeshData mesh;
  mesh.verts = {
    {_a.x, _a.y, _a.z, _colour.x, _colour.y, _colour.z},
    {_b.x, _b.y, _b.z, _colour.x, _colour.y, _colour.z},
    {_c.x, _c.y, _c.z, _colour.x, _colour.y, _colour.z},
  };
  mesh.boundsMin = XMFLOAT3(std::min({_a.x, _b.x, _c.x}), std::min({_a.y, _b.y, _c.y}), std::min({_a.z, _b.z, _c.z}));
  mesh.boundsMax = XMFLOAT3(std::max({_a.x, _b.x, _c.x}), std::max({_a.y, _b.y, _c.y}), std::max({_a.z, _b.z, _c.z}));
  return mesh;
}

// _count triangles, each with a circumference of about 10 m so none is rejected for size.
Neuron::MeshData ManyTriangles(int _count)
{
  Neuron::MeshData mesh;
  mesh.verts.reserve(static_cast<std::size_t>(_count) * 3);
  for (int i = 0; i < _count; ++i)
  {
    const float x = static_cast<float>(i) * 10.0f;
    mesh.verts.push_back({x, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f});
    mesh.verts.push_back({x + 3.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f});
    mesh.verts.push_back({x, 3.0f, 0.0f, 1.0f, 1.0f, 1.0f});
  }
  mesh.boundsMin = XMFLOAT3(0.0f, 0.0f, 0.0f);
  mesh.boundsMax = XMFLOAT3(static_cast<float>(_count) * 10.0f, 3.0f, 0.0f);
  return mesh;
}

XMFLOAT4X4 Identity()
{
  XMFLOAT4X4 world;
  XMStoreFloat4x4(&world, XMMatrixIdentity());
  return world;
}

XMFLOAT4X4 Translation(float _x, float _y, float _z)
{
  XMFLOAT4X4 world;
  XMStoreFloat4x4(&world, XMMatrixTranslation(_x, _y, _z));
  return world;
}

void AssertFloat3(const XMFLOAT3& _expected, const XMFLOAT3& _actual, float _tolerance, const wchar_t* _what)
{
  Assert::AreEqual(_expected.x, _actual.x, _tolerance, _what);
  Assert::AreEqual(_expected.y, _actual.y, _tolerance, _what);
  Assert::AreEqual(_expected.z, _actual.z, _tolerance, _what);
}
} // namespace

TEST_CLASS(MeshShatterTests)
{
public:
  TEST_METHOD(OneTriangleBecomesOneFragmentAboutItsCentroid)
  {
    const Neuron::MeshData mesh = OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f));
    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter shatter;
    const std::uint32_t dropped = shatter.Spawn(mesh, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), rng);

    Assert::AreEqual(0u, dropped, L"a single triangle was reported dropped");
    Assert::AreEqual(1u, shatter.FragmentCount(), L"one triangle did not give one fragment");

    const Neuron::MeshShatter::Fragment& fragment = shatter.FragmentAt(0);
    AssertFloat3(XMFLOAT3(2.0f / 3.0f, 2.0f / 3.0f, 0.0f), fragment.pos, 1e-5f, L"the fragment is not at the triangle's centroid");

    // The three corners are stored relative to the centroid, so they have to sum to nothing.
    const XMFLOAT3 sum(fragment.v0.x + fragment.v1.x + fragment.v2.x, fragment.v0.y + fragment.v1.y + fragment.v2.y,
                       fragment.v0.z + fragment.v1.z + fragment.v2.z);
    AssertFloat3(XMFLOAT3(0.0f, 0.0f, 0.0f), sum, 1e-5f, L"the corners are not relative to the centroid");
    Assert::AreEqual(1.0f, XMVectorGetX(XMVector3Length(XMLoadFloat3(&fragment.normal))), 1e-5f, L"the fragment normal is not unit length");

    std::vector<Neuron::FxVertex> verts;
    shatter.Build(verts);
    Assert::AreEqual(static_cast<std::size_t>(3), verts.size(), L"a fragment did not build three vertices");
    constexpr float US[3] = {0.0f, 0.0f, 1.0f};
    constexpr float VS[3] = {0.0f, 1.0f, 1.0f};
    for (int i = 0; i < 3; ++i)
    {
      Assert::AreEqual(US[i], verts[static_cast<std::size_t>(i)].u, 1e-5f, L"the fragment's u is not the decal's");
      Assert::AreEqual(VS[i], verts[static_cast<std::size_t>(i)].v, 1e-5f, L"the fragment's v is not the decal's");
      Assert::AreEqual(1.0f, verts[static_cast<std::size_t>(i)].a, 1e-5f, L"a fragment is not opaque at age zero");
    }
  }

  TEST_METHOD(VelocityIsRadialFromTheCentrePlusWhatTheShipCarried)
  {
    Neuron::MeshData mesh = OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f));
    mesh.boundsMin = XMFLOAT3(-2.0f, -2.0f, -2.0f);
    mesh.boundsMax = XMFLOAT3(2.0f, 2.0f, 2.0f); // bounds centre at the origin

    const XMFLOAT3 inherited(5.0f, 0.0f, -7.0f);
    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter shatter;
    Neuron::MeshShatter::Desc desc;
    desc.radialSpeedPerSec = 3.0f;
    shatter.Spawn(mesh, Identity(), inherited, desc, rng);

    const XMFLOAT3 centroid(2.0f / 3.0f, 2.0f / 3.0f, 0.0f);
    const XMFLOAT3 expected(centroid.x * 3.0f + inherited.x, centroid.y * 3.0f + inherited.y, centroid.z * 3.0f + inherited.z);
    AssertFloat3(expected, shatter.FragmentAt(0).velMetresPerSec, 1e-5f, L"the throw is not radial, or the ship's velocity was lost");
  }

  TEST_METHOD(TheWorldTransformMovesTheCentreTooSoTheThrowIsUnchanged)
  {
    // The trap this pins: transforming the vertices but shattering about the untransformed centre
    // makes every fragment of a ship far from the origin fly the same way.
    Neuron::MeshData mesh = OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f));
    mesh.boundsMin = XMFLOAT3(-2.0f, -2.0f, -2.0f);
    mesh.boundsMax = XMFLOAT3(2.0f, 2.0f, 2.0f);

    Neuron::Pcg32 here(1u);
    Neuron::MeshShatter atOrigin;
    atOrigin.Spawn(mesh, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), here);

    Neuron::Pcg32 there(1u);
    Neuron::MeshShatter moved;
    moved.Spawn(mesh, Translation(100.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), there);

    AssertFloat3(atOrigin.FragmentAt(0).velMetresPerSec, moved.FragmentAt(0).velMetresPerSec, 1e-4f,
                 L"moving the mesh changed the throw, so the centre did not move with it");
  }

  TEST_METHOD(DegenerateAndSmallTrianglesAreSkipped)
  {
    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter shatter;

    const Neuron::MeshData coincident = OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 5.0f, 0.0f));
    shatter.Spawn(coincident, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), rng);
    Assert::AreEqual(0u, shatter.FragmentCount(), L"a triangle with two coincident vertices became a fragment");

    // A right triangle with legs L has a circumference of L * (2 + sqrt(2)); this one is 5.9 m.
    const float leg = 5.9f / (2.0f + std::sqrt(2.0f));
    const Neuron::MeshData small = OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(leg, 0.0f, 0.0f), XMFLOAT3(0.0f, leg, 0.0f));

    Neuron::MeshShatter::Desc strict;
    strict.minCircumferenceMetres = 6.0f;
    shatter.Spawn(small, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), strict, rng);
    Assert::AreEqual(0u, shatter.FragmentCount(), L"a triangle under the minimum circumference became a fragment");

    Neuron::MeshShatter::Desc lenient;
    lenient.minCircumferenceMetres = 5.0f;
    shatter.Spawn(small, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), lenient, rng);
    Assert::AreEqual(1u, shatter.FragmentCount(), L"the same triangle was skipped with the threshold below it");
  }

  TEST_METHOD(TheCapCountsKeptFragmentsAndReportsTheRest)
  {
    // The cap is on fragments kept, not triangles visited, and what it refused is the return value.
    // A hull with more faces than the cap loses the difference in silence otherwise.
    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter shatter;
    Neuron::MeshShatter::Desc desc;
    desc.maxFragments = 5;

    const std::uint32_t dropped = shatter.Spawn(ManyTriangles(20), Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), desc, rng);
    Assert::AreEqual(5u, shatter.FragmentCount(), L"the cap did not hold");
    Assert::AreEqual(15u, dropped, L"the reported drop count is not what the cap refused");

    // Half the triangles here are under the minimum circumference. Counting triangles visited
    // instead of fragments kept would stop after five of them and shatter three.
    Neuron::MeshData mixed = ManyTriangles(20);
    for (std::size_t i = 1; i < 20; i += 2)
    {
      mixed.verts[i * 3 + 1].px = mixed.verts[i * 3 + 0].px + 0.1f;
      mixed.verts[i * 3 + 2].py = 0.1f;
    }
    const std::uint32_t mixedDropped = shatter.Spawn(mixed, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), desc, rng);
    Assert::AreEqual(5u, shatter.FragmentCount(), L"the cap counted triangles visited rather than fragments kept");
    Assert::AreEqual(5u, mixedDropped, L"triangles rejected for size were counted as dropped by the cap");
  }

  TEST_METHOD(FractionThinsTheHullWithoutCountingDrops)
  {
    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter shatter;
    Neuron::MeshShatter::Desc desc;
    desc.fraction = 0.5f;
    desc.maxFragments = 100000;

    const std::uint32_t dropped = shatter.Spawn(ManyTriangles(2000), Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), desc, rng);
    Assert::IsTrue(shatter.FragmentCount() >= 900u && shatter.FragmentCount() <= 1100u,
                   L"half of two thousand triangles is not what survived");
    Assert::AreEqual(0u, dropped, L"thinning was reported as a drop; only the cap drops");
  }

  TEST_METHOD(EveryFragmentPicksOneOfTheFiveTumblers)
  {
    Neuron::Pcg32 rng(4u);
    Neuron::MeshShatter shatter;
    Neuron::MeshShatter::Desc desc;
    desc.maxFragments = 100000;
    shatter.Spawn(ManyTriangles(1000), Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), desc, rng);

    Assert::AreEqual(1000u, shatter.FragmentCount(), L"the mesh did not shatter completely");
    for (std::uint32_t i = 0; i < shatter.FragmentCount(); ++i)
      Assert::IsTrue(shatter.FragmentAt(i).tumbler < Neuron::MeshShatter::TUMBLER_COUNT,
                     L"a fragment indexed a tumbler that does not exist");
  }

  TEST_METHOD(TheFragmentColourIsTheHullsOwnMix)
  {
    const Neuron::MeshData mesh =
      OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f), XMFLOAT3(1.0f, 0.0f, 0.0f));
    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter shatter;
    std::vector<Neuron::FxVertex> verts;

    shatter.Spawn(mesh, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), rng);
    shatter.Build(verts);
    Assert::AreEqual(1.0f, verts[0].r, 1e-5f, L"the default Desc changed the vertex colour");
    Assert::AreEqual(0.0f, verts[0].g, 1e-5f, L"the default Desc changed the vertex colour");
    Assert::AreEqual(0.0f, verts[0].b, 1e-5f, L"the default Desc changed the vertex colour");

    // lerp(tint, vertex, mix) -- the same operand order ScenePS mixes a hull in.
    Neuron::MeshShatter::Desc tinted;
    tinted.tintColour = XMFLOAT3(0.0f, 0.0f, 1.0f);
    tinted.tintMix = 0.25f;
    verts.clear();
    shatter.Spawn(mesh, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), tinted, rng);
    shatter.Build(verts);
    Assert::AreEqual(0.25f, verts[0].r, 1e-5f, L"the tint mixed the wrong way round");
    Assert::AreEqual(0.0f, verts[0].g, 1e-5f, L"the tint mixed the wrong way round");
    Assert::AreEqual(0.75f, verts[0].b, 1e-5f, L"the tint mixed the wrong way round");
  }

  TEST_METHOD(ARotationAboutYIsLeftHanded)
  {
    // The convention the tumble is built on, pinned on its own so that a failure below points at
    // the composition rather than at the handedness. LH: +x turns towards -z.
    const XMMATRIX quarterTurn = XMMatrixRotationRollPitchYaw(0.0f, PI * 0.5f, 0.0f);
    XMFLOAT3 turned;
    const XMFLOAT3 alongX(1.0f, 0.0f, 0.0f);
    XMStoreFloat3(&turned, XMVector3TransformNormal(XMLoadFloat3(&alongX), quarterTurn));
    AssertFloat3(XMFLOAT3(0.0f, 0.0f, -1.0f), turned, 1e-4f, L"a quarter turn about Y is not left-handed");

    XMStoreFloat3(&turned, XMVector3TransformNormal(XMLoadFloat3(&alongX), XMMatrixMultiply(quarterTurn, quarterTurn)));
    AssertFloat3(XMFLOAT3(-1.0f, 0.0f, 0.0f), turned, 1e-4f, L"two quarter turns about Y are not a half turn");
  }

  TEST_METHOD(TheTumbleIsPostMultipliedAndTheBuildUsesIt)
  {
    // Pre-multiplying spins a fragment about world axes and it visibly precesses. The two steps are
    // deliberately different lengths, because a rotation always commutes with itself and a test
    // that took two equal steps would pass either way round.
    Neuron::Pcg32 rng(11u);
    Neuron::MeshShatter shatter;
    shatter.Spawn(OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f)), Identity(),
                  XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), rng);

    const std::uint8_t index = shatter.FragmentAt(0).tumbler;
    constexpr float FIRST_DT = 0.25f;
    constexpr float SECOND_DT = 0.5f;

    const XMFLOAT3 before = shatter.TumblerAt(index).angVelRadPerSec;
    Assert::IsTrue(std::fabs(before.x) + std::fabs(before.y) + std::fabs(before.z) > 0.1f,
                   L"the tumbler is barely turning, so this proves nothing");
    const XMMATRIX first = XMMatrixRotationRollPitchYaw(before.x * FIRST_DT, before.y * FIRST_DT, before.z * FIRST_DT);
    shatter.Advance(FIRST_DT);

    const XMFLOAT3 between = shatter.TumblerAt(index).angVelRadPerSec;
    const XMMATRIX second = XMMatrixRotationRollPitchYaw(between.x * SECOND_DT, between.y * SECOND_DT, between.z * SECOND_DT);
    shatter.Advance(SECOND_DT);

    XMFLOAT3X3 got = shatter.TumblerAt(index).rot;
    XMFLOAT3X3 postMultiplied;
    XMFLOAT3X3 preMultiplied;
    XMStoreFloat3x3(&postMultiplied, XMMatrixMultiply(first, second));
    XMStoreFloat3x3(&preMultiplied, XMMatrixMultiply(second, first));

    float separation = 0.0f;
    for (int row = 0; row < 3; ++row)
    {
      for (int column = 0; column < 3; ++column)
      {
        Assert::AreEqual(postMultiplied.m[row][column], got.m[row][column], 1e-4f, L"the tumble did not compose as rot * delta");
        separation = std::max(separation, std::fabs(postMultiplied.m[row][column] - preMultiplied.m[row][column]));
      }
    }
    Assert::IsTrue(separation > 1e-3f, L"the two orders agree on this tumbler, so the test cannot tell them apart");

    // And the built vertex is that rotation applied to the stored corner, not the corner itself.
    std::vector<Neuron::FxVertex> verts;
    shatter.Build(verts);
    const Neuron::MeshShatter::Fragment& fragment = shatter.FragmentAt(0);
    XMFLOAT3 expected;
    XMStoreFloat3(&expected,
                  XMVectorAdd(XMVector3TransformNormal(XMLoadFloat3(&fragment.v0), XMLoadFloat3x3(&got)), XMLoadFloat3(&fragment.pos)));
    AssertFloat3(expected, XMFLOAT3(verts[0].px, verts[0].py, verts[0].pz), 1e-4f, L"the build ignored the tumbler's rotation");
  }

  TEST_METHOD(FrictionSlowsAFragmentAndNeverReversesIt)
  {
    Neuron::MeshData mesh = OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f));
    mesh.boundsMin = XMFLOAT3(0.0f, 0.0f, 0.0f);
    mesh.boundsMax = XMFLOAT3(0.0f, 0.0f, 0.0f); // centre on the origin, so the throw is the centroid

    Neuron::MeshShatter::Desc desc;
    desc.radialSpeedPerSec = 0.0f; // the whole velocity is the inherited one, so it is exactly known
    desc.frictionCoef = 0.05f;
    desc.lifetimeSec = 1000.0f;

    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter slowing;
    slowing.Spawn(mesh, Identity(), XMFLOAT3(100.0f, 0.0f, 0.0f), desc, rng);
    slowing.Advance(1.0f);
    const XMFLOAT3 slowed = slowing.FragmentAt(0).velMetresPerSec;
    Assert::AreEqual(95.0f, slowed.x, 1e-4f, L"one second of drag did not take 5 per cent off");
    Assert::IsTrue(slowed.x > 0.0f && slowed.x < 100.0f, L"the drag did not slow the fragment, or turned it round");

    Neuron::MeshShatter stopping;
    stopping.Spawn(mesh, Identity(), XMFLOAT3(100.0f, 0.0f, 0.0f), desc, rng);
    stopping.Advance(100.0f);
    AssertFloat3(XMFLOAT3(0.0f, 0.0f, 0.0f), stopping.FragmentAt(0).velMetresPerSec, 1e-4f,
                 L"a step long enough to remove all the speed reversed the fragment instead");
  }

  TEST_METHOD(AngularFrictionShrinksTheSpinByTheTunedFraction)
  {
    Neuron::Pcg32 rng(3u);
    Neuron::MeshShatter shatter;
    Neuron::MeshShatter::Desc desc;
    desc.rotFrictionCoef = 0.2f;
    shatter.Spawn(ManyTriangles(4), Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), desc, rng);

    XMFLOAT3 before[Neuron::MeshShatter::TUMBLER_COUNT];
    for (int i = 0; i < Neuron::MeshShatter::TUMBLER_COUNT; ++i)
      before[i] = shatter.TumblerAt(i).angVelRadPerSec;

    constexpr float DT = 0.5f;
    shatter.Advance(DT);
    const float expected = 1.0f - DT * 0.2f;
    for (int i = 0; i < Neuron::MeshShatter::TUMBLER_COUNT; ++i)
    {
      const XMFLOAT3& after = shatter.TumblerAt(i).angVelRadPerSec;
      Assert::AreEqual(before[i].x * expected, after.x, 1e-5f, L"angular drag is not 1 - dt * rotFriction");
      Assert::AreEqual(before[i].y * expected, after.y, 1e-5f, L"angular drag is not 1 - dt * rotFriction");
      Assert::AreEqual(before[i].z * expected, after.z, 1e-5f, L"angular drag is not 1 - dt * rotFriction");
    }
  }

  TEST_METHOD(TheShatterFadesOverItsLifetimeAndThenReportsItself)
  {
    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter shatter;
    shatter.Spawn(OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f)), Identity(),
                  XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), rng);

    Assert::IsFalse(shatter.Advance(4.99f), L"the shatter expired before its lifetime");

    Neuron::MeshShatter halfway;
    halfway.Spawn(OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f)), Identity(),
                  XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), rng);
    Assert::IsFalse(halfway.Advance(2.5f), L"the shatter expired halfway through its lifetime");

    std::vector<Neuron::FxVertex> verts;
    halfway.Build(verts);
    Assert::AreEqual(0.5f, verts[0].a, 1e-5f, L"the fade is not linear over the lifetime");

    Assert::IsTrue(halfway.Advance(2.5f), L"the shatter did not report itself finished at its lifetime");
  }

  TEST_METHOD(GravityIsOffUnlessItIsAskedFor)
  {
    Neuron::MeshData mesh = OneTriangle(XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(2.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 2.0f, 0.0f));
    mesh.boundsMin = XMFLOAT3(2.0f / 3.0f, 2.0f / 3.0f, 0.0f);
    mesh.boundsMax = XMFLOAT3(2.0f / 3.0f, 2.0f / 3.0f, 0.0f); // centre on the centroid: nothing is thrown

    Neuron::Pcg32 rng(1u);
    Neuron::MeshShatter weightless;
    weightless.Spawn(mesh, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), Neuron::MeshShatter::Desc(), rng);
    weightless.Advance(1.0f);
    AssertFloat3(XMFLOAT3(0.0f, 0.0f, 0.0f), weightless.FragmentAt(0).velMetresPerSec, 1e-5f, L"a fragment fell with the default Desc");

    Neuron::MeshShatter::Desc planetary;
    planetary.gravityMetresPerSec2 = XMFLOAT3(0.0f, -10.0f, 0.0f);
    Neuron::MeshShatter falling;
    falling.Spawn(mesh, Identity(), XMFLOAT3(0.0f, 0.0f, 0.0f), planetary, rng);
    falling.Advance(1.0f);
    Assert::AreEqual(-10.0f, falling.FragmentAt(0).velMetresPerSec.y, 1e-5f, L"gravity did not reach the fragment");
  }

  TEST_METHOD(TheSameSeedShattersTheSameWay)
  {
    // What makes a recorded death replay: two clients, or two runs, that agree on the seed agree on
    // every tumble and every fragment.
    Neuron::MeshShatter::Desc desc;
    desc.maxFragments = 100000;
    const Neuron::MeshData mesh = ManyTriangles(200);

    Neuron::Pcg32 firstRng(7u);
    Neuron::MeshShatter first;
    first.Spawn(mesh, Identity(), XMFLOAT3(1.0f, 2.0f, 3.0f), desc, firstRng);

    Neuron::Pcg32 secondRng(7u);
    Neuron::MeshShatter second;
    second.Spawn(mesh, Identity(), XMFLOAT3(1.0f, 2.0f, 3.0f), desc, secondRng);

    Assert::AreEqual(first.FragmentCount(), second.FragmentCount(), L"the same seed shattered into a different number of fragments");
    for (int i = 0; i < Neuron::MeshShatter::TUMBLER_COUNT; ++i)
    {
      const XMFLOAT3& a = first.TumblerAt(i).angVelRadPerSec;
      const XMFLOAT3& b = second.TumblerAt(i).angVelRadPerSec;
      Assert::IsTrue(a.x == b.x && a.y == b.y && a.z == b.z, L"the same seed produced a different tumble");
    }
    for (std::uint32_t i = 0; i < first.FragmentCount(); ++i)
      Assert::IsTrue(first.FragmentAt(i).tumbler == second.FragmentAt(i).tumbler, L"the same seed assigned a different tumbler");
  }
};
} // namespace NeuronClientTests
