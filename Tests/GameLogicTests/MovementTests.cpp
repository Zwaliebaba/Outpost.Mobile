#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace GameLogicTests
{
namespace
{
// Enough ticks for a ship to cross a few hundred metres and settle; the arrival tests fail loudly
// rather than hanging if the motion model stops converging.
constexpr int MAX_TICKS = 60 * 60;

int RunUntilIdle(Game::Universe& _universe, Game::ShipId _id)
{
  for (int tick = 0; tick < MAX_TICKS; ++tick)
  {
    if (_universe.Ship(_id).order == Game::OrderState::Idle)
      return tick;
    _universe.Step();
  }
  return -1;
}
} // namespace

TEST_CLASS(MovementTests)
{
public:
  TEST_METHOD(AShipArrivesAtItsOrder)
  {
    Game::Universe universe;
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, 0);

    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 300.0f), false, 0.0f);

    const int ticks = RunUntilIdle(universe, ship);
    Assert::IsTrue(ticks > 0, L"the ship never reached its order");

    const Game::ShipState& state = universe.Ship(ship);
    const float dz = 300.0f - UniverseZ(state.posUniverse);
    const float arrival = Game::ArrivalRadiusMetres(Game::HullSpecOf(state.hullId));
    Assert::IsTrue(std::fabs(dz) <= arrival * 2.0f, L"the ship stopped short of, or past, its order");
  }

  TEST_METHOD(AShipComesToRestOnceIdle)
  {
    Game::Universe universe;
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, 0);

    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, Game::LocalPos(120.0f, 120.0f), false, 0.0f);
    Assert::IsTrue(RunUntilIdle(universe, ship) > 0, L"the ship never reached its order");

    for (int tick = 0; tick < 240; ++tick)
      universe.Step();
    Assert::AreEqual(0.0f, universe.Ship(ship).speed, 1e-4f, L"an idle ship is still moving");
  }

  TEST_METHOD(AnOrderedFacingIsHeldAfterArrival)
  {
    Game::Universe universe;
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, 0);

    const Game::ShipId order[] = {ship};
    const float facing = XMConvertToRadians(90.0f);
    universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 200.0f), true, facing);
    Assert::IsTrue(RunUntilIdle(universe, ship) > 0, L"the ship never finished aligning");

    const float error = XMScalarModAngle(universe.Ship(ship).headingRad - facing);
    Assert::IsTrue(std::fabs(error) < 0.05f, L"the ship settled on the wrong heading");
  }

  TEST_METHOD(TheSameOrderProducesTheSameRun)
  {
    // The determinism gate in miniature. Two universes given identical input must stay bit-identical
    // tick for tick; if this ever fails, something in the simulation has started reading state it
    // does not own -- a clock, an address, an iteration order.
    // A fleet spanning the whole table with architecture in it, so the run exercises every pass:
    // the index's static store, the neighbour sort, avoidance, separation, blocking and a planned
    // route. A determinism gate over one hull in an empty universe proves much less than it looks.
    const auto play = [](std::vector<Game::UniversePos>& _outTrack)
    {
      Game::Universe universe;
      universe.SpawnShip(Game::LocalPos(-260.0f, 700.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure));

      std::vector<Game::ShipId> ships;
      const Game::HullId fleet[] = {Game::HullId::Interceptor, Game::HullId::Corvette, Game::HullId::Frigate, Game::HullId::Battleship,
                                    Game::HullId::Carrier};
      for (int i = 0; i < 5; ++i)
        ships.push_back(
          universe.SpawnShip(Game::LocalPos(static_cast<float>(i) * 240.0f - 480.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(fleet[i])));

      universe.IssueMoveOrder(ships, Game::LocalPos(200.0f, 1350.0f), true, 1.2f);
      for (int tick = 0; tick < 600; ++tick)
      {
        universe.Step();
        for (const Game::ShipId id : ships)
          _outTrack.push_back(universe.Ship(id).posUniverse);
      }
    };

    std::vector<Game::UniversePos> first;
    std::vector<Game::UniversePos> second;
    play(first);
    play(second);

    Assert::AreEqual(first.size(), second.size(), L"the two runs produced different numbers of samples");
    for (size_t i = 0; i < first.size(); ++i)
    {
      Assert::IsTrue(IsSamePosition(first[i], second[i]), L"a position diverged between two identical runs");
    }
  }

  TEST_METHOD(AFormationOrderSpreadsShipsOut)
  {
    Game::Universe universe;
    std::vector<Game::ShipId> ships;
    for (int i = 0; i < 4; ++i)
      ships.push_back(universe.SpawnShip(Game::LocalPos(static_cast<float>(i) * 50.0f, 0.0f), 0.0f, 0));

    universe.IssueMoveOrder(ships, Game::LocalPos(0.0f, 400.0f), false, 0.0f);

    // No two ships may be sent to the same slot, or they would arrive inside each other.
    for (size_t a = 0; a < ships.size(); ++a)
    {
      for (size_t b = a + 1; b < ships.size(); ++b)
      {
        const float distance = Game::Distance(universe.Ship(ships[a]).steerTargetPos, universe.Ship(ships[b]).steerTargetPos);
        Assert::IsTrue(distance > 1.0f, L"two ships were given the same formation slot");
      }
    }
  }
};
} // namespace GameLogicTests
