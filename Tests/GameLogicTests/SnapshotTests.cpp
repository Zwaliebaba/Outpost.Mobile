#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// A Transport that keeps what it is given. The format is what these tests are about; latency and
// loss belong to LoopbackTransport and are tested in NeuronCoreTests, where that class lives.
class CaptureTransport final : public Neuron::Transport
{
public:
  [[nodiscard]] bool Send(const std::uint8_t* _bytes, std::uint32_t _count) override
  {
    if (_count > Neuron::MAX_DATAGRAM_BYTES || refuseFrom == sent.size())
      return false;
    sent.emplace_back(_bytes, _bytes + _count);
    return true;
  }

  [[nodiscard]] std::uint32_t Receive(std::uint8_t*, std::uint32_t) override
  {
    return 0;
  }

  void Poll() override {}

  [[nodiscard]] Neuron::ConnectionState State() const override
  {
    return Neuron::ConnectionState::Connected;
  }

  std::vector<std::vector<std::uint8_t>> sent;
  std::size_t refuseFrom = static_cast<std::size_t>(-1); // refuse once this many have been sent
};

Game::ShipId SpawnAt(Game::World& _world, float _x, float _z, Game::HullId _hull = Game::HullId::Corvette,
                     Game::FactionId _faction = Game::FACTION_PLAYER)
{
  return _world.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(_hull), _faction);
}

[[nodiscard]] bool Holds(std::span<const Game::ShipHandle> _set, Game::ShipHandle _handle)
{
  for (const Game::ShipHandle handle : _set)
  {
    if (handle == _handle)
      return true;
  }
  return false;
}

// A faction id is one byte, and a byte is a character to anything that prints one -- so a failure
// would report an unprintable glyph rather than "1". Widened for the assertion, never for the wire.
[[nodiscard]] std::uint32_t Faction(Game::FactionId _faction)
{
  return _faction;
}

[[nodiscard]] const Game::ShipSnapshot* Find(const Game::WorldSnapshot& _snapshot, Game::ShipHandle _handle)
{
  for (const Game::ShipSnapshot& ship : _snapshot.ships)
  {
    if (ship.handle == _handle)
      return &ship;
  }
  return nullptr;
}
} // namespace

TEST_CLASS(SnapshotTests)
{
public:
  TEST_METHOD(TheFragmentSizeIsDerivedFromTheDatagram)
  {
    // Derived rather than chosen, so the day the record grows these follow it. The numbers are
    // recorded because they are the argument for interest management: 13 ships per datagram means a
    // 5,000-ship snapshot is 385 fragments, which is what slice 6 exists to stop sending.
    Assert::IsTrue(Game::ShipsPerSnapshotFragment() > 0, L"no ship fits in a datagram");
    Assert::IsTrue(Game::ShipsPerSnapshotFragment() < 64, L"the record got suspiciously small");
    Assert::IsTrue(Game::MaxShipsPerOrder() > Game::ShipsPerSnapshotFragment(), L"an order holds fewer ships than a snapshot fragment");
  }

  TEST_METHOD(OneShipRoundTripsFieldForField)
  {
    // Spawned hostile so that every field is compared against a value that is not its default: a
    // faction that round-trips only because both ends default to zero proves nothing.
    Game::World world;
    const Game::ShipId ship = SpawnAt(world, 120.0f, -340.0f, Game::HullId::Frigate, Game::FACTION_HOSTILE);
    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::LocalPos(0.0f, 600.0f), false, 0.0f);
    for (int tick = 0; tick < 30; ++tick)
      world.Step();

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::AreEqual(1u, writer.Write(world, transport), L"one ship did not fit in one fragment");

    Game::SnapshotReceiver receiver;
    Assert::IsTrue(receiver.Accept(transport.sent[0]), L"a single-fragment snapshot did not complete");

    const Game::WorldSnapshot& got = receiver.Latest();
    Assert::AreEqual(world.Tick(), got.tick, L"the snapshot carries the wrong tick");
    Assert::AreEqual(static_cast<std::size_t>(1), got.ships.size(), L"the wrong number of ships came back");

    const Game::ShipState& source = world.Ship(ship);
    const Game::ShipSnapshot& copy = got.ships[0];
    Assert::IsTrue(copy.handle == world.HandleOf(ship), L"the handle did not survive");
    Assert::IsTrue(IsSamePosition(source.posWorld, copy.posWorld), L"posWorld did not survive");
    Assert::IsTrue(IsSamePosition(source.prevPos, copy.prevPos), L"prevPos did not survive");
    Assert::AreEqual(source.headingRad, copy.headingRad, 0.0f, L"headingRad did not survive");
    Assert::AreEqual(source.prevHeading, copy.prevHeading, 0.0f, L"prevHeading did not survive");
    Assert::AreEqual(source.speed, copy.speed, 0.0f, L"speed did not survive");
    Assert::AreEqual(source.accelSample, copy.accelSample, 0.0f, L"accelSample did not survive");
    Assert::AreEqual(source.turnRateRadPerSec, copy.turnRateRadPerSec, 0.0f, L"turnRateRadPerSec did not survive");
    Assert::AreEqual(source.order, copy.order, L"the order state did not survive");
    Assert::AreEqual(Faction(source.factionId), Faction(copy.factionId), L"factionId did not survive");
    Assert::AreEqual(source.hullId, copy.hullId, L"hullId did not survive");
  }

  TEST_METHOD(FactionSurvivesTheWire)
  {
    // Both shapes of send, because they are the two the game uses and only one of them is exercised
    // by the round-trip test above.
    Game::World world;
    const Game::ShipId ours = SpawnAt(world, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId theirs = SpawnAt(world, 200.0f, 0.0f, Game::HullId::Interceptor, Game::FACTION_HOSTILE);
    world.Step();

    const Game::ShipHandle ourHandle = world.HandleOf(ours);
    const Game::ShipHandle theirHandle = world.HandleOf(theirs);

    CaptureTransport whole;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(world, whole) > 0, L"the full snapshot did not send");
    Game::SnapshotReceiver fromWhole;
    for (const std::vector<std::uint8_t>& datagram : whole.sent)
      (void)fromWhole.Accept(datagram);
    Assert::IsTrue(Find(fromWhole.Latest(), ourHandle) != nullptr, L"the player's ship is missing from the full snapshot");
    Assert::AreEqual(Faction(Game::FACTION_PLAYER), Faction(Find(fromWhole.Latest(), ourHandle)->factionId),
                     L"the player's faction did not survive Write");
    Assert::AreEqual(Faction(Game::FACTION_HOSTILE), Faction(Find(fromWhole.Latest(), theirHandle)->factionId),
                     L"the hostile faction did not survive Write");

    world.Step();
    CaptureTransport update;
    const Game::ShipHandle both[] = {ourHandle, theirHandle};
    Assert::IsTrue(writer.WriteInterest(world, both, {}, {}, update) > 0, L"the interest update did not send");
    Game::SnapshotReceiver fromUpdate;
    for (const std::vector<std::uint8_t>& datagram : update.sent)
      (void)fromUpdate.Accept(datagram);
    Assert::AreEqual(Faction(Game::FACTION_PLAYER), Faction(Find(fromUpdate.Latest(), ourHandle)->factionId),
                     L"the player's faction did not survive WriteInterest");
    Assert::AreEqual(Faction(Game::FACTION_HOSTILE), Faction(Find(fromUpdate.Latest(), theirHandle)->factionId),
                     L"the hostile faction did not survive WriteInterest");
  }

  TEST_METHOD(ADeathAndADepartureDifferOnTheWire)
  {
    // The distinction the client's explosion hangs on. Until now a leave meant both, so a hostile
    // patrol crossing the edge of the interest radius detonated on screen while it was alive and
    // well (Design/Hostiles.md 4.4).
    //
    // The split is WorldSimulation's, which lives in the executable and has no suite, so the rule is
    // restated here against the same two inputs: the world's despawn log, and the interest set's
    // leaves.
    Game::World world;
    const Game::ShipId doomed = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId departing = SpawnAt(world, 100.0f, 0.0f);
    const Game::ShipHandle doomedHandle = world.HandleOf(doomed);
    const Game::ShipHandle departingHandle = world.HandleOf(departing);
    world.Step();

    Game::InterestSet interest;
    interest.Update(world, Game::LocalPos(0.0f, 0.0f));

    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;
    CaptureTransport link;
    const Game::ShipHandle both[] = {doomedHandle, departingHandle};
    Assert::IsTrue(writer.WriteInterest(world, both, {}, {}, link) > 0, L"the first update did not send");
    for (const std::vector<std::uint8_t>& datagram : link.sent)
      (void)receiver.Accept(datagram);
    Assert::AreEqual(static_cast<std::size_t>(2), receiver.Latest().ships.size(), L"the client did not take both ships");
    Assert::IsTrue(receiver.Destroyed().empty(), L"an update that killed nothing reported a death");

    // One dies; the other is left alive but carried out of range by moving the viewpoint, which is
    // the case a subscriber actually creates. Both drop out on the same update, which is the point:
    // they arrive in one Left() list and only the log tells them apart.
    Assert::IsTrue(world.DespawnShip(doomedHandle), L"the despawn failed");
    world.Step();
    interest.Update(world, Game::LocalPos(Game::INTEREST_RADIUS_METRES * 3.0f, 0.0f));
    Assert::AreEqual(static_cast<std::size_t>(2), interest.Left().size(), L"both ships should have dropped out on one update");

    std::vector<Game::ShipHandle> destroyed;
    std::vector<Game::ShipHandle> left;
    for (const Game::ShipHandle handle : interest.Left())
    {
      if (Holds(world.DespawnsSince(0), handle))
        destroyed.push_back(handle);
      else
        left.push_back(handle);
    }
    Assert::AreEqual(static_cast<std::size_t>(1), destroyed.size(), L"exactly one of the two died");
    Assert::AreEqual(static_cast<std::size_t>(1), left.size(), L"exactly one of the two merely departed");

    link.sent.clear();
    Assert::IsTrue(writer.WriteInterest(world, {}, left, destroyed, link) > 0, L"the second update did not send");
    for (const std::vector<std::uint8_t>& datagram : link.sent)
      (void)receiver.Accept(datagram);

    Assert::IsTrue(receiver.Latest().ships.empty(), L"the client kept a ship it was told about losing");
    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Destroyed().size(), L"the update did not state exactly one death");
    Assert::IsTrue(Holds(receiver.Destroyed(), doomedHandle), L"the despawned ship was not reported destroyed");
    Assert::IsFalse(Holds(receiver.Destroyed(), departingHandle), L"a ship that only left the radius was reported destroyed");
  }

  TEST_METHOD(AWorldTooBigForOneDatagramFragmentsAndReassembles)
  {
    // 200 ships against 13 per fragment. The point is not the number but that the count and the
    // order come back exactly, because a snapshot that reassembles out of order is a world where
    // ships have swapped places.
    Game::World world;
    for (int at = 0; at < 200; ++at)
      SpawnAt(world, static_cast<float>(at) * 40.0f, 0.0f);
    world.Step();

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    const std::uint32_t fragments = writer.Write(world, transport);
    Assert::IsTrue(fragments > 1, L"200 ships did not fragment");

    Game::SnapshotReceiver receiver;
    bool complete = false;
    for (const std::vector<std::uint8_t>& datagram : transport.sent)
      complete = receiver.Accept(datagram);

    Assert::IsTrue(complete, L"the last fragment did not complete the snapshot");
    Assert::AreEqual(static_cast<std::size_t>(200), receiver.Latest().ships.size(), L"ships went missing across fragments");
    for (Game::ShipId id = 0; id < 200; ++id)
      Assert::IsTrue(receiver.Latest().ships[id].handle == world.HandleOf(id), L"ships came back in a different order");
  }

  TEST_METHOD(ASnapshotMissingAFragmentIsDroppedWhole)
  {
    // Half a world is worse than a stale one: stale reads as lag, partial reads as ships vanishing.
    Game::World world;
    for (int at = 0; at < 60; ++at)
      SpawnAt(world, static_cast<float>(at) * 40.0f, 0.0f);
    world.Step();

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(world, transport) > 2, L"60 ships did not need three fragments");

    Game::SnapshotReceiver receiver;
    bool complete = false;
    for (std::size_t at = 0; at < transport.sent.size(); ++at)
    {
      if (at == 1)
        continue; // the one that went missing
      complete = receiver.Accept(transport.sent[at]) || complete;
    }

    Assert::IsFalse(complete, L"a snapshot completed with a fragment missing");
    Assert::IsFalse(receiver.HasSnapshot(), L"a partial snapshot was published");
  }

  TEST_METHOD(AStaleSnapshotIsIgnored)
  {
    // Latency makes out-of-order arrival real, and applying an older snapshot steps the view back.
    Game::World world;
    SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId order[] = {0};
    world.IssueMoveOrder(order, Game::LocalPos(0.0f, 900.0f), false, 0.0f);

    CaptureTransport early;
    Game::SnapshotWriter writer;
    for (int tick = 0; tick < 10; ++tick)
      world.Step();
    Assert::AreEqual(1u, writer.Write(world, early), L"the early snapshot did not send");

    CaptureTransport late;
    for (int tick = 0; tick < 10; ++tick)
      world.Step();
    Assert::AreEqual(1u, writer.Write(world, late), L"the later snapshot did not send");

    Game::SnapshotReceiver receiver;
    Assert::IsTrue(receiver.Accept(late.sent[0]), L"the later snapshot did not apply");
    const std::uint64_t applied = receiver.Latest().tick;

    Assert::IsFalse(receiver.Accept(early.sent[0]), L"an older snapshot was applied over a newer one");
    Assert::AreEqual(applied, receiver.Latest().tick, L"an older snapshot changed what was current");
  }

  TEST_METHOD(AMalformedDatagramIsRefusedRatherThanRead)
  {
    Game::World world;
    SpawnAt(world, 0.0f, 0.0f);
    world.Step();

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    (void)writer.Write(world, transport);

    Game::SnapshotReceiver receiver;
    // Truncated part-way through the ship record.
    const std::vector<std::uint8_t> truncated(transport.sent[0].begin(), transport.sent[0].begin() + 40);
    Assert::IsFalse(receiver.Accept(truncated), L"a truncated datagram was accepted");
    Assert::IsFalse(receiver.HasSnapshot(), L"a truncated datagram published a snapshot");

    // Empty, and one whose kind byte belongs to the other direction.
    Assert::IsFalse(receiver.Accept({}), L"an empty datagram was accepted");
    const std::vector<std::uint8_t> wrongKind{2, 0, 0, 0, 0};
    Assert::IsFalse(receiver.Accept(wrongKind), L"a move order was read as a snapshot");
  }

  TEST_METHOD(AWriteStopsWhenTheTransportRefuses)
  {
    // A refusal part-way through is not retried: the receiver drops the incomplete snapshot whole
    // and the next tick brings another.
    Game::World world;
    for (int at = 0; at < 60; ++at)
      SpawnAt(world, static_cast<float>(at) * 40.0f, 0.0f);
    world.Step();

    CaptureTransport transport;
    transport.refuseFrom = 2;
    Game::SnapshotWriter writer;
    Assert::AreEqual(2u, writer.Write(world, transport), L"the writer did not stop at the first refusal");
  }

  TEST_METHOD(AnEmptyWorldStillSendsASnapshot)
  {
    // "No ships" is information. No snapshot at all is indistinguishable from a stalled server.
    Game::World world;
    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::AreEqual(1u, writer.Write(world, transport), L"an empty world sent nothing");

    Game::SnapshotReceiver receiver;
    Assert::IsTrue(receiver.Accept(transport.sent[0]), L"an empty snapshot did not complete");
    Assert::IsTrue(receiver.Latest().ships.empty(), L"an empty world produced ships");
  }

  TEST_METHOD(AMoveOrderRoundTrips)
  {
    Game::World world;
    const Game::ShipId first = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId second = SpawnAt(world, 100.0f, 0.0f);

    Game::MoveOrder sent;
    sent.ships = {world.HandleOf(first), world.HandleOf(second)};
    sent.destination = Game::LocalPos(-450.5f, 1200.25f);
    sent.facingRad = 1.25f;
    sent.hasFacing = true;

    CaptureTransport transport;
    Assert::IsTrue(Game::WriteMoveOrder(sent, transport), L"the order did not send");

    Game::MoveOrder got;
    Assert::IsTrue(Game::ReadMoveOrder(transport.sent[0], got), L"the order did not decode");
    Assert::AreEqual(sent.ships.size(), got.ships.size(), L"the order lost ships");
    for (std::size_t at = 0; at < sent.ships.size(); ++at)
      Assert::IsTrue(sent.ships[at] == got.ships[at], L"an ordered ship changed identity");
    Assert::IsTrue(IsSamePosition(sent.destination, got.destination), L"the destination did not survive");
    Assert::AreEqual(sent.facingRad, got.facingRad, 0.0f, L"the facing did not survive");
    Assert::IsTrue(got.hasFacing, L"the has-facing flag did not survive");
  }

  TEST_METHOD(AnOrderForADespawnedShipResolvesToNothing)
  {
    // The reason an order carries handles and not ids. Between the click and the order arriving, a
    // ship can die and swap-and-pop can move a stranger into its index (ADR 0005).
    Game::World world;
    const Game::ShipId doomed = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId other = SpawnAt(world, 100.0f, 0.0f);
    const Game::ShipHandle doomedHandle = world.HandleOf(doomed);
    const Game::ShipHandle otherHandle = world.HandleOf(other);

    Game::MoveOrder sent;
    sent.ships = {doomedHandle, otherHandle};
    sent.destination = Game::LocalPos(0.0f, 500.0f);

    CaptureTransport transport;
    Assert::IsTrue(Game::WriteMoveOrder(sent, transport), L"the order did not send");
    Assert::IsTrue(world.DespawnShip(doomedHandle), L"the despawn failed");

    Game::MoveOrder got;
    Assert::IsTrue(Game::ReadMoveOrder(transport.sent[0], got), L"the order did not decode");
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(got.ships[0]), L"a dead ship's handle resolved to something");

    // The survivor is still there, but swap-and-pop moved it into the freed slot, so its id is not
    // the one it had when the order was written. That is the whole of ADR 0005: the handle follows
    // the ship, an id does not, and an order carrying ids would now be steering the wrong hull.
    const Game::ShipId resolved = world.Resolve(got.ships[1]);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, resolved, L"the surviving ship's handle stopped resolving");
    Assert::AreNotEqual(other, resolved, L"the survivor's index did not move, so this test proves nothing");
    Assert::IsTrue(world.HandleOf(resolved) == otherHandle, L"the handle resolved to a stranger");
  }

  TEST_METHOD(AnOversizedOrderIsRefused)
  {
    Game::MoveOrder tooMany;
    tooMany.ships.assign(Game::MaxShipsPerOrder() + 1, Game::ShipHandle{1, 1});
    tooMany.destination = Game::LocalPos(0.0f, 0.0f);

    CaptureTransport transport;
    Assert::IsFalse(Game::WriteMoveOrder(tooMany, transport), L"an order larger than a datagram was sent");
    Assert::IsTrue(transport.sent.empty(), L"a refused order still put bytes on the wire");

    Game::MoveOrder empty;
    Assert::IsFalse(Game::WriteMoveOrder(empty, transport), L"an order with no ships was sent");
  }
};
} // namespace GameLogicTests
