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

int RunUntilIdle(Game::World& _world, Game::ShipId _id)
{
  for (int tick = 0; tick < MAX_TICKS; ++tick)
  {
    if (_world.Ship(_id).order == Game::OrderState::Idle)
      return tick;
    _world.Step();
  }
  return -1;
}
} // namespace

TEST_CLASS(MovementTests)
{
public:
  TEST_METHOD(AShipArrivesAtItsOrder)
  {
    Game::World world;
    const Game::ShipId ship = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, 0);

    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::WorldPos{0.0f, 300.0f}, false, 0.0f);

    const int ticks = RunUntilIdle(world, ship);
    Assert::IsTrue(ticks > 0, L"the ship never reached its order");

    const Game::ShipState& state = world.Ship(ship);
    const float dz = 300.0f - state.posWorld.localZ;
    const float arrival = Game::ArrivalRadiusMetres(Game::HullSpecOf(state.hullId));
    Assert::IsTrue(std::fabs(dz) <= arrival * 2.0f, L"the ship stopped short of, or past, its order");
  }

  TEST_METHOD(AShipComesToRestOnceIdle)
  {
    Game::World world;
    const Game::ShipId ship = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, 0);

    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::WorldPos{120.0f, 120.0f}, false, 0.0f);
    Assert::IsTrue(RunUntilIdle(world, ship) > 0, L"the ship never reached its order");

    for (int tick = 0; tick < 240; ++tick)
      world.Step();
    Assert::AreEqual(0.0f, world.Ship(ship).speed, 1e-4f, L"an idle ship is still moving");
  }

  TEST_METHOD(AnOrderedFacingIsHeldAfterArrival)
  {
    Game::World world;
    const Game::ShipId ship = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, 0);

    const Game::ShipId order[] = {ship};
    const float facing = XMConvertToRadians(90.0f);
    world.IssueMoveOrder(order, Game::WorldPos{0.0f, 200.0f}, true, facing);
    Assert::IsTrue(RunUntilIdle(world, ship) > 0, L"the ship never finished aligning");

    const float error = XMScalarModAngle(world.Ship(ship).headingRad - facing);
    Assert::IsTrue(std::fabs(error) < 0.05f, L"the ship settled on the wrong heading");
  }

  TEST_METHOD(TheSameOrderProducesTheSameRun)
  {
    // The determinism gate in miniature. Two worlds given identical input must stay bit-identical
    // tick for tick; if this ever fails, something in the simulation has started reading state it
    // does not own -- a clock, an address, an iteration order.
    const auto play = [](std::vector<Game::WorldPos>& _outTrack)
    {
      Game::World world;
      std::vector<Game::ShipId> ships;
      for (int i = 0; i < 5; ++i)
        ships.push_back(world.SpawnShip(Game::WorldPos{static_cast<float>(i) * 40.0f, 0.0f}, 0.0f, 0));

      world.IssueMoveOrder(ships, Game::WorldPos{200.0f, 350.0f}, true, 1.2f);
      for (int tick = 0; tick < 600; ++tick)
      {
        world.Step();
        for (const Game::ShipId id : ships)
          _outTrack.push_back(world.Ship(id).posWorld);
      }
    };

    std::vector<Game::WorldPos> first;
    std::vector<Game::WorldPos> second;
    play(first);
    play(second);

    Assert::AreEqual(first.size(), second.size(), L"the two runs produced different numbers of samples");
    for (size_t i = 0; i < first.size(); ++i)
    {
      Assert::AreEqual(first[i].localX, second[i].localX, 0.0f, L"x diverged between two identical runs");
      Assert::AreEqual(first[i].localZ, second[i].localZ, 0.0f, L"z diverged between two identical runs");
    }
  }

  TEST_METHOD(AFormationOrderSpreadsShipsOut)
  {
    Game::World world;
    std::vector<Game::ShipId> ships;
    for (int i = 0; i < 4; ++i)
      ships.push_back(world.SpawnShip(Game::WorldPos{static_cast<float>(i) * 50.0f, 0.0f}, 0.0f, 0));

    world.IssueMoveOrder(ships, Game::WorldPos{0.0f, 400.0f}, false, 0.0f);

    // No two ships may be sent to the same slot, or they would arrive inside each other.
    for (size_t a = 0; a < ships.size(); ++a)
    {
      for (size_t b = a + 1; b < ships.size(); ++b)
      {
        const float distance = Game::Distance(world.Ship(ships[a]).orderPos, world.Ship(ships[b]).orderPos);
        Assert::IsTrue(distance > 1.0f, L"two ships were given the same formation slot");
      }
    }
  }
};
} // namespace GameLogicTests
