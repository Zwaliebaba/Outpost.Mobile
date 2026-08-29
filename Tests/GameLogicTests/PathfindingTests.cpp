#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
constexpr std::uint32_t STRUCTURE = static_cast<std::uint32_t>(Game::HullId::Structure);

// A U of Structures walled to the north, east and west and open only to the south, with the ship
// inside it and its destination beyond the north wall. Leaving means travelling away from where it
// was sent, which is the one thing local steering cannot work out: the information that the way
// around is south is not available anywhere in the neighbourhood.
//
// Spacing is well inside twice the bounding radius at every join, including the corners, so the
// walls have no gap a hull could be routed through.
void SpawnPocket(Game::World& _world)
{
  for (const float x : {-800.0f, -400.0f, 0.0f, 400.0f, 800.0f})
    _world.SpawnShip(Game::LocalPos(x, 500.0f), 0.0f, STRUCTURE);
  for (const float z : {100.0f, -300.0f})
  {
    _world.SpawnShip(Game::LocalPos(-800.0f, z), 0.0f, STRUCTURE);
    _world.SpawnShip(Game::LocalPos(800.0f, z), 0.0f, STRUCTURE);
  }
}
} // namespace

TEST_CLASS(PathfindingTests)
{
public:
  TEST_METHOD(TheSameStaticSetAndEndpointsGiveTheSamePath)
  {
    // What the total A* tie-break buys, and the reason it is (f, g, cellIndex) rather than (f, g).
    // Two cells with equal cost must still order the same way, or a recorded game replays down a
    // different route (Design/Collision.md 12).
    const float clearance = Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    const std::vector<Game::PathGrid::Obstacle> obstacles = {
      {Game::LocalPos(0.0f, 0.0f), 251.77f}, {Game::LocalPos(560.0f, 120.0f), 251.77f}, {Game::LocalPos(-400.0f, -300.0f), 131.61f}};

    std::vector<Game::WorldPos> first;
    std::vector<Game::WorldPos> second;
    {
      Game::PathGrid grid;
      grid.Rebuild(obstacles);
      Assert::IsTrue(grid.FindPath(Game::LocalPos(-900.0f, 0.0f), Game::LocalPos(900.0f, 0.0f), clearance, first),
                     L"no route was found at all");
    }
    {
      Game::PathGrid grid;
      grid.Rebuild(obstacles);
      Assert::IsTrue(grid.FindPath(Game::LocalPos(-900.0f, 0.0f), Game::LocalPos(900.0f, 0.0f), clearance, second),
                     L"no route was found at all");
    }

    Assert::IsTrue(first.size() > 1, L"a route straight through a Structure was reported as clear");
    Assert::AreEqual(first.size(), second.size(), L"the same endpoints produced routes of different lengths");
    for (size_t at = 0; at < first.size(); ++at)
    {
      Assert::IsTrue(IsSamePosition(first[at], second[at]), L"the same endpoints produced a different route");
    }
  }

  TEST_METHOD(EveryWaypointOfARouteIsClear)
  {
    // A route is only worth planning if the legs between its waypoints are actually flyable. This
    // is what the string-pull has to preserve: it removes waypoints, and every one it removes has
    // to leave a leg that still clears.
    const float clearance = Game::HullSpecOf(Game::HullId::Frigate).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    // A named vector rather than a braced list: constructing a std::span straight from one is
    // C++26, and AGENTS.md 5 holds this tree to C++20 whatever /std:c++latest would let through.
    const std::vector<Game::PathGrid::Obstacle> obstacles = {{Game::LocalPos(0.0f, 0.0f), 251.77f}};
    Game::PathGrid grid;
    grid.Rebuild(obstacles);

    std::vector<Game::WorldPos> route;
    const Game::WorldPos from = Game::LocalPos(-800.0f, 0.0f);
    Assert::IsTrue(grid.FindPath(from, Game::LocalPos(800.0f, 0.0f), clearance, route), L"no route was found round a single Structure");

    Game::WorldPos at = from;
    for (const Game::WorldPos& waypoint : route)
    {
      Assert::IsTrue(grid.IsClearBetween(at, waypoint, clearance), L"a leg of the route passes through a Structure");
      at = waypoint;
    }
    Assert::AreEqual(800.0f, WorldX(route.back()), 1e-3f, L"the route does not end at the destination");
  }

  TEST_METHOD(ClearanceRespectsTheHull)
  {
    // One grid serves every hull: the same query, asked with a Carrier's radius, routes around a
    // gap an Interceptor threads. That is the property that makes a clearance field the right
    // structure rather than a per-hull occupancy map.
    const float gapHalf = 300.0f;
    const std::vector<Game::PathGrid::Obstacle> obstacles = {{Game::LocalPos(0.0f, -gapHalf - 251.77f), 251.77f},
                                                             {Game::LocalPos(0.0f, gapHalf + 251.77f), 251.77f}};
    Game::PathGrid grid;
    grid.Rebuild(obstacles);

    const Game::WorldPos from = Game::LocalPos(-900.0f, 0.0f);
    const Game::WorldPos to = Game::LocalPos(900.0f, 0.0f);

    const float fighterClearance = Game::HullSpecOf(Game::HullId::Interceptor).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    std::vector<Game::WorldPos> throughTheGap;
    Assert::IsTrue(grid.FindPath(from, to, fighterClearance, throughTheGap), L"an Interceptor could not thread a 600 m gap");
    Assert::AreEqual(size_t{1}, throughTheGap.size(), L"an Interceptor took a detour round a gap it fits through");

    // The Carrier's own bounding radius is 107.5 m, so nothing in the table needs more than about
    // 116 m of clearance -- which the gap gives. Ask for more than the gap holds and the same field
    // has to say so rather than threading it anyway.
    std::vector<Game::WorldPos> tooWide;
    const bool routed = grid.FindPath(from, to, gapHalf + 40.0f, tooWide);
    const bool threaded = routed && tooWide.size() == 1;
    Assert::IsFalse(threaded, L"a hull too wide for the gap was routed straight through it");
  }

  TEST_METHOD(AConcavePocketIsEscaped)
  {
    // The case local steering provably cannot solve, and therefore the test that proves this phase
    // earned its place. A ship in the bottom of a U has to travel away from its destination to get
    // out, and the information that says so -- that the way around is left, not right -- is not
    // available locally at any tuning (Design/Collision.md 12, 16).
    Game::World world;
    SpawnPocket(world);
    const Game::ShipId ship = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::LocalPos(0.0f, 1400.0f), false, 0.0f);
    Assert::IsTrue(world.RouteOf(ship).size() > 1, L"the planner produced a straight line out of a closed pocket");

    // It has to start by going the wrong way, which is the whole point of the case.
    Assert::IsTrue(WorldZ(world.RouteOf(ship)[0]) < 0.0f, L"the first waypoint out of a south-facing pocket heads north");

    for (int tick = 0; tick < 20000 && world.Ship(ship).order != Game::OrderState::Idle; ++tick)
      world.Step();

    Assert::AreEqual(Game::OrderState::Idle, world.Ship(ship).order, L"a ship ordered out of a concave pocket never got there");
    Assert::IsTrue(WorldZ(world.Ship(ship).posWorld) > 1200.0f, L"the ship stopped short of its destination");
  }

  TEST_METHOD(AShipRoutesRoundAStructureRatherThanIntoIt)
  {
    // The everyday version: architecture between a ship and where it was sent.
    Game::World world;
    const Game::ShipId structure = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, STRUCTURE);
    const Game::ShipId ship = world.SpawnShip(Game::LocalPos(0.0f, -800.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::LocalPos(0.0f, 800.0f), false, 0.0f);
    Assert::IsTrue(world.RouteOf(ship).size() > 1, L"a ship was sent straight through a Structure");

    float closest = 1e30f;
    for (int tick = 0; tick < 20000 && world.Ship(ship).order != Game::OrderState::Idle; ++tick)
    {
      world.Step();
      closest = std::min(closest, Game::Distance(world.Ship(ship).posWorld, world.Ship(structure).posWorld));
    }
    Assert::AreEqual(Game::OrderState::Idle, world.Ship(ship).order, L"a ship routed round a Structure never arrived");
    Assert::IsTrue(closest > Game::HullSpecOf(Game::HullId::Structure).capsuleRadiusMetres,
                   std::format(L"the ship came within {:.1f} m of the Structure's centre", closest).c_str());
  }

  TEST_METHOD(AStructureSpawnedAcrossAPathForcesAReplan)
  {
    // Routes are planned once and not re-run per tick, so a route has to notice when the world it
    // was planned against stops being the world.
    Game::World world;
    const Game::ShipId ship = world.SpawnShip(Game::LocalPos(0.0f, -1400.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::LocalPos(0.0f, 1400.0f), false, 0.0f);
    Assert::AreEqual(size_t{1}, world.RouteOf(ship).size(), L"an empty world produced a route with waypoints in it");

    for (int tick = 0; tick < 600; ++tick)
      world.Step();

    // Dropped across the run the ship is already making.
    const Game::ShipId structure = world.SpawnShip(Game::LocalPos(0.0f, 200.0f), 0.0f, STRUCTURE);
    world.Step();
    Assert::IsTrue(world.RouteOf(ship).size() > 1, L"a Structure across the path did not force a re-plan");

    // And the steered point moves without the discontinuity an integrator would turn into a swerve.
    float worstHeadingStep = 0.0f;
    float closest = 1e30f;
    for (int tick = 0; tick < 20000 && world.Ship(ship).order != Game::OrderState::Idle; ++tick)
    {
      const float before = world.Ship(ship).headingRad;
      world.Step();
      worstHeadingStep = std::max(worstHeadingStep, std::fabs(DirectX::XMScalarModAngle(world.Ship(ship).headingRad - before)));
      closest = std::min(closest, Game::Distance(world.Ship(ship).posWorld, world.Ship(structure).posWorld));
    }

    const float turnLimit = Game::HullSpecOf(Game::HullId::Corvette).maxTurnRateRadPerSec * Game::TICK_DT;
    Assert::IsTrue(worstHeadingStep <= turnLimit * 1.01f, L"the re-plan snapped the ship's heading past its own turn rate");
    Assert::AreEqual(Game::OrderState::Idle, world.Ship(ship).order, L"the ship never arrived after the re-plan");
    Assert::IsTrue(closest > Game::HullSpecOf(Game::HullId::Structure).capsuleRadiusMetres, L"the ship flew into the new Structure");
  }

  TEST_METHOD(AnEmptyWorldPlansNothing)
  {
    // Every phase before this one was verified with no planner in the tree. With no architecture in
    // it, the planner must hand back the destination and change nothing at all.
    Game::PathGrid grid;
    Assert::IsFalse(grid.HasObstacles(), L"an unbuilt grid claims to hold obstacles");

    std::vector<Game::WorldPos> route;
    Assert::IsTrue(grid.FindPath(Game::LocalPos(-500.0f, 0.0f), Game::LocalPos(500.0f, 0.0f), 100.0f, route),
                   L"an empty grid refused a route");
    Assert::AreEqual(size_t{1}, route.size(), L"an empty grid invented waypoints");
    Assert::AreEqual(500.0f, WorldX(route[0]), 0.0f, L"an empty grid did not hand back the destination");
  }
};
} // namespace GameLogicTests
