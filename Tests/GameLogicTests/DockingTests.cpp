#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// Far enough to be a journey and near enough that the test does not run for a simulated hour: a
// Corvette covers this in well under the tick budget below.
constexpr float STATION_EAST_METRES = 900.0f;
constexpr float STATION_NORTH_METRES = 0.0f;
constexpr int TICKS_TO_ARRIVE = 6000; // 100 s at 60 Hz, several times what the approach needs

[[nodiscard]] std::uint32_t Faction(Game::FactionId _faction)
{
  return _faction;
}

// A Vanguard station and one player Corvette at the origin. Returns the station's row id; the ship
// is always id 1, because the structure is spawned first and nothing has despawned yet.
Game::World::StationId BuildDockScene(Game::World& _world, Game::ShipId& _outStation, Game::ShipId& _outShip,
                                      Game::HullId _hull = Game::HullId::Corvette)
{
  _outStation = _world.SpawnShip(Game::LocalPos(STATION_EAST_METRES, STATION_NORTH_METRES), 0.0f,
                                 static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
  Game::World::StationDesc desc;
  desc.ownerFaction = Game::FACTION_VANGUARD;
  const Game::World::StationId station = _world.MakeStation(_outStation, desc);

  _outShip = _world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(_hull), Game::FACTION_PLAYER);
  return station;
}

[[nodiscard]] bool StepUntilDocked(Game::World& _world, Game::ShipHandle _ship, int _maxTicks = TICKS_TO_ARRIVE)
{
  for (int tick = 0; tick < _maxTicks; ++tick)
  {
    _world.Step();
    if (_world.Resolve(_ship) == Game::INVALID_SHIP_ID)
      return true;
  }
  return false;
}
} // namespace

TEST_CLASS(DockingTests)
{
public:
  TEST_METHOD(AShipDocksAndLeavesTheWorld)
  {
    Game::World world;
    Game::ShipId structure = 0;
    Game::ShipId ship = 0;
    const Game::World::StationId station = BuildDockScene(world, structure, ship);
    const Game::ShipHandle shipHandle = world.HandleOf(ship);
    const std::uint32_t hullId = world.Ship(ship).hullId;

    Assert::IsTrue(world.IssueDockOrder(std::array{ship}, structure, Game::FACTION_PLAYER) == Game::World::DockOrderResult::Ordered,
                   L"the dock order was refused");

    // The order is an order, not a next-tick suggestion: the first leg is issued immediately.
    Assert::IsTrue(world.Ship(ship).order == Game::OrderState::Moving, L"the ship did not set off on the tick it was ordered");
    Assert::IsTrue(world.DockingOf(ship).active, L"the docking intent was not set");

    Assert::IsTrue(StepUntilDocked(world, shipHandle), L"the ship never reached the station");

    // Captured, not merely arrived: it is gone from the world and inside the ledger.
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(shipHandle), L"a docked ship is still in the world");
    Assert::AreEqual(static_cast<std::size_t>(1), world.StationOf(station).docked.size(), L"the ledger did not gain a row");
    Assert::AreEqual(hullId, world.StationOf(station).docked[0].hullId, L"the ledger row forgot the hull");
    Assert::AreEqual(Faction(Game::FACTION_PLAYER), Faction(world.StationOf(station).docked[0].factionId),
                     L"the ledger row forgot the faction");

    // And it left with a cause, which is the whole reason this is not just a despawn.
    const std::span<const Game::DespawnRecord> log = world.DespawnsSince(0);
    Assert::AreEqual(static_cast<std::size_t>(1), log.size(), L"the departure was not logged exactly once");
    Assert::IsTrue(log[0].handle == shipHandle, L"the wrong ship was logged");
    Assert::IsTrue(log[0].cause == Game::DespawnCause::Docked, L"a docking was logged as a death");
  }

  // The capture range is derived per pair, so the two ends of the hull table must both dock -- a
  // flat range would put one inside the station's no-go band and leave the other a canyon short.
  TEST_METHOD(EveryHullDocksAtItsOwnRange)
  {
    for (const Game::HullId hull : {Game::HullId::Interceptor, Game::HullId::Corvette, Game::HullId::Carrier})
    {
      Game::World world;
      Game::ShipId structure = 0;
      Game::ShipId ship = 0;
      const Game::World::StationId station = BuildDockScene(world, structure, ship, hull);
      const Game::ShipHandle shipHandle = world.HandleOf(ship);
      const float expected =
        Game::DockRangeMetres(Game::HullSpecOf(world.Ship(structure).hullId), Game::HullSpecOf(world.Ship(ship).hullId));

      Assert::IsTrue(world.IssueDockOrder(std::array{ship}, structure, Game::FACTION_PLAYER) == Game::World::DockOrderResult::Ordered,
                     L"the dock order was refused");

      // Captured on the first tick it is within range, and never before: a hull that docked early
      // would be one whose range came from somebody else's bounding radius.
      float lastDistance = Game::Distance(world.Ship(structure).posWorld, world.Ship(ship).posWorld);
      bool docked = false;
      for (int tick = 0; tick < TICKS_TO_ARRIVE && !docked; ++tick)
      {
        world.Step();
        const Game::ShipId live = world.Resolve(shipHandle);
        if (live == Game::INVALID_SHIP_ID)
          docked = true;
        else
          lastDistance = Game::Distance(world.Ship(0).posWorld, world.Ship(live).posWorld);
      }
      Assert::IsTrue(docked, L"a hull never reached its station");
      Assert::IsTrue(lastDistance <= expected + 1.0f, L"a hull was captured further out than its own dock range");
      Assert::AreEqual(static_cast<std::size_t>(1), world.StationOf(station).docked.size(), L"the ledger did not gain a row");
    }
  }

  // A later move order is a change of mind. Before capture a docking ship is just a ship flying
  // somewhere, and nothing about it should survive being told to go elsewhere.
  TEST_METHOD(AMoveOrderCancelsDocking)
  {
    Game::World world;
    Game::ShipId structure = 0;
    Game::ShipId ship = 0;
    const Game::World::StationId station = BuildDockScene(world, structure, ship);
    const Game::ShipHandle shipHandle = world.HandleOf(ship);

    Assert::IsTrue(world.IssueDockOrder(std::array{ship}, structure, Game::FACTION_PLAYER) == Game::World::DockOrderResult::Ordered,
                   L"the dock order was refused");
    for (int tick = 0; tick < 60; ++tick)
      world.Step();
    Assert::IsTrue(world.DockingOf(ship).active, L"the ship stopped docking on its own");

    // Away from the station, and far enough that an unclean cancel would show as a ship that turns
    // round again.
    (void)world.IssueMoveOrder(std::array{ship}, Game::LocalPos(-1200.0f, 0.0f), false, 0.0f, Game::FACTION_PLAYER);
    Assert::IsFalse(world.DockingOf(ship).active, L"a move order did not clear the docking intent");

    for (int tick = 0; tick < TICKS_TO_ARRIVE; ++tick)
      world.Step();

    Assert::AreNotEqual(Game::INVALID_SHIP_ID, world.Resolve(shipHandle), L"the ship docked after being told to go elsewhere");
    Assert::IsTrue(world.StationOf(station).docked.empty(), L"a cancelled docking still reached the ledger");
    Assert::IsTrue(Game::Distance(world.Ship(structure).posWorld, world.Ship(world.Resolve(shipHandle)).posWorld) > 1000.0f,
                   L"the ship never left for its new destination");
  }

  TEST_METHOD(TheStandingGateRefusesADock)
  {
    Game::World world;
    Game::ShipId structure = 0;
    Game::ShipId ship = 0;
    const Game::World::StationId station = BuildDockScene(world, structure, ship);

    // A Vandal base beside it, which every other faction is hostile to from the first tick.
    const Game::ShipId base =
      world.SpawnShip(Game::LocalPos(-900.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANDAL);
    Game::World::StationDesc vandalDesc;
    vandalDesc.ownerFaction = Game::FACTION_VANDAL;
    (void)world.MakeStation(base, vandalDesc);

    Assert::IsTrue(world.IssueDockOrder(std::array{ship}, base, Game::FACTION_PLAYER) == Game::World::DockOrderResult::RefusedStanding,
                   L"the player was allowed to dock at the pirates");

    // Refused means *nothing changes*. Not a diverted ship, not a cleared patrol, not an intent.
    Assert::IsFalse(world.DockingOf(ship).active, L"a refused order still set a docking intent");
    Assert::IsTrue(world.Ship(ship).order == Game::OrderState::Idle, L"a refused order still moved the ship");

    // A target that is not a station at all.
    const Game::ShipId scenery =
      world.SpawnShip(Game::LocalPos(0.0f, 900.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    Assert::IsTrue(world.IssueDockOrder(std::array{ship}, scenery, Game::FACTION_PLAYER) == Game::World::DockOrderResult::NotAStation,
                   L"a Structure that is only scenery accepted a dock");
    Assert::IsFalse(world.DockingOf(ship).active, L"an order at scenery set a docking intent");

    // Somebody else's ship is dropped from an otherwise good order, and the rest of it still goes.
    const Game::ShipId theirs =
      world.SpawnShip(Game::LocalPos(100.0f, 100.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    Assert::IsTrue(world.IssueDockOrder(std::array{ship, theirs}, structure, Game::FACTION_PLAYER) == Game::World::DockOrderResult::Ordered,
                   L"a mixed order was refused outright");
    Assert::IsTrue(world.DockingOf(ship).active, L"the issuer's own ship was dropped from its order");
    Assert::IsFalse(world.DockingOf(theirs).active, L"another faction's ship took the order");
    Assert::IsTrue(world.StationOf(station).docked.empty(), L"nobody should have docked yet");
  }

  // The door is guarded, not just the doorbell: an aggression recorded during the flight turns the
  // ship away at capture range rather than admitting it on a permission it no longer has.
  TEST_METHOD(AggressionAbortsAnApproach)
  {
    Game::World world;
    Game::ShipId structure = 0;
    Game::ShipId ship = 0;
    const Game::World::StationId station = BuildDockScene(world, structure, ship);
    const Game::ShipHandle shipHandle = world.HandleOf(ship);

    Assert::IsTrue(world.IssueDockOrder(std::array{ship}, structure, Game::FACTION_PLAYER) == Game::World::DockOrderResult::Ordered,
                   L"the dock order was refused");

    // Mid-flight, well clear of capture range.
    for (int tick = 0; tick < 120; ++tick)
      world.Step();
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, world.Resolve(shipHandle), L"the ship docked before the test could provoke anybody");
    world.RecordAggression(shipHandle, station);

    for (int tick = 0; tick < TICKS_TO_ARRIVE; ++tick)
      world.Step();

    Assert::AreNotEqual(Game::INVALID_SHIP_ID, world.Resolve(shipHandle), L"a criminal was admitted to the station it had just attacked");
    Assert::IsTrue(world.StationOf(station).docked.empty(), L"a criminal reached the ledger");
    Assert::IsFalse(world.DockingOf(world.Resolve(shipHandle)).active, L"the aborted approach is still running");

    // Turned away at the door, not sent home: it stands down where it is, which is inside the
    // approach it had already flown.
    const float distance = Game::Distance(world.Ship(structure).posWorld, world.Ship(world.Resolve(shipHandle)).posWorld);
    Assert::IsTrue(distance < STATION_EAST_METRES, L"the aborted ship went back the way it came");
  }

  // The patrol table's test, widened to the third table. Swap-and-pop must move a docking intent
  // with the ship it belongs to, or a despawn silently retargets somebody else's approach.
  TEST_METHOD(ADespawnRepairsEveryTable)
  {
    Game::World world;
    const Game::ShipId structure = world.SpawnShip(Game::LocalPos(STATION_EAST_METRES, 0.0f), 0.0f,
                                                   static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    Game::World::StationDesc desc;
    desc.ownerFaction = Game::FACTION_VANGUARD;
    const Game::World::StationId station = world.MakeStation(structure, desc);

    const Game::ShipId first = world.SpawnShip(Game::LocalPos(0.0f, -100.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    const Game::ShipId second = world.SpawnShip(Game::LocalPos(0.0f, 100.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    const Game::ShipHandle secondHandle = world.HandleOf(second);

    Assert::IsTrue(world.IssueDockOrder(std::array{second}, structure, Game::FACTION_PLAYER) == Game::World::DockOrderResult::Ordered,
                   L"the dock order was refused");
    Assert::IsFalse(world.DockingOf(first).active, L"the wrong ship took the order");

    // Despawn the ship in front of it, so swap-and-pop moves the docking one down the array.
    Assert::IsTrue(world.DespawnShip(world.HandleOf(first)), L"the despawn failed");
    const Game::ShipId moved = world.Resolve(secondHandle);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, moved, L"the surviving ship stopped resolving");
    Assert::IsTrue(world.DockingOf(moved).active, L"the moved ship lost its docking intent");

    Assert::IsTrue(StepUntilDocked(world, secondHandle), L"the moved ship never docked");
    Assert::AreEqual(static_cast<std::size_t>(1), world.StationOf(station).docked.size(), L"the moved ship docked as somebody else");
  }

  // A station that stops being one -- or whose structure dies -- ends the approach where it stands,
  // rather than flying ships at an index that now means something else.
  TEST_METHOD(ADeadStationStandsItsVisitorsDown)
  {
    Game::World world;
    Game::ShipId structure = 0;
    Game::ShipId ship = 0;
    (void)BuildDockScene(world, structure, ship);
    const Game::ShipHandle shipHandle = world.HandleOf(ship);

    Assert::IsTrue(world.IssueDockOrder(std::array{ship}, structure, Game::FACTION_PLAYER) == Game::World::DockOrderResult::Ordered,
                   L"the dock order was refused");
    for (int tick = 0; tick < 60; ++tick)
      world.Step();

    Assert::IsTrue(world.DespawnShip(world.HandleOf(structure)), L"the despawn failed");
    world.Step();

    const Game::ShipId live = world.Resolve(shipHandle);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, live, L"the visitor vanished with its station");
    Assert::IsFalse(world.DockingOf(live).active, L"the visitor is still docking at a station that is gone");
  }

  // The replay gate over the new pass, in the shape the patrol test set.
  TEST_METHOD(TheSameDockProducesTheSameRun)
  {
    const auto play = [](std::vector<Game::WorldPos>& _outTrack, std::vector<float>& _outMotion, std::vector<std::size_t>& _outLedger)
    {
      Game::World world;
      Game::ShipId structure = 0;
      Game::ShipId ship = 0;
      const Game::World::StationId station = BuildDockScene(world, structure, ship);
      const Game::ShipId second =
        world.SpawnShip(Game::LocalPos(-120.0f, 60.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_PLAYER);
      (void)world.IssueDockOrder(std::array{ship, second}, structure, Game::FACTION_PLAYER);

      for (int tick = 0; tick < 4000; ++tick)
      {
        world.Step();
        for (Game::ShipId id = 0; id < world.ShipCount(); ++id)
        {
          _outTrack.push_back(world.Ship(id).posWorld);
          _outMotion.push_back(world.Ship(id).speed);
          _outMotion.push_back(world.Ship(id).headingRad);
        }
        _outLedger.push_back(world.StationOf(station).docked.size());
      }
    };

    std::vector<Game::WorldPos> firstTrack, secondTrack;
    std::vector<float> firstMotion, secondMotion;
    std::vector<std::size_t> firstLedger, secondLedger;
    play(firstTrack, firstMotion, firstLedger);
    play(secondTrack, secondMotion, secondLedger);

    Assert::AreEqual(firstTrack.size(), secondTrack.size(), L"the two runs produced different numbers of samples");
    for (std::size_t at = 0; at < firstTrack.size(); ++at)
      Assert::IsTrue(IsSamePosition(firstTrack[at], secondTrack[at]), L"a docking ship's position diverged between two identical runs");
    for (std::size_t at = 0; at < firstMotion.size(); ++at)
      Assert::AreEqual(firstMotion[at], secondMotion[at], 0.0f, L"a docking ship's motion diverged between two identical runs");
    for (std::size_t at = 0; at < firstLedger.size(); ++at)
      Assert::AreEqual(firstLedger[at], secondLedger[at], L"the ledger filled on a different tick between two identical runs");

    // The scene has to actually dock somebody, or the replay above proves nothing.
    Assert::AreEqual(static_cast<std::size_t>(2), firstLedger.back(), L"the replay scene never docked both ships");
  }
};
} // namespace GameLogicTests
