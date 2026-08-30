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
    const Game::ShipId first = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, 0);
    const Game::ShipId middle = world.SpawnShip(Game::LocalPos(100.0f, 0.0f), 0.0f, 0);
    const Game::ShipId last = world.SpawnShip(Game::LocalPos(200.0f, 0.0f), 0.0f, 0);

    const Game::ShipHandle middleHandle = world.HandleOf(middle);
    const Game::ShipHandle lastHandle = world.HandleOf(last);
    Assert::IsTrue(world.DespawnShip(middleHandle), L"despawning a live ship reported failure");

    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(middleHandle), L"a despawned ship's handle still resolves");
    Assert::AreEqual(2u, world.ShipCount(), L"despawn did not remove exactly one ship");

    // And the half the shorter form of the handle gets wrong: the ship swap-and-pop moved is still
    // alive, so its handle must still find it -- at its new index.
    const Game::ShipId movedTo = world.Resolve(lastHandle);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, movedTo, L"the moved ship's handle stopped resolving");
    Assert::AreEqual(200.0f, WorldX(world.Ship(movedTo).posWorld), 1e-4f, L"the moved ship's handle resolved to a stranger");
    Assert::AreEqual(0.0f, WorldX(world.Ship(world.Resolve(world.HandleOf(first))).posWorld), 1e-4f, L"the untouched ship moved");
  }

  TEST_METHOD(ADespawnedSlotIsReusedWithoutRevivingItsHandle)
  {
    Game::World world;
    const Game::ShipId only = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, 0);
    const Game::ShipHandle stale = world.HandleOf(only);
    Assert::IsTrue(world.DespawnShip(stale), L"despawning a live ship reported failure");
    Assert::IsFalse(world.DespawnShip(stale), L"despawning the same handle twice succeeded");

    // The next spawn takes the freed slot back. The generation is what keeps the old handle dead.
    const Game::ShipId reused = world.SpawnShip(Game::LocalPos(50.0f, 0.0f), 0.0f, 0);
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(stale), L"a reused slot revived a stale handle");
    Assert::AreEqual(reused, world.Resolve(world.HandleOf(reused)), L"the reusing ship's own handle does not resolve");
  }

  TEST_METHOD(ArrayOrderCannotChangeTheAnswer)
  {
    // The test that protects the MMO property. Every pass reads a start-of-tick snapshot and writes
    // its corrections to scratch, so the same fleet spawned in a different array order must produce
    // the same run -- and the day someone writes a pass that mutates in place, this is what catches
    // it rather than a replay failing months later (Design/Archive/Collision.md 16).
    //
    // Positions are jittered rather than laid out on a lattice, and deliberately so. ShipId is the
    // documented tie-break on the neighbour sort, and a permutation renames every ship, so a fleet
    // holding exact proximity ties legitimately resolves them the other way round. Identity is an
    // input. What must not be an input is where in the array a ship happens to sit.
    constexpr int SHIPS = 12;
    const auto play = [](bool _reversed, std::vector<Game::WorldPos>& _outTrack)
    {
      std::uint32_t noise = 7u;
      const auto jitter = [&noise]
      {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        return static_cast<float>(noise >> 8) / static_cast<float>(1u << 24);
      };
      // Drawn before the spawn loop so both orders lay the fleet out identically.
      std::vector<Game::WorldPos> layout;
      for (int i = 0; i < SHIPS; ++i)
        layout.push_back(Game::LocalPos((jitter() - 0.5f) * 40.0f, (jitter() - 0.5f) * 40.0f));

      Game::World world;
      std::vector<Game::ShipId> byPosition(SHIPS, Game::INVALID_SHIP_ID);
      for (int spawn = 0; spawn < SHIPS; ++spawn)
      {
        const int at = _reversed ? (SHIPS - 1 - spawn) : spawn;
        // Tight enough that everything is in contact with everything: separation is the pass most
        // likely to be order-dependent, so it has to be the one under load.
        byPosition[static_cast<size_t>(at)] = world.SpawnShip(
          layout[static_cast<size_t>(at)], 0.0f, static_cast<std::uint32_t>(at % 2 ? Game::HullId::Corvette : Game::HullId::Interceptor));
      }
      world.IssueMoveOrder(byPosition, Game::LocalPos(300.0f, 300.0f), false, 0.0f);
      for (int tick = 0; tick < 400; ++tick)
      {
        world.Step();
        for (const Game::ShipId id : byPosition)
          _outTrack.push_back(world.Ship(id).posWorld);
      }
    };

    std::vector<Game::WorldPos> forward;
    std::vector<Game::WorldPos> reversed;
    play(false, forward);
    play(true, reversed);

    Assert::AreEqual(forward.size(), reversed.size(), L"the two orders produced different numbers of samples");
    for (size_t i = 0; i < forward.size(); ++i)
    {
      Assert::IsTrue(IsSamePosition(forward[i], reversed[i]), L"a position depends on the order ships were spawned in");
    }
  }

  TEST_METHOD(TheDespawnLogHoldsUntilDrained)
  {
    // The log is the publisher's, not the tick's. An interest update happens once every six ticks,
    // so a log cleared by Step would lose every death that did not fall on the tick an update
    // happened to land on (Design/Archive/Hostiles.md 4.4).
    Game::World world;
    std::vector<Game::ShipHandle> doomed;
    for (int at = 0; at < 3; ++at)
      doomed.push_back(world.HandleOf(world.SpawnShip(Game::LocalPos(static_cast<float>(at) * 100.0f, 0.0f), 0.0f, 0)));
    Assert::IsTrue(world.DespawnLog().empty(), L"a world that has killed nothing logged a death");

    for (const Game::ShipHandle handle : doomed)
    {
      Assert::IsTrue(world.DespawnShip(handle), L"the despawn failed");
      world.Step();
    }

    Assert::AreEqual(doomed.size(), world.DespawnLog().size(), L"three despawns across three ticks did not all survive to be read");
    for (std::size_t at = 0; at < doomed.size(); ++at)
      Assert::IsTrue(world.DespawnLog()[at] == doomed[at], L"the log is not in despawn order");

    // A despawn that failed is not a death, and a Step is not a drain.
    Assert::IsFalse(world.DespawnShip(doomed[0]), L"despawning the same handle twice succeeded");
    Assert::AreEqual(doomed.size(), world.DespawnLog().size(), L"a failed despawn was logged");

    world.ClearDespawnLog();
    Assert::IsTrue(world.DespawnLog().empty(), L"the log was not drained");
  }

  TEST_METHOD(ADefaultHandleIsNull)
  {
    Game::World world;
    world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, 0);
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(Game::ShipHandle{}), L"a default-constructed handle resolved to a ship");
  }
};
} // namespace GameLogicTests
