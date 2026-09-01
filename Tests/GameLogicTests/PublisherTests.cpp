#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// Its own copy rather than a shared one: both suites keep theirs in an anonymous namespace, which is
// what the tree does instead of a test-support header nobody owns.
// The wire's sets are identities since ADR 0047, so this asks the wire's question. A handle names a
// ship inside one Universe and never leaves it.
[[nodiscard]] bool Holds(std::span<const Game::EntityId> _set, Game::EntityId _entity)
{
  for (const Game::EntityId held : _set)
  {
    if (held == _entity)
      return true;
  }
  return false;
}

Game::ShipId SpawnAt(Game::Universe& _universe, float _x, float _z, Game::FactionId _faction = Game::FACTION_PLAYER)
{
  return _universe.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), _faction);
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

  // Everything the client end has been given on the reliable lane, as raw messages -- so a test can
  // count what was sent and of which kind, which a receiver's decoded state cannot say.
  std::vector<std::vector<std::uint8_t>> DrainReliable()
  {
    std::vector<std::vector<std::uint8_t>> messages;
    std::vector<std::uint8_t> message(Neuron::MAX_RELIABLE_BYTES, 0u);
    for (;;)
    {
      const std::uint32_t size = client.ReceiveReliable(message.data(), Neuron::MAX_RELIABLE_BYTES);
      if (size == 0)
        break;
      messages.emplace_back(message.data(), message.data() + size);
    }
    return messages;
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
Game::Universe::StationId MakeStationAt(Game::Universe& _universe, float _x, float _z, Game::FactionId _owner = Game::FACTION_VANGUARD)
{
  const Game::ShipId structure =
    _universe.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), _owner);
  Game::Universe::StationDesc desc;
  desc.ownerFaction = _owner;
  return _universe.MakeStation(structure, desc);
}

// Ships parked at a station's door and told to dock, so a ledger has something in it. FleetTests
// keeps its own copy of this: both suites keep their helpers in an anonymous namespace, which is
// what the tree does instead of a test-support header nobody owns.
void DockShips(Game::Universe& _universe, Game::Universe::StationId _station, Game::HullId _hull, int _count,
               Game::FactionId _faction = Game::FACTION_PLAYER)
{
  const Game::ShipId structure = _universe.Resolve(_universe.StationOf(_station).structure);
  const Game::UniversePos stationPos = _universe.Ship(structure).posUniverse;
  const float range = Game::DockApproachRangeMetres(Game::HullSpecOf(_universe.Ship(structure).hullId), Game::HullSpecOf(_hull));

  std::vector<Game::ShipId> ships;
  for (int at = 0; at < _count; ++at)
  {
    const float bearingRad = static_cast<float>(at) * 0.6f;
    Game::UniversePos pos = stationPos;
    Game::Translate(pos, std::sin(bearingRad) * range, std::cos(bearingRad) * range);
    ships.push_back(_universe.SpawnShip(pos, bearingRad, static_cast<std::uint32_t>(_hull), _faction));
  }
  (void)_universe.IssueDockOrder(ships, structure, _faction);
  _universe.Step();
}

std::vector<std::uint32_t> ZeroCounts()
{
  return std::vector<std::uint32_t>(Game::HULL_COUNT, 0u);
}

// One tick as the composition root runs it: orders in, the universe steps, the update goes out, the
// wire moves, the client reads. Publish AFTER Step, so a status block states the tick the client is
// about to draw; drain every tick, because a client that does not is a client whose queue overflows
// -- the loopback holds 256 datagrams and then refuses, which silently leaves a receiver holding
// state from hundreds of ticks ago rather than the state a test meant to assert against.
void RunTick(Game::Universe& _universe, Game::Publisher& _publisher, Link& _link, std::uint64_t _tick,
             Game::SnapshotReceiver* _receiver = nullptr)
{
  _publisher.ApplyOrders(_universe);
  _universe.Step();
  _publisher.Publish(_universe);
  _link.Pump(_tick);
  if (_receiver != nullptr)
    _link.DrainInto(*_receiver);
}

// How many of the messages drained this tick were rosters, and what the last one said.
std::uint32_t CountRosters(const std::vector<std::vector<std::uint8_t>>& _messages, Game::FleetRoster& _outLast)
{
  std::uint32_t rosters = 0;
  for (const std::vector<std::uint8_t>& message : _messages)
  {
    Game::FleetRoster roster;
    if (Game::ReadFleetRoster(message, roster))
    {
      ++rosters;
      _outLast = roster;
    }
  }
  return rosters;
}
} // namespace

TEST_CLASS(PublisherTests)
{
public:
  TEST_METHOD(AFireEventReachesBothEnds)
  {
    // A real battle rather than a hand-built log: the point of this row is that the shot the fire
    // pass recorded is the shot the client is told about, through the cursor, the filter and both
    // codecs (Design/Combat-slice-2.md 2.3, 2.4).
    Game::Universe universe;
    (void)SpawnAt(universe, 0.0f, 0.0f, Game::FACTION_PLAYER);
    (void)SpawnAt(universe, 0.0f, 120.0f, Game::FACTION_VANDAL);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    (void)publisher.Add(desc);

    for (std::uint64_t tick = 0; tick < 120; ++tick)
    {
      link.Pump(tick);
      universe.Step();
      publisher.Publish(universe);
      link.Pump(tick);
    }

    Game::SnapshotReceiver view;
    link.DrainInto(view);
    Assert::IsFalse(view.Fire().empty(), L"a battle in plain view produced no gunfire on the wire");

    // And the damage arrived as state rather than as an event, which is the other half of the slice.
    bool sawDamage = false;
    for (const Game::ShipSnapshot& ship : view.Latest().ships)
      sawDamage = sawDamage || ship.hullFraction < 255;
    Assert::IsTrue(sawDamage, L"nobody's hull fraction moved");
  }

  TEST_METHOD(AFireEventReachesTheShotAsWellAsTheShooter)
  {
    // Being shot at from outside your own interest set is exactly the event a player must not be
    // denied, so either end being in view is enough. The subscriber watches the victim; the shooter
    // sits beyond its radius, which a Corvette's turrets comfortably out-range.
    Game::Universe universe;
    const Game::ShipId victim = SpawnAt(universe, 0.0f, 0.0f, Game::FACTION_PLAYER);
    (void)SpawnAt(universe, 0.0f, 120.0f, Game::FACTION_VANDAL);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    // A radius that holds the victim and not much else, so the shooter is outside it.
    desc.interest.radiusMetres = 60.0f;
    (void)publisher.Add(desc);

    for (std::uint64_t tick = 0; tick < 120; ++tick)
    {
      link.Pump(tick);
      universe.Step();
      publisher.Publish(universe);
      link.Pump(tick);
    }

    Game::SnapshotReceiver view;
    link.DrainInto(view);
    const Game::EntityId victimEntity = universe.EntityIdOf(victim);
    bool aimedAtTheVictim = false;
    for (const Game::FireEvent& event : view.Fire())
      aimedAtTheVictim = aimedAtTheVictim || event.target == victimEntity;
    Assert::IsTrue(aimedAtTheVictim, L"a shot at a ship in view from a shooter outside it was not delivered");
  }

  TEST_METHOD(AJoiningSubscriberHearsNoOldGunfire)
  {
    // ADR 0027's joining rule, at the shot log: opened at the head, a subscriber is told about
    // gunfire from now on and about none of the battle it did not watch.
    Game::Universe universe;
    (void)SpawnAt(universe, 0.0f, 0.0f, Game::FACTION_PLAYER);
    (void)SpawnAt(universe, 0.0f, 120.0f, Game::FACTION_VANDAL);
    for (int tick = 0; tick < 120; ++tick)
      universe.Step();
    Assert::IsTrue(universe.ShotHead() > 0, L"the battle produced no shots to be late for");

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.openingDespawnCursor = universe.DespawnHead();
    desc.openingShotCursor = universe.ShotHead();
    (void)publisher.Add(desc);

    const std::uint64_t joined = universe.ShotHead();
    for (std::uint64_t tick = 0; tick < Game::INTEREST_UPDATE_EVERY_TICKS; ++tick)
    {
      link.Pump(tick);
      universe.Step();
      publisher.Publish(universe);
      link.Pump(tick);
    }

    Game::SnapshotReceiver view;
    link.DrainInto(view);
    // However many arrived, none may be from before it joined -- which is what the head-opened
    // cursor buys, and there are only so many shots one update period can hold.
    Assert::IsTrue(view.Fire().size() <= universe.ShotHead() - joined, L"a joining subscriber was told about an old battle");
  }

  TEST_METHOD(TheShotLogIsTrimmedToTheSlowestSubscriber)
  {
    // The despawn log's rule, at the shot log: what remains is what at least one subscriber has
    // still to hear about, and with nobody reading it the head is the minimum.
    Game::Universe universe;
    (void)SpawnAt(universe, 0.0f, 0.0f, Game::FACTION_PLAYER);
    (void)SpawnAt(universe, 0.0f, 120.0f, Game::FACTION_VANDAL);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    (void)publisher.Add(desc);

    for (std::uint64_t tick = 0; tick < 200; ++tick)
    {
      link.Pump(tick);
      universe.Step();
      publisher.Publish(universe);
      link.Pump(tick);
    }
    Assert::IsTrue(universe.ShotHead() > 0, L"the battle produced no shots at all");

    // The one subscriber has read everything, so nothing older than the head is owed to anybody.
    Assert::IsTrue(universe.ShotsSince(universe.ShotHead()).empty(), L"the log still held gunfire past its own head");
    Assert::IsTrue(universe.ShotsSince(0).size() <= Game::INTEREST_UPDATE_EVERY_TICKS * 4,
                   L"the shot log is not being trimmed behind its readers");
  }

  TEST_METHOD(TwoSubscribersSeeTheirOwnNeighbourhoods)
  {
    // The property the single-subscriber adapter could not have: two ends, two interest sets, two
    // writers, and neither one's bytes are the other's (Design/Archive/MmoScalabilityReview.md E2).
    Game::Universe universe;
    (void)SpawnAt(universe, 0.0f, 0.0f);
    (void)SpawnAt(universe, 50.0f, 0.0f);
    // Far enough that a subscriber standing at the origin cannot see it.
    (void)SpawnAt(universe, Game::INTEREST_RADIUS_METRES * 4.0f, 0.0f);

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
      universe.Step();
      publisher.Publish(universe);
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
    Game::Universe universe;
    (void)SpawnAt(universe, 0.0f, 0.0f);

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
    Game::Universe universe;
    const Game::ShipId doomed = SpawnAt(universe, 0.0f, 0.0f);
    (void)SpawnAt(universe, 40.0f, 0.0f);
    const Game::ShipHandle doomedHandle = universe.HandleOf(doomed);
    const Game::EntityId doomedEntity = universe.EntityIdOf(doomed);

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
      universe.Step();
      publisher.Publish(universe);
      a.Pump(tick);
      b.Pump(tick);
      a.DrainInto(viewA);
      b.DrainInto(viewB);
    }
    Assert::IsTrue(universe.DespawnShip(doomedHandle), L"the despawn failed");

    for (std::uint64_t tick = settle; tick < settle + Game::INTEREST_UPDATE_EVERY_TICKS * 2; ++tick)
    {
      a.Pump(tick);
      b.Pump(tick);
      universe.Step();
      publisher.Publish(universe);
      a.Pump(tick);
      b.Pump(tick);
      a.DrainInto(viewA);
      b.DrainInto(viewB);
    }

    Assert::IsTrue(Holds(viewA.Destroyed(), doomedEntity), L"the first subscriber was not told about the death");
    Assert::IsTrue(Holds(viewB.Destroyed(), doomedEntity), L"the second subscriber lost the death to the first one's read");
  }

  TEST_METHOD(AFleetOrderArrivesThroughTheSeam)
  {
    // The adapter's whole job for this kind: read it, resolve the one id it carries, and hand the
    // subscriber's faction to the gate. There is no ship list to filter, which is what the message
    // was shaped for (ADR 0049).
    Game::Universe universe;
    const Game::ShipId ship = SpawnAt(universe, 0.0f, 0.0f);
    const Game::ShipId ships[] = {ship};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::FACTION_PLAYER, 2, ships), L"the fleet was refused");

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.faction = Game::FACTION_PLAYER;
    (void)publisher.Add(desc);

    link.Pump(0);
    Game::FleetOrder order;
    order.slot = 2;
    order.kind = Game::FleetOrderKind::Move;
    order.point = Game::LocalPos(900.0f, 0.0f);
    Assert::IsTrue(Game::WriteFleetOrder(order, link.client), L"the order was refused by the lane");
    link.Pump(0);

    publisher.ApplyOrders(universe);
    Assert::IsTrue(universe.FleetOf(universe.FleetInSlot(Game::FACTION_PLAYER, 2)).orderKind == Game::FleetOrderKind::Move,
                   L"a fleet order did not reach the universe through the seam");
    Assert::AreEqual(Game::OrderState::Moving, universe.Ship(ship).order, L"the fleet's member was not put under way");

    // The same message from a subscriber of another faction reaches the same gate and is refused by
    // it: a client cannot order a slot that is not its own, and the adapter never had to check.
    Game::Universe other;
    const Game::ShipId theirs = SpawnAt(other, 0.0f, 0.0f);
    const Game::ShipId theirShips[] = {theirs};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, other.FormFleet(Game::FACTION_PLAYER, 2, theirShips), L"the fleet was refused");

    Link stranger;
    Game::Publisher strangerPublisher;
    Game::Publisher::Desc strangerDesc;
    strangerDesc.transport = &stranger.server;
    strangerDesc.faction = Game::FACTION_VANDAL;
    (void)strangerPublisher.Add(strangerDesc);

    stranger.Pump(0);
    Assert::IsTrue(Game::WriteFleetOrder(order, stranger.client), L"the order was refused by the lane");
    stranger.Pump(0);
    strangerPublisher.ApplyOrders(other);
    Assert::IsTrue(other.FleetOf(other.FleetInSlot(Game::FACTION_PLAYER, 2)).orderKind == Game::FleetOrderKind::Idle,
                   L"one faction ordered another faction's slot");
  }

  TEST_METHOD(TheRosterFollowsTheFleet)
  {
    // Who is in a fleet, stated whenever it changes and never otherwise. The publisher is told
    // about none of the four events that change it -- compose, launch, loss, retire -- and finds
    // all of them by diffing what it last sent (Design/Archive/Fleets.md 8.1).
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 3);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.faction = Game::FACTION_PLAYER;

    // The DEFAULT update period, deliberately: a roster goes out on the tick membership changed and
    // not on the tick this subscriber is next due, and a test that published every tick could not
    // tell the two apart. The mask assertion at the end runs the extra ticks that costs.
    (void)publisher.Add(desc);

    Game::FleetRoster last;
    std::uint64_t tick = 1;
    RunTick(universe, publisher, link, tick++);
    Assert::AreEqual(0u, CountRosters(link.DrainReliable(), last), L"a fleetless universe stated a roster");

    std::vector<std::uint32_t> counts = ZeroCounts();
    counts[static_cast<std::size_t>(Game::HullId::Corvette)] = 3;
    Assert::IsTrue(universe.ComposeFleet(station, 2, counts, Game::FACTION_PLAYER) == Game::Universe::ComposeResult::Composed,
                   L"the compose was refused");

    Game::SnapshotReceiver watching;

    // The design asks for a roster "on compose" and this slice does not send one, which is a
    // narrowing rather than an omission: a composed fleet has no membership yet, and that the slot
    // is now held rides the status block's mask -- stamped on every update, so it cannot be lost,
    // which is a better place for occupancy than a message stated once (Design/Archive/Fleets.md 8.1).
    //
    // It is also unobservable: the metronome starts cold, so the very first Step after a compose
    // already puts a hull in space. What the loop below pins is the consequence -- the first launch
    // states ONE member, so no empty roster preceded it, and at most one roster per slot per tick
    // exists to state anyway.
    //
    // One roster on the tick a hull appears and on no other tick at all -- checked against the
    // universe's own membership each tick rather than over a window, so a roster that waited for this
    // subscriber's next due tick fails here rather than passing late.
    std::uint32_t held = 0;
    for (std::uint32_t step = 0; step < 3 * Game::FLEET_LAUNCH_EVERY_TICKS + 30; ++step)
    {
      RunTick(universe, publisher, link, tick++);
      const Game::Universe::FleetId id = universe.FleetInSlot(Game::FACTION_PLAYER, 2);
      const std::uint32_t now = (id == Game::Universe::INVALID_FLEET_ID) ? 0u : universe.FleetOf(id).memberCount;
      const std::uint32_t rosters = CountRosters(link.DrainReliable(), last);
      if (now == held)
      {
        Assert::AreEqual(0u, rosters, L"a tick that changed no membership stated a roster");
        continue;
      }
      Assert::AreEqual(1u, rosters, L"a launch did not state exactly one roster, on its own tick");
      Assert::AreEqual(now, static_cast<std::uint32_t>(last.members.size()), L"the roster does not say what the fleet holds");
      Assert::AreEqual(2u, static_cast<std::uint32_t>(last.slot), L"the roster named the wrong slot");
      held = now;
    }
    Assert::AreEqual(3u, held, L"the fleet did not finish launching");

    // A subscriber that joins now is told the whole roster on its first publish, without anything
    // having changed -- its own stored lists are empty, so the diff finds every occupied slot. The
    // despawn cursor's joining rule, arriving at fleets with no second mechanism (ADR 0027).
    Link joiner;
    Game::Publisher::Desc joinerDesc;
    joinerDesc.transport = &joiner.server;
    joinerDesc.faction = Game::FACTION_PLAYER;
    joinerDesc.openingDespawnCursor = universe.DespawnHead();
    (void)publisher.Add(joinerDesc);

    publisher.ApplyOrders(universe);
    universe.Step();
    publisher.Publish(universe);
    link.Pump(tick);
    joiner.Pump(tick++);
    Game::FleetRoster joined;
    Assert::AreEqual(1u, CountRosters(joiner.DrainReliable(), joined), L"a joining subscriber was not told its roster");
    Assert::AreEqual(3u, static_cast<std::uint32_t>(joined.members.size()), L"a joining subscriber was told a partial roster");
    Assert::AreEqual(0u, CountRosters(link.DrainReliable(), last), L"somebody else joining restated an unchanged roster");

    // A loss states the roster again, one member shorter.
    const Game::Universe::Fleet& fleet = universe.FleetOf(universe.FleetInSlot(Game::FACTION_PLAYER, 2));
    Assert::IsTrue(universe.DespawnShip(fleet.members[0]), L"the despawn failed");
    RunTick(universe, publisher, link, tick++);
    Assert::AreEqual(1u, CountRosters(link.DrainReliable(), last), L"a pruned loss did not state the roster");
    Assert::AreEqual(2u, static_cast<std::uint32_t>(last.members.size()), L"the roster did not shrink by the ship that died");

    // And the last loss states an empty one, on its way to the slot freeing. Read back from the
    // fleet each time: the row's members move as the prune swaps them down.
    for (int at = 0; at < 2; ++at)
    {
      const Game::Universe::FleetId id = universe.FleetInSlot(Game::FACTION_PLAYER, 2);
      Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, id, L"the fleet retired early");
      Assert::IsTrue(universe.DespawnShip(universe.FleetOf(id).members[0]), L"the despawn failed");
      RunTick(universe, publisher, link, tick++);
      Assert::AreEqual(1u, CountRosters(link.DrainReliable(), last), L"a loss did not state the roster");
    }
    Assert::IsTrue(last.members.empty(), L"the last loss did not state an empty roster");
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FleetInSlot(Game::FACTION_PLAYER, 2), L"the slot was not freed");

    // The extra ticks the default period costs: the mask rides the update, so it states the freed
    // slot on this subscriber's next due tick rather than on the tick the row went.
    for (std::uint32_t step = 0; step < Game::INTEREST_UPDATE_EVERY_TICKS + 1; ++step)
      RunTick(universe, publisher, link, tick++, &watching);
    Assert::AreEqual(0u, static_cast<std::uint32_t>(watching.FleetMask()), L"the freed slot is still held in the mask");
    Assert::IsTrue(watching.RosterOf(2).empty(), L"the freed slot still holds a membership");
  }

  TEST_METHOD(TheStatusBlockStatesEveryFleet)
  {
    // The five buttons, and the only thing on this seam that describes a fleet the interest set has
    // never heard of. Every field is read from state the simulation already holds, except the
    // position, which is derived here and held nowhere (Design/Archive/Fleets.md 8.2).
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::UniversePos door = universe.Ship(universe.Resolve(universe.StationOf(station).structure)).posUniverse;
    DockShips(universe, station, Game::HullId::Corvette, 3);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.faction = Game::FACTION_PLAYER;

    // Due every tick, so this test reads the block on the tick it means rather than on whichever
    // tick the default period lands on. It is a per-subscriber setting for exactly this reason.
    desc.interest.updateEveryTicks = 1;
    (void)publisher.Add(desc);

    Game::SnapshotReceiver receiver;
    std::uint64_t tick = 1;
    RunTick(universe, publisher, link, tick++);
    link.DrainInto(receiver);
    Assert::AreEqual(0u, static_cast<std::uint32_t>(receiver.FleetMask()), L"a fleetless universe stated a fleet");

    std::vector<std::uint32_t> counts = ZeroCounts();
    counts[static_cast<std::size_t>(Game::HullId::Corvette)] = 3;
    Assert::IsTrue(universe.ComposeFleet(station, 0, counts, Game::FACTION_PLAYER) == Game::Universe::ComposeResult::Composed,
                   L"the compose was refused");
    RunTick(universe, publisher, link, tick++);
    link.DrainInto(receiver);

    // Launching, and counting the whole composed set -- so the button says three from the moment
    // the fleet exists rather than climbing as the hulls appear.
    Assert::AreEqual(0x01u, static_cast<std::uint32_t>(receiver.FleetMask()), L"the composed slot is not held");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::FLEET_STATUS_LAUNCHING),
                     static_cast<std::uint32_t>(receiver.FleetStatusOf(0).status & Game::FLEET_STATUS_KIND_MASK),
                     L"a pouring fleet is not launching");
    Assert::AreEqual(3u, static_cast<std::uint32_t>(receiver.FleetStatusOf(0).count), L"the count is not the whole composed set");

    // The door, which is where a fleet with nobody in space is stated. Reachable exactly here: the
    // metronome starts cold, so a composed fleet is never published empty -- what empties it again
    // is losing the one hull that is out while the manifest still holds the rest.
    const Game::Universe::FleetId launching = universe.FleetInSlot(Game::FACTION_PLAYER, 0);
    Assert::AreEqual(1u, universe.FleetOf(launching).memberCount, L"the metronome did not launch on the first step");
    Assert::IsTrue(universe.DespawnShip(universe.FleetOf(launching).members[0]), L"the despawn failed");
    RunTick(universe, publisher, link, tick++);
    link.DrainInto(receiver);
    Assert::AreEqual(2u, static_cast<std::uint32_t>(receiver.FleetStatusOf(0).count), L"the count is not what is left to launch");
    Assert::AreEqual(0x01u, static_cast<std::uint32_t>(receiver.FleetMask()), L"a fleet with an empty roster lost its slot");
    Assert::IsTrue(receiver.RosterOf(0).empty(), L"the lost member is still in the roster");
    Assert::IsTrue(Distance(receiver.FleetStatusOf(0).position, door) < 1.0f, L"a fleet with nobody out is not stated at its door");

    for (std::uint32_t step = 0; step < 2 * Game::FLEET_LAUNCH_EVERY_TICKS + 400; ++step)
      RunTick(universe, publisher, link, tick++, &receiver);

    const Game::Universe::FleetId id = universe.FleetInSlot(Game::FACTION_PLAYER, 0);
    Assert::AreEqual(2u, universe.FleetOf(id).memberCount, L"the fleet did not finish launching");

    // Out, and the centroid is the mean of the two. Computed against the same two positions here
    // rather than against a constant, so the assertion is the definition and not a snapshot of one
    // run's arithmetic.
    Game::UniversePos centre = universe.Ship(universe.Resolve(universe.FleetOf(id).members[0])).posUniverse;
    const Game::UniversePos second = universe.Ship(universe.Resolve(universe.FleetOf(id).members[1])).posUniverse;
    Game::Translate(centre, OffsetX(centre, second) * 0.5f, OffsetZ(centre, second) * 0.5f);
    Assert::IsTrue(Distance(receiver.FleetStatusOf(0).position, centre) < 0.2f, L"the stated position is not the fleet's centroid");
    Assert::AreEqual(0u, static_cast<std::uint32_t>(receiver.FleetStatusOf(0).status & Game::FLEET_STATUS_KIND_MASK),
                     L"a fleet with nothing to do is not idle");

    Game::Universe::FleetCommand move;
    move.kind = Game::FleetOrderKind::Move;
    move.point = Game::LocalPos(2000.0f, 0.0f);
    Assert::IsTrue(universe.IssueFleetOrder(Game::FACTION_PLAYER, 0, move) == Game::Universe::FleetOrderResult::Ordered,
                   L"the order was refused");
    RunTick(universe, publisher, link, tick++);
    link.DrainInto(receiver);
    Assert::AreEqual(static_cast<std::uint32_t>(Game::FleetOrderKind::Move),
                     static_cast<std::uint32_t>(receiver.FleetStatusOf(0).status & Game::FLEET_STATUS_KIND_MASK),
                     L"the standing order does not reach the status byte");

    // An act lights both bits. Close enough to be inside the engagement leash, because a threat
    // already past it stands the fleet down on the same tick it was recorded -- which is the case
    // two assertions further down, and it would be no test at all if it were also this one.
    Game::UniversePos raiderPos = centre;
    Game::Translate(raiderPos, 120.0f, 0.0f);
    const Game::ShipId raider = universe.SpawnShip(raiderPos, 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber), Game::FACTION_VANDAL);
    universe.RecordHostileAct(universe.HandleOf(raider), universe.FleetOf(id).members[0]);
    RunTick(universe, publisher, link, tick++);
    link.DrainInto(receiver);
    Assert::AreNotEqual(0u, static_cast<std::uint32_t>(receiver.FleetStatusOf(0).status & Game::FLEET_STATUS_UNDER_ATTACK),
                        L"an act did not light the under-attack bit");
    Assert::AreNotEqual(0u, static_cast<std::uint32_t>(receiver.FleetStatusOf(0).status & Game::FLEET_STATUS_ENGAGED),
                        L"an act did not light the engaged bit");

    // The attacker gone stands the fleet down while the alert is still burning, which is the one
    // case the two bits were bought to tell apart (Design/Archive/Fleets.md 7.2, 7.3).
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(raider)), L"the despawn failed");
    RunTick(universe, publisher, link, tick++);
    link.DrainInto(receiver);
    Assert::AreNotEqual(0u, static_cast<std::uint32_t>(receiver.FleetStatusOf(0).status & Game::FLEET_STATUS_UNDER_ATTACK),
                        L"the alert went out with the fight");
    Assert::AreEqual(0u, static_cast<std::uint32_t>(receiver.FleetStatusOf(0).status & Game::FLEET_STATUS_ENGAGED),
                     L"a fleet with nothing to chase is still engaged");

    // Five at once: the widest the block ever is, and every slot decoded on its own terms.
    for (std::uint8_t slot = 1; slot < Game::FLEET_SLOTS; ++slot)
    {
      const Game::ShipId ship = SpawnAt(universe, -3000.0f - static_cast<float>(slot) * 300.0f, 0.0f);
      const Game::ShipId one[] = {ship};
      Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::FACTION_PLAYER, slot, one), L"a fleet was refused");
    }
    RunTick(universe, publisher, link, tick++);
    link.DrainInto(receiver);
    Assert::AreEqual(0x1Fu, static_cast<std::uint32_t>(receiver.FleetMask()), L"five fleets did not all reach the mask");
    for (std::uint8_t slot = 1; slot < Game::FLEET_SLOTS; ++slot)
      Assert::AreEqual(1u, static_cast<std::uint32_t>(receiver.FleetStatusOf(slot).count), L"a one-ship fleet did not state one ship");
  }

  TEST_METHOD(AFleetStatusReachesADistantSubscriber)
  {
    // The case the whole feature is for. This subscriber's camera is over empty space, so its
    // interest set never enters, leaves or refreshes anything -- and without the guard the update
    // that carries the status block is never sent at all, leaving a player who owns five fleets
    // told about none of them (Design/Archive/Fleets-slice-5.md 2.8).
    Game::Universe universe;
    const Game::ShipId ship = SpawnAt(universe, 0.0f, 0.0f);
    const Game::ShipId one[] = {ship};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::FACTION_PLAYER, 3, one), L"the fleet was refused");

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.faction = Game::FACTION_PLAYER;
    desc.centre = Game::LocalPos(Game::INTEREST_RADIUS_METRES * 20.0f, 0.0f);
    (void)publisher.Add(desc);

    Game::SnapshotReceiver receiver;
    for (std::uint64_t tick = 1; tick <= 40; ++tick)
      RunTick(universe, publisher, link, tick, &receiver);

    Assert::IsTrue(receiver.Latest().ships.empty(), L"a distant subscriber was sent records it cannot see");
    Assert::AreEqual(0x08u, static_cast<std::uint32_t>(receiver.FleetMask()), L"a distant subscriber was not told about its own fleet");
    Assert::IsTrue(Distance(receiver.FleetStatusOf(3).position, universe.Ship(ship).posUniverse) < 0.2f,
                   L"the distant fleet is not stated where it is");

    // And the guard has to still hold the other way: a subscriber with no fleet and nothing in
    // range is sent nothing at all, which is what an empty update being uninformative means.
    Link quiet;
    Game::Publisher::Desc quietDesc;
    quietDesc.transport = &quiet.server;
    quietDesc.faction = Game::FACTION_VANDAL;
    quietDesc.centre = desc.centre;
    (void)publisher.Add(quietDesc);
    for (std::uint64_t tick = 41; tick <= 80; ++tick)
      RunTick(universe, publisher, quiet, tick);
    std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> datagram{};
    Assert::AreEqual(0u, quiet.client.Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size())),
                     L"a subscriber with no fleet and nothing in range was sent an empty update");
  }

  TEST_METHOD(TheLedgerAnswersItsOwner)
  {
    // The one request/reply on this seam. What comes back is the asker's own rows and nothing else:
    // whose ships are inside a station is nobody else's business, and the gate that says so is the
    // same one a compose passes (ADR 0051, Design/Archive/Stations.md 6.2).
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::ShipId structure = universe.Resolve(universe.StationOf(station).structure);
    DockShips(universe, station, Game::HullId::Corvette, 3);
    DockShips(universe, station, Game::HullId::Miner, 2);
    DockShips(universe, station, Game::HullId::Frigate, 4, Game::FACTION_VANDAL);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.faction = Game::FACTION_PLAYER;
    (void)publisher.Add(desc);

    Game::LedgerRequest asked;
    asked.station = universe.EntityIdOf(structure);
    link.Pump(0);
    Assert::IsTrue(Game::WriteLedgerRequest(asked, link.client), L"the request was refused by the lane");
    link.Pump(0);

    // Answered where it is asked, on this tick, without a Publish: a reply is an answer rather than
    // an announcement, so it does not wait for an update slot.
    publisher.ApplyOrders(universe);
    link.Pump(0);

    Game::SnapshotReceiver receiver;
    link.DrainInto(receiver);
    Assert::AreEqual(1u, receiver.LedgerReplyCount(), L"the request was not answered on the tick it was applied");
    Assert::IsTrue(receiver.Ledger().station == asked.station, L"the reply named the wrong station");
    Assert::AreEqual(3u, receiver.Ledger().hullCounts[static_cast<std::size_t>(Game::HullId::Corvette)],
                     L"the Corvettes did not come back");
    Assert::AreEqual(2u, receiver.Ledger().hullCounts[static_cast<std::size_t>(Game::HullId::Miner)], L"the Miners did not come back");
    Assert::AreEqual(0u, receiver.Ledger().hullCounts[static_cast<std::size_t>(Game::HullId::Frigate)],
                     L"another faction's rows came back in the reply");

    // A hostile asker reads zeros: the same standing gate that refuses a compose in a hostile port,
    // applied to looking as well as to taking, so a screen cannot offer what the gate will refuse.
    // The rows are the asker's OWN and are still there -- what the gate withholds is the sight of
    // them, which is why this is not the same assertion as the faction filter above.
    Game::Universe hostile;
    const Game::Universe::StationId post = MakeStationAt(hostile, 0.0f, 0.0f);
    const Game::ShipId postStructure = hostile.Resolve(hostile.StationOf(post).structure);
    DockShips(hostile, post, Game::HullId::Corvette, 2);

    // Turn the law, the way AHostilePortRefusesComposition does: an aggression the station saw.
    const Game::ShipId criminal = hostile.SpawnShip(Game::LocalPos(2000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    hostile.RecordAggression(hostile.HandleOf(criminal), post);
    Assert::IsTrue(hostile.StandingOf(Game::FACTION_VANGUARD, Game::FACTION_PLAYER) == Game::Standing::Hostile,
                   L"the aggression did not turn the station's standing");

    Link raiderLink;
    Game::Publisher raiderPublisher;
    Game::Publisher::Desc raiderDesc;
    raiderDesc.transport = &raiderLink.server;
    raiderDesc.faction = Game::FACTION_PLAYER;
    (void)raiderPublisher.Add(raiderDesc);

    Game::LedgerRequest raiderAsked;
    raiderAsked.station = hostile.EntityIdOf(postStructure);
    raiderLink.Pump(0);
    Assert::IsTrue(Game::WriteLedgerRequest(raiderAsked, raiderLink.client), L"the request was refused by the lane");
    raiderLink.Pump(0);
    raiderPublisher.ApplyOrders(hostile);
    raiderLink.Pump(0);

    Game::SnapshotReceiver raiderReceiver;
    raiderLink.DrainInto(raiderReceiver);
    Assert::AreEqual(1u, raiderReceiver.LedgerReplyCount(), L"a hostile asker was not answered at all");
    Assert::AreEqual(static_cast<std::size_t>(2), hostile.StationOf(post).docked.size(), L"the rows are not there to be withheld");
    Assert::AreEqual(0u, raiderReceiver.Ledger().hullCounts[static_cast<std::size_t>(Game::HullId::Corvette)],
                     L"a hostile port stated its contents");

    // A station that is not one is answered with zeros too, rather than with silence: a screen has
    // to open on something, and a reply that never comes is indistinguishable from a lost one.
    Game::LedgerRequest nowhere;
    nowhere.station = universe.EntityIdOf(SpawnAt(universe, 4000.0f, 0.0f));
    link.Pump(1);
    Assert::IsTrue(Game::WriteLedgerRequest(nowhere, link.client), L"the request was refused by the lane");
    link.Pump(1);
    publisher.ApplyOrders(universe);
    link.Pump(1);
    link.DrainInto(receiver);
    Assert::AreEqual(2u, receiver.LedgerReplyCount(), L"a request naming no station went unanswered");
    Assert::AreEqual(0u, receiver.Ledger().hullCounts[static_cast<std::size_t>(Game::HullId::Corvette)],
                     L"a ship that is not a station has a ledger");
  }

  TEST_METHOD(AComposeOrderArrivesThroughTheSeam)
  {
    // The last of the four order kinds, and the only one that names no ship at all. Every gate is
    // Universe's, and the issuing faction is the subscriber's rather than anything the message says.
    // Two stations, and the order names the second: with only one in the table, a seam that ignored
    // the id in the message and composed at station zero would be indistinguishable from one that
    // read it.
    Game::Universe universe;
    const Game::Universe::StationId decoy = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::StationId station = MakeStationAt(universe, 6000.0f, 0.0f);
    const Game::ShipId structure = universe.Resolve(universe.StationOf(station).structure);
    DockShips(universe, decoy, Game::HullId::Frigate, 2);
    DockShips(universe, station, Game::HullId::Corvette, 2);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.faction = Game::FACTION_PLAYER;
    (void)publisher.Add(desc);

    Game::ComposeOrder sent;
    sent.station = universe.EntityIdOf(structure);
    sent.slot = 4;
    sent.hullCounts[static_cast<std::size_t>(Game::HullId::Corvette)] = 2;

    link.Pump(0);
    Assert::IsTrue(Game::WriteComposeOrder(sent, link.client), L"the compose order was refused by the lane");
    link.Pump(0);
    publisher.ApplyOrders(universe);

    const Game::Universe::FleetId id = universe.FleetInSlot(Game::FACTION_PLAYER, 4);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, id, L"a compose order did not reach the universe through the seam");
    Assert::AreEqual(2u, universe.FleetOf(id).manifestCount, L"the draft did not become a manifest");
    Assert::IsTrue(universe.StationOf(station).docked.empty(), L"the composed rows did not leave the ledger");
    Assert::IsTrue(universe.Resolve(universe.FleetOf(id).launchStructure) == structure, L"the fleet will launch from the wrong station");
    Assert::AreEqual(static_cast<std::size_t>(2), universe.StationOf(decoy).docked.size(), L"another station's ledger was drawn from");

    // The same message from another faction's subscriber reaches the same gate and is refused by it:
    // the ledger it would draw from is not its own, and the adapter never had to check.
    Game::Universe other;
    const Game::Universe::StationId theirPost = MakeStationAt(other, 0.0f, 0.0f);
    const Game::ShipId theirStructure = other.Resolve(other.StationOf(theirPost).structure);
    DockShips(other, theirPost, Game::HullId::Corvette, 2);

    Link stranger;
    Game::Publisher strangerPublisher;
    Game::Publisher::Desc strangerDesc;
    strangerDesc.transport = &stranger.server;
    strangerDesc.faction = Game::FACTION_VANDAL;
    (void)strangerPublisher.Add(strangerDesc);

    Game::ComposeOrder theirs = sent;
    theirs.station = other.EntityIdOf(theirStructure);
    stranger.Pump(0);
    Assert::IsTrue(Game::WriteComposeOrder(theirs, stranger.client), L"the compose order was refused by the lane");
    stranger.Pump(0);
    strangerPublisher.ApplyOrders(other);
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, other.FleetInSlot(Game::FACTION_VANDAL, 4),
                     L"one faction composed a fleet out of another faction's ledger");
    Assert::AreEqual(static_cast<std::size_t>(2), other.StationOf(theirPost).docked.size(), L"somebody else's rows left the ledger");
  }

  TEST_METHOD(OrdersPastTheBudgetWaitTheirTurnAndTheTickIsCounted)
  {
    // A client saturating its send rate buys formation solves and route planning at a leverage no
    // other message has. The faction gate says whose ships; this says how many per tick (E6).
    //
    // Over budget does not mean lost: the rest stay queued and are read next tick. What is counted
    // is the tick on which the budget bound.
    Game::Universe universe;
    const Game::ShipId ship = SpawnAt(universe, 0.0f, 0.0f);
    const Game::ShipHandle handle = universe.HandleOf(ship);

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    desc.ordersPerTick = 2;
    const Game::Publisher::Handle subscriber = publisher.Add(desc);

    link.Pump(0);
    Game::FleetOrder order;
    order.slot = 0;
    order.kind = Game::FleetOrderKind::Move;
    order.point = Game::LocalPos(500.0f, 0.0f);
    for (int at = 0; at < 5; ++at)
      Assert::IsTrue(Game::WriteFleetOrder(order, link.client), L"the order was refused by the lane");
    link.Pump(0);

    publisher.ApplyOrders(universe);
    Assert::AreEqual(1u, publisher.ThrottledTickCount(subscriber), L"going over budget was not counted");

    // What was over budget is still queued, not thrown away, and next tick reads more of it.
    link.Pump(1);
    publisher.ApplyOrders(universe);
    Assert::AreEqual(2u, publisher.ThrottledTickCount(subscriber), L"the budget did not refill");
  }

  TEST_METHOD(ASubscriberAddedLaterHearsOnlyWhatFollowsIt)
  {
    // Its cursor opens at the head, so it is not told about ships it never held (ADR 0027).
    Game::Universe universe;
    const Game::ShipId first = SpawnAt(universe, 0.0f, 0.0f);

    // The survivor is held as a HANDLE, taken before the despawn. Its ShipId does not survive:
    // despawning the first ship swap-and-pops the last one into index 0, so the id captured at
    // spawn would name a ship that is no longer there -- which is the whole reason handles exist
    // (ADR 0005), and this test got it wrong first time round.
    const Game::ShipHandle survivor = universe.HandleOf(SpawnAt(universe, 40.0f, 0.0f));
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(first)), L"the despawn failed");
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, universe.Resolve(survivor), L"the survivor's handle stopped resolving");

    Link link;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &link.server;
    // The line that makes it a late subscriber rather than one that happens to miss the death
    // because it never held the ship.
    desc.openingDespawnCursor = universe.DespawnHead();
    const Game::Publisher::Handle late = publisher.Add(desc);
    Assert::IsFalse(universe.DespawnsSince(0).empty(), L"the death was trimmed before the test could use it");
    Assert::IsTrue(universe.DespawnsSince(universe.DespawnHead()).empty(), L"the head is not past the death");
    Assert::AreEqual(0u, publisher.RefusedLeaveCount(late), L"a fresh subscriber had already refused something");

    Game::SnapshotReceiver view;
    for (std::uint64_t tick = 0; tick < Game::INTEREST_UPDATE_EVERY_TICKS * 2; ++tick)
    {
      link.Pump(tick);
      universe.Step();
      publisher.Publish(universe);
      link.Pump(tick);
      link.DrainInto(view);
    }

    Assert::IsTrue(view.Destroyed().empty(), L"a late subscriber was told about a death that preceded it");
    Assert::AreEqual(static_cast<std::size_t>(1), view.Latest().ships.size(), L"the late subscriber did not get the surviving ship");
    Assert::IsTrue(view.Latest().ships[0].entity == universe.EntityIdOf(universe.Resolve(survivor)),
                   L"the late subscriber got the wrong ship");
  }

  TEST_METHOD(RemovingASubscriberDoesNotStrandTheLog)
  {
    // The trim takes the minimum across whoever remains. A removed subscriber must stop holding the
    // log back, or one client leaving would grow it for the rest of the match.
    Game::Universe universe;
    const Game::ShipId doomed = SpawnAt(universe, 0.0f, 0.0f);

    Link a;
    Link b;
    Game::Publisher publisher;
    Game::Publisher::Desc desc;
    desc.transport = &a.server;
    const Game::Publisher::Handle first = publisher.Add(desc);
    desc.transport = &b.server;
    (void)publisher.Add(desc);

    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(doomed)), L"the despawn failed");
    Assert::AreEqual(static_cast<std::size_t>(1), universe.DespawnsSince(0).size(), L"the death was not logged");

    // With the first subscriber gone, the second's cursor is the only one holding the log.
    Assert::IsTrue(publisher.Remove(first), L"the removal failed");
    Assert::IsFalse(publisher.Remove(first), L"removing the same handle twice succeeded");
    Assert::AreEqual(1u, publisher.Count(), L"the publisher did not drop exactly one subscriber");

    for (std::uint64_t tick = 0; tick < Game::INTEREST_UPDATE_EVERY_TICKS * 2; ++tick)
    {
      b.Pump(tick);
      universe.Step();
      publisher.Publish(universe);
      b.Pump(tick);
    }
    Assert::IsTrue(universe.DespawnsSince(0).empty(), L"the log was still held by a subscriber that had left");
  }
};
} // namespace GameLogicTests
