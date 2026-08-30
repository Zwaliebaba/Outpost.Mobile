#include "pch.h"

#include <chrono>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// Keeps what it is given, so a test can weigh an update rather than watch one.
class CaptureLink final : public Neuron::Transport
{
public:
  [[nodiscard]] bool Send(const std::uint8_t* _bytes, std::uint32_t _count) override
  {
    if (_count > Neuron::MAX_DATAGRAM_BYTES)
      return false;
    sent.emplace_back(_bytes, _bytes + _count);
    bytes += _count;
    return true;
  }

  // Departures travel on the reliable lane since ADR 0029, so a double that captured only datagrams
  // would silently drop half of an update that states one.
  [[nodiscard]] bool SendReliable(const std::uint8_t* _bytes, std::uint32_t _count) override
  {
    if (_count > Neuron::MAX_RELIABLE_BYTES)
      return false;
    sentReliable.emplace_back(_bytes, _bytes + _count);
    return true;
  }

  [[nodiscard]] std::uint32_t ReceiveReliable(std::uint8_t*, std::uint32_t) override
  {
    return 0;
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
  std::vector<std::vector<std::uint8_t>> sentReliable;
  std::uint32_t bytes = 0;
};

Game::ShipId SpawnCorvetteAt(Game::World& _world, float _x, float _z)
{
  return _world.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
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

[[nodiscard]] Game::InterestSet::Desc DescWith(float _radius, std::uint32_t _everyTicks = 6)
{
  Game::InterestSet::Desc desc;
  desc.radiusMetres = _radius;
  desc.updateEveryTicks = _everyTicks;
  return desc;
}
} // namespace

TEST_CLASS(InterestTests)
{
public:
  TEST_METHOD(TheSetIsWhatIsInsideTheRadius)
  {
    Game::World world;
    const Game::ShipId inRange = SpawnCorvetteAt(world, 100.0f, 0.0f);
    const Game::ShipId outOfRange = SpawnCorvetteAt(world, 5000.0f, 0.0f);
    world.Step();

    Game::InterestSet interest;
    interest.Configure(DescWith(1000.0f));
    interest.Update(world, Game::LocalPos(0.0f, 0.0f));

    Assert::AreEqual(static_cast<std::size_t>(1), interest.Subscribed().size(), L"the set is not just what is in range");
    Assert::IsTrue(Holds(interest.Subscribed(), world.HandleOf(inRange)), L"a ship in range was not subscribed");
    Assert::IsFalse(Holds(interest.Subscribed(), world.HandleOf(outOfRange)), L"a ship out of range was subscribed");
    Assert::AreEqual(static_cast<std::size_t>(1), interest.Entered().size(), L"the first update did not report an enter");
  }

  TEST_METHOD(MovingInEntersAndMovingOutLeaves)
  {
    Game::World world;
    const Game::ShipId ship = SpawnCorvetteAt(world, 0.0f, 0.0f);
    world.Step();

    Game::InterestSet interest;
    interest.Configure(DescWith(500.0f));
    interest.Update(world, Game::LocalPos(0.0f, 0.0f));
    Assert::AreEqual(static_cast<std::size_t>(1), interest.Entered().size(), L"the ship did not enter");

    // Standing still is neither an enter nor a leave, however many updates pass.
    interest.Update(world, Game::LocalPos(0.0f, 0.0f));
    Assert::IsTrue(interest.Entered().empty(), L"a ship that stayed entered again");
    Assert::IsTrue(interest.Left().empty(), L"a ship that stayed left");

    // Move the viewpoint away rather than the ship, which is the case a subscriber actually creates.
    interest.Update(world, Game::LocalPos(4000.0f, 0.0f));
    Assert::AreEqual(static_cast<std::size_t>(1), interest.Left().size(), L"the ship did not leave when the view moved off");
    Assert::IsTrue(Holds(interest.Left(), world.HandleOf(ship)), L"the wrong ship left");
    Assert::IsTrue(interest.Subscribed().empty(), L"the set kept a ship it had reported gone");

    interest.Update(world, Game::LocalPos(0.0f, 0.0f));
    Assert::AreEqual(static_cast<std::size_t>(1), interest.Entered().size(), L"coming back was not an enter");
  }

  TEST_METHOD(SpawnOrderCannotChangeTheSet)
  {
    // ArrayOrderCannotChangeTheAnswer, for interest. QueryCircle walks cells in its own order, and
    // the sets must not inherit it -- which is the whole reason they are sorted rather than hashed.
    const auto run = [](bool _reversed)
    {
      Game::World world;
      const float xs[] = {-400.0f, 200.0f, -100.0f, 350.0f, 50.0f};
      if (_reversed)
      {
        for (int at = 4; at >= 0; --at)
          SpawnCorvetteAt(world, xs[at], 0.0f);
      }
      else
      {
        for (const float x : xs)
          SpawnCorvetteAt(world, x, 0.0f);
      }
      world.Step();

      Game::InterestSet interest;
      interest.Configure(DescWith(1000.0f));
      interest.Update(world, Game::LocalPos(0.0f, 0.0f));

      std::vector<std::uint32_t> slots;
      for (const Game::ShipHandle handle : interest.Subscribed())
        slots.push_back(handle.slot);
      return slots;
    };

    const std::vector<std::uint32_t> forward = run(false);
    const std::vector<std::uint32_t> reversed = run(true);
    Assert::AreEqual(static_cast<std::size_t>(5), forward.size(), L"not every ship was in range");
    Assert::IsTrue(forward == reversed, L"the subscribed set depends on the order ships were spawned in");
  }

  TEST_METHOD(TheUpdateRateIsCountedInTicks)
  {
    // Ten updates in sixty ticks at six ticks apiece. Exactly ten, because the rate is counted and
    // not timed -- a wall clock would make this assertion a range.
    Game::InterestSet interest;
    interest.Configure(DescWith(1000.0f, 6));

    int due = 0;
    for (std::uint64_t tick = 0; tick < 60; ++tick)
      due += interest.IsDueOn(tick) ? 1 : 0;

    Assert::AreEqual(10, due, L"six ticks per update did not give ten updates in sixty ticks");
  }

  TEST_METHOD(NearRefreshesEveryUpdateAndFarDoesNot)
  {
    // The whole of what priority buys: the near world stays smooth while the far world stays cheap.
    Game::World world;
    const Game::ShipId centre = SpawnCorvetteAt(world, 0.0f, 0.0f);
    const Game::ShipId edge = SpawnCorvetteAt(world, 990.0f, 0.0f);
    world.Step();

    Game::InterestSet interest;
    Game::InterestSet::Desc desc = DescWith(1000.0f);
    desc.minWeight = 0.125f;
    interest.Configure(desc);
    interest.Update(world, Game::LocalPos(0.0f, 0.0f)); // both enter

    int centreRefreshes = 0;
    int edgeRefreshes = 0;
    for (int update = 0; update < 32; ++update)
    {
      interest.Update(world, Game::LocalPos(0.0f, 0.0f));
      centreRefreshes += Holds(interest.Refreshed(), world.HandleOf(centre)) ? 1 : 0;
      edgeRefreshes += Holds(interest.Refreshed(), world.HandleOf(edge)) ? 1 : 0;
    }

    Assert::AreEqual(32, centreRefreshes, L"a ship at the centre did not refresh on every update");
    Assert::IsTrue(edgeRefreshes >= 3 && edgeRefreshes <= 5, L"a ship at the edge did not refresh at about an eighth of the rate");
  }

  TEST_METHOD(AnEnteringShipIsSentOnTheUpdateItEntered)
  {
    Game::World world;
    SpawnCorvetteAt(world, 0.0f, 0.0f);
    world.Step();

    Game::InterestSet interest;
    interest.Configure(DescWith(1000.0f));
    interest.Update(world, Game::LocalPos(0.0f, 0.0f));

    // Entering is the send, so it is not also queued as a refresh on the same update.
    Assert::AreEqual(static_cast<std::size_t>(1), interest.Entered().size(), L"the ship did not enter");
    Assert::IsTrue(interest.Refreshed().empty(), L"an entering ship was sent twice on one update");
  }

  TEST_METHOD(ADespawnedShipLeavesTheSet)
  {
    Game::World world;
    const Game::ShipId doomed = SpawnCorvetteAt(world, 0.0f, 0.0f);
    SpawnCorvetteAt(world, 100.0f, 0.0f);
    const Game::ShipHandle handle = world.HandleOf(doomed);
    world.Step();

    Game::InterestSet interest;
    interest.Configure(DescWith(1000.0f));
    interest.Update(world, Game::LocalPos(0.0f, 0.0f));
    Assert::AreEqual(static_cast<std::size_t>(2), interest.Subscribed().size(), L"both ships should be in range");

    Assert::IsTrue(world.DespawnShip(handle), L"the despawn failed");
    world.Step();
    interest.Update(world, Game::LocalPos(0.0f, 0.0f));

    Assert::IsTrue(Holds(interest.Left(), handle), L"a despawned ship did not leave the set");
    Assert::AreEqual(static_cast<std::size_t>(1), interest.Subscribed().size(), L"the set kept a dead ship");
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(handle), L"a dead handle still resolved");
  }

  TEST_METHOD(AnUpdateUpsertsAndALeaveRemoves)
  {
    Game::World world;
    const Game::ShipId first = SpawnCorvetteAt(world, 0.0f, 0.0f);
    const Game::ShipId second = SpawnCorvetteAt(world, 200.0f, 0.0f);
    world.Step();

    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;
    CaptureLink link;

    const Game::ShipHandle handles[] = {world.HandleOf(first), world.HandleOf(second)};
    Assert::IsTrue(writer.WriteInterest(world, handles, {}, {}, {}, link) > 0, L"the first update did not send");
    for (const std::vector<std::uint8_t>& datagram : link.sent)
      (void)receiver.Accept(datagram);
    Assert::AreEqual(static_cast<std::size_t>(2), receiver.Latest().ships.size(), L"the client did not take both ships");

    // Refreshing one updates it in place rather than appending a second copy.
    link.sent.clear();
    world.Step();
    const Game::ShipHandle justFirst[] = {world.HandleOf(first)};
    Assert::IsTrue(writer.WriteInterest(world, justFirst, {}, {}, {}, link) > 0, L"the refresh did not send");
    for (const std::vector<std::uint8_t>& datagram : link.sent)
      (void)receiver.Accept(datagram);
    Assert::AreEqual(static_cast<std::size_t>(2), receiver.Latest().ships.size(), L"a refresh appended instead of updating in place");

    // A leave removes exactly that one.
    link.sent.clear();
    link.sentReliable.clear();
    world.Step();
    // Ids, not handles: the departure runs on the wire name ships that may already be gone, so the
    // wire's currency is identity (ADR 0047). The sent list two lines up is still handles.
    const Game::EntityId leaving[] = {world.EntityIdOf(second)};
    Assert::IsTrue(writer.WriteInterest(world, {}, leaving, {}, {}, link) > 0, L"the leave did not send");
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"the leave did not take the reliable lane");
    for (const std::vector<std::uint8_t>& message : link.sentReliable)
      (void)receiver.Accept(message);
    for (const std::vector<std::uint8_t>& datagram : link.sent)
      (void)receiver.Accept(datagram);
    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Latest().ships.size(), L"a leave did not remove a ship");
    Assert::IsTrue(receiver.Latest().ships[0].entity == world.EntityIdOf(first), L"a leave removed the wrong ship");
  }

  TEST_METHOD(AFullSnapshotStillDropsWhatIsGone)
  {
    // The regression an upserting receiver invites: a complete snapshot carries no leave list, so
    // without the complete flag it could never remove a despawned ship -- it would simply stop
    // being mentioned and stay on screen forever.
    Game::World world;
    const Game::ShipId doomed = SpawnCorvetteAt(world, 0.0f, 0.0f);
    SpawnCorvetteAt(world, 200.0f, 0.0f);
    const Game::ShipHandle handle = world.HandleOf(doomed);
    world.Step();

    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;

    CaptureLink before;
    Assert::IsTrue(writer.Write(world, before) > 0, L"the first snapshot did not send");
    for (const std::vector<std::uint8_t>& datagram : before.sent)
      (void)receiver.Accept(datagram);
    Assert::AreEqual(static_cast<std::size_t>(2), receiver.Latest().ships.size(), L"the client did not take both ships");

    Assert::IsTrue(world.DespawnShip(handle), L"the despawn failed");
    world.Step();

    CaptureLink after;
    Assert::IsTrue(writer.Write(world, after) > 0, L"the second snapshot did not send");
    for (const std::vector<std::uint8_t>& datagram : after.sent)
      (void)receiver.Accept(datagram);

    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Latest().ships.size(), L"a full snapshot did not drop a despawned ship");
  }

  TEST_METHOD(AnIncompleteUpdateLeavesTheWorldAlone)
  {
    // A delta stream cannot resynchronise by waiting, so a partly applied update is worse than a
    // dropped one: the client would hold a world nothing will ever correct.
    //
    // Three fragments, derived from the record rather than spelled: this test wanted 40 ships when a
    // fragment held 13, and quantizing the record to 23 quietly turned it into a two-fragment test
    // that skipped the last one instead of a middle one. A count taken from the writer's own number
    // cannot go stale that way (Design/Archive/QuantizedWire-work-order.md 5).
    const std::uint32_t needed = Game::ShipsPerSnapshotFragment() * 2 + 1;

    Game::World world;
    for (std::uint32_t at = 0; at < needed; ++at)
      SpawnCorvetteAt(world, static_cast<float>(at) * 30.0f, 0.0f);
    world.Step();

    std::vector<Game::ShipHandle> all;
    for (Game::ShipId id = 0; id < world.ShipCount(); ++id)
      all.push_back(world.HandleOf(id));

    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;
    CaptureLink link;
    Assert::IsTrue(writer.WriteInterest(world, all, {}, {}, {}, link) > 2, L"the ship count did not need three fragments");

    for (std::size_t at = 0; at < link.sent.size(); ++at)
    {
      if (at == 1)
        continue; // the one that went missing
      (void)receiver.Accept(link.sent[at]);
    }

    Assert::IsFalse(receiver.HasSnapshot(), L"an incomplete update was applied");
    Assert::IsTrue(receiver.Latest().ships.empty(), L"an incomplete update left the world partly changed");
  }

  TEST_METHOD(InterestCostTracksTheNeighbourhoodNotTheWorld)
  {
    // The claim this slice exists to make good: without interest management an update costs O(N),
    // with it O(k), and once N exceeds the neighbourhood the cost stops growing. Design/Archive/Collision.md
    // 1 calls this the hardest problem in the MMO target; this is the number behind it.
    //
    // One subscriber is enough to show it. The quadratic is in connected players, and each player
    // costs what one costs -- so bringing the one down is the whole of bringing the N^2 down.
    std::wstring report = L"\n";
    for (const int shipCount : {100, 1000, 5000})
    {
      Game::World world;
      for (int at = 0; at < shipCount; ++at)
      {
        // A square spiral, so density is even and the neighbourhood is a fair sample of the world.
        const float side = std::sqrt(static_cast<float>(shipCount));
        const float x = (static_cast<float>(at % static_cast<int>(side)) - side * 0.5f) * 120.0f;
        const float z = (static_cast<float>(at / static_cast<int>(side)) - side * 0.5f) * 120.0f;
        SpawnCorvetteAt(world, x, z);
      }
      world.Step();

      Game::InterestSet interest;
      interest.Configure(DescWith(1000.0f));
      interest.Update(world, Game::LocalPos(0.0f, 0.0f));

      std::vector<Game::ShipHandle> sent;
      sent.insert(sent.end(), interest.Entered().begin(), interest.Entered().end());

      Game::SnapshotWriter scoped;
      CaptureLink scopedLink;
      (void)scoped.WriteInterest(world, sent, {}, {}, {}, scopedLink);

      Game::SnapshotWriter whole;
      CaptureLink wholeLink;
      (void)whole.Write(world, wholeLink);

      wchar_t line[224] = {};
      std::swprintf(line, std::size(line), L"      N=%5d   k=%4d   interest %7u B in %3zu datagram(s)   full world %8u B in %4zu\n",
                    shipCount, static_cast<int>(interest.Subscribed().size()), scopedLink.bytes, scopedLink.sent.size(), wholeLink.bytes,
                    wholeLink.sent.size());
      report += line;

      Assert::IsTrue(scopedLink.bytes < wholeLink.bytes || shipCount <= 100, L"an interest update was not cheaper than the whole world");
      Assert::IsTrue(interest.Subscribed().size() < static_cast<std::size_t>(shipCount) || shipCount <= 100,
                     L"the neighbourhood was the whole world, so this measures nothing");
    }
    Logger::WriteMessage(report.c_str());
  }
};
} // namespace GameLogicTests
