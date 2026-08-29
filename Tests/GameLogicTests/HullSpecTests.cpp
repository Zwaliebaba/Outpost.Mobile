#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
const wchar_t* HULL_NAMES[Game::HULL_COUNT] = {L"Interceptor", L"Bomber",     L"Corvette", L"Miner",    L"Frigate",
                                               L"Hauler",      L"Battleship", L"Carrier",  L"Stargate", L"Structure"};
} // namespace

TEST_CLASS(HullSpecTests)
{
public:
  TEST_METHOD(NoHullCanTunnelThroughAnother)
  {
    // A discrete overlap test misses a contact entirely when the pair passes through each other
    // inside one tick. The conservative form of the safety condition is that a hull's travel per
    // tick stays under the smallest capsule radius in the table, and the point of writing it as a
    // gate rather than as arithmetic in a document is that the day someone adds a 90 m/s
    // Interceptor or a 300 m/s missile, the suite goes red naming the hull -- rather than the hull
    // going through another hull in a live match six months later (Design/Collision.md 11).
    //
    // Parameterised on TICK_HZ, never on a baked 1/60: lowering the tick rate for an MMO server
    // multiplies this ratio, and it has to go red loudly rather than quietly halve the margin.
    const float smallest = Game::SmallestCapsuleRadiusMetres();
    const float limit = Game::TUNNEL_HEADROOM * smallest;

    float worstRatio = 0.0f;
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    {
      const Game::HullSpec& spec = Game::HULL_SPECS[hull];
      const float travel = spec.maxSpeedMetresPerSec * Game::TICK_DT;
      const float ratio = travel / smallest;
      worstRatio = std::max(worstRatio, ratio);

      const std::wstring message = std::format(L"{} travels {:.3f} m per tick at {:.0f} Hz against a smallest capsule radius of {:.3f} m: "
                                               L"ratio {:.3f}, threshold {:.3f}. Either slow the hull, raise the tick rate, or fatten the "
                                               L"smallest capsule -- do not raise TUNNEL_HEADROOM.",
                                               HULL_NAMES[hull], travel, Game::TICK_HZ, smallest, ratio, Game::TUNNEL_HEADROOM);
      Assert::IsTrue(travel < limit, message.c_str());
    }

    // Recorded on a passing run too: the value of the gate is that someone reading a green run can
    // see how much room is left, not that it eventually fails.
    Logger::WriteMessage(std::format(L"tunnelling: worst ratio {:.3f} against a threshold of {:.3f} at {:.0f} Hz\n", worstRatio,
                                     Game::TUNNEL_HEADROOM, Game::TICK_HZ)
                           .c_str());
  }

  TEST_METHOD(ADefaultHullSpecIsAWorkingShip)
  {
    // The horizon derivation divides by the turn rate, so a default-constructed spec with a zero
    // one divides by zero on first use. A default has to be the ship the tree already has, not an
    // inert one (Design/Collision.md 5).
    const Game::HullSpec spec;
    Assert::IsTrue(spec.maxTurnRateRadPerSec > 0.0f, L"a default hull cannot turn");
    Assert::IsTrue(spec.maxSpeedMetresPerSec > 0.0f, L"a default hull cannot move");
    Assert::IsTrue(spec.decelerationMetresPerSec2 > 0.0f, L"a default hull cannot stop");
    Assert::IsTrue(spec.BoundingRadiusMetres() > 0.0f, L"a default hull has no size");
    Assert::IsTrue(std::isfinite(spec.AvoidHorizonSec()), L"a default hull's avoidance horizon is not a number");
    Assert::IsTrue(std::isfinite(spec.BrakeHorizonSec()), L"a default hull's braking horizon is not a number");

    // And the same, one step further: a hull authored with no turn rate at all must not produce an
    // infinity that then propagates through a query radius and into a position.
    Game::HullSpec inert;
    inert.maxTurnRateRadPerSec = 0.0f;
    inert.decelerationMetresPerSec2 = 0.0f;
    Assert::IsTrue(std::isfinite(inert.AvoidHorizonSec()), L"a zero turn rate produced a non-finite horizon");
    Assert::IsTrue(std::isfinite(Game::QueryRadiusMetres(inert)), L"a zero turn rate produced a non-finite query radius");
  }

  TEST_METHOD(EveryHullHasAWorkingSpec)
  {
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    {
      const Game::HullSpec& spec = Game::HULL_SPECS[hull];
      const std::wstring name(HULL_NAMES[hull]);
      Assert::IsTrue(spec.capsuleRadiusMetres > 0.0f, (name + L" has no capsule radius").c_str());
      Assert::IsTrue(spec.capsuleHalfLengthMetres >= 0.0f, (name + L" has a negative capsule half-length").c_str());
      Assert::IsTrue(spec.avoidanceAuthority > 0.0f, (name + L" has no avoidance authority, which is a zero denominator").c_str());
      Assert::IsTrue(spec.neighbourCap > 0, (name + L" tracks no neighbours").c_str());
      Assert::IsTrue(spec.maxTurnRateRadPerSec > 0.0f, (name + L" cannot turn, and the horizon derivation divides by it").c_str());
      // The view normalises bank and thruster flare by these, so a zero is a divide by zero on the
      // client rather than merely an inert hull.
      Assert::IsTrue(spec.accelerationMetresPerSec2 > 0.0f, (name + L" has no acceleration, which the view divides by").c_str());
      Assert::IsTrue(spec.turnAccelerationRadPerSec2 > 0.0f, (name + L" has no angular acceleration").c_str());
      Assert::AreEqual(spec.capsuleRadiusMetres + spec.capsuleHalfLengthMetres, spec.BoundingRadiusMetres(), 1e-4f,
                       (name + L" has a bounding radius that is not L + r").c_str());
      // A hull that moves faster than the design's own numbers were computed against is exactly
      // what the tunnelling gate is for, so it must not also be silently immovable.
      Assert::IsTrue(!spec.immovable || spec.maxSpeedMetresPerSec == 0.0f, (name + L" is immovable but carries a speed").c_str());
    }
  }

  TEST_METHOD(TheAvoidanceHorizonIsCappedAndTheBrakingOneIsNot)
  {
    // The split exists because a Carrier does not need to *avoid* thirty-seven seconds out, it
    // needs to *decelerate* thirty-seven seconds out, and only the first drives a query radius.
    const Game::HullSpec& carrier = Game::HullSpecOf(Game::HullId::Carrier);
    Assert::IsTrue(carrier.BrakeHorizonSec() > Game::AVOID_HORIZON_MAX_SEC,
                   L"the Carrier's uncapped horizon is no longer the case worth capping");
    Assert::AreEqual(Game::AVOID_HORIZON_MAX_SEC, carrier.AvoidHorizonSec(), 1e-4f, L"the Carrier's avoidance horizon is not capped");

    const Game::HullSpec& interceptor = Game::HullSpecOf(Game::HullId::Interceptor);
    Assert::IsTrue(interceptor.AvoidHorizonSec() < Game::AVOID_HORIZON_MAX_SEC, L"an agile hull should not be reaching the cap");
    Assert::AreEqual(interceptor.BrakeHorizonSec(), interceptor.AvoidHorizonSec(), 1e-4f, L"an uncapped hull's two horizons should agree");
  }

  TEST_METHOD(NoQueryRadiusForcesAnUnreasonableGhostZone)
  {
    // A world-layout constraint arriving out of a collision table: sharding by space needs a ghost
    // zone at least as wide as the widest query, so this number is the floor on region size and it
    // is far cheaper to know now than after a map exists (Design/Collision.md 10).
    constexpr float REGION_GHOST_BUDGET_METRES = 700.0f;
    float widest = 0.0f;
    std::uint32_t widestHull = 0;
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    {
      const float radius = Game::QueryRadiusMetres(Game::HULL_SPECS[hull]);
      if (radius > widest)
      {
        widest = radius;
        widestHull = hull;
      }
    }
    Assert::IsTrue(widest < REGION_GHOST_BUDGET_METRES,
                   std::format(L"the widest query radius is {}'s {:.0f} m, past the {:.0f} m ghost-zone budget. Lower "
                               L"AVOID_HORIZON_MAX_SEC or slow the hull.",
                               HULL_NAMES[widestHull], widest, REGION_GHOST_BUDGET_METRES)
                     .c_str());
    Logger::WriteMessage(std::format(L"widest query radius: {} at {:.0f} m\n", HULL_NAMES[widestHull], widest).c_str());
  }
};
} // namespace GameLogicTests
