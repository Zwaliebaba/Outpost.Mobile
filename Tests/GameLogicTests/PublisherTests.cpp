#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// Its own copy rather than a shared one: both suites keep theirs in an anonymous namespace, which is
// what the tree does instead of a test-support header nobody owns.
[[nodiscard]] bool Holds(std::span<const Game::ShipHandle> _set, Game::ShipHandle _handle)
{
  for (const Game::ShipHandle held : _set)
  {
    if (held == _handle)
      return true;
  }
  return false;
}

Game::ShipId SpawnAt(Game::World& _world, float _x, float _z, Game::FactionId _faction = Game::FACTION_PLAYER)
{
  return _world.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), _faction);
}

// A pair of ends wired to each other, driven from one tick the way the composition root does.
struct Link
{
  Neuron::LoopbackTransport server;
  Neuron::LoopbackTransport client;

  explicit Link(std::uint32_t _dropOneInN = 0)
  {
    Neuron::LoopbackTransport::Desc desc;
    desc.dropOneInN = _dropOneInN;
    Neuron::LoopbackTransport::Connect(server, client, desc);
  }

  void Pump(std::uint64_t _tick)
  {
    server.AdvanceTo(_tick);
    client.AdvanceTo(_tick);
    server.Poll();
    client.Poll();
  }

  // Everything the client end has been given, both lanes, fed to a receiver.
  void DrainInto(Game::SnapshotReceiver& _receiver)
  {
    std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> datagram{};
    for (;;)
    {
      const std::uint32_t size = client.Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size()));
      if (size == 0)
        break;
      (void)_receiver.Accept(std::span<const std::uint8_t>(datagram.data(), size));
    }

    std::vector<std::uint8_t> message(Neuron::MAX_RELIABLE_BYTES, 0u);
    for (;;)
    {
      const std::uint32_t size = client.ReceiveReliable(message.data(), Neuron::MAX_RELIABLE_BYTES);
      if (size == 0)
        break;
      (void)_receiver.Accept(std::span<const std::uint8_t>(message.data(), size));
    }
  }
};
} // namespace

TEST_CLASS(PublisherTests)
{
public:
  TEST_METHOD(TwoSubscribersSeeTheirOwnNeighbourhoods)
  {
    // The property the single-subscriber adapter could not have: two ends, two interest sets, two
    // writers, and neither one's bytes are the other's (Design/MmoScalabilityReview.md E2).
    Game::World world;
    (void)SpawnAt(world, 0.0f, 0.0f);
    (void)SpawnAt(world, 50.0f, 0.0f);
    // Far enough that a subscriber standing at the origin cannot see it.
    (void)SpawnAt(world, Game::INTEREST_RADIUS_METRES * 4.0f, 0.0f);

    // Not "near" and "far": both are legacy memory-model macros from <windows.h>, which the
    // umbrella pulls in, so `Link near;` expands to `Link ;` and every use of it becomes a syntax
    // error pointing at the dot rather than at the name.
    Link nearLink;
    Link farLink;
    Game::Publisher publisher;

    Game::Publisher::Desc nearDesc;
    nearDesc.transport = &nearLink.server;
    const Game::Publisher::Handle nearSub = publisher.Add(nearDesc);

    Game::Publisher::Desc farDesc;
    farDesc.transport = &farLink.server;
    farDesc.centre = Game::LocalPos(Game::INTEREST_RADIUS_METRES * 4.0f, 0.0f);
    const Game::Publisher::Handle farSub = publisher.Add(farDesc);
    Assert::AreEqual(2u, publisher.Count(), L"the publisher did not take both subscribers");

    // Run far enough that both phases have come due at least once.
    for (std::uint64_t tick = 0; tick < Game::INTEREST_UPDATE_EVERY_TICKS * 2; ++tick)
    {
      nearLink.Pump(tick);
      farLink.Pump(tick);
      world.Step();
      publisher.Publish(world);
      nearLink.Pump(tick);
      farLink.Pump(tick);
    }

    Game::SnapshotReceiver nearView;
    Game::SnapshotReceiver farView;
    nearLink.DrainInto(nearView);
    farLink.DrainInto(farView);

    Assert::AreEqual(static_cast<std::size_t>(2), nearView.Latest().ships.size(), L"the near subscriber did not get its own two ships");
    Assert::AreEqual(static_cast<std::size_t>(1), farView.Latest().ships.size(), L"the far subscriber did not get the one ship near it");
    Assert::AreNotEqual(publisher.PhaseOf(nearSub), publisher.PhaseOf(farSub), L"two subscribers were given the same phase");
  }

  TEST_METHOD(SubscribersAreSpreadAcrossTheUpdatePeriod)
  {
    // The spike this retires: with one phase, every subscriber's query, sort and egress lands on the
    // same tick and the worst frame is the period times the average one (E4). One per tick is what
    // the phase buys, and it is asserted rather than assumed.
    Game::World world;
    (void)SpawnAt(world, 0.0f, 0.0f);

    std::vector<std::unique_ptr<Link>> links;
    Game::Publisher publisher;
    std::vector<Game::Publisher::Handle> handles;
    for (std::uint32_t at = 0; at < Game::INTEREST_UPDATE_EVERY_TICKS; ++at)
    {
      links.push_back(std::make_unique<Link>());
      Game::Publisher::Desc desc;
      desc.transport = &links.back()->server;
      handles.push_back(publisher.Add(desc));
    }

    std::vector<bool> seen(Game::INTEREST_UPDATE_EVERY_TICKS, false);
    for (const Game::Publisher::Handle handle : handles)
    {
      const std::uint32_t phase = publisher.PhaseOf(handle);
      Assert::IsTrue(phase < Game::INTEREST_UPDATE_EVERY_TICKS, L"a phase fell outside the update period");
      Assert::IsFalse(seen[phase], L"two subscribers were given the same phase");
      seen[phase] = true;
    }
  }

  TEST_METHOD(EverySubscriberHearsEveryDeathExactlyOnce)
  {
    // Slice 2's cursors, doing the job they exist for. With the old drain-once log, whichever
    // subscriber published first consumed the death and the other never heard it.
    Game::World world;
    const Game::ShipId doomed = SpawnAt(world, 0.0f, 0.0f);
    (void)SpawnAt(world, 40.0f, 0.0f);
    const Game::ShipHandle doomedHandle = world.HandleOf(doomed);

    Link a;
    Link b;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &a.server;
    (void)publisher.Add(desc);
    desc.transport = &b.server;
    (void)publisher.Add(desc);

    Game::SnapshotReceiver viewA;
    Game::SnapshotReceiver viewB;

    // Long enough for both to have entered the ship, then it dies and both are told.
    const std::uint64_t settle = Game::INTEREST_UPDATE_EVERY_TICKS * 2;
    for (std::uint64_t tick = 0; tick < settle; ++tick)
    {
      a.Pump(tick);
      b.Pump(tick);
      world.Step();
      publisher.Publish(world);
      a.Pump(tick);
      b.Pump(tick);
      a.DrainInto(viewA);
      b.DrainInto(viewB);
    }
    Assert::IsTrue(world.DespawnShip(doomedHandle), L"the despawn failed");

    for (std::uint64_t tick = settle; tick < settle + Game::INTEREST_UPDATE_EVERY_TICKS * 2; ++tick)
    {
      a.Pump(tick);
      b.Pump(tick);
      world.Step();
      publisher.Publish(world);
      a.Pump(tick);
      b.Pump(tick);
      a.DrainInto(viewA);
      b.DrainInto(viewB);
    }

    Assert::IsTrue(Holds(viewA.Destroyed(), doomedHandle), L"the first subscriber was not told about the death");
    Assert::IsTrue(Holds(viewB.Destroyed(), doomedHandle), L"the second subscriber lost the death to the first one's read");
  }

  TEST_METHOD(OrdersPastTheBudgetAreDroppedAndCounted)
  {
    // A client saturating its send rate buys formation solves and route planning at a leverage no
    // other message has. The faction gate says whose ships; this says how often (E6).
    Game::World world;
    const Game::ShipId ship = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipHandle handle = world.HandleOf(ship);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.ordersPerTick = 2;
    const Game::Publisher::Handle subscriber = publisher.Add(desc);

    link.Pump(0);
    Game::MoveOrder order;
    order.ships.push_back(handle);
    order.destination = Game::LocalPos(500.0f, 0.0f);
    for (int at = 0; at < 5; ++at)
      Assert::IsTrue(Game::WriteMoveOrder(order, link.client), L"the order was refused by the lane");
    link.Pump(0);

    publisher.ApplyOrders(world);
    Assert::AreEqual(1u, publisher.DroppedOrderCount(subscriber), L"going over budget was not counted");

    // What was over budget is still queued, not thrown away, and next tick reads more of it.
    link.Pump(1);
    publisher.ApplyOrders(world);
    Assert::AreEqual(2u, publisher.DroppedOrderCount(subscriber), L"the budget did not refill");
  }

  TEST_METHOD(ASubscriberAddedLaterHearsOnlyWhatFollowsIt)
  {
    // Its cursor opens at the head, so it is not told about ships it never held (ADR 0026).
    Game::World world;
    const Game::ShipId first = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId second = SpawnAt(world, 40.0f, 0.0f);
    Assert::IsTrue(world.DespawnShip(world.HandleOf(first)), L"the despawn failed");

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    // The line that makes it a late subscriber rather than one that happens to miss the death
    // because it never held the ship.
    desc.openingDespawnCursor = world.DespawnHead();
    const Game::Publisher::Handle late = publisher.Add(desc);
    Assert::IsFalse(world.DespawnsSince(0).empty(), L"the death was trimmed before the test could use it");
    Assert::IsTrue(world.DespawnsSince(world.DespawnHead()).empty(), L"the head is not past the death");
    Assert::AreEqual(0u, publisher.RefusedLeaveCount(late), L"a fresh subscriber had already refused something");

    Game::SnapshotReceiver view;
    for (std::uint64_t tick = 0; tick < Game::INTEREST_UPDATE_EVERY_TICKS * 2; ++tick)
    {
      link.Pump(tick);
      world.Step();
      publisher.Publish(world);
      link.Pump(tick);
      link.DrainInto(view);
    }

    Assert::IsTrue(view.Destroyed().empty(), L"a late subscriber was told about a death that preceded it");
    Assert::AreEqual(static_cast<std::size_t>(1), view.Latest().ships.size(), L"the late subscriber did not get the surviving ship");
    Assert::IsTrue(view.Latest().ships[0].handle == world.HandleOf(second), L"the late subscriber got the wrong ship");
  }

  TEST_METHOD(RemovingASubscriberDoesNotStrandTheLog)
  {
    // The trim takes the minimum across whoever remains. A removed subscriber must stop holding the
    // log back, or one client leaving would grow it for the rest of the match.
    Game::World world;
    const Game::ShipId doomed = SpawnAt(world, 0.0f, 0.0f);

    Link a;
    Link b;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &a.server;
    const Game::Publisher::Handle first = publisher.Add(desc);
    desc.transport = &b.server;
    (void)publisher.Add(desc);

    Assert::IsTrue(world.DespawnShip(world.HandleOf(doomed)), L"the despawn failed");
    Assert::AreEqual(static_cast<std::size_t>(1), world.DespawnsSince(0).size(), L"the death was not logged");

    // With the first subscriber gone, the second's cursor is the only one holding the log.
    Assert::IsTrue(publisher.Remove(first), L"the removal failed");
    Assert::IsFalse(publisher.Remove(first), L"removing the same handle twice succeeded");
    Assert::AreEqual(1u, publisher.Count(), L"the publisher did not drop exactly one subscriber");

    for (std::uint64_t tick = 0; tick < Game::INTEREST_UPDATE_EVERY_TICKS * 2; ++tick)
    {
      b.Pump(tick);
      world.Step();
      publisher.Publish(world);
      b.Pump(tick);
    }
    Assert::IsTrue(world.DespawnsSince(0).empty(), L"the log was still held by a subscriber that had left");
  }
};
} // namespace GameLogicTests
