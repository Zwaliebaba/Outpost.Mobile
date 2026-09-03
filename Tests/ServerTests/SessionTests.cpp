#include "pch.h"

#include "ShardSimulation.h"

#include "LoopbackTransport.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ServerTests
{
namespace
{
// A universe to serve. Built rather than loaded, because a suite that needs a .sav beside it is a
// suite that fails for a reason that is not about the code (Design/CrossShard-slice-2.md 5).
[[nodiscard]] Game::Universe BuildShard()
{
  const Game::GalaxyLayout galaxy =
    Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, Game::STARTING_GALAXY, Game::GALAXY_PINS);
  Game::Universe world;
  Game::BuildStartingUniverse(galaxy, Game::ShardId{0}, world);
  return world;
}

// One client's end of a loopback pair, and what it has been sent. This is the substitution the whole
// seam exists to make: the shard is handed a Neuron::Transport& and cannot tell that the far end is
// in this process rather than on a socket (ADR 0008).
struct Client
{
  Neuron::LoopbackTransport server;
  Neuron::LoopbackTransport client;
  std::size_t bytes = 0;

  void Connect()
  {
    Neuron::LoopbackTransport::Connect(server, client, {});
  }

  void Drain(std::uint64_t _tick)
  {
    client.AdvanceTo(_tick);
    client.Poll();
    std::uint8_t buffer[16384];
    for (;;)
    {
      const std::uint32_t got = client.Receive(buffer, sizeof(buffer));
      if (got == 0)
        break;
      bytes += got;
    }
    for (;;)
    {
      const std::uint32_t got = client.ReceiveReliable(buffer, sizeof(buffer));
      if (got == 0)
        break;
      bytes += got;
    }
  }
};
} // namespace

TEST_CLASS(SessionTests)
{
public:
  TEST_METHOD(AShardWithNoSessionTicksAsThoughThereWereNoPublisher)
  {
    // Slice 1's shard and slice 2's, side by side, for three thousand ticks. A publisher with no
    // subscribers must cost nothing but a branch, and byte equality of the state is what says so --
    // this is the row that would catch a session layer that had started to affect the simulation.
    Game::Universe served = BuildShard();
    Game::Universe control = BuildShard();
    Shard::ShardSimulation simulation(served);
    for (int tick = 0; tick < 3000; ++tick)
    {
      simulation.Step();
      (void)control.DrainInbox();
      control.Step();
    }

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    Game::WriteUniverseState(served, a);
    Game::WriteUniverseState(control, b);
    Assert::IsTrue(a == b, L"a shard with no session no longer ticks the way one without a publisher does");
  }

  TEST_METHOD(TheFirstSessionIsTakenAndTheSecondIsRefused)
  {
    // There is no login, so a second session would be OWNER_LOCAL as well and two clients would give
    // orders to one fleet. The refusal is the feature and not a limit of the listener
    // (Design/ShardServer.md 5.4).
    Game::Universe world = BuildShard();
    Shard::ShardSimulation simulation(world);
    Game::ServerConfig config;
    Client first;
    Client second;
    first.Connect();
    second.Connect();

    Assert::IsTrue(simulation.OpenSession(first.server, config), L"the first session was not taken");
    Assert::AreEqual(1u, simulation.SessionCount(), L"the shard does not say it has one session");
    Assert::IsFalse(simulation.OpenSession(second.server, config), L"the second session was taken");
    Assert::AreEqual(1u, simulation.SessionCount(), L"the refusal disturbed the first session");
    Assert::IsTrue(simulation.HasSessionOn(&first.server), L"the shard does not know which wire its session is on");
    Assert::IsFalse(simulation.HasSessionOn(&second.server), L"the shard thinks the refused connection is a session");
  }

  TEST_METHOD(ASessionReceivesAndARefusedConnectionDoesNot)
  {
    Game::Universe world = BuildShard();
    Shard::ShardSimulation simulation(world);
    Game::ServerConfig config;
    Client served;
    Client refused;
    served.Connect();
    refused.Connect();
    Assert::IsTrue(simulation.OpenSession(served.server, config), L"the session was not taken");
    Assert::IsFalse(simulation.OpenSession(refused.server, config), L"the second session was taken");

    for (int tick = 0; tick < 2000; ++tick)
    {
      simulation.Step();
      served.Drain(static_cast<std::uint64_t>(tick));
      refused.Drain(static_cast<std::uint64_t>(tick));
    }

    Assert::IsTrue(served.bytes > 0, L"the session was sent nothing at all");
    Assert::AreEqual(std::size_t{0}, refused.bytes, L"the refused connection was sent something");
  }

  TEST_METHOD(ASessionThatLeavesIsRemovedAndTheShardGoesOnTicking)
  {
    // The failure this is most likely to have: a server that dies, or stops, when its client leaves.
    Game::Universe world = BuildShard();
    Shard::ShardSimulation simulation(world);
    Game::ServerConfig config;
    Client leaving;
    leaving.Connect();
    Assert::IsTrue(simulation.OpenSession(leaving.server, config), L"the session was not taken");

    const std::uint64_t at = world.Tick();
    Assert::IsTrue(simulation.CloseSession(&leaving.server), L"closing the session did not remove it");
    Assert::AreEqual(0u, simulation.SessionCount(), L"the shard still thinks it has a session");
    Assert::IsFalse(simulation.CloseSession(&leaving.server), L"closing it twice claimed to remove a second one");

    for (int tick = 0; tick < 500; ++tick)
      simulation.Step();
    Assert::AreEqual(at + 500, world.Tick(), L"the shard stopped ticking when its client left");
  }

  TEST_METHOD(TheSlotIsReusedRatherThanSpent)
  {
    // A shard that served one client and then refuses every later one has a leak rather than a
    // ceiling, and the two look identical from outside until somebody reconnects.
    Game::Universe world = BuildShard();
    Shard::ShardSimulation simulation(world);
    Game::ServerConfig config;
    Client first;
    Client second;
    first.Connect();
    second.Connect();

    Assert::IsTrue(simulation.OpenSession(first.server, config), L"the first session was not taken");
    Assert::IsTrue(simulation.CloseSession(&first.server), L"the first session did not close");
    Assert::IsTrue(simulation.OpenSession(second.server, config), L"a client arriving after the first left was refused");
    Assert::AreEqual(1u, simulation.SessionCount(), L"the shard does not say it has one session");
  }
};
} // namespace ServerTests
