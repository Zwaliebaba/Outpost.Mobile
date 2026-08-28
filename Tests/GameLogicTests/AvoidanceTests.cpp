#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace GameLogicTests
{
namespace
{
[[nodiscard]] float HullOverlap(const Game::World& _world, Game::ShipId _a, Game::ShipId _b)
{
  const Game::ShipState& first = _world.Ship(_a);
  const Game::ShipState& second = _world.Ship(_b);
  const Game::HullSpec& firstHull = Game::HullSpecOf(first.hullId);
  const Game::HullSpec& secondHull = Game::HullSpecOf(second.hullId);

  const Game::Capsule self{
    0.0f, 0.0f, std::sin(first.headingRad), std::cos(first.headingRad), firstHull.capsuleHalfLengthMetres, firstHull.capsuleRadiusMetres};
  const Game::Capsule against{Game::OffsetX(first.posWorld, second.posWorld),
                              Game::OffsetZ(first.posWorld, second.posWorld),
                              std::sin(second.headingRad),
                              std::cos(second.headingRad),
                              secondHull.capsuleHalfLengthMetres,
                              secondHull.capsuleRadiusMetres};
  const Game::Contact contact = Game::CapsuleContact(self, against, _a, _b);
  return contact.touching ? contact.overlapMetres : 0.0f;
}

// Which way a ship is steering relative to where it is pointed, with a deadband so that settling
// onto a heading does not read as a decision.
[[nodiscard]] int SteeringSide(const Game::ShipState& _ship) noexcept
{
  const float offset = XMScalarModAngle(_ship.avoidHeadingRad - _ship.headingRad);
  if (offset > 0.02f)
    return 1;
  return (offset < -0.02f) ? -1 : 0;
}

const wchar_t* HullName(Game::HullId _hull)
{
  switch (_hull)
  {
  case Game::HullId::Interceptor:
    return L"Interceptor";
  case Game::HullId::Corvette:
    return L"Corvette";
  case Game::HullId::Frigate:
    return L"Frigate";
  case Game::HullId::Battleship:
    return L"Battleship";
  case Game::HullId::Carrier:
    return L"Carrier";
  default:
    return L"hull";
  }
}
} // namespace

TEST_CLASS(AvoidanceTests)
{
public:
  TEST_METHOD(AnEqualHeadOnPairBreaksToStarboard)
  {
    // Two identical hulls meeting head-on have equal authority and mirror each other exactly. Left
    // to the danger term alone they deadlock, or resolve by whichever way the arithmetic happens to
    // fall -- which is a coin toss that looks like a bug when the same encounter goes the other way
    // a minute later. The rule is what makes it read as seamanship (Design/Collision.md 9).
    for (const Game::HullId hull : {Game::HullId::Interceptor, Game::HullId::Corvette, Game::HullId::Frigate})
    {
      Game::World world;
      const Game::ShipId north = world.SpawnShip(Game::WorldPos{0.0f, -600.0f}, 0.0f, static_cast<std::uint32_t>(hull));
      const Game::ShipId south = world.SpawnShip(Game::WorldPos{0.0f, 600.0f}, XM_PI, static_cast<std::uint32_t>(hull));

      const Game::ShipId northOrder[] = {north};
      const Game::ShipId southOrder[] = {south};
      world.IssueMoveOrder(northOrder, Game::WorldPos{0.0f, 600.0f}, false, 0.0f);
      world.IssueMoveOrder(southOrder, Game::WorldPos{0.0f, -600.0f}, false, 0.0f);

      float northMaxX = 0.0f;
      float southMinX = 0.0f;
      float worstOverlap = 0.0f;
      for (int tick = 0; tick < 4000; ++tick)
      {
        world.Step();
        northMaxX = std::max(northMaxX, world.Ship(north).posWorld.localX);
        southMinX = std::min(southMinX, world.Ship(south).posWorld.localX);
        worstOverlap = std::max(worstOverlap, HullOverlap(world, north, south));
      }

      const std::wstring name(HullName(hull));
      // Heading 0 is north and increasing heading turns east, so the northbound ship's starboard is
      // +x; the southbound one is pointed the other way, so its starboard is -x. Both to their own
      // right means they pass port to port.
      Assert::IsTrue(northMaxX > 1.0f, (name + L": the northbound ship did not break to starboard").c_str());
      Assert::IsTrue(southMinX < -1.0f, (name + L": the southbound ship did not break to starboard").c_str());
      Assert::AreEqual(0.0f, worstOverlap, 1e-3f, (name + L": a head-on pair passed through each other").c_str());
      Assert::IsTrue(world.Ship(north).posWorld.localZ > 400.0f, (name + L": the northbound ship never got past").c_str());
    }
  }

  TEST_METHOD(NoPairingPassesThroughItself)
  {
    // Every pairing of small hull and large, not just the symmetric ones. The mixed cases are where
    // the horizon and the authority split do the work, and they are the ones that were wrong.
    constexpr Game::HullId FLEET[] = {Game::HullId::Interceptor, Game::HullId::Corvette, Game::HullId::Frigate, Game::HullId::Battleship,
                                      Game::HullId::Carrier};
    float worstFraction = 0.0f;
    std::wstring worstPair;
    for (const Game::HullId northHull : FLEET)
    {
      for (const Game::HullId southHull : FLEET)
      {
        Game::World world;
        const Game::ShipId north = world.SpawnShip(Game::WorldPos{0.0f, -900.0f}, 0.0f, static_cast<std::uint32_t>(northHull));
        const Game::ShipId south = world.SpawnShip(Game::WorldPos{0.0f, 900.0f}, XM_PI, static_cast<std::uint32_t>(southHull));

        const Game::ShipId northOrder[] = {north};
        const Game::ShipId southOrder[] = {south};
        world.IssueMoveOrder(northOrder, Game::WorldPos{0.0f, 900.0f}, false, 0.0f);
        world.IssueMoveOrder(southOrder, Game::WorldPos{0.0f, -900.0f}, false, 0.0f);

        float worstOverlap = 0.0f;
        for (int tick = 0; tick < 6000; ++tick)
        {
          world.Step();
          worstOverlap = std::max(worstOverlap, HullOverlap(world, north, south));
        }

        // As a fraction of what the pair would have to interpenetrate to be concentric, so a
        // Carrier pair and an Interceptor pair are held to the same standard rather than the same
        // number of metres.
        const float reach = Game::HullSpecOf(northHull).capsuleRadiusMetres + Game::HullSpecOf(southHull).capsuleRadiusMetres;
        const float fraction = worstOverlap / reach;
        if (fraction > worstFraction)
        {
          worstFraction = fraction;
          worstPair = std::wstring(HullName(northHull)) + L"/" + HullName(southHull);
        }
        Assert::IsTrue(fraction < 0.02f, std::format(L"{} against {} interpenetrated by {:.1f}% of their combined radii",
                                                     HullName(northHull), HullName(southHull), fraction * 100.0f)
                                           .c_str());
      }
    }
    Logger::WriteMessage(
      std::format(L"head-on pairings: worst was {} at {:.2f}% of combined radii\n", worstPair, worstFraction * 100.0f).c_str());
  }

  TEST_METHOD(AGiveWayTurnDoesNotChatter)
  {
    // Not merely "clears". Whenever two candidate headings score within noise of each other -- the
    // normal condition for a symmetric head-on pair, which is exactly what the starboard rule
    // creates -- a plain argmax flips left, right, left on successive ticks and the ship shivers
    // down the middle. Counted per second rather than per encounter, because a ship that weaves
    // once a second is manoeuvring and one that reverses fourteen times a second is broken
    // (Design/Collision.md 10, 16).
    constexpr int WINDOW_TICKS = 60;
    constexpr int REVERSALS_ALLOWED = 6;

    for (const Game::HullId hull :
         {Game::HullId::Interceptor, Game::HullId::Corvette, Game::HullId::Frigate, Game::HullId::Battleship, Game::HullId::Carrier})
    {
      Game::World world;
      const Game::ShipId north = world.SpawnShip(Game::WorldPos{0.0f, -600.0f}, 0.0f, static_cast<std::uint32_t>(hull));
      const Game::ShipId south = world.SpawnShip(Game::WorldPos{0.0f, 600.0f}, XM_PI, static_cast<std::uint32_t>(hull));

      const Game::ShipId northOrder[] = {north};
      const Game::ShipId southOrder[] = {south};
      world.IssueMoveOrder(northOrder, Game::WorldPos{0.0f, 600.0f}, false, 0.0f);
      world.IssueMoveOrder(southOrder, Game::WorldPos{0.0f, -600.0f}, false, 0.0f);

      std::vector<int> reversalAt;
      int previousSide = 0;
      for (int tick = 0; tick < 5000; ++tick)
      {
        world.Step();
        const int side = SteeringSide(world.Ship(north));
        if (side != 0)
        {
          if (previousSide != 0 && side != previousSide)
            reversalAt.push_back(tick);
          previousSide = side;
        }
      }

      int worstWindow = 0;
      for (size_t first = 0; first < reversalAt.size(); ++first)
      {
        int inWindow = 0;
        for (size_t at = first; at < reversalAt.size() && reversalAt[at] - reversalAt[first] < WINDOW_TICKS; ++at)
          ++inWindow;
        worstWindow = std::max(worstWindow, inWindow);
      }

      Assert::IsTrue(worstWindow <= REVERSALS_ALLOWED,
                     std::format(L"{} reversed its give-way turn {} times inside one second", HullName(hull), worstWindow).c_str());
      Logger::WriteMessage(std::format(L"{:<12} give-way reversals: {} in the worst second, {} over the encounter\n", HullName(hull),
                                       worstWindow, reversalAt.size())
                             .c_str());
    }
  }

  TEST_METHOD(AFighterYieldsAndACapitalHoldsCourse)
  {
    // A Carrier that swerves for an Interceptor reads as a bug. The fighter crosses the capital's
    // bow; the capital should barely notice.
    // Started so that the two actually meet: a Carrier accelerating at 5 m/s^2 to 20 m/s covers
    // 320 m in about the seventeen seconds an Interceptor takes over 500 m at 34 m/s.
    Game::World world;
    const Game::ShipId carrier = world.SpawnShip(Game::WorldPos{0.0f, -320.0f}, 0.0f, static_cast<std::uint32_t>(Game::HullId::Carrier));
    const Game::ShipId fighter =
      world.SpawnShip(Game::WorldPos{-500.0f, 0.0f}, XM_PIDIV2, static_cast<std::uint32_t>(Game::HullId::Interceptor));

    const Game::ShipId carrierOrder[] = {carrier};
    const Game::ShipId fighterOrder[] = {fighter};
    world.IssueMoveOrder(carrierOrder, Game::WorldPos{0.0f, 600.0f}, false, 0.0f);
    world.IssueMoveOrder(fighterOrder, Game::WorldPos{500.0f, 0.0f}, false, 0.0f);

    float carrierMaxOffTrack = 0.0f;
    float fighterMaxOffTrack = 0.0f;
    float worstOverlap = 0.0f;
    for (int tick = 0; tick < 4000; ++tick)
    {
      world.Step();
      carrierMaxOffTrack = std::max(carrierMaxOffTrack, std::fabs(world.Ship(carrier).posWorld.localX));
      fighterMaxOffTrack = std::max(fighterMaxOffTrack, std::fabs(world.Ship(fighter).posWorld.localZ));
      worstOverlap = std::max(worstOverlap, HullOverlap(world, carrier, fighter));
    }

    // Not zero, and asserting zero would be asserting something this design does not claim. Local
    // steering is deliberately capped -- a fighter's look-ahead cannot fully clear a 215 m hull it
    // is crossing at point-blank range -- and what steering leaves is separation's to absorb. The
    // bound is what separates a graze that pass cleans up from a collision it has to dig out of.
    const float reach =
      Game::HullSpecOf(Game::HullId::Carrier).capsuleRadiusMetres + Game::HullSpecOf(Game::HullId::Interceptor).capsuleRadiusMetres;
    Assert::IsTrue(worstOverlap < reach * 0.01f,
                   std::format(L"the fighter buried itself {:.2f} m into the capital, past the {:.2f} m that counts as a graze",
                               worstOverlap, reach * 0.01f)
                     .c_str());
    // "Holds course" needs a scale, and the hull's own beam is the honest one: a capital that moves
    // less than its own half-width has not swerved, whatever the absolute number says.
    const float held = Game::HullSpecOf(Game::HullId::Carrier).capsuleRadiusMetres;
    Assert::IsTrue(carrierMaxOffTrack < held,
                   std::format(L"the Carrier swerved {:.1f} m, more than its own {:.1f} m beam", carrierMaxOffTrack, held).c_str());
    Assert::IsTrue(
      fighterMaxOffTrack > carrierMaxOffTrack * 4.0f,
      std::format(L"the Carrier left its track by {:.1f} m and the Interceptor by only {:.1f} m", carrierMaxOffTrack, fighterMaxOffTrack)
        .c_str());
    Logger::WriteMessage(std::format(L"crossing: Carrier deviated {:.1f} m, Interceptor {:.1f} m, worst graze {:.2f} m\n",
                                     carrierMaxOffTrack, fighterMaxOffTrack, worstOverlap)
                           .c_str());
  }

  TEST_METHOD(ShipsInCompanyDoNotFightEachOther)
  {
    // Zero relative velocity is the divide-by-zero in the time-to-closest-approach formula, and it
    // is not an edge case: it is a formation flying in company, which is what a fleet does for its
    // entire journey. Getting it wrong produces a NaN in the most ordinary situation the game has
    // (Design/Collision.md 10, 16).
    Game::World world;
    std::vector<Game::ShipId> wing;
    for (int i = 0; i < 6; ++i)
    {
      wing.push_back(world.SpawnShip(Game::WorldPos{static_cast<float>(i) * 40.0f - 100.0f, 0.0f}, 0.0f,
                                     static_cast<std::uint32_t>(Game::HullId::Corvette)));
    }
    world.IssueMoveOrder(wing, Game::WorldPos{0.0f, 900.0f}, false, 0.0f);

    for (int tick = 0; tick < 3000; ++tick)
    {
      world.Step();
      for (const Game::ShipId id : wing)
      {
        const Game::ShipState& ship = world.Ship(id);
        Assert::IsTrue(std::isfinite(ship.posWorld.localX) && std::isfinite(ship.posWorld.localZ) && std::isfinite(ship.headingRad),
                       L"a ship flying in company produced a NaN");
      }
    }

    for (const Game::ShipId id : wing)
      Assert::AreEqual(Game::OrderState::Idle, world.Ship(id).order, L"a ship in formation never arrived");
    for (size_t a = 0; a < wing.size(); ++a)
    {
      for (size_t b = a + 1; b < wing.size(); ++b)
        Assert::AreEqual(0.0f, HullOverlap(world, wing[a], wing[b]), 1e-2f, L"a formation arrived inside itself");
    }
  }

  TEST_METHOD(AParkedFormationDoesNotDrift)
  {
    // Formation drift under traffic is not a separate problem; it is the authority split with a
    // different number, plus traffic that steers around rather than through. An idle ship holds its
    // station harder than one under way (Design/Collision.md 9).
    Game::World world;
    std::vector<Game::ShipId> parked;
    for (int i = 0; i < 5; ++i)
    {
      parked.push_back(world.SpawnShip(Game::WorldPos{static_cast<float>(i) * 40.0f - 80.0f, 0.0f}, 0.0f,
                                       static_cast<std::uint32_t>(Game::HullId::Corvette)));
    }

    std::vector<Game::ShipId> traffic;
    for (int i = 0; i < 5; ++i)
    {
      traffic.push_back(world.SpawnShip(Game::WorldPos{static_cast<float>(i) * 40.0f - 80.0f, -600.0f}, 0.0f,
                                        static_cast<std::uint32_t>(Game::HullId::Corvette)));
    }
    world.IssueMoveOrder(traffic, Game::WorldPos{0.0f, 600.0f}, false, 0.0f);

    std::vector<Game::WorldPos> station;
    for (const Game::ShipId id : parked)
      station.push_back(world.Ship(id).posWorld);

    for (int tick = 0; tick < 2400; ++tick)
      world.Step();

    float worstDrift = 0.0f;
    for (size_t i = 0; i < parked.size(); ++i)
      worstDrift = std::max(worstDrift, Game::Distance(station[i], world.Ship(parked[i]).posWorld));

    // A hull's own bounding radius is the honest scale for "did not drift": nudged aside is fine,
    // shoved off station is not.
    const float allowed = Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres();
    Assert::IsTrue(
      worstDrift < allowed,
      std::format(L"a parked ship was shoved {:.1f} m off station, past its own {:.1f} m hull radius", worstDrift, allowed).c_str());
    Logger::WriteMessage(std::format(L"parked formation: worst drift {:.2f} m under traffic\n", worstDrift).c_str());
  }

  TEST_METHOD(AClearSkyChangesNothing)
  {
    // Every phase before this one was verified against a world with no avoidance in it. If a ship
    // with nothing converging on it steers even slightly, none of those verifications mean anything
    // any more -- so a neighbourhood that holds no threat must return the order's intent untouched.
    const Game::HullSpec& hull = Game::HullSpecOf(Game::HullId::Corvette);
    Game::ShipState ship;
    ship.hullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
    ship.order = Game::OrderState::Moving;
    ship.steerTargetPos = Game::WorldPos{0.0f, 500.0f};

    const Game::MotionIntent ordered = Game::SolveOrder(ship, hull);

    // One neighbour, close, but drawing away rather than converging.
    Game::Neighbour alongside;
    alongside.id = 1;
    alongside.offsetX = 60.0f;
    alongside.offsetZ = 20.0f;
    alongside.velocityX = 30.0f; // opening the range
    alongside.velocityZ = 0.0f;
    alongside.boundingRadiusMetres = hull.BoundingRadiusMetres();
    alongside.distanceSquared = 60.0f * 60.0f + 20.0f * 20.0f;
    alongside.proximityMetres = std::sqrt(alongside.distanceSquared) - alongside.boundingRadiusMetres;

    const Game::Neighbour list[] = {alongside};
    const Game::MotionIntent avoided = Game::AvoidNeighbours(ship, hull, ordered, list);
    Assert::AreEqual(ordered.desiredHeadingRad, avoided.desiredHeadingRad, 0.0f, L"a ship steered around a neighbour drawing away from it");
    Assert::AreEqual(ordered.desiredSpeedMetresPerSec, avoided.desiredSpeedMetresPerSec, 0.0f,
                     L"a ship shed speed for a neighbour drawing away");

    const Game::MotionIntent empty = Game::AvoidNeighbours(ship, hull, ordered, {});
    Assert::AreEqual(ordered.desiredHeadingRad, empty.desiredHeadingRad, 0.0f, L"a ship with no neighbours at all still steered");
  }
};
} // namespace GameLogicTests
