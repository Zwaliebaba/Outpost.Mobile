#include "pch.h"

#include "ShardLinks.h"

#include "LoopbackTransport.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ServerTests
{
namespace
{
struct Galaxy
{
  Game::GalaxyDesc desc;
  Game::GalaxyLayout layout;
  std::vector<Game::Universe> shards;
};

[[nodiscard]] Galaxy BuildGalaxy(std::uint32_t _shardCount)
{
  Galaxy world;
  world.desc = Game::STARTING_GALAXY;
  world.desc.shardCount = _shardCount;
  world.layout = Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, world.desc, Game::GALAXY_PINS);
  world.shards.resize(_shardCount);
  Game::BuildStartingGalaxy(world.layout, world.desc, world.shards);
  return world;
}

[[nodiscard]] std::uint32_t ShardHoldingTheFleet(const Galaxy& _world)
{
  for (std::uint32_t at = 0; at < _world.shards.size(); ++at)
  {
    if (_world.shards[at].FleetInSlot(Game::OWNER_LOCAL, 0) != Game::Universe::INVALID_FLEET_ID)
      return at;
  }
  return 0;
}

[[nodiscard]] Game::ShipId GateLeadingOut(const Game::Universe& _shard)
{
  for (std::uint32_t at = 0; at < _shard.GateCount(); ++at)
  {
    const Game::EntityId destination = _shard.GateOf(at).destination;
    if (destination == Game::INVALID_ENTITY_ID)
      continue;
    if (Game::EntityShardOf(destination) != static_cast<std::uint32_t>(_shard.Shard()))
      return _shard.Resolve(_shard.GateOf(at).structure);
  }
  return Game::INVALID_SHIP_ID;
}

// The player's ships across every shard, and deliberately NOT the outbox with them.
//
// Whether an outbox entry counts depends entirely on when you ask, and both directions of that trap
// have now been walked into. Between a fleet leaving and its handoff being delivered, the ships are
// in no universe and the entry is the only record of them -- so CrossShard-slice-4.md's row adds the
// held count, or the census reads low. AFTER delivery the ships exist on the arriving side and the
// entry is a duplicate held only until it is acknowledged (which is what at-least-once means), so
// adding it reads HIGH. Measured on this galaxy: three ships and three held entries at the moment of
// arrival, which is six if you add them and three if you do not.
//
// This row asks after delivery, so it does not add them.
[[nodiscard]] std::uint32_t PlayerShips(const Galaxy& _world)
{
  std::uint32_t held = 0;
  for (const Game::Universe& shard : _world.shards)
  {
    for (Game::ShipId id = 0; id < shard.ShipCount(); ++id)
      held += (shard.Ships()[id].factionId == Game::FACTION_PLAYER) ? 1u : 0u;
  }
  return held;
}
} // namespace

TEST_CLASS(LinkTests)
{
public:
  TEST_METHOD(LinksAreOnePerNeighbourAndAShardAloneHasNone)
  {
    for (std::uint32_t shardCount = 1; shardCount <= 4; ++shardCount)
    {
      Galaxy world = BuildGalaxy(shardCount);
      for (std::uint32_t at = 0; at < shardCount; ++at)
      {
        Shard::ShardLinks links;
        const std::uint32_t opened = links.Open(world.shards[at]);
        Assert::AreEqual(opened, links.Count(), L"Open reported a count it did not build");

        std::vector<Game::ShardId> expected(world.shards[at].NeighbourShards({}));
        (void)world.shards[at].NeighbourShards(expected);
        Assert::AreEqual(static_cast<std::uint32_t>(expected.size()), links.Count(), L"there is not one link per neighbouring shard");
        for (std::uint32_t link = 0; link < links.Count(); ++link)
          Assert::AreEqual(expected[link], links.PeerAt(link), L"a link names a shard that is not a neighbour");
      }
      if (shardCount == 1)
      {
        Shard::ShardLinks alone;
        Assert::AreEqual(0u, alone.Open(world.shards[0]), L"a shard that borders nothing built a link");
      }
    }
  }

  TEST_METHOD(ALinkWithNoTransportPumpsNothingAndIsNotAnError)
  {
    // What lets this slice land without an address anywhere in it: slice 4 is the one that dials a
    // neighbour, and until it does, a link is built and silent.
    Galaxy world = BuildGalaxy(2);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    Shard::ShardLinks links;
    Assert::AreEqual(1u, links.Open(world.shards[departing]), L"a shard of a two-shard galaxy has other than one neighbour");
    Assert::IsNull(links.TransportFor(links.PeerAt(0)), L"a link was born with a transport");

    const std::uint64_t before = world.shards[departing].Tick();
    for (int pass = 0; pass < 100; ++pass)
    {
      world.shards[departing].Step();
      links.Pump(world.shards[departing]);
    }
    Assert::AreEqual(before + 100, world.shards[departing].Tick(), L"pumping links that have no transport disturbed the tick");
  }

  TEST_METHOD(AttachNamesAShardThisOneBorders)
  {
    Galaxy world = BuildGalaxy(3);
    Game::Universe& middle = world.shards[1];
    Shard::ShardLinks links;
    Assert::AreEqual(2u, links.Open(middle), L"the middle shard of three does not border two others");

    Neuron::LoopbackTransport here;
    Neuron::LoopbackTransport there;
    Neuron::LoopbackTransport::Connect(here, there, {});
    Assert::IsTrue(links.Attach(links.PeerAt(0), &here), L"attaching to a neighbour was refused");
    Assert::IsTrue(links.TransportFor(links.PeerAt(0)) == &here, L"the link did not take the transport");
    Assert::IsNull(links.TransportFor(links.PeerAt(1)), L"attaching one link gave another a transport");

    // A shard this one does not border: a deployment pointed at the wrong neighbour, refused rather
    // than silently attached to nothing.
    Assert::IsFalse(links.Attach(Game::ShardId{9}, &here), L"attaching to a shard this one does not border was accepted");
  }

  TEST_METHOD(ARefusedSaveMakesNothingDurable)
  {
    // ADR 0066's rule, executed rather than read. ShardLinks is handed WHETHER the write happened
    // rather than trusted to be called only when it did, which is what makes this row possible: a
    // refused write needs a filesystem that refuses, and this suite has none.
    Galaxy world = BuildGalaxy(3);
    Shard::ShardLinks links;
    Assert::AreEqual(2u, links.Open(world.shards[1]), L"the middle shard of three does not border two others");
    Assert::AreEqual(std::uint64_t{0}, links.DurableTick(), L"a link was durable through something before any save");

    // A save that was REFUSED makes nothing durable. This is the row ADR 0066 exists for, and until
    // ShardLinks took the flag it could not be written at all: the rule lived in ShardApp's control
    // flow, a refused write needs a filesystem that refuses, and the guard was a comment.
    links.NoteSaved(false, 4242);
    Assert::AreEqual(std::uint64_t{0}, links.DurableTick(), L"a refused save made something durable");

    links.NoteSaved(true, 4242);
    Assert::AreEqual(std::uint64_t{4242}, links.DurableTick(), L"a save that happened made nothing durable");

    // And a later refusal does not walk it back either: what is on disk stays on disk.
    links.NoteSaved(false, 9999);
    Assert::AreEqual(std::uint64_t{4242}, links.DurableTick(), L"a refused save moved what was already durable");
  }

  TEST_METHOD(AFleetCrossesThroughTheServersOwnLinksAndNothingIsLost)
  {
    // The whole slice, end to end, through the wiring a shard actually uses rather than through a
    // test's own plumbing: two shards, each with its links opened off its own gates, attached to the
    // two ends of one wire.
    Galaxy world = BuildGalaxy(2);
    const std::uint32_t before = PlayerShips(world);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const std::uint32_t arriving = (departing == 0) ? 1u : 0u;

    Shard::ShardLinks out;
    Shard::ShardLinks in;
    Assert::AreEqual(1u, out.Open(world.shards[departing]), L"the departing shard has other than one neighbour");
    Assert::AreEqual(1u, in.Open(world.shards[arriving]), L"the arriving shard has other than one neighbour");

    Neuron::LoopbackTransport here;
    Neuron::LoopbackTransport there;
    Neuron::LoopbackTransport::Connect(here, there, {});
    Assert::IsTrue(out.Attach(static_cast<Game::ShardId>(arriving), &here), L"the departing link would not attach");
    Assert::IsTrue(in.Attach(static_cast<Game::ShardId>(departing), &there), L"the arriving link would not attach");

    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, gate, L"the departing shard has no gate leading out");
    Game::Universe::FleetCommand command;
    command.kind = Game::FleetOrderKind::Jump;
    command.gate = gate;
    Assert::IsTrue(world.shards[departing].IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, command) ==
                     Game::Universe::FleetOrderResult::Ordered,
                   L"the jump order was refused");

    bool arrivedThere = false;
    for (int pass = 0; pass < 40000 && !arrivedThere; ++pass)
    {
      for (Game::Universe& shard : world.shards)
        shard.Step();
      here.AdvanceTo(world.shards[departing].Tick());
      there.AdvanceTo(world.shards[departing].Tick());
      here.Poll();
      there.Poll();

      // Everything the arriving shard has is on disk in this test, so it may acknowledge what it
      // receives -- which is what a real deployment's save cadence buys it (ADR 0066).
      in.NoteSaved(true, world.shards[arriving].Tick() + 100000);
      out.Pump(world.shards[departing]);
      in.Pump(world.shards[arriving]);
      arrivedThere = world.shards[arriving].DrainInbox() > 0;
    }

    Assert::IsTrue(arrivedThere, L"the fleet never crossed through the server's own links");
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, world.shards[arriving].FleetInSlot(Game::OWNER_LOCAL, 0),
                        L"the fleet did not re-form on the far side");
    Assert::AreEqual(before, PlayerShips(world), L"a ship was lost or duplicated crossing through the links");
    Assert::IsFalse(world.shards[departing].Outbox().empty(),
                    L"the departing outbox was already empty at arrival, so this row is not watching the window it thinks it is");

    // And the outbox empties, which is the acknowledgement coming back the other way.
    for (int pass = 0; pass < 400 && !world.shards[departing].Outbox().empty(); ++pass)
    {
      world.shards[departing].Step();
      world.shards[arriving].Step();
      here.AdvanceTo(world.shards[departing].Tick());
      there.AdvanceTo(world.shards[departing].Tick());
      here.Poll();
      there.Poll();
      in.NoteSaved(true, world.shards[arriving].Tick() + 100000);
      in.Pump(world.shards[arriving]);
      out.Pump(world.shards[departing]);
    }
    Assert::IsTrue(world.shards[departing].Outbox().empty(), L"the departing outbox was never acknowledged");
    Assert::AreEqual(before, PlayerShips(world), L"a ship was lost or duplicated once the acknowledgement came back");
  }
};
} // namespace ServerTests
