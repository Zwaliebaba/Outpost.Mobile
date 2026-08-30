#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// The scene of Design/Archive/Hostiles.md 6, which is also what slice 3's ViewTuning.h constants spell. The
// two must agree: these are the numbers the patrol was argued against -- 400 m clears the station's
// 251.77 m skin by 148 m, and the chords clear its center by 386 m against the 263 m an Interceptor
// needs -- so a test run at different ones would prove something about a scene nobody plays.
constexpr float STATION_EAST_METRES = 850.0f;
constexpr float STATION_NORTH_METRES = 850.0f;
constexpr float RING_METRES = 400.0f;
constexpr float CRUISE_MPS = 10.0f;
constexpr int PATROL_COUNT = 3;

// One lap is twelve 207 m chords at 10 m/s: about 249 s, so 16,000 ticks is a lap with room to
// spare. Spelled once because four tests below need it and none of them should guess.
constexpr int TICKS_PER_LAP = 16000;

// Builds the scene into a caller's world and returns the station. The world is never copied: it
// holds every scratch buffer the tick uses, and a copy would be a second one silently.
Game::ShipId BuildHostileBase(Game::World& _world, std::vector<Game::ShipId>& _outPatrol, int _count = PATROL_COUNT)
{
  const Game::ShipId station = _world.SpawnShip(Game::LocalPos(STATION_EAST_METRES, STATION_NORTH_METRES), 0.0f,
                                                static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANDAL);
  const Game::WorldPos anchor = _world.Ship(station).posWorld;
  for (int at = 0; at < _count; ++at)
  {
    const std::uint32_t index =
      static_cast<std::uint32_t>(at) * Game::PATROL_RING_WAYPOINTS / static_cast<std::uint32_t>(std::max(1, _count));
    const Game::ShipId ship = _world.SpawnShip(Game::PatrolRingPoint(anchor, index, RING_METRES), Game::PatrolRingHeadingRad(index),
                                               static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    _world.AssignPatrol(ship, station, RING_METRES, CRUISE_MPS);
    _outPatrol.push_back(ship);
  }
  return station;
}
} // namespace

TEST_CLASS(PatrolTests)
{
public:
  TEST_METHOD(TheSamePatrolProducesTheSameRun)
  {
    // The replay gate, extended over the new pass. The patrol is the first intent in the tree that
    // does not come from a client, so the claim that it reproduces has to be measured rather than
    // argued -- and it is measured over the fields the pass itself writes, not only over position.
    const auto play = [](std::vector<Game::WorldPos>& _outTrack, std::vector<float>& _outMotion, std::vector<std::uint32_t>& _outWaypoints)
    {
      Game::World world;
      std::vector<Game::ShipId> patrol;
      (void)BuildHostileBase(world, patrol);
      for (int tick = 0; tick < 3600; ++tick)
      {
        world.Step();
        for (const Game::ShipId id : patrol)
        {
          _outTrack.push_back(world.Ship(id).posWorld);
          _outMotion.push_back(world.Ship(id).speed);
          _outMotion.push_back(world.Ship(id).headingRad);
          _outMotion.push_back(world.Ship(id).orderSpeedCapMetresPerSec);
          _outWaypoints.push_back(world.PatrolOf(id).waypointIndex);
        }
      }
    };

    std::vector<Game::WorldPos> firstTrack, secondTrack;
    std::vector<float> firstMotion, secondMotion;
    std::vector<std::uint32_t> firstWaypoints, secondWaypoints;
    play(firstTrack, firstMotion, firstWaypoints);
    play(secondTrack, secondMotion, secondWaypoints);

    Assert::AreEqual(firstTrack.size(), secondTrack.size(), L"the two runs produced different numbers of samples");
    for (std::size_t at = 0; at < firstTrack.size(); ++at)
      Assert::IsTrue(IsSamePosition(firstTrack[at], secondTrack[at]), L"a patrolling ship's position diverged between two identical runs");
    for (std::size_t at = 0; at < firstMotion.size(); ++at)
      Assert::AreEqual(firstMotion[at], secondMotion[at], 0.0f, L"a patrolling ship's motion diverged between two identical runs");
    for (std::size_t at = 0; at < firstWaypoints.size(); ++at)
      Assert::AreEqual(firstWaypoints[at], secondWaypoints[at], L"the ring walk diverged between two identical runs");
  }

  TEST_METHOD(APatrolWalksItsRingInOrder)
  {
    // Every waypoint, in index order, and no other order. A ring walked by any other rule -- nearest
    // point, or a carrot led round the circle -- would still look like a patrol from far enough away
    // and would be a different thing to test against.
    Game::World world;
    std::vector<Game::ShipId> patrol;
    const Game::ShipId station = BuildHostileBase(world, patrol, 1);
    const Game::WorldPos anchor = world.Ship(station).posWorld;
    const Game::ShipId ship = patrol[0];
    const float arrival = Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Interceptor));

    std::uint32_t expected = 1;
    int reached = 0;
    float previousBearing = 0.0f;
    bool haveBearing = false;
    for (int tick = 0; tick < TICKS_PER_LAP && reached < static_cast<int>(Game::PATROL_RING_WAYPOINTS); ++tick)
    {
      world.Step();
      const Game::WorldPos here = world.Ship(ship).posWorld;

      // Clockwise: the bearing from the anchor only ever increases, wrapping once per lap.
      const float bearing = std::atan2(Game::OffsetX(anchor, here), Game::OffsetZ(anchor, here));
      if (haveBearing)
      {
        const float step = DirectX::XMScalarModAngle(bearing - previousBearing);
        Assert::IsTrue(step >= -1e-3f, L"a patrolling ship went anticlockwise around its ring");
      }
      previousBearing = bearing;
      haveBearing = true;

      if (Game::Distance(here, Game::PatrolRingPoint(anchor, expected, RING_METRES)) <= arrival)
      {
        ++reached;
        expected = (expected + 1) % Game::PATROL_RING_WAYPOINTS;
      }
    }

    Assert::AreEqual(static_cast<int>(Game::PATROL_RING_WAYPOINTS), reached, L"the patrol did not visit every ring point in index order");
  }

  TEST_METHOD(TheCruiseCapHolds)
  {
    // "Slowly patrolling" is a property of the order, not of the hull, so the same Interceptor has
    // to do both: hold 10 m/s on the ring, and still reach its own maximum under a player's order.
    Game::World world;
    std::vector<Game::ShipId> patrol;
    (void)BuildHostileBase(world, patrol, 1);
    const Game::ShipId ship = patrol[0];

    float fastest = 0.0f;
    for (int tick = 0; tick < TICKS_PER_LAP; ++tick)
    {
      world.Step();
      fastest = std::max(fastest, world.Ship(ship).speed);
    }
    Assert::IsTrue(fastest <= CRUISE_MPS + 0.01f,
                   std::format(L"a patrolling Interceptor reached {:.3f} m/s against a {:.1f} m/s cap", fastest, CRUISE_MPS).c_str());

    Game::World open;
    const Game::ShipId free = open.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
    const Game::ShipId order[] = {free};
    (void)open.IssueMoveOrder(order, Game::LocalPos(0.0f, 2000.0f), false, 0.0f);
    float reached = 0.0f;
    for (int tick = 0; tick < 3000; ++tick)
    {
      open.Step();
      reached = std::max(reached, open.Ship(free).speed);
    }
    const float maximum = Game::HullSpecOf(Game::HullId::Interceptor).maxSpeedMetresPerSec;
    Assert::IsTrue(reached >= maximum * 0.99f,
                   std::format(L"an uncapped Interceptor only reached {:.2f} m/s of its {:.2f} m/s maximum", reached, maximum).c_str());
  }

  TEST_METHOD(APatrolNeverEntersItsStation)
  {
    // The geometry argument, measured. Design/Archive/Hostiles.md 5.2 says the legs clear the station by
    // about 120 m and that no avoidance or separation machinery has to change to make that true; if
    // this fails, the ring is wrong rather than the machinery.
    Game::World world;
    std::vector<Game::ShipId> patrol;
    const Game::ShipId station = BuildHostileBase(world, patrol);
    const float clearance =
      Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres() + Game::HullSpecOf(Game::HullId::Interceptor).BoundingRadiusMetres();

    float closest = 1e30f;
    for (int tick = 0; tick < TICKS_PER_LAP * 2; ++tick)
    {
      world.Step();
      for (const Game::ShipId id : patrol)
        closest = std::min(closest, Game::Distance(world.Ship(id).posWorld, world.Ship(station).posWorld));
    }
    Assert::IsTrue(closest > clearance,
                   std::format(L"a patrol came within {:.1f} m of a station needing {:.1f} m", closest, clearance).c_str());
  }

  TEST_METHOD(APatrolStandsDownWhenItsAnchorDies)
  {
    // A handle rather than a position, so the ring dies with the thing it was a ring around. The
    // ship finishes the leg it is on -- the pass has no arrival logic of its own -- and then stops.
    Game::World world;
    std::vector<Game::ShipId> patrol;
    const Game::ShipId station = BuildHostileBase(world, patrol, 1);
    const Game::ShipHandle shipHandle = world.HandleOf(patrol[0]);
    const Game::ShipHandle stationHandle = world.HandleOf(station);

    for (int tick = 0; tick < 600; ++tick)
      world.Step(); // well into the first leg

    Assert::IsTrue(world.DespawnShip(stationHandle), L"the despawn failed");
    Assert::AreEqual(Game::OrderState::Moving, world.Ship(world.Resolve(shipHandle)).order, L"the ship was not mid-leg to begin with");

    bool settled = false;
    for (int tick = 0; tick < TICKS_PER_LAP && !settled; ++tick)
    {
      world.Step();
      settled = world.Ship(world.Resolve(shipHandle)).order == Game::OrderState::Idle;
    }
    Assert::IsTrue(settled, L"the ship never finished the leg it was on when its station died");

    for (int tick = 0; tick < 600; ++tick)
    {
      world.Step();
      Assert::AreEqual(Game::OrderState::Idle, world.Ship(world.Resolve(shipHandle)).order, L"a stood-down patrol issued another leg");
    }
    Assert::IsFalse(world.PatrolOf(world.Resolve(shipHandle)).active, L"the patrol is still marked active with no anchor to walk around");
  }

  TEST_METHOD(ADespawnRepairsThePatrolTable)
  {
    // The cost of a second parallel array: swap-and-pop has to move it too, and the symptom of
    // forgetting is a surviving ship inheriting a stranger's ring rather than a crash.
    Game::World world;
    std::vector<Game::ShipId> patrol;
    const Game::ShipId station = BuildHostileBase(world, patrol);
    const Game::ShipHandle stationHandle = world.HandleOf(station);
    const Game::ShipHandle firstHandle = world.HandleOf(patrol[0]);
    const Game::ShipHandle lastHandle = world.HandleOf(patrol.back());
    const std::uint32_t lastWaypoint = world.PatrolOf(patrol.back()).waypointIndex;

    world.Step();
    Assert::IsTrue(world.DespawnShip(firstHandle), L"the despawn failed");

    const Game::ShipId moved = world.Resolve(lastHandle);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, moved, L"the surviving ship's handle stopped resolving");
    Assert::AreNotEqual(patrol.back(), moved, L"the survivor's index did not move, so this test proves nothing");

    const Game::World::Patrol& carried = world.PatrolOf(moved);
    Assert::IsTrue(carried.active, L"the moved ship lost its patrol");
    Assert::IsTrue(carried.anchor == stationHandle, L"the moved ship inherited a stranger's anchor");
    Assert::AreEqual(RING_METRES, carried.ringRadiusMetres, 0.0f, L"the moved ship inherited a stranger's ring");

    // And it keeps walking it: the next ring point after the one it had been issued is reached.
    const Game::WorldPos anchor = world.Ship(world.Resolve(stationHandle)).posWorld;
    const std::uint32_t next = (lastWaypoint + 1) % Game::PATROL_RING_WAYPOINTS;
    const float arrival = Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Interceptor));
    bool arrived = false;
    for (int tick = 0; tick < TICKS_PER_LAP && !arrived; ++tick)
    {
      world.Step();
      arrived = Game::Distance(world.Ship(world.Resolve(lastHandle)).posWorld, Game::PatrolRingPoint(anchor, next, RING_METRES)) <= arrival;
    }
    Assert::IsTrue(arrived, L"the ship swap-and-pop moved never reached its next ring point");
  }

  TEST_METHOD(AnOrderOutranksThePatrol)
  {
    // Otherwise the standing behavior is a ghost: the ship obeys, arrives, and then quietly wanders
    // back to a ring nobody asked it to be on.
    Game::World world;
    std::vector<Game::ShipId> patrol;
    (void)BuildHostileBase(world, patrol, 1);
    const Game::ShipId ship = patrol[0];

    for (int tick = 0; tick < 120; ++tick)
      world.Step();

    const Game::WorldPos destination = Game::LocalPos(STATION_EAST_METRES + 1200.0f, STATION_NORTH_METRES);
    const Game::ShipId order[] = {ship};
    (void)world.IssueMoveOrder(order, destination, false, 0.0f, Game::FACTION_VANDAL);
    Assert::IsFalse(world.PatrolOf(ship).active, L"an explicit order left the patrol running underneath it");
    Assert::AreEqual(0.0f, world.Ship(ship).orderSpeedCapMetresPerSec, 0.0f, L"a player's order inherited the patrol's cruise cap");

    bool settled = false;
    for (int tick = 0; tick < TICKS_PER_LAP && !settled; ++tick)
    {
      world.Step();
      settled = world.Ship(ship).order == Game::OrderState::Idle;
    }
    Assert::IsTrue(settled, L"the ordered ship never arrived");
    Assert::IsTrue(Game::Distance(world.Ship(ship).posWorld, destination) < 20.0f, L"the ordered ship stopped somewhere else");

    for (int tick = 0; tick < 600; ++tick)
    {
      world.Step();
      Assert::AreEqual(Game::OrderState::Idle, world.Ship(ship).order, L"a ship that had been ordered went back to patrolling");
    }
  }

  TEST_METHOD(AssignPatrolStartsAtTheNearestPoint)
  {
    // An assignment must not teleport intent. A ship sitting at 100 degrees is sent to the 90 degree
    // point, not all the way back round to north.
    Game::World world;
    const Game::ShipId station =
      world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANDAL);
    const float bearing = DirectX::XMConvertToRadians(100.0f);
    const Game::ShipId ship =
      world.SpawnShip(Game::LocalPos(std::sin(bearing) * RING_METRES, std::cos(bearing) * RING_METRES), bearing + DirectX::XM_PIDIV2,
                      static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    world.AssignPatrol(ship, station, RING_METRES, CRUISE_MPS);

    world.Step();
    Assert::AreEqual(3u, world.PatrolOf(ship).waypointIndex, L"the first leg was not the ring point nearest the ship");

    // And a station cannot be given a patrol around itself.
    world.AssignPatrol(station, station, RING_METRES, CRUISE_MPS);
    Assert::IsFalse(world.PatrolOf(station).active, L"a station was assigned to patrol itself");
  }
};
} // namespace GameLogicTests
