#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
[[nodiscard]] Game::Capsule Circle(float _x, float _z, float _radius) noexcept
{
  return Game::Capsule{_x, _z, 0.0f, 1.0f, 0.0f, _radius};
}

// Axis along +X, so a hull lying across the frame is easy to reason about in a test.
[[nodiscard]] Game::Capsule LyingEastWest(float _x, float _z, float _halfLength, float _radius) noexcept
{
  return Game::Capsule{_x, _z, 1.0f, 0.0f, _halfLength, _radius};
}
} // namespace

TEST_CLASS(CollisionTests)
{
public:
  TEST_METHOD(TwoCirclesTouchOnTheirCombinedRadii)
  {
    const Game::Contact clear = Game::CapsuleContact(Circle(0.0f, 0.0f, 2.0f), Circle(10.0f, 0.0f, 3.0f), 0, 1);
    Assert::IsFalse(clear.touching, L"circles 10 m apart with a combined radius of 5 m are touching");

    const Game::Contact overlapping = Game::CapsuleContact(Circle(0.0f, 0.0f, 2.0f), Circle(4.0f, 0.0f, 3.0f), 0, 1);
    Assert::IsTrue(overlapping.touching, L"circles 4 m apart with a combined radius of 5 m are not touching");
    Assert::AreEqual(1.0f, overlapping.overlapMetres, 1e-4f, L"the overlap is not (rA + rB) - distance");
    // The normal points from B towards A, which is the direction A has to be pushed.
    Assert::AreEqual(-1.0f, overlapping.normalX, 1e-4f, L"the contact normal does not point from B towards A");
    Assert::AreEqual(0.0f, overlapping.normalZ, 1e-4f, L"the contact normal is not along the line of centres");
  }

  TEST_METHOD(ACapsuleIsNotAFatCircle)
  {
    // The whole reason for capsules. An Interceptor is 2.23 by 7.01, so a bounding circle is three
    // times too fat across the beam and a formation of them would behave as though each were a
    // Bomber. This pair is clear abeam and overlapping along the length.
    const Game::Capsule lying = LyingEastWest(0.0f, 0.0f, 10.0f, 2.0f);

    const Game::Contact abeam = Game::CapsuleContact(lying, Circle(0.0f, 6.0f, 2.0f), 0, 1);
    Assert::IsFalse(abeam.touching, L"a capsule is behaving as its bounding circle abeam");

    const Game::Contact ahead = Game::CapsuleContact(lying, Circle(13.0f, 0.0f, 2.0f), 0, 1);
    Assert::IsTrue(ahead.touching, L"a capsule is not reaching along its own length");
    Assert::AreEqual(1.0f, ahead.overlapMetres, 1e-4f, L"the overlap along the axis is wrong");
  }

  TEST_METHOD(ParallelCapsulesResolveDeterministically)
  {
    // Two hulls in line abreast: every point along the overlap is equally close, so the closest-
    // point solve is underdetermined and a naive one divides by a zero denominator.
    const Game::Capsule first = LyingEastWest(0.0f, 0.0f, 20.0f, 3.0f);
    const Game::Capsule second = LyingEastWest(5.0f, 4.0f, 20.0f, 3.0f);

    const Game::Contact contact = Game::CapsuleContact(first, second, 0, 1);
    Assert::IsTrue(contact.touching, L"parallel capsules 4 m apart with a combined radius of 6 m are not touching");
    Assert::AreEqual(2.0f, contact.overlapMetres, 1e-3f, L"parallel capsules resolved to the wrong separation");
    Assert::AreEqual(-1.0f, contact.normalZ, 1e-3f, L"parallel capsules resolved to a normal that is not across the pair");

    const Game::Contact again = Game::CapsuleContact(first, second, 0, 1);
    Assert::AreEqual(contact.overlapMetres, again.overlapMetres, 0.0f, L"the same parallel pair resolved two ways");
    Assert::AreEqual(contact.normalZ, again.normalZ, 0.0f, L"the same parallel pair produced two normals");
  }

  TEST_METHOD(ConcentricHullsGetADeterministicNormal)
  {
    // An ordinary situation, not an exotic one: two ships spawned on the same point. The closest
    // points coincide, so there is no normal to compute and the obvious implementation divides by
    // zero or reads whatever was in the register.
    const Game::Contact first = Game::CapsuleContact(Circle(0.0f, 0.0f, 2.0f), Circle(0.0f, 0.0f, 3.0f), 4, 9);
    Assert::IsTrue(first.touching, L"concentric hulls are not touching");
    Assert::AreEqual(5.0f, first.overlapMetres, 1e-4f, L"concentric hulls do not overlap by their combined radii");

    const float length = std::sqrt(first.normalX * first.normalX + first.normalZ * first.normalZ);
    Assert::AreEqual(1.0f, length, 1e-4f, L"the degenerate normal is not a unit vector");

    const Game::Contact repeated = Game::CapsuleContact(Circle(0.0f, 0.0f, 2.0f), Circle(0.0f, 0.0f, 3.0f), 4, 9);
    Assert::AreEqual(first.normalX, repeated.normalX, 0.0f, L"the same concentric pair produced two normals");
    Assert::AreEqual(first.normalZ, repeated.normalZ, 0.0f, L"the same concentric pair produced two normals");

    // And the half that matters to the gather: evaluated from the other side, the normal has to be
    // exactly opposite, or both ships would be pushed the same way and never separate.
    const Game::Contact mirrored = Game::CapsuleContact(Circle(0.0f, 0.0f, 3.0f), Circle(0.0f, 0.0f, 2.0f), 9, 4);
    Assert::AreEqual(-first.normalX, mirrored.normalX, 0.0f, L"the two sides of a concentric pair do not disagree exactly");
    Assert::AreEqual(-first.normalZ, mirrored.normalZ, 0.0f, L"the two sides of a concentric pair do not disagree exactly");
  }

  TEST_METHOD(AuthorityDecidesWhoGivesWay)
  {
    // A Carrier shouldering aside for an Interceptor reads as a bug to anyone watching, so the
    // share a hull takes is the *other* hull's authority over the sum.
    const float fighter = Game::HullSpecOf(Game::HullId::Interceptor).avoidanceAuthority;
    const float capital = Game::HullSpecOf(Game::HullId::Carrier).avoidanceAuthority;

    const Game::SeparationShares even = Game::SeparationSharesFor(fighter, false, fighter, false);
    Assert::AreEqual(0.5f, even.a, 1e-4f, L"two identical hulls do not split evenly");
    Assert::AreEqual(1.0f, even.a + even.b, 1e-4f, L"the shares do not sum to one");

    const Game::SeparationShares uneven = Game::SeparationSharesFor(fighter, false, capital, false);
    Assert::IsTrue(uneven.a > 0.9f, L"an Interceptor is not doing substantially all of the work against a Carrier");
    Assert::AreEqual(1.0f, uneven.a + uneven.b, 1e-4f, L"the shares do not sum to one");
  }

  TEST_METHOD(ImmovableHullsTakeNoCorrection)
  {
    const Game::SeparationShares againstArchitecture = Game::SeparationSharesFor(1.0f, false, 1.0f, true);
    Assert::AreEqual(1.0f, againstArchitecture.a, 1e-4f, L"a ship does not take the whole correction against a Structure");
    Assert::AreEqual(0.0f, againstArchitecture.b, 1e-4f, L"a Structure moved");

    // Two structures authored overlapping, or a turret placed against a station: neither can move,
    // so the correct answer is no correction rather than a NaN discovered three passes later.
    const Game::SeparationShares bothPinned = Game::SeparationSharesFor(1.0f, true, 1.0f, true);
    Assert::AreEqual(0.0f, bothPinned.a, 0.0f, L"two immovable hulls produced a correction");
    Assert::AreEqual(0.0f, bothPinned.b, 0.0f, L"two immovable hulls produced a correction");

    // And the arithmetic guard behind it, reached if a hull is ever authored with no authority.
    const Game::SeparationShares zeroed = Game::SeparationSharesFor(0.0f, false, 0.0f, false);
    Assert::IsTrue(std::isfinite(zeroed.a) && std::isfinite(zeroed.b), L"a zero authority denominator produced a NaN");
    Assert::AreEqual(1.0f, zeroed.a + zeroed.b, 1e-4f, L"the fallback shares do not sum to one");
  }
};
} // namespace GameLogicTests
