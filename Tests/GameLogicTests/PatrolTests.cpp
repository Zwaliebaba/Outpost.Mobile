#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// The scene of Design/Archive/Hostiles.md 6 -- read from the shipped constants rather than spelled
// again beside them.
//
// It used to be five literals under a comment saying "the two must agree", and nothing made them.
// Measured: moving the base from 850 m to 9 000 m was a content change that no row in this suite
// noticed, because every row was testing its own copy of the scene rather than the one the game
// ships (Design/Archive/Universe-slice-5b.md 8, mutation 7). The copies could not be deleted while the
// constants lived in a client header this project cannot see; slice 5b moved them into GameLogic,
// which is what makes this possible now.
constexpr float STATION_EAST_METRES = Game::HOSTILE_BASE_EAST_METRES;
constexpr float STATION_NORTH_METRES = Game::HOSTILE_BASE_NORTH_METRES;
constexpr float RING_METRES = Game::HOSTILE_PATROL_RING_METRES;
constexpr float CRUISE_MPS = Game::HOSTILE_PATROL_CRUISE_MPS;
constexpr int PATROL_COUNT = static_cast<int>(Game::HOSTILE_PATROL_COUNT);

// One lap is twelve 207 m chords at 10 m/s: about 249 s, so 16,000 ticks is a lap with room to
// spare. Spelled once because four tests below need it and none of them should guess.
constexpr int TICKS_PER_LAP = 16000;

// Builds the scene into a caller's universe and returns the station. The universe is never copied: it
// holds every scratch buffer the tick uses, and a copy would be a second one silently.
Game::ShipId BuildHostileBase(Game::Universe& _universe, std::vector<Game::ShipId>& _outPatrol, int _count = PATROL_COUNT)
{
  const Game::ShipId station = _universe.SpawnShip(Game::LocalPos(STATION_EAST_METRES, STATION_NORTH_METRES), 0.0f,
                                                   static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANDAL);
  const Game::UniversePos anchor = _universe.Ship(station).posUniverse;
  for (int at = 0; at < _count; ++at)
  {
    const std::uint32_t index =
      static_cast<std::uint32_t>(at) * Game::PATROL_RING_WAYPOINTS / static_cast<std::uint32_t>(std::max(1, _count));
    const Game::ShipId ship = _universe.SpawnShip(Game::PatrolRingPoint(anchor, index, RING_METRES), Game::PatrolRingHeadingRad(index),
                                                  static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    _universe.AssignPatrol(ship, station, RING_METRES, CRUISE_MPS);
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
    const auto play =
      [](std::vector<Game::UniversePos>& _outTrack, std::vector<float>& _outMotion, std::vector<std::uint32_t>& _outWaypoints)
    {
      Game::Universe universe;
      std::vector<Game::ShipId> patrol;
      (void)BuildHostileBase(universe, patrol);
      for (int tick = 0; tick < 3600; ++tick)
      {
        universe.Step();
        for (const Game::ShipId id : patrol)
        {
          _outTrack.push_back(universe.Ship(id).posUniverse);
          _outMotion.push_back(universe.Ship(id).speed);
          _outMotion.push_back(universe.Ship(id).headingRad);
          _outMotion.push_back(universe.Ship(id).orderSpeedCapMetresPerSec);
          _outWaypoints.push_back(universe.PatrolOf(id).waypointIndex);
        }
      }
    };

    std::vector<Game::UniversePos> firstTrack, secondTrack;
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
    Game::Universe universe;
    std::vector<Game::ShipId> patrol;
    const Game::ShipId station = BuildHostileBase(universe, patrol, 1);
    const Game::UniversePos anchor = universe.Ship(station).posUniverse;
    const Game::ShipId ship = patrol[0];
    const float arrival = Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Interceptor));

    std::uint32_t expected = 1;
    int reached = 0;
    float previousBearing = 0.0f;
    bool haveBearing = false;
    for (int tick = 0; tick < TICKS_PER_LAP && reached < static_cast<int>(Game::PATROL_RING_WAYPOINTS); ++tick)
    {
      universe.Step();
      const Game::UniversePos here = universe.Ship(ship).posUniverse;

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
    Game::Universe universe;
    std::vector<Game::ShipId> patrol;
    (void)BuildHostileBase(universe, patrol, 1);
    const Game::ShipId ship = patrol[0];

    float fastest = 0.0f;
    for (int tick = 0; tick < TICKS_PER_LAP; ++tick)
    {
      universe.Step();
      fastest = std::max(fastest, universe.Ship(ship).speed);
    }
    Assert::IsTrue(fastest <= CRUISE_MPS + 0.01f,
                   std::format(L"a patrolling Interceptor reached {:.3f} m/s against a {:.1f} m/s cap", fastest, CRUISE_MPS).c_str());

    Game::Universe open;
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
    Game::Universe universe;
    std::vector<Game::ShipId> patrol;
    const Game::ShipId station = BuildHostileBase(universe, patrol);
    const float clearance =
      Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres() + Game::HullSpecOf(Game::HullId::Interceptor).BoundingRadiusMetres();

    float closest = 1e30f;
    for (int tick = 0; tick < TICKS_PER_LAP * 2; ++tick)
    {
      universe.Step();
      for (const Game::ShipId id : patrol)
        closest = std::min(closest, Game::Distance(universe.Ship(id).posUniverse, universe.Ship(station).posUniverse));
    }
    Assert::IsTrue(closest > clearance,
                   std::format(L"a patrol came within {:.1f} m of a station needing {:.1f} m", closest, clearance).c_str());
  }

  TEST_METHOD(APatrolStandsDownWhenItsAnchorDies)
  {
    // A handle rather than a position, so the ring dies with the thing it was a ring around. The
    // ship finishes the leg it is on -- the pass has no arrival logic of its own -- and then stops.
    Game::Universe universe;
    std::vector<Game::ShipId> patrol;
    const Game::ShipId station = BuildHostileBase(universe, patrol, 1);
    const Game::ShipHandle shipHandle = universe.HandleOf(patrol[0]);
    const Game::ShipHandle stationHandle = universe.HandleOf(station);

    for (int tick = 0; tick < 600; ++tick)
      universe.Step(); // well into the first leg

    Assert::IsTrue(universe.DespawnShip(stationHandle), L"the despawn failed");
    Assert::AreEqual(Game::OrderState::Moving, universe.Ship(universe.Resolve(shipHandle)).order,
                     L"the ship was not mid-leg to begin with");

    bool settled = false;
    for (int tick = 0; tick < TICKS_PER_LAP && !settled; ++tick)
    {
      universe.Step();
      settled = universe.Ship(universe.Resolve(shipHandle)).order == Game::OrderState::Idle;
    }
    Assert::IsTrue(settled, L"the ship never finished the leg it was on when its station died");

    for (int tick = 0; tick < 600; ++tick)
    {
      universe.Step();
      Assert::AreEqual(Game::OrderState::Idle, universe.Ship(universe.Resolve(shipHandle)).order,
                       L"a stood-down patrol issued another leg");
    }
    Assert::IsFalse(universe.PatrolOf(universe.Resolve(shipHandle)).active,
                    L"the patrol is still marked active with no anchor to walk around");
  }

  TEST_METHOD(ADespawnRepairsThePatrolTable)
  {
    // The cost of a second parallel array: swap-and-pop has to move it too, and the symptom of
    // forgetting is a surviving ship inheriting a stranger's ring rather than a crash.
    Game::Universe universe;
    std::vector<Game::ShipId> patrol;
    const Game::ShipId station = BuildHostileBase(universe, patrol);
    const Game::ShipHandle stationHandle = universe.HandleOf(station);
    const Game::ShipHandle firstHandle = universe.HandleOf(patrol[0]);
    const Game::ShipHandle lastHandle = universe.HandleOf(patrol.back());
    const std::uint32_t lastWaypoint = universe.PatrolOf(patrol.back()).waypointIndex;

    universe.Step();
    Assert::IsTrue(universe.DespawnShip(firstHandle), L"the despawn failed");

    const Game::ShipId moved = universe.Resolve(lastHandle);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, moved, L"the surviving ship's handle stopped resolving");
    Assert::AreNotEqual(patrol.back(), moved, L"the survivor's index did not move, so this test proves nothing");

    const Game::Universe::Patrol& carried = universe.PatrolOf(moved);
    Assert::IsTrue(carried.active, L"the moved ship lost its patrol");
    Assert::IsTrue(carried.anchor == stationHandle, L"the moved ship inherited a stranger's anchor");
    Assert::AreEqual(RING_METRES, carried.ringRadiusMetres, 0.0f, L"the moved ship inherited a stranger's ring");

    // And it keeps walking it: the next ring point after the one it had been issued is reached.
    const Game::UniversePos anchor = universe.Ship(universe.Resolve(stationHandle)).posUniverse;
    const std::uint32_t next = (lastWaypoint + 1) % Game::PATROL_RING_WAYPOINTS;
    const float arrival = Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Interceptor));
    bool arrived = false;
    for (int tick = 0; tick < TICKS_PER_LAP && !arrived; ++tick)
    {
      universe.Step();
      arrived = Game::Distance(universe.Ship(universe.Resolve(lastHandle)).posUniverse, Game::PatrolRingPoint(anchor, next, RING_METRES)) <=
                arrival;
    }
    Assert::IsTrue(arrived, L"the ship swap-and-pop moved never reached its next ring point");
  }

  TEST_METHOD(AnOrderOutranksThePatrol)
  {
    // Otherwise the standing behavior is a ghost: the ship obeys, arrives, and then quietly wanders
    // back to a ring nobody asked it to be on.
    Game::Universe universe;
    std::vector<Game::ShipId> patrol;
    (void)BuildHostileBase(universe, patrol, 1);
    const Game::ShipId ship = patrol[0];

    for (int tick = 0; tick < 120; ++tick)
      universe.Step();

    const Game::UniversePos destination = Game::LocalPos(STATION_EAST_METRES + 1200.0f, STATION_NORTH_METRES);
    const Game::ShipId order[] = {ship};
    (void)universe.IssueMoveOrder(order, destination, false, 0.0f, Game::FACTION_VANDAL);
    Assert::IsFalse(universe.PatrolOf(ship).active, L"an explicit order left the patrol running underneath it");
    Assert::AreEqual(0.0f, universe.Ship(ship).orderSpeedCapMetresPerSec, 0.0f, L"a player's order inherited the patrol's cruise cap");

    bool settled = false;
    for (int tick = 0; tick < TICKS_PER_LAP && !settled; ++tick)
    {
      universe.Step();
      settled = universe.Ship(ship).order == Game::OrderState::Idle;
    }
    Assert::IsTrue(settled, L"the ordered ship never arrived");
    Assert::IsTrue(Game::Distance(universe.Ship(ship).posUniverse, destination) < 20.0f, L"the ordered ship stopped somewhere else");

    for (int tick = 0; tick < 600; ++tick)
    {
      universe.Step();
      Assert::AreEqual(Game::OrderState::Idle, universe.Ship(ship).order, L"a ship that had been ordered went back to patrolling");
    }
  }

  TEST_METHOD(AssignPatrolStartsAtTheNearestPoint)
  {
    // An assignment must not teleport intent. A ship sitting at 100 degrees is sent to the 90 degree
    // point, not all the way back round to north.
    Game::Universe universe;
    const Game::ShipId station =
      universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANDAL);
    const float bearing = DirectX::XMConvertToRadians(100.0f);
    const Game::ShipId ship =
      universe.SpawnShip(Game::LocalPos(std::sin(bearing) * RING_METRES, std::cos(bearing) * RING_METRES), bearing + DirectX::XM_PIDIV2,
                         static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    universe.AssignPatrol(ship, station, RING_METRES, CRUISE_MPS);

    universe.Step();
    Assert::AreEqual(3u, universe.PatrolOf(ship).waypointIndex, L"the first leg was not the ring point nearest the ship");

    // And a station cannot be given a patrol around itself.
    universe.AssignPatrol(station, station, RING_METRES, CRUISE_MPS);
    Assert::IsFalse(universe.PatrolOf(station).active, L"a station was assigned to patrol itself");
  }

  // The scene's own arguments, pinned.
  //
  // Reading the shipped constants stops this suite from testing a scene nobody plays, but it does
  // not stop the scene itself from being moved somewhere the design's reasoning no longer holds: a
  // base at 9 km still patrols correctly, it is just no longer the base Hostiles.md argued for.
  // These are the four claims in that argument, each against the value it was argued from rather
  // than a number repeated here (Design/Archive/Universe-slice-5b.md 8).
  TEST_METHOD(TheHostileBaseSitsWhereItsDesignArguedFor)
  {
    const float outMetres = std::sqrt(Game::HOSTILE_BASE_EAST_METRES * Game::HOSTILE_BASE_EAST_METRES +
                                      Game::HOSTILE_BASE_NORTH_METRES * Game::HOSTILE_BASE_NORTH_METRES);

    // Inside the interest radius, so the base is subscribed from the first update and the overview
    // shows red immediately -- which is what makes the rival visible at all without flying to it.
    Assert::IsTrue(outMetres < Game::INTEREST_RADIUS_METRES,
                   L"the hostile base is outside the interest radius: it would not be sent to a client at the origin");

    // And so is its patrol, at its farthest.
    Assert::IsTrue(outMetres + Game::HOSTILE_PATROL_RING_METRES < Game::INTEREST_RADIUS_METRES,
                   L"the farthest patrol point is outside the interest radius");

    // The ring clears the station's own skin, or the patrol flies through the thing it is guarding.
    const float skin = Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres();
    Assert::IsTrue(Game::HOSTILE_PATROL_RING_METRES > skin, L"the patrol ring is inside the station's skin");

    // The chords clear the station's centre by more than an Interceptor needs, so the legs plan
    // straight and the station never even scores as a threat. A ring of PATROL_RING_WAYPOINTS
    // points has chords at radius * cos(pi / n) from the centre.
    const float chordClearance =
      Game::HOSTILE_PATROL_RING_METRES * std::cos(DirectX::XM_PI / static_cast<float>(Game::PATROL_RING_WAYPOINTS));
    const float needed = Game::HullSpecOf(Game::HullId::Interceptor).BoundingRadiusMetres() + Game::PATH_CLEARANCE_MARGIN_METRES;
    Assert::IsTrue(chordClearance > needed,
                   L"a patrol chord passes closer to the station than an Interceptor needs -- the legs would not plan straight");
  }
};
} // namespace GameLogicTests
