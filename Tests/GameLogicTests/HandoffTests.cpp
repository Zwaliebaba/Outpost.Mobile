#include "pch.h"

#include <algorithm>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// A partitioned galaxy and the universes it was cut into. Built through BuildStartingGalaxy rather
// than by hand, because the thing under test is a gate whose destination is in another shard, and
// only the real layout produces one (Design/CrossShard-slice-2.md 5).
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

// The PLAYER's ships across every shard, and deliberately not every ship.
//
// A global census is not conserved over these scenarios and should not be: flying a fleet across the
// home system takes it past the hostile base, a fight happens, and something dies -- measured, at
// tick 1842, a Vandal Interceptor (Design/CrossShard-slice-2.md 8). Combat is not this slice's
// business. What a handoff must never do is lose or duplicate the ships it is carrying, and those
// are the player's.
[[nodiscard]] std::uint32_t PlayerShips(const Galaxy& _world)
{
  std::uint32_t total = 0;
  for (const Game::Universe& shard : _world.shards)
  {
    for (const Game::ShipState& ship : shard.Ships())
      total += (ship.factionId == Game::FACTION_PLAYER) ? 1u : 0u;
  }
  return total;
}

// Which shard the starting fleet was built into, since the partition decides that and no test should
// assume it.
[[nodiscard]] std::uint32_t ShardHoldingTheFleet(const Galaxy& _world)
{
  for (std::uint32_t at = 0; at < _world.shards.size(); ++at)
  {
    if (_world.shards[at].FleetInSlot(Game::OWNER_LOCAL, 0) != Game::Universe::INVALID_FLEET_ID)
      return at;
  }
  return static_cast<std::uint32_t>(_world.shards.size());
}

// A gate in _shard whose destination is in another shard: the one thing this whole slice is about.
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

void OrderThroughGate(Game::Universe& _shard, Game::ShipId _gate)
{
  Game::Universe::FleetCommand command;
  command.kind = Game::FleetOrderKind::Jump;
  command.gate = _gate;
  Assert::IsTrue(_shard.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, command) ==
                   Game::Universe::FleetOrderResult::Ordered,
                 L"the jump order was refused");
}

// Steps every shard until the departing one has something in its outbox, or gives up. The fleet has
// to fly to the gate first, which is thousands of ticks at cruise.
[[nodiscard]] bool StepUntilHandedOff(Galaxy& _world, std::uint32_t _departing, int _maxTicks = 30000)
{
  for (int tick = 0; tick < _maxTicks; ++tick)
  {
    if (!_world.shards[_departing].Outbox().empty())
      return true;
    for (Game::Universe& shard : _world.shards)
      shard.Step();
  }
  return !_world.shards[_departing].Outbox().empty();
}

[[nodiscard]] std::vector<Game::Universe::Handoff> TakeOutbox(const Game::Universe& _shard)
{
  const std::span<const Game::Universe::Handoff> outbox = _shard.Outbox();
  return std::vector<Game::Universe::Handoff>(outbox.begin(), outbox.end());
}

[[nodiscard]] std::uint32_t ShardOfDestination(const Game::Universe& _shard, Game::ShipId _gate)
{
  return Game::EntityShardOf(_shard.GateOf(_shard.GateAt(_gate)).destination);
}
} // namespace

TEST_CLASS(HandoffTests)
{
public:
  TEST_METHOD(AFleetCrossingOutOfItsShardIsInTheOutboxAndNowhereElse)
  {
    // The design's own honest sentence, as a census: a fleet mid-handoff is in neither universe
    // (Design/CrossShard.md 4). If this row ever fails by ships going UP, the apply duplicated; by
    // going down, the despawn lost them.
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t before = PlayerShips(world);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    Assert::IsTrue(departing < world.shards.size(), L"no shard holds the starting fleet");

    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, gate, L"the fleet's shard has no gate leading out of it");
    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");

    const std::size_t inFlight = world.shards[departing].Outbox().size();
    Assert::AreEqual(std::size_t{3}, inFlight, L"the whole fleet should be in the outbox, whole or not at all");
    Assert::AreEqual(before, PlayerShips(world) + static_cast<std::uint32_t>(inFlight),
                     L"a ship was lost or duplicated while it was in flight");
  }

  TEST_METHOD(TheFleetArrivesWholeInItsOwnSlot)
  {
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t before = PlayerShips(world);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    const std::uint32_t arriving = ShardOfDestination(world.shards[departing], gate);

    // What it left with, taken before it goes: the damage rides across and the intent does not.
    const Game::Universe::FleetId was = world.shards[departing].FleetInSlot(Game::OWNER_LOCAL, 0);
    const std::uint32_t wasMembers = world.shards[departing].FleetOf(was).memberCount;

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");

    for (const Game::Universe::Handoff& handoff : TakeOutbox(world.shards[departing]))
      world.shards[arriving].DeliverHandoff(handoff);
    Assert::AreEqual(wasMembers, world.shards[arriving].DrainInbox(), L"the drain did not spawn every member");

    const Game::Universe::FleetId now = world.shards[arriving].FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, now, L"the fleet was not re-formed on the far side");
    Assert::AreEqual(wasMembers, world.shards[arriving].FleetOf(now).memberCount, L"the fleet did not arrive whole");
    Assert::AreEqual(Game::OWNER_LOCAL, world.shards[arriving].FleetOf(now).owner, L"the fleet arrived under another owner");

    // Intent is re-derived, which for an arriving fleet means it has none (ADR 0056).
    Assert::IsTrue(world.shards[arriving].FleetOf(now).orderKind == Game::FleetOrderKind::Idle, L"the order crossed with the fleet");
    Assert::AreEqual(std::uint32_t{0}, world.shards[arriving].FleetOf(now).alertTicks, L"the alert crossed with the fleet");
    Assert::AreEqual(before, PlayerShips(world), L"the census did not survive the crossing");
  }

  TEST_METHOD(ApplyingAHandoffTwiceIsApplyingItOnce)
  {
    // The idempotence the whole scheme rests on. At-least-once delivery is only safe because
    // SpawnShipAs refuses an entity this universe already holds -- this is the first caller that
    // depends on it and therefore the row that proves it (Design/CrossShard.md 4).
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    const std::uint32_t arriving = ShardOfDestination(world.shards[departing], gate);

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");
    const std::vector<Game::Universe::Handoff> batch = TakeOutbox(world.shards[departing]);

    for (const Game::Universe::Handoff& handoff : batch)
      world.shards[arriving].DeliverHandoff(handoff);
    const std::uint32_t first = world.shards[arriving].DrainInbox();
    Assert::IsTrue(first > 0, L"the first drain spawned nothing");

    std::vector<std::uint8_t> once;
    Game::WriteUniverseState(world.shards[arriving], once);

    // Again, and again after that: any number of times is once.
    for (int round = 0; round < 2; ++round)
    {
      for (const Game::Universe::Handoff& handoff : batch)
        world.shards[arriving].DeliverHandoff(handoff);
      Assert::AreEqual(std::uint32_t{0}, world.shards[arriving].DrainInbox(), L"a replayed handoff spawned a second ship");
    }

    std::vector<std::uint8_t> twice;
    Game::WriteUniverseState(world.shards[arriving], twice);
    Assert::IsTrue(once == twice, L"replaying a handoff changed the universe");

    const Game::Universe::FleetId fleet = world.shards[arriving].FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreEqual(first, world.shards[arriving].FleetOf(fleet).memberCount, L"a replayed handoff grew the fleet");
  }

  TEST_METHOD(TheOrderABatchIsDeliveredInDoesNotChangeTheUniverse)
  {
    // §9's first "what would make this design wrong": a handoff that is not deterministic is not
    // compatible with the replay gates. The inbox is drained in ENTITY order for exactly this.
    Galaxy forwards = BuildGalaxy(4);
    Galaxy backwards = BuildGalaxy(4);
    const std::uint32_t departing = ShardHoldingTheFleet(forwards);
    const Game::ShipId gate = GateLeadingOut(forwards.shards[departing]);
    const std::uint32_t arriving = ShardOfDestination(forwards.shards[departing], gate);

    for (Galaxy* world : {&forwards, &backwards})
    {
      OrderThroughGate(world->shards[departing], gate);
      Assert::IsTrue(StepUntilHandedOff(*world, departing), L"the fleet never reached the gate");
    }

    std::vector<Game::Universe::Handoff> ordered = TakeOutbox(forwards.shards[departing]);
    std::vector<Game::Universe::Handoff> reversed = TakeOutbox(backwards.shards[departing]);
    std::reverse(reversed.begin(), reversed.end());

    for (const Game::Universe::Handoff& handoff : ordered)
      forwards.shards[arriving].DeliverHandoff(handoff);
    for (const Game::Universe::Handoff& handoff : reversed)
      backwards.shards[arriving].DeliverHandoff(handoff);
    (void)forwards.shards[arriving].DrainInbox();
    (void)backwards.shards[arriving].DrainInbox();

    // Not just at the drain -- six hundred ticks later, so a difference in member order or arrival
    // position has had time to become a different universe.
    for (int tick = 0; tick < 600; ++tick)
    {
      forwards.shards[arriving].Step();
      backwards.shards[arriving].Step();
    }

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    Game::WriteUniverseState(forwards.shards[arriving], a);
    Game::WriteUniverseState(backwards.shards[arriving], b);
    Assert::IsTrue(a == b, L"the delivery order changed the universe");
  }

  TEST_METHOD(AHandoffNamingAGateThisShardDoesNotHoldStaysQueued)
  {
    // Held rather than dropped: a handoff in flight when a layout changed arrives when its gate does,
    // and a fleet is never deleted by being delivered to the wrong place.
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");

    // Delivered to the shard it came FROM, which holds the near gate and not the far one.
    const std::uint32_t before = world.shards[departing].ShipCount();
    for (const Game::Universe::Handoff& handoff : TakeOutbox(world.shards[departing]))
      world.shards[departing].DeliverHandoff(handoff);
    Assert::AreEqual(std::uint32_t{0}, world.shards[departing].DrainInbox(), L"a handoff was applied against a gate this shard lacks");
    Assert::AreEqual(before, world.shards[departing].ShipCount(), L"a refused handoff changed the universe");
  }

  TEST_METHOD(ARepeatedDeliveryDoesNotQueueTheSameShipTwice)
  {
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    const std::uint32_t arriving = ShardOfDestination(world.shards[departing], gate);

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");
    const std::vector<Game::Universe::Handoff> batch = TakeOutbox(world.shards[departing]);

    // Delivered three times before a single drain: at-least-once delivery, with the re-sends landing
    // between the deliver and the drain rather than after it.
    for (int round = 0; round < 3; ++round)
    {
      for (const Game::Universe::Handoff& handoff : batch)
        world.shards[arriving].DeliverHandoff(handoff);
    }
    Assert::AreEqual(static_cast<std::uint32_t>(batch.size()), world.shards[arriving].DrainInbox(),
                     L"a repeated delivery queued the same ship more than once");
  }

  TEST_METHOD(AnAcknowledgementClearsTheOutboxAndKeepsItsOrder)
  {
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");
    const std::vector<Game::Universe::Handoff> batch = TakeOutbox(world.shards[departing]);
    Assert::IsTrue(batch.size() >= 2, L"this row needs more than one entry to say anything about order");

    // The middle one acknowledged: what is left must be the others, in the order they were written,
    // or a re-send would go out in a different order every time.
    const std::uint64_t middle = batch[1].sequence;
    world.shards[departing].AcknowledgeHandoffs(std::span<const std::uint64_t>(&middle, 1));

    const std::vector<Game::Universe::Handoff> left = TakeOutbox(world.shards[departing]);
    Assert::AreEqual(batch.size() - 1, left.size(), L"the acknowledgement cleared the wrong number of entries");
    Assert::AreEqual(batch[0].sequence, left[0].sequence, L"the outbox lost its order under an acknowledgement");
    Assert::AreEqual(batch[2].sequence, left[1].sequence, L"the outbox lost its order under an acknowledgement");

    // Every sequence is distinct, which is what an acknowledgement addresses one entry by.
    for (std::size_t at = 1; at < batch.size(); ++at)
      Assert::AreNotEqual(batch[at - 1].sequence, batch[at].sequence, L"two handoffs share a sequence number");
  }

  TEST_METHOD(AOneShardGalaxyHandsOffNothing)
  {
    // The no-regression claim in its simplest form: with nothing partitioned, no gate leads out, the
    // outbox is never written, and a local jump is the path it always was.
    Galaxy world = BuildGalaxy(1);
    Assert::AreEqual(Game::INVALID_SHIP_ID, GateLeadingOut(world.shards[0]), L"a one-shard galaxy has a gate leading out of it");

    const std::uint32_t departing = ShardHoldingTheFleet(world);
    Game::ShipId gate = Game::INVALID_SHIP_ID;
    for (std::uint32_t at = 0; at < world.shards[departing].GateCount() && gate == Game::INVALID_SHIP_ID; ++at)
    {
      const Game::ShipId structure = world.shards[departing].Resolve(world.shards[departing].GateOf(at).structure);
      if (structure != Game::INVALID_SHIP_ID &&
          world.shards[departing].ResolveEntity(world.shards[departing].GateOf(at).destination) != Game::INVALID_SHIP_ID)
        gate = structure;
    }
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, gate, L"the shipped galaxy has no local gate");

    const std::uint32_t before = PlayerShips(world);
    const Game::UniversePos wasAt =
      world.shards[0]
        .Ships()[world.shards[0].Resolve(world.shards[0].FleetOf(world.shards[0].FleetInSlot(Game::OWNER_LOCAL, 0)).members[0])]
        .posUniverse;
    OrderThroughGate(world.shards[departing], gate);
    for (int tick = 0; tick < 30000; ++tick)
      world.shards[0].Step();

    Assert::IsTrue(world.shards[0].Outbox().empty(), L"a local jump wrote to the outbox");
    Assert::AreEqual(before, PlayerShips(world), L"a local jump lost or duplicated one of the player's ships");

    // And it actually crossed, rather than sitting at the gate for thirty thousand ticks: the fleet
    // is alive, whole, and somewhere else.
    const Game::Universe::FleetId fleet = world.shards[0].FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, fleet, L"the fleet retired on a local jump");
    Assert::AreEqual(std::uint32_t{3}, world.shards[0].FleetOf(fleet).memberCount, L"the fleet did not cross whole");
    const Game::UniversePos nowAt = world.shards[0].Ships()[world.shards[0].Resolve(world.shards[0].FleetOf(fleet).members[0])].posUniverse;
    Assert::IsTrue(Game::Distance(wasAt, nowAt) > 1000.0f, L"the fleet never went anywhere");
  }
};
} // namespace GameLogicTests
