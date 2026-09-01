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
void SpawnPocket(Game::Universe& _universe)
{
  for (const float x : {-800.0f, -400.0f, 0.0f, 400.0f, 800.0f})
    _universe.SpawnShip(Game::LocalPos(x, 500.0f), 0.0f, STRUCTURE);
  for (const float z : {100.0f, -300.0f})
  {
    _universe.SpawnShip(Game::LocalPos(-800.0f, z), 0.0f, STRUCTURE);
    _universe.SpawnShip(Game::LocalPos(800.0f, z), 0.0f, STRUCTURE);
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
    // different route (Design/Archive/Collision.md 12).
    const float clearance = Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    const std::vector<Game::PathGrid::Obstacle> obstacles = {
      {Game::LocalPos(0.0f, 0.0f), 251.77f}, {Game::LocalPos(560.0f, 120.0f), 251.77f}, {Game::LocalPos(-400.0f, -300.0f), 131.61f}};

    std::vector<Game::UniversePos> first;
    std::vector<Game::UniversePos> second;
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

    std::vector<Game::UniversePos> route;
    const Game::UniversePos from = Game::LocalPos(-800.0f, 0.0f);
    Assert::IsTrue(grid.FindPath(from, Game::LocalPos(800.0f, 0.0f), clearance, route), L"no route was found round a single Structure");

    Game::UniversePos at = from;
    for (const Game::UniversePos& waypoint : route)
    {
      Assert::IsTrue(grid.IsClearBetween(at, waypoint, clearance), L"a leg of the route passes through a Structure");
      at = waypoint;
    }
    Assert::AreEqual(800.0f, UniverseX(route.back()), 1e-3f, L"the route does not end at the destination");
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

    const Game::UniversePos from = Game::LocalPos(-900.0f, 0.0f);
    const Game::UniversePos to = Game::LocalPos(900.0f, 0.0f);

    const float fighterClearance = Game::HullSpecOf(Game::HullId::Interceptor).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    std::vector<Game::UniversePos> throughTheGap;
    Assert::IsTrue(grid.FindPath(from, to, fighterClearance, throughTheGap), L"an Interceptor could not thread a 600 m gap");
    Assert::AreEqual(size_t{1}, throughTheGap.size(), L"an Interceptor took a detour round a gap it fits through");

    // The Carrier's own bounding radius is 107.5 m, so nothing in the table needs more than about
    // 116 m of clearance -- which the gap gives. Ask for more than the gap holds and the same field
    // has to say so rather than threading it anyway.
    std::vector<Game::UniversePos> tooWide;
    const bool routed = grid.FindPath(from, to, gapHalf + 40.0f, tooWide);
    const bool threaded = routed && tooWide.size() == 1;
    Assert::IsFalse(threaded, L"a hull too wide for the gap was routed straight through it");
  }

  TEST_METHOD(AConcavePocketIsEscaped)
  {
    // The case local steering provably cannot solve, and therefore the test that proves this phase
    // earned its place. A ship in the bottom of a U has to travel away from its destination to get
    // out, and the information that says so -- that the way around is left, not right -- is not
    // available locally at any tuning (Design/Archive/Collision.md 12, 16).
    Game::Universe universe;
    SpawnPocket(universe);
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 1400.0f), false, 0.0f);
    Assert::IsTrue(universe.RouteOf(ship).size() > 1, L"the planner produced a straight line out of a closed pocket");

    // It has to start by going the wrong way, which is the whole point of the case.
    Assert::IsTrue(UniverseZ(universe.RouteOf(ship)[0]) < 0.0f, L"the first waypoint out of a south-facing pocket heads north");

    for (int tick = 0; tick < 20000 && universe.Ship(ship).order != Game::OrderState::Idle; ++tick)
      universe.Step();

    Assert::AreEqual(Game::OrderState::Idle, universe.Ship(ship).order, L"a ship ordered out of a concave pocket never got there");
    Assert::IsTrue(UniverseZ(universe.Ship(ship).posUniverse) > 1200.0f, L"the ship stopped short of its destination");
  }

  TEST_METHOD(AShipRoutesRoundAStructureRatherThanIntoIt)
  {
    // The everyday version: architecture between a ship and where it was sent.
    Game::Universe universe;
    const Game::ShipId structure = universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, STRUCTURE);
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(0.0f, -800.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 800.0f), false, 0.0f);
    Assert::IsTrue(universe.RouteOf(ship).size() > 1, L"a ship was sent straight through a Structure");

    float closest = 1e30f;
    for (int tick = 0; tick < 20000 && universe.Ship(ship).order != Game::OrderState::Idle; ++tick)
    {
      universe.Step();
      closest = std::min(closest, Game::Distance(universe.Ship(ship).posUniverse, universe.Ship(structure).posUniverse));
    }
    Assert::AreEqual(Game::OrderState::Idle, universe.Ship(ship).order, L"a ship routed round a Structure never arrived");
    Assert::IsTrue(closest > Game::HullSpecOf(Game::HullId::Structure).capsuleRadiusMetres,
                   std::format(L"the ship came within {:.1f} m of the Structure's centre", closest).c_str());
  }

  TEST_METHOD(AStructureSpawnedAcrossAPathForcesAReplan)
  {
    // Routes are planned once and not re-run per tick, so a route has to notice when the universe it
    // was planned against stops being the universe.
    Game::Universe universe;
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(0.0f, -1400.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 1400.0f), false, 0.0f);
    Assert::AreEqual(size_t{1}, universe.RouteOf(ship).size(), L"an empty universe produced a route with waypoints in it");

    for (int tick = 0; tick < 600; ++tick)
      universe.Step();

    // Dropped across the run the ship is already making.
    const Game::ShipId structure = universe.SpawnShip(Game::LocalPos(0.0f, 200.0f), 0.0f, STRUCTURE);
    universe.Step();
    Assert::IsTrue(universe.RouteOf(ship).size() > 1, L"a Structure across the path did not force a re-plan");

    // And the steered point moves without the discontinuity an integrator would turn into a swerve.
    float worstHeadingStep = 0.0f;
    float closest = 1e30f;
    for (int tick = 0; tick < 20000 && universe.Ship(ship).order != Game::OrderState::Idle; ++tick)
    {
      const float before = universe.Ship(ship).headingRad;
      universe.Step();
      worstHeadingStep = std::max(worstHeadingStep, std::fabs(DirectX::XMScalarModAngle(universe.Ship(ship).headingRad - before)));
      closest = std::min(closest, Game::Distance(universe.Ship(ship).posUniverse, universe.Ship(structure).posUniverse));
    }

    const float turnLimit = Game::HullSpecOf(Game::HullId::Corvette).maxTurnRateRadPerSec * Game::TICK_DT;
    Assert::IsTrue(worstHeadingStep <= turnLimit * 1.01f, L"the re-plan snapped the ship's heading past its own turn rate");
    Assert::AreEqual(Game::OrderState::Idle, universe.Ship(ship).order, L"the ship never arrived after the re-plan");
    Assert::IsTrue(closest > Game::HullSpecOf(Game::HullId::Structure).capsuleRadiusMetres, L"the ship flew into the new Structure");
  }

  TEST_METHOD(MobileChurnLeavesRoutesAlone)
  {
    // The cost this retires: every spawn and despawn used to dirty the static set, so a fighter
    // dying rebuilt the whole static index, rebuilt the PathGrid, bumped its version and made every
    // routed ship in the universe re-plan. At MMO churn that is a universe-wide replan on every death
    // (Design/Archive/MmoScalabilityReview.md U4).
    Game::Universe universe;
    (void)universe.SpawnShip(Game::LocalPos(0.0f, 200.0f), 0.0f, STRUCTURE);
    // Held as a handle rather than an id, because a despawn below can renumber it (ADR 0005). It
    // happens not to here -- the passer is the last ship -- and a test that relies on that is one
    // spawn away from being wrong for a reason nobody would look for.
    const Game::ShipHandle ship =
      universe.HandleOf(universe.SpawnShip(Game::LocalPos(0.0f, -1400.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette)));
    const Game::ShipId order[] = {universe.Resolve(ship)};
    universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 1400.0f), false, 0.0f);
    universe.Step();

    const std::size_t planned = universe.RouteOf(universe.Resolve(ship)).size();
    Assert::IsTrue(planned > 1, L"the Structure did not force a route round it");

    // A mobile ship arrives and leaves. Neither touches the architecture, so neither may disturb a
    // route that was planned against it.
    const Game::ShipHandle passer =
      universe.HandleOf(universe.SpawnShip(Game::LocalPos(600.0f, 600.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor)));
    universe.Step();
    Assert::AreEqual(planned, universe.RouteOf(universe.Resolve(ship)).size(),
                     L"a mobile spawn re-planned a route it could not have affected");

    Assert::IsTrue(universe.DespawnShip(passer), L"the despawn failed");
    universe.Step();
    Assert::AreEqual(planned, universe.RouteOf(universe.Resolve(ship)).size(),
                     L"a mobile despawn re-planned a route it could not have affected");
  }

  TEST_METHOD(DespawningAStructureStillReplans)
  {
    // The other half, and the one that matters more: gating the rebuild must not gate away the case
    // it exists for. Architecture leaving is architecture changing.
    Game::Universe universe;
    const Game::ShipHandle wall = universe.HandleOf(universe.SpawnShip(Game::LocalPos(0.0f, 200.0f), 0.0f, STRUCTURE));
    const Game::ShipHandle ship =
      universe.HandleOf(universe.SpawnShip(Game::LocalPos(0.0f, -1400.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette)));
    const Game::UniversePos destination = Game::LocalPos(0.0f, 1400.0f);
    const Game::ShipId order[] = {universe.Resolve(ship)};
    universe.IssueMoveOrder(order, destination, false, 0.0f);
    universe.Step();

    const std::span<const Game::UniversePos> detour = universe.RouteOf(universe.Resolve(ship));
    Assert::IsTrue(detour.size() > 1, L"the Structure did not force a route round it");
    Assert::IsFalse(IsSamePosition(detour[0], destination), L"the route round the Structure starts at the destination");

    // Both ships are held as handles across the despawn, because the Structure is id 0 and
    // swap-and-pop moves the last ship into its place: the surviving ship's *id* changes even though
    // the ship does not, which is the rule ADR 0005 exists to state.
    Assert::IsTrue(universe.DespawnShip(wall), L"the despawn failed");
    universe.Step();

    const Game::ShipId survivor = universe.Resolve(ship);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, survivor, L"the surviving ship's handle went stale");
    const std::span<const Game::UniversePos> freed = universe.RouteOf(survivor);
    Assert::AreEqual(size_t{1}, freed.size(), L"removing the Structure did not free the route");
    Assert::IsTrue(IsSamePosition(freed[0], destination), L"the freed route does not go straight at the destination");
  }

  TEST_METHOD(ARebuildWithTheSameObstaclesKeepsItsVersion)
  {
    // The version is what makes a route re-plan, so it may only move when the architecture does.
    Game::PathGrid grid;
    const Game::PathGrid::Obstacle wall[] = {{Game::LocalPos(0.0f, 0.0f), 100.0f}};
    grid.Rebuild(wall);
    const std::uint32_t first = grid.Version();

    grid.Rebuild(wall);
    Assert::AreEqual(first, grid.Version(), L"rebuilding with the same obstacles bumped the version");

    const Game::PathGrid::Obstacle moved[] = {{Game::LocalPos(0.0f, 300.0f), 100.0f}};
    grid.Rebuild(moved);
    Assert::AreNotEqual(first, grid.Version(), L"rebuilding with a moved obstacle did not bump the version");

    const Game::PathGrid::Obstacle wider[] = {{Game::LocalPos(0.0f, 300.0f), 140.0f}};
    const std::uint32_t second = grid.Version();
    grid.Rebuild(wider);
    Assert::AreNotEqual(second, grid.Version(), L"a radius change alone did not bump the version");
  }

  TEST_METHOD(TwoStationsTwentyKilometresApartBothRoute)
  {
    // The headline failure, and the reason islands exist. One grid sweeps a single bounding box over
    // every obstacle in the universe, so two stations 20 km apart ask for a grid past
    // PATH_GRID_MAX_CELLS_PER_AXIS, it declines to build, and A* goes off for *every ship in the
    // universe* rather than for the space between them. Run against that grid, neither ship below
    // arrives at all: each flies its straight line, hugs its station at 265 m and orbits until the
    // tick budget runs out (Design/Archive/RegionalPathfinding.md 1.1).
    for (const float station : {0.0f, 20000.0f})
    {
      Game::Universe universe;
      (void)universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, STRUCTURE);
      (void)universe.SpawnShip(Game::LocalPos(20000.0f, 0.0f), 0.0f, STRUCTURE);
      const Game::ShipId ship =
        universe.SpawnShip(Game::LocalPos(station, -800.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

      const Game::ShipId order[] = {ship};
      universe.IssueMoveOrder(order, Game::LocalPos(station, 800.0f), false, 0.0f);
      Assert::IsTrue(universe.RouteOf(ship).size() > 1,
                     std::format(L"the station at {:.0f} m did not force a route round it", station).c_str());

      float closest = 1e30f;
      for (int tick = 0; tick < 20000 && universe.Ship(ship).order != Game::OrderState::Idle; ++tick)
      {
        universe.Step();
        closest = std::min(closest, Game::Distance(universe.Ship(ship).posUniverse, Game::LocalPos(station, 0.0f)));
      }
      Assert::AreEqual(Game::OrderState::Idle, universe.Ship(ship).order,
                       std::format(L"the ship at the station at {:.0f} m never arrived", station).c_str());
      Assert::IsTrue(closest > Game::HullSpecOf(Game::HullId::Structure).capsuleRadiusMetres,
                     std::format(L"the ship came within {:.1f} m of the station's centre", closest).c_str());
    }
  }

  TEST_METHOD(ArchitectureIsOneIslandExactlyWhenNoHullFitsBetween)
  {
    // The partition rule, at its own boundary. A gap wider than IslandGapMetres is one the
    // straight-line test flies through with no plan at all, so the two are separate problems; a
    // narrower one is a wall A* has to find its way around, so they are one
    // (Design/Archive/RegionalPathfinding.md 3.2).
    //
    // The threshold is derived rather than chosen: twice the widest mobile hull's bounding radius
    // plus its clearance margin, because a ship's centre has to stay that far clear of each surface
    // to pass between them. Read off the hull table here too, so the day a wider hull lands the test
    // moves with it.
    const float radius = Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres();
    const float gap = Game::IslandGapMetres();
    Assert::IsTrue(gap > 0.0f, L"the island gap came out at nothing, so every pair would be one island");

    Game::PathIslands islands;
    const std::vector<Game::PathGrid::Obstacle> tooTight = {{Game::LocalPos(0.0f, 0.0f), radius},
                                                            {Game::LocalPos(2.0f * radius + gap - 32.0f, 0.0f), radius}};
    islands.Rebuild(tooTight);
    Assert::AreEqual(size_t{1}, islands.IslandCount(), L"a gap no hull fits through was split into two islands");

    const std::vector<Game::PathGrid::Obstacle> roomToPass = {{Game::LocalPos(0.0f, 0.0f), radius},
                                                              {Game::LocalPos(2.0f * radius + gap + 32.0f, 0.0f), radius}};
    islands.Rebuild(roomToPass);
    Assert::AreEqual(size_t{2}, islands.IslandCount(), L"a gap the widest hull passes through was kept as one island");

    // And the grids are the islands': each holds its own station and not the other's, which is what
    // makes a hundred scattered Structures a hundred small grids rather than one that declines.
    for (size_t at = 0; at < islands.IslandCount(); ++at)
      Assert::IsTrue(islands.Island(at).HasObstacles(), L"an island built no grid at all");
  }

  TEST_METHOD(TheIslandOrderDoesNotFollowShipIds)
  {
    // The partition is a function of the obstacle set, but the obstacles arrive in ShipId order and
    // ShipIds move under swap-and-pop (ADR 0005) -- so the order the islands are *found* in would
    // follow the ids if it were left to the walk. It is not: they are sorted by the lowest path cell
    // any member sits in, which is a universe coordinate (Design/Archive/RegionalPathfinding.md 3.2, 5).
    //
    // Said twice. First directly, because the order is what the rule is about: the same two stations
    // in opposite array orders must come back as the same island in the same slot. Then end to end,
    // because a rule the router does not actually follow is not a rule.
    const float radius = Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres();
    const std::vector<Game::PathGrid::Obstacle> westFirst = {{Game::LocalPos(0.0f, 0.0f), radius}, {Game::LocalPos(4000.0f, 0.0f), radius}};
    const std::vector<Game::PathGrid::Obstacle> eastFirst = {{Game::LocalPos(4000.0f, 0.0f), radius}, {Game::LocalPos(0.0f, 0.0f), radius}};
    Game::PathIslands asStored;
    Game::PathIslands asPermuted;
    asStored.Rebuild(westFirst);
    asPermuted.Rebuild(eastFirst);
    Assert::AreEqual(size_t{2}, asStored.IslandCount(), L"two stations 4 km apart were not two islands");
    Assert::AreEqual(size_t{2}, asPermuted.IslandCount(), L"two stations 4 km apart were not two islands");

    // Both stations sit at z = 0, so the key comes down to the cell on x and island 0 is the
    // westerly one whichever way the array ran. Read through the clearance each island's own grid
    // reports beside that station: near it if the island holds it, and "far" if it does not.
    const Game::UniversePos besideTheWesterly = Game::LocalPos(600.0f, 0.0f);
    const float stored = asStored.Island(0).ClearanceAt(besideTheWesterly);
    const float permuted = asPermuted.Island(0).ClearanceAt(besideTheWesterly);
    Assert::IsTrue(stored < 1000.0f, L"island 0 is not the station the universe-fixed order puts first");
    Assert::AreEqual(stored, permuted, 0.0f, L"permuting the obstacle array reordered the islands");

    std::vector<Game::UniversePos> first;
    std::vector<Game::UniversePos> second;
    for (int flipped = 0; flipped < 2; ++flipped)
    {
      Game::Universe universe;
      const float spawnedFirst = (flipped == 0) ? 0.0f : 4000.0f;
      const float spawnedSecond = (flipped == 0) ? 4000.0f : 0.0f;
      Game::Universe& into = universe;
      (void)into.SpawnShip(Game::LocalPos(spawnedFirst, 0.0f), 0.0f, STRUCTURE);
      (void)into.SpawnShip(Game::LocalPos(spawnedSecond, 0.0f), 0.0f, STRUCTURE);
      const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(0.0f, -800.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

      const Game::ShipId order[] = {ship};
      universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 800.0f), false, 0.0f);
      const std::span<const Game::UniversePos> route = universe.RouteOf(ship);
      std::vector<Game::UniversePos>& kept = (flipped == 0) ? first : second;
      kept.assign(route.begin(), route.end());
    }

    Assert::IsTrue(first.size() > 1, L"the station did not force a route round it, so there is nothing to compare");
    Assert::AreEqual(first.size(), second.size(), L"the spawn order changed how many waypoints the route had");
    for (size_t at = 0; at < first.size(); ++at)
      Assert::IsTrue(IsSamePosition(first[at], second[at]), L"the spawn order changed the route");
  }

  TEST_METHOD(ARouteAcrossTwoIslandsIsStitched)
  {
    // The third case: the run meets more than one island, and no single island's grid can plan it,
    // because the first one cannot see the second. The first island plans as far as its own far
    // side and the route reports itself unfinished, which is what makes Universe::AdvanceRoute come
    // back for the rest on arrival -- the same rule that already handled a route too long for one
    // waypoint list (Design/Archive/RegionalPathfinding.md 3.4).
    Game::Universe universe;
    (void)universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, STRUCTURE);
    (void)universe.SpawnShip(Game::LocalPos(0.0f, 3000.0f), 0.0f, STRUCTURE);
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(0.0f, -1500.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    const Game::UniversePos destination = Game::LocalPos(0.0f, 4500.0f);

    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, destination, false, 0.0f);

    // The first plan stops on the near side of the far station rather than steering through it.
    const std::span<const Game::UniversePos> planned = universe.RouteOf(ship);
    Assert::IsTrue(planned.size() >= 1, L"a route across two islands produced no waypoints at all");
    Assert::IsFalse(IsSamePosition(planned.back(), destination), L"a route across two islands aimed its last waypoint past the second one");

    float nearer = 1e30f;
    float further = 1e30f;
    for (int tick = 0; tick < 40000 && universe.Ship(ship).order != Game::OrderState::Idle; ++tick)
    {
      universe.Step();
      nearer = std::min(nearer, Game::Distance(universe.Ship(ship).posUniverse, Game::LocalPos(0.0f, 0.0f)));
      further = std::min(further, Game::Distance(universe.Ship(ship).posUniverse, Game::LocalPos(0.0f, 3000.0f)));
    }

    Assert::AreEqual(Game::OrderState::Idle, universe.Ship(ship).order, L"a ship crossing two islands never arrived");
    const float wall = Game::HullSpecOf(Game::HullId::Structure).capsuleRadiusMetres;
    Assert::IsTrue(nearer > wall, std::format(L"the ship came within {:.1f} m of the first station", nearer).c_str());
    Assert::IsTrue(further > wall, std::format(L"the ship came within {:.1f} m of the second station", further).c_str());
  }

  TEST_METHOD(TheFirstLegOfACrossingReachesPastTheFirstIsland)
  {
    // The refinement over simply truncating what the first island planned. A crossing is planned in
    // the first island only, and its leg is aimed at the open water on the run -- past where the
    // first island stops blocking it, short of where the next one starts. Truncating instead ends
    // the route at the last *turn*, because the string-pull stops adding waypoints the moment the
    // way ahead is clear and the far side is only ever reached by the destination waypoint that has
    // to be dropped. Measured over 72 crossings from starts all round the first island, the shortest
    // first leg went from 125 m to 1,484 m (Design/Archive/RegionalPathfinding.md 3.4).
    const float clearance = Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    const float radius = Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres();
    const std::vector<Game::PathGrid::Obstacle> inLine = {{Game::LocalPos(0.0f, 0.0f), radius}, {Game::LocalPos(0.0f, 3000.0f), radius}};
    Game::PathIslands islands;
    islands.Rebuild(inLine);
    Assert::AreEqual(size_t{2}, islands.IslandCount(), L"two stations 3 km apart were not two islands");

    const Game::UniversePos from = Game::LocalPos(0.0f, -1500.0f);
    std::vector<Game::UniversePos> route;
    Assert::IsFalse(islands.FindPath(from, Game::LocalPos(0.0f, 4500.0f), clearance, route),
                    L"a route across two islands reported itself finished");
    Assert::IsTrue(!route.empty(), L"a route across two islands produced no waypoints at all");

    // Past the first station and short of the second, both by a wide margin: the leg is the gap
    // between them, not a step out of the first one's shadow.
    const float reached = UniverseZ(route.back());
    Assert::IsTrue(reached > radius * 2.0f, std::format(L"the first leg only reached z = {:.1f} m", reached).c_str());
    Assert::IsTrue(reached < 3000.0f - radius * 2.0f,
                   std::format(L"the first leg aimed into the second station at z = {:.1f} m", reached).c_str());

    // And it is a place a ship can be: clear of every island, not merely clear of the first.
    for (size_t at = 0; at < islands.IslandCount(); ++at)
      Assert::IsTrue(islands.Island(at).ClearanceAt(route.back()) >= clearance, L"the first leg was aimed at a point inside an island");
  }

  TEST_METHOD(AnIslandThatDeclinesSaysSoAndLeavesItsNeighbourRouting)
  {
    // The per-island ceiling. One grid over everything meant a single distant outpost put the whole
    // universe past PATH_GRID_MAX_CELLS_PER_AXIS; per island, only an island genuinely 16 km across can
    // do it, and when one does its neighbours keep routing. That is the gain -- and it comes with a
    // failure that looks exactly like success, because a grid that declined calls every run clear
    // just as an empty one does. So it is counted (Design/Archive/RegionalPathfinding.md 3.3).
    const float radius = Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres();
    std::vector<Game::PathGrid::Obstacle> obstacles;

    // A wall of architecture 16.8 km long, each piece close enough to the next to be one island:
    // 700 m apart leaves a 196 m surface gap, inside IslandGapMetres, so the gap rule welds them.
    for (int at = 0; at < 25; ++at)
      obstacles.push_back({Game::LocalPos(static_cast<float>(at) * 700.0f, 0.0f), radius});
    const std::size_t wallPieces = obstacles.size();
    // And one station well clear of it, which must be unaffected.
    obstacles.push_back({Game::LocalPos(-6000.0f, 0.0f), radius});

    Game::PathIslands islands;
    islands.Rebuild(obstacles);
    Assert::AreEqual(size_t{2}, islands.IslandCount(), L"the wall and the lone station were not two islands");
    Assert::AreEqual(size_t{1}, islands.DeclinedCount(), L"a 16.8 km island did not decline, or the lone station did");
    Assert::IsTrue(wallPieces > 20, L"the wall was built from too few pieces to reach the ceiling");

    const float clearance = Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    std::vector<Game::UniversePos> route;

    // The neighbour still routes, which is the whole point: before islands, this universe had one grid
    // and it declined, so nothing in it routed at all.
    Assert::IsTrue(islands.FindPath(Game::LocalPos(-6000.0f, -800.0f), Game::LocalPos(-6000.0f, 800.0f), clearance, route),
                   L"the route round the lone station did not complete");
    Assert::IsTrue(route.size() > 1, L"the lone station did not force a route round it");

    // And a run through the declining island is the straight line it was before there was a planner,
    // which is the honest degradation rather than a refusal to move.
    Assert::IsTrue(islands.FindPath(Game::LocalPos(5000.0f, -800.0f), Game::LocalPos(5000.0f, 800.0f), clearance, route),
                   L"a run through a declining island refused");
    Assert::AreEqual(size_t{1}, route.size(), L"a declining island planned a route it has no grid for");
  }

  TEST_METHOD(OnlyTheIslandThatChangedIsRebuilt)
  {
    // What islands are for, on the rebuild side. One grid over everything meant a station moving
    // anywhere cost a clearance field over the whole universe; per island it costs its own. Measured
    // over a hundred scattered stations, a whole rebuild is 1.58 ms and one station moving is
    // 0.064 ms, and the evaluations go from the review's 7.9 M worst legal case to 2,304
    // (Design/Archive/RegionalPathfinding.md 4).
    //
    // Matched by content and not by slot, because the islands are ordered by where they sit in the
    // universe and building anything renumbers every island after it -- an island index is not a
    // handle, for the same reason a ShipId is not (ADR 0005, ADR 0034).
    const float radius = Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres();
    std::vector<Game::PathGrid::Obstacle> scattered;
    for (int at = 0; at < 12; ++at)
      scattered.push_back({Game::LocalPos(static_cast<float>(at % 4) * 3000.0f, static_cast<float>(at / 4) * 3000.0f), radius});

    Game::PathIslands islands;
    islands.Rebuild(scattered);
    Assert::AreEqual(size_t{12}, islands.IslandCount(), L"twelve stations 3 km apart were not twelve islands");
    Assert::AreEqual(std::uint32_t{12}, islands.RebuiltIslandCount(), L"the first build did not build every island");

    // One station widens. Its own island is dirty; the other eleven hold exactly the obstacles they
    // held before and keep the grids they have.
    std::vector<Game::PathGrid::Obstacle> widened = scattered;
    widened.front().radiusMetres += 0.5f;
    islands.Rebuild(widened);
    Assert::AreEqual(size_t{12}, islands.IslandCount(), L"widening one station changed how many islands there are");
    Assert::AreEqual(std::uint32_t{1}, islands.RebuiltIslandCount(), L"a station moving rebuilt more than its own island");

    // A station appears, which renumbers the islands after it -- and still only the new one builds.
    std::vector<Game::PathGrid::Obstacle> grown = widened;
    grown.push_back({Game::LocalPos(-9000.0f, -9000.0f), radius});
    islands.Rebuild(grown);
    Assert::AreEqual(size_t{13}, islands.IslandCount(), L"the new station did not become its own island");
    Assert::AreEqual(std::uint32_t{1}, islands.RebuiltIslandCount(), L"a spawn rebuilt islands it did not touch");

    // And a kept grid is a right grid, not merely a fast one. The same architecture built from
    // scratch has to route identically -- which it does only because the lattice is the universe's, so
    // a grid carried across a repartition still holds the cells it did (slice 1).
    Game::PathIslands afresh;
    afresh.Rebuild(grown);
    const float clearance = Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    std::vector<Game::UniversePos> carried;
    std::vector<Game::UniversePos> rebuilt;
    for (int at = 0; at < 12; ++at)
    {
      const Game::UniversePos station = Game::LocalPos(static_cast<float>(at % 4) * 3000.0f, static_cast<float>(at / 4) * 3000.0f);
      Game::UniversePos from = station;
      Game::Translate(from, 0.0f, -800.0f);
      Game::UniversePos to = station;
      Game::Translate(to, 0.0f, 800.0f);
      const bool one = islands.FindPath(from, to, clearance, carried);
      const bool other = afresh.FindPath(from, to, clearance, rebuilt);
      Assert::AreEqual(one, other, L"a carried grid and a fresh one disagreed about whether the route finished");
      Assert::AreEqual(carried.size(), rebuilt.size(), L"a carried grid and a fresh one produced routes of different lengths");
      for (size_t step = 0; step < carried.size(); ++step)
        Assert::IsTrue(IsSamePosition(carried[step], rebuilt[step]), L"a carried grid routed differently from a fresh one");
    }
  }

  TEST_METHOD(ADistantObstacleDoesNotMoveTheCells)
  {
    // The lattice is the universe's, not the grid's. Before this, a grid's origin was the corner of the
    // box over its own obstacles, so building something a kilometre away moved every cell centre
    // under every fixed point in the universe -- and cell centres are what ClearanceAt samples and what
    // A* searches. The same architecture, approached from the same place, could then give a
    // different route because of something built somewhere else entirely
    // (Design/Archive/RegionalPathfinding.md 1.3, 3.1).
    //
    // Invisible today, because every rebuild bumps the version and every route re-plans, so the
    // shifted answer is simply the new answer. It stops being invisible the moment routes are
    // cached, compared across machines or replayed -- and SimTuning.h already puts the cell size in
    // the replay contract.
    const Game::UniversePos probe = Game::LocalPos(400.0f, 0.0f);
    const std::vector<Game::PathGrid::Obstacle> alone = {{Game::LocalPos(0.0f, 0.0f), 251.77f}};
    const std::vector<Game::PathGrid::Obstacle> andARockFourKilometresWest = {{Game::LocalPos(0.0f, 0.0f), 251.77f},
                                                                              {Game::LocalPos(-4000.0f, 0.0f), 131.61f}};

    Game::PathGrid grid;
    grid.Rebuild(alone);
    const float before = grid.ClearanceAt(probe);
    grid.Rebuild(andARockFourKilometresWest);
    const float after = grid.ClearanceAt(probe);

    // Exactly equal rather than close, because the probe falls in the same cell of the same lattice
    // both times and the rock is 4 km further off than the Structure: this is the same distance
    // computed twice. It read 136.5 m and then 160.6 m before the lattice was fixed to the universe.
    Assert::AreEqual(before, after, 0.0f, L"a rock 4 km away moved the clearance under a fixed point");
    Assert::IsTrue(before < 1000.0f, L"the probe is not inside the grid at all, so the check proves nothing");
  }

  TEST_METHOD(ACellIndexIsAFunctionOfThePositionAlone)
  {
    // The lattice said directly, rather than through a grid. A cell's index is derived from the
    // sector pair and the local offset, which is exact only because a sector is a whole number of
    // cells across (SimTuning.h, PATH_CELLS_PER_SECTOR) -- so the round trip has to hold at a sector
    // join and west of the origin, where the floor division is the thing that can be wrong.
    const Game::UniversePos probe = Game::LocalPos(400.0f, 0.0f);
    Assert::AreEqual(std::int64_t{12}, Game::PathCellX(probe), L"400 m is not in the thirteenth cell");
    Assert::AreEqual(std::int64_t{0}, Game::PathCellZ(probe), L"0 m is not in the first cell");

    const Game::UniversePos centre = Game::PathCellCentre(Game::PathCellX(probe), Game::PathCellZ(probe));
    Assert::AreEqual(400.0f, UniverseX(centre), 0.0f, L"the cell centre is not on the lattice");
    Assert::AreEqual(16.0f, UniverseZ(centre), 0.0f, L"the cell centre is not on the lattice");

    // A cell one west of the universe origin is the last cell of the sector before it, not the
    // first of this one -- which is the case a truncating division gets wrong.
    const Game::UniversePos westOfOrigin = Game::PathCellCentre(-1, -1);
    Assert::AreEqual(std::int64_t{-1}, westOfOrigin.sectorX, L"cell -1 did not land in the sector before the origin");
    Assert::AreEqual(-16.0f, UniverseX(westOfOrigin), 0.0f, L"cell -1's centre is not half a cell west of the origin");

    for (const float metres : {-9000.0f, -8192.0f, -8191.5f, -33.0f, -1.0f, 0.0f, 31.9f, 8191.0f, 20000.0f})
    {
      const Game::UniversePos at = Game::LocalPos(metres, metres);
      const std::int64_t cellX = Game::PathCellX(at);
      const Game::UniversePos back = Game::PathCellCentre(cellX, Game::PathCellZ(at));
      Assert::AreEqual(cellX, Game::PathCellX(back), L"a cell centre does not fall in its own cell");
      Assert::IsTrue(std::fabs(Game::OffsetX(at, back)) <= Game::PATH_CELL_SIZE_METRES * 0.5f,
                     L"a position is further than half a cell from its own cell centre");
      Assert::IsTrue(back.localX >= 0.0f && back.localX < Game::SECTOR_SIZE_METRES, L"a cell centre broke the UniversePos invariant");
    }
  }

  TEST_METHOD(AnEmptyUniversePlansNothing)
  {
    // Every phase before this one was verified with no planner in the tree. With no architecture in
    // it, the planner must hand back the destination and change nothing at all.
    Game::PathGrid grid;
    Assert::IsFalse(grid.HasObstacles(), L"an unbuilt grid claims to hold obstacles");

    std::vector<Game::UniversePos> route;
    Assert::IsTrue(grid.FindPath(Game::LocalPos(-500.0f, 0.0f), Game::LocalPos(500.0f, 0.0f), 100.0f, route),
                   L"an empty grid refused a route");
    Assert::AreEqual(size_t{1}, route.size(), L"an empty grid invented waypoints");
    Assert::AreEqual(500.0f, UniverseX(route[0]), 0.0f, L"an empty grid did not hand back the destination");
  }
};
} // namespace GameLogicTests
