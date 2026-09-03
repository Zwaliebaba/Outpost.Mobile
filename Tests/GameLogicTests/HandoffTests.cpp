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

// Two shards wired to each other, each with its own end of the link. The pattern QUIC across
// 127.0.0.1 already set: the transport is real, the two ends are in one process, and nothing about
// the code under test knows the difference (ADR 0021, 0028).
struct Wire
{
  Neuron::LoopbackTransport departing;
  Neuron::LoopbackTransport arriving;
  Game::ShardLink departingLink;
  Game::ShardLink arrivingLink;

  void Connect(const Neuron::LoopbackTransport::Desc& _desc)
  {
    Neuron::LoopbackTransport::Connect(departing, arriving, _desc);
  }

  void Poll(std::uint64_t _tick)
  {
    departing.AdvanceTo(_tick);
    arriving.AdvanceTo(_tick);
    departing.Poll();
    arriving.Poll();
  }
};

// Runs the link until the fleet has arrived or the budget is spent. Returns the tick it stopped on,
// so a row can say how long a crossing actually took rather than only that it happened.
[[nodiscard]] int PumpUntilArrived(Wire& _wire, Galaxy& _world, std::uint32_t _departing, std::uint32_t _arriving, int _maxPumps = 4000)
{
  for (int pump = 0; pump < _maxPumps; ++pump)
  {
    _world.shards[_departing].Step();
    _world.shards[_arriving].Step();
    _wire.Poll(_world.shards[_departing].Tick());
    (void)_wire.departingLink.Pump(_world.shards[_departing], _wire.departing, static_cast<Game::ShardId>(_arriving));
    (void)_wire.arrivingLink.Pump(_world.shards[_arriving], _wire.arriving, static_cast<Game::ShardId>(_departing));
    if (_world.shards[_arriving].DrainInbox() > 0)
      return pump;
  }
  return -1;
}
} // namespace

TEST_CLASS(HandoffTests)
{
public:
  TEST_METHOD(AShardNamesEveryShardItHasAGateTo)
  {
    // Which shards a server opens a link to, derived from the save's own gates rather than from the
    // layout: a shard that re-derived the partition would need the seed, the pins and the
    // GalaxyDesc, and would disagree with its own file the day any of them drifted
    // (Design/ShardServer-slice-3.md 2.1).
    for (std::uint32_t shardCount = 1; shardCount <= 5; ++shardCount)
    {
      const Galaxy world = BuildGalaxy(shardCount);
      std::vector<std::vector<Game::ShardId>> named(shardCount);
      for (std::uint32_t at = 0; at < shardCount; ++at)
      {
        // Asked with an empty span first: the count has to be right with nowhere to put the answer,
        // which is how a caller sizes its buffer. A dedupe that read back out of the span could not
        // do that -- what did not fit would be counted twice.
        const std::uint32_t size = world.shards[at].NeighbourShards({});
        named[at].resize(size);
        Assert::AreEqual(size, world.shards[at].NeighbourShards(named[at]),
                         L"the count changed when there was somewhere to put the answer");

        for (std::size_t after = 1; after < named[at].size(); ++after)
        {
          Assert::IsTrue(named[at][after] > named[at][after - 1], L"the neighbours are not ascending, so one is repeated or out of order");
        }
        for (const Game::ShardId neighbour : named[at])
          Assert::AreNotEqual(world.shards[at].Shard(), neighbour, L"a shard named itself as its own neighbour");
      }

      if (shardCount == 1)
        Assert::AreEqual(std::size_t{0}, named[0].size(), L"a galaxy of one shard has a neighbour");

      // The property a link actually depends on, and the one worth guarding: the path shape the
      // shipped galaxy happens to have is a fact about this galaxy, and symmetry is a fact about the
      // partition. A link with one end is not a link.
      for (std::uint32_t at = 0; at < shardCount; ++at)
      {
        for (const Game::ShardId neighbour : named[at])
        {
          const std::vector<Game::ShardId>& back = named[neighbour];
          Assert::IsTrue(std::find(back.begin(), back.end(), static_cast<Game::ShardId>(at)) != back.end(),
                         L"a shard names a neighbour that does not name it back");
        }
      }
    }
  }

  TEST_METHOD(ALinkCarriesNothingThatIsNotForItsOwnPeer)
  {
    // The defect ShardServer slice 3 found, guarded. The outbox is ONE queue for every destination,
    // and until this slice a link sent all of it -- so a shard with two neighbours would hand each
    // of them the other's entries, naming a gate that shard does not have. Invisible with two
    // shards, where every entry is for the one peer there is, which is why four slices of tests did
    // not see it (Design/ShardServer-slice-3.md 8).
    //
    // Stated as the property rather than by staging a double crossing, which would need two fleets
    // in one shard and there is one: a real outbox, one link to the shard it is for and one to a
    // shard it is not, and the second must carry nothing.
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    const std::uint32_t arriving = ShardOfDestination(world.shards[departing], gate);

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");
    Assert::IsFalse(world.shards[departing].Outbox().empty(), L"there is nothing in the outbox to filter");

    // A shard that is NOT the destination. Any of the four will do so long as it is neither end.
    std::uint32_t elsewhere = 0;
    while (elsewhere == departing || elsewhere == arriving)
      ++elsewhere;

    Neuron::LoopbackTransport right;
    Neuron::LoopbackTransport rightFar;
    Neuron::LoopbackTransport wrong;
    Neuron::LoopbackTransport wrongFar;
    Neuron::LoopbackTransport::Connect(right, rightFar, {});
    Neuron::LoopbackTransport::Connect(wrong, wrongFar, {});

    Game::ShardLink toArriving;
    Game::ShardLink toElsewhere;
    const Game::ShardLink::Pumped sentRight = toArriving.Pump(world.shards[departing], right, static_cast<Game::ShardId>(arriving));
    const Game::ShardLink::Pumped sentWrong = toElsewhere.Pump(world.shards[departing], wrong, static_cast<Game::ShardId>(elsewhere));

    Assert::IsTrue(sentRight.handoffsSent > 0, L"the link to the destination shard sent nothing");
    Assert::AreEqual(0u, sentWrong.handoffsSent, L"a link sent entries that were for another shard");
    Assert::IsFalse(sentWrong.laneRefused, L"the wrong link reported a refusal rather than having nothing to say");

    // And nothing reached the far end of the wrong wire, which is the claim a count alone does not
    // quite make: a message with zero entries in it is still a message.
    rightFar.AdvanceTo(world.shards[departing].Tick());
    rightFar.Poll();
    wrongFar.AdvanceTo(world.shards[departing].Tick());
    wrongFar.Poll();
    std::vector<std::uint8_t> buffer(Neuron::MAX_RELIABLE_BYTES);
    Assert::AreNotEqual(0u, rightFar.ReceiveReliable(buffer.data(), Neuron::MAX_RELIABLE_BYTES),
                        L"the destination shard's lane carried nothing");
    Assert::AreEqual(0u, wrongFar.ReceiveReliable(buffer.data(), Neuron::MAX_RELIABLE_BYTES),
                     L"a shard that was not the destination was sent something");
  }

  TEST_METHOD(AShortSpanStillCountsAndFillsWhatItCan)
  {
    // The awkward case, because it is the one a caller hits when the partition grew since it sized
    // its buffer: the count is the true one and what fits is the front of the answer.
    const Galaxy world = BuildGalaxy(3);
    const Game::Universe& middle = world.shards[1];
    std::vector<Game::ShardId> all(middle.NeighbourShards({}));
    Assert::AreEqual(std::size_t{2}, all.size(), L"the middle shard of three does not border two others");
    (void)middle.NeighbourShards(all);

    std::vector<Game::ShardId> few(1);
    Assert::AreEqual(2u, middle.NeighbourShards(few), L"a short span reported only what it could hold");
    Assert::AreEqual(all[0], few[0], L"a short span did not hold the front of the answer");
  }

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

  TEST_METHOD(AFleetCrossesAWireWithNobodyCopyingTheOutbox)
  {
    // Everything before this row moved a Handoff by hand. This one puts it on a transport, which is
    // the whole of slice 4: the same four calls, driven by a link instead of by a test.
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t before = PlayerShips(world);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    const std::uint32_t arriving = ShardOfDestination(world.shards[departing], gate);

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");

    Wire wire;
    wire.Connect(Neuron::LoopbackTransport::Desc{});
    // Everything up to now is on disk, so the arriving end may acknowledge what it receives.
    wire.arrivingLink.NoteDurableThrough(world.shards[arriving].Tick() + 100000);

    Assert::IsTrue(PumpUntilArrived(wire, world, departing, arriving) >= 0, L"the fleet never arrived over the wire");
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, world.shards[arriving].FleetInSlot(Game::OWNER_LOCAL, 0),
                        L"the fleet did not re-form on the far side");
    Assert::AreEqual(before, PlayerShips(world), L"a ship was lost or duplicated crossing the wire");

    // And the outbox empties, which is the acknowledgement arriving back.
    for (int pump = 0; pump < 200 && !world.shards[departing].Outbox().empty(); ++pump)
    {
      world.shards[departing].Step();
      wire.Poll(world.shards[departing].Tick());
      (void)wire.arrivingLink.Pump(world.shards[arriving], wire.arriving, static_cast<Game::ShardId>(departing));
      (void)wire.departingLink.Pump(world.shards[departing], wire.departing, static_cast<Game::ShardId>(arriving));
    }
    Assert::IsTrue(world.shards[departing].Outbox().empty(), L"the outbox was never acknowledged");
  }

  TEST_METHOD(ALaneTooShallowToTakeTheMessageLosesNothing)
  {
    // The loss model, and it is a real one rather than an injected fault: SendReliable refuses when
    // the queue is full. The entry stays in the outbox -- which is the point of the outbox -- and the
    // next re-send delivers it.
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t before = PlayerShips(world);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    const std::uint32_t arriving = ShardOfDestination(world.shards[departing], gate);

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");

    Wire wire;
    Neuron::LoopbackTransport::Desc desc;
    desc.capacityReliableMessages = 0; // nothing may be sent at all
    wire.Connect(desc);
    wire.arrivingLink.NoteDurableThrough(world.shards[arriving].Tick() + 100000);

    const std::size_t held = world.shards[departing].Outbox().size();
    bool refused = false;
    for (int pump = 0; pump < 200; ++pump)
    {
      world.shards[departing].Step();
      wire.Poll(world.shards[departing].Tick());
      refused =
        wire.departingLink.Pump(world.shards[departing], wire.departing, static_cast<Game::ShardId>(arriving)).laneRefused || refused;
    }
    Assert::IsTrue(refused, L"a lane with no room never refused a send");
    Assert::AreEqual(held, world.shards[departing].Outbox().size(), L"a refused send lost an outbox entry");
    // In the outbox and nowhere else, which is where a ship IS while a send is being refused -- the
    // same census the first row of this suite states, and the reason it counts the queue.
    Assert::AreEqual(before, PlayerShips(world) + static_cast<std::uint32_t>(held), L"a refused send lost a ship");

    // Room appears, and the same entries go across untouched.
    Wire open;
    open.Connect(Neuron::LoopbackTransport::Desc{});
    open.arrivingLink.NoteDurableThrough(world.shards[arriving].Tick() + 100000);
    Assert::IsTrue(PumpUntilArrived(open, world, departing, arriving) >= 0, L"the fleet never arrived once the lane had room");
    Assert::AreEqual(before, PlayerShips(world), L"the retry duplicated a ship");
  }

  TEST_METHOD(NothingIsAcknowledgedUntilTheArrivingShardHasSavedIt)
  {
    // ADR 0066. With no save noted, the arriving shard may not acknowledge, so the departing outbox
    // must still hold every entry however long the two are pumped -- and the ships must be safe on
    // BOTH sides while that is true, which is the whole reason for the rule.
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    const std::uint32_t arriving = ShardOfDestination(world.shards[departing], gate);

    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");

    Wire wire;
    wire.Connect(Neuron::LoopbackTransport::Desc{});
    // Deliberately NOT noted durable: this shard has saved nothing.
    const std::size_t held = world.shards[departing].Outbox().size();

    std::uint32_t acked = 0;
    for (int pump = 0; pump < 400; ++pump)
    {
      world.shards[departing].Step();
      world.shards[arriving].Step();
      wire.Poll(world.shards[departing].Tick());
      (void)wire.departingLink.Pump(world.shards[departing], wire.departing, static_cast<Game::ShardId>(arriving));
      acked += wire.arrivingLink.Pump(world.shards[arriving], wire.arriving, static_cast<Game::ShardId>(departing)).acksSent;
      (void)world.shards[arriving].DrainInbox();
    }
    Assert::AreEqual(std::uint32_t{0}, acked, L"a shard acknowledged a handoff it had not saved");
    Assert::AreEqual(held, world.shards[departing].Outbox().size(), L"the outbox was cleared without an acknowledgement");
    Assert::IsTrue(world.shards[arriving].ShipCount() > 0, L"the far side has nothing at all");

    // Now it saves, and the same pump clears the outbox.
    wire.arrivingLink.NoteDurableThrough(world.shards[arriving].Tick());
    for (int pump = 0; pump < 400 && !world.shards[departing].Outbox().empty(); ++pump)
    {
      world.shards[departing].Step();
      world.shards[arriving].Step();
      wire.Poll(world.shards[departing].Tick());
      (void)wire.arrivingLink.Pump(world.shards[arriving], wire.arriving, static_cast<Game::ShardId>(departing));
      (void)wire.departingLink.Pump(world.shards[departing], wire.departing, static_cast<Game::ShardId>(arriving));
    }
    Assert::IsTrue(world.shards[departing].Outbox().empty(), L"a noted save did not clear the outbox");
  }

  TEST_METHOD(AHandoffMessageRoundTripsThroughItsOwnCodec)
  {
    // The wire format, on its own, because a codec tested only through a link is a codec whose
    // failures look like link failures.
    Galaxy world = BuildGalaxy(4);
    const std::uint32_t departing = ShardHoldingTheFleet(world);
    const Game::ShipId gate = GateLeadingOut(world.shards[departing]);
    OrderThroughGate(world.shards[departing], gate);
    Assert::IsTrue(StepUntilHandedOff(world, departing), L"the fleet never reached the gate");
    const std::vector<Game::Universe::Handoff> sent = TakeOutbox(world.shards[departing]);

    Neuron::LoopbackTransport a;
    Neuron::LoopbackTransport b;
    Neuron::LoopbackTransport::Connect(a, b, Neuron::LoopbackTransport::Desc{});
    std::uint32_t taken = 0;
    Assert::IsTrue(Game::WriteHandoffs(sent, a, taken), L"the lane refused a handoff message");
    Assert::AreEqual(static_cast<std::uint32_t>(sent.size()), taken, L"the message did not take every entry");

    b.AdvanceTo(1);
    b.Poll();
    std::vector<std::uint8_t> buffer(Neuron::MAX_RELIABLE_BYTES);
    const std::uint32_t size = b.ReceiveReliable(buffer.data(), Neuron::MAX_RELIABLE_BYTES);
    Assert::IsTrue(size > 0, L"nothing arrived on the reliable lane");

    std::vector<Game::Universe::Handoff> got;
    Assert::IsTrue(Game::ReadHandoffs(std::span<const std::uint8_t>(buffer.data(), size), got), L"the handoff message was refused");
    Assert::AreEqual(sent.size(), got.size(), L"the message lost an entry");
    for (std::size_t at = 0; at < sent.size(); ++at)
    {
      Assert::AreEqual(sent[at].entity, got[at].entity, L"an entry changed identity on the wire");
      Assert::AreEqual(sent[at].gate, got[at].gate, L"an entry lost the gate it is bound for");
      Assert::AreEqual(sent[at].sequence, got[at].sequence, L"an entry lost its sequence");
      Assert::AreEqual(sent[at].hullPoints, got[at].hullPoints, L"an entry lost its damage");
      Assert::AreEqual(sent[at].owner, got[at].owner, L"an entry lost its owner");
      Assert::AreEqual(sent[at].crossingCount, got[at].crossingCount, L"an entry lost the width of its spread");
    }

    // And an ack, the same way round.
    const std::uint64_t sequences[] = {sent[0].sequence, sent[1].sequence};
    Assert::IsTrue(Game::WriteHandoffAck(sequences, a), L"the lane refused an acknowledgement");
    b.AdvanceTo(2);
    b.Poll();
    const std::uint32_t ackSize = b.ReceiveReliable(buffer.data(), Neuron::MAX_RELIABLE_BYTES);
    std::vector<std::uint64_t> gotAck;
    Assert::IsTrue(Game::ReadHandoffAck(std::span<const std::uint8_t>(buffer.data(), ackSize), gotAck), L"the acknowledgement was refused");
    Assert::AreEqual(std::size_t{2}, gotAck.size(), L"the acknowledgement lost a sequence");
    Assert::AreEqual(sequences[0], gotAck[0], L"an acknowledgement named a different entry");
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
