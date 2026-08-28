#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
TEST_CLASS(WorldTests)
{
public:
  TEST_METHOD(AStaleHandleResolvesToNothing)
  {
    // The test phase 0 exists for. Swap-and-pop moves the last ship into the freed slot, so an id
    // stored across the despawn would silently name a different ship -- and in a snapshot baseline
    // that is not a crash, it is one ship interpolating into another while a player watches.
    Game::World world;
    const Game::ShipId first = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, 0);
    const Game::ShipId middle = world.SpawnShip(Game::WorldPos{100.0f, 0.0f}, 0.0f, 0);
    const Game::ShipId last = world.SpawnShip(Game::WorldPos{200.0f, 0.0f}, 0.0f, 0);

    const Game::ShipHandle middleHandle = world.HandleOf(middle);
    const Game::ShipHandle lastHandle = world.HandleOf(last);
    Assert::IsTrue(world.DespawnShip(middleHandle), L"despawning a live ship reported failure");

    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(middleHandle), L"a despawned ship's handle still resolves");
    Assert::AreEqual(2u, world.ShipCount(), L"despawn did not remove exactly one ship");

    // And the half the shorter form of the handle gets wrong: the ship swap-and-pop moved is still
    // alive, so its handle must still find it -- at its new index.
    const Game::ShipId movedTo = world.Resolve(lastHandle);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, movedTo, L"the moved ship's handle stopped resolving");
    Assert::AreEqual(200.0f, world.Ship(movedTo).posWorld.localX, 1e-4f, L"the moved ship's handle resolved to a stranger");
    Assert::AreEqual(0.0f, world.Ship(world.Resolve(world.HandleOf(first))).posWorld.localX, 1e-4f, L"the untouched ship moved");
  }

  TEST_METHOD(ADespawnedSlotIsReusedWithoutRevivingItsHandle)
  {
    Game::World world;
    const Game::ShipId only = world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, 0);
    const Game::ShipHandle stale = world.HandleOf(only);
    Assert::IsTrue(world.DespawnShip(stale), L"despawning a live ship reported failure");
    Assert::IsFalse(world.DespawnShip(stale), L"despawning the same handle twice succeeded");

    // The next spawn takes the freed slot back. The generation is what keeps the old handle dead.
    const Game::ShipId reused = world.SpawnShip(Game::WorldPos{50.0f, 0.0f}, 0.0f, 0);
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(stale), L"a reused slot revived a stale handle");
    Assert::AreEqual(reused, world.Resolve(world.HandleOf(reused)), L"the reusing ship's own handle does not resolve");
  }

  TEST_METHOD(ADefaultHandleIsNull)
  {
    Game::World world;
    world.SpawnShip(Game::WorldPos{0.0f, 0.0f}, 0.0f, 0);
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(Game::ShipHandle{}), L"a default-constructed handle resolved to a ship");
  }
};
} // namespace GameLogicTests
