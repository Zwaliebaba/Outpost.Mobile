#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
constexpr int TICKS_TO_CLEAR = 3600; // 60 s at 60 Hz, many times what a Corvette needs to round a station

// A station at the origin and a ship pressed against its southern skin, nose-in: the state a ship is
// left in after an order that ended at the wall, or after traffic shoved it there.
Game::ShipId PressAgainstStation(Game::World& _world, Game::HullId _hull, Game::ShipId& _outStation)
{
  _outStation =
    _world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
  const float skin = Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres() + Game::HullSpecOf(_hull).BoundingRadiusMetres();
  return _world.SpawnShip(Game::LocalPos(0.0f, -skin), 0.0f, static_cast<std::uint32_t>(_hull), Game::FACTION_PLAYER);
}

[[nodiscard]] float StepTowards(Game::World& _world, Game::ShipId _ship, const Game::WorldPos& _to, int _maxTicks = TICKS_TO_CLEAR)
{
  const float arrival = Game::ArrivalRadiusMetres(Game::HullSpecOf(_world.Ship(_ship).hullId));
  float nearest = 1e9f;
  for (int tick = 0; tick < _maxTicks; ++tick)
  {
    _world.Step();
    nearest = std::min(nearest, Game::Distance(_world.Ship(_ship).posWorld, _to));
    if (nearest <= arrival)
      break;
  }
  return nearest;
}

std::wstring Describe(const Game::World& _world, Game::ShipId _ship, float _nearest)
{
  const Game::ShipState& ship = _world.Ship(_ship);
  return L"nearest " + std::to_wstring(_nearest) + L" m, at (" + std::to_wstring(Game::OffsetX(Game::WorldPos{}, ship.posWorld)) + L", " +
         std::to_wstring(Game::OffsetZ(Game::WorldPos{}, ship.posWorld)) + L"), heading " + std::to_wstring(ship.headingRad) + L", speed " +
         std::to_wstring(ship.speed) + L", order " + std::to_wstring(static_cast<int>(ship.order));
}
} // namespace

TEST_CLASS(BackOffTests)
{
public:
  TEST_METHOD(AShipPressedAgainstAStationBacksOffAndTurns)
  {
    Game::World world;
    Game::ShipId station = 0;
    const Game::ShipId ship = PressAgainstStation(world, Game::HullId::Corvette, station);
    const Game::WorldPos farSide = Game::LocalPos(0.0f, 600.0f);
    world.IssueMoveOrder(std::array{ship}, farSide, false, 0.0f);
    const float nearest = StepTowards(world, ship, farSide);
    Assert::IsTrue(nearest <= Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Corvette)),
                   (L"pressed nose-in, never reached the far side: " + Describe(world, ship, nearest)).c_str());
  }

  TEST_METHOD(AnOrderIntoAStationThenAwayIsObeyed)
  {
    // What a tap on a station does today: an order into the middle of it. The ship gets as close as
    // it can and then has to obey the next order, whichever way it points.
    Game::World world;
    const Game::ShipId station =
      world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    (void)station;
    const Game::ShipId ship =
      world.SpawnShip(Game::LocalPos(0.0f, -900.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
    world.IssueMoveOrder(std::array{ship}, Game::LocalPos(0.0f, 0.0f), false, 0.0f);
    for (int tick = 0; tick < 1800; ++tick)
      world.Step();
    const float pressed = Game::Distance(world.Ship(ship).posWorld, Game::LocalPos(0.0f, 0.0f));
    Assert::IsTrue(pressed < 300.0f, (L"the ship never reached the station's skin: " + Describe(world, ship, pressed)).c_str());

    const Game::WorldPos away[] = {Game::LocalPos(0.0f, 600.0f), Game::LocalPos(600.0f, -400.0f), Game::LocalPos(0.0f, -900.0f)};
    for (const Game::WorldPos& to : away)
    {
      world.IssueMoveOrder(std::array{ship}, to, false, 0.0f);
      const float nearest = StepTowards(world, ship, to);
      Assert::IsTrue(nearest <= Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Corvette)),
                     (L"after an order into the station, an order away was not obeyed: " + Describe(world, ship, nearest)).c_str());
    }
  }

  TEST_METHOD(TwoShipsOrderedIntoAStationBothLeaveOnTheNextOrder)
  {
    Game::World world;
    (void)world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    const Game::ShipId corvette =
      world.SpawnShip(Game::LocalPos(-30.0f, -900.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
    const Game::ShipId frigate =
      world.SpawnShip(Game::LocalPos(30.0f, -900.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Frigate), Game::FACTION_PLAYER);
    const std::array pair{corvette, frigate};
    world.IssueMoveOrder(pair, Game::LocalPos(0.0f, 0.0f), false, 0.0f);
    for (int tick = 0; tick < 1800; ++tick)
      world.Step();

    const Game::WorldPos to = Game::LocalPos(500.0f, -700.0f);
    world.IssueMoveOrder(pair, to, false, 0.0f);
    for (int tick = 0; tick < TICKS_TO_CLEAR; ++tick)
      world.Step();
    for (const Game::ShipId ship : pair)
    {
      const float left = Game::Distance(world.Ship(ship).posWorld, to);
      Assert::IsTrue(left < 150.0f,
                     (L"one of a pair ordered into a station did not leave on the next order: " + Describe(world, ship, left)).c_str());
    }
  }

  TEST_METHOD(TheStartingFleetOrderedOntoAStationObeysEveryLaterOrder)
  {
    // The scene the owner reported from: the three starting hulls, ordered onto a Vanguard station
    // as a formation, then ordered about. Every ship must follow every later order.
    Game::World world;
    (void)world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    const std::array fleet{
      world.SpawnShip(Game::LocalPos(-55.0f, -1200.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber), Game::FACTION_PLAYER),
      world.SpawnShip(Game::LocalPos(0.0f, -1200.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER),
      world.SpawnShip(Game::LocalPos(55.0f, -1200.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Frigate), Game::FACTION_PLAYER)};

    // Onto the station itself, then onto its arm just outside the collision circle, then onto the
    // centre again from the other side, then away.
    const Game::WorldPos stops[] = {Game::LocalPos(0.0f, 0.0f),   Game::LocalPos(0.0f, -270.0f),  Game::LocalPos(200.0f, -200.0f),
                                    Game::LocalPos(0.0f, 0.0f),   Game::LocalPos(-260.0f, 60.0f), Game::LocalPos(0.0f, 0.0f),
                                    Game::LocalPos(0.0f, -900.0f)};
    for (const Game::WorldPos& to : stops)
    {
      world.IssueMoveOrder(fleet, to, false, 0.0f);
      for (int tick = 0; tick < 2400; ++tick)
        world.Step();
    }
    // The last leg is the whole way round the station for whichever hull ended on its far side, and
    // a Frigate rounds it at a third of its speed with avoidance shedding the rest: two minutes,
    // where the pre-fix failure was a ship at 19 m/s making no progress at all.
    for (int tick = 0; tick < 2 * TICKS_TO_CLEAR; ++tick)
      world.Step();
    for (const Game::ShipId ship : fleet)
    {
      const float left = Game::Distance(world.Ship(ship).posWorld, Game::LocalPos(0.0f, -900.0f));
      Assert::IsTrue(left < 200.0f, (L"a ship of the fleet did not follow the last order away: " + Describe(world, ship, left)).c_str());
    }
  }

  TEST_METHOD(AnOrderIntoAStationEndsAtItsSkin)
  {
    // The order a tap on a station is today. It cannot be obeyed, so it has to *end*: the ship
    // reaches the skin, pushes for BLOCKED_WAYPOINT_TICKS, and the order completes there rather
    // than running at the wall for ever.
    Game::World world;
    (void)world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    const Game::ShipId ship =
      world.SpawnShip(Game::LocalPos(0.0f, -600.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
    world.IssueMoveOrder(std::array{ship}, Game::LocalPos(0.0f, 0.0f), false, 0.0f);

    int idleAt = -1;
    for (int tick = 0; tick < TICKS_TO_CLEAR; ++tick)
    {
      world.Step();
      if (world.Ship(ship).order == Game::OrderState::Idle)
      {
        idleAt = tick;
        break;
      }
    }
    Assert::IsTrue(idleAt >= 0, (L"an order into a station never ended: " + Describe(world, ship, 0.0f)).c_str());
    const float skin =
      Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres() + Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres();
    const float standing = Game::Distance(world.Ship(ship).posWorld, Game::LocalPos(0.0f, 0.0f));
    Assert::IsTrue(standing < skin + 2.0f * Game::PATH_CELL_SIZE_METRES,
                   (L"the order ended short of the skin: " + Describe(world, ship, standing)).c_str());
    // A capsule against a circle rests a little inside the sum of the two bounding radii when its
    // nose is the contact, so the tolerance is a hull's capsule radius rather than zero.
    Assert::IsTrue(standing >= skin - Game::HullSpecOf(Game::HullId::Corvette).capsuleRadiusMetres,
                   (L"the ship is inside the station: " + Describe(world, ship, standing)).c_str());
  }

  TEST_METHOD(AShipSandwichedBetweenAFriendAndAStationLeaves)
  {
    Game::World world;
    (void)world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
    const float skin =
      Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres() + Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres();
    const Game::ShipId corvette =
      world.SpawnShip(Game::LocalPos(0.0f, -skin), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
    const Game::ShipId frigate =
      world.SpawnShip(Game::LocalPos(0.0f, -skin - 40.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Frigate), Game::FACTION_PLAYER);
    world.IssueMoveOrder(std::array{corvette, frigate}, Game::LocalPos(0.0f, 0.0f), false, 0.0f);
    for (int tick = 0; tick < 1200; ++tick)
      world.Step();

    const Game::WorldPos to = Game::LocalPos(700.0f, 0.0f);
    world.IssueMoveOrder(std::array{corvette}, to, false, 0.0f);
    const float nearest = StepTowards(world, corvette, to);
    Assert::IsTrue(nearest <= Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Corvette)),
                   (L"a sandwiched ship never left: " + Describe(world, corvette, nearest)).c_str());
  }
};
} // namespace GameLogicTests
