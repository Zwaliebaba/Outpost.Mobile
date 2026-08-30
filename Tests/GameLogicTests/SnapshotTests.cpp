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

  // The reliable lane, captured separately so a test can see which lane a message took -- which is
  // the whole of what slice 3b changed (ADR 0029).
  [[nodiscard]] bool SendReliable(const std::uint8_t* _bytes, std::uint32_t _count) override
  {
    if (_count > Neuron::MAX_RELIABLE_BYTES || refuseReliable)
      return false;
    sentReliable.emplace_back(_bytes, _bytes + _count);
    return true;
  }

  [[nodiscard]] std::uint32_t ReceiveReliable(std::uint8_t*, std::uint32_t) override
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
  std::size_t refuseFrom = static_cast<std::size_t>(-1); // refuse once this many have been sent
  bool refuseReliable = false;
};

// Both lanes, in the order the writer used them: departures go on the reliable lane now (ADR 0029),
// so a test that fed only `sent` would be asserting against half the update -- which is exactly how
// this suite went red when the lane landed.
void FeedBothLanes(Game::SnapshotReceiver& _receiver, const CaptureTransport& _link)
{
  for (const std::vector<std::uint8_t>& message : _link.sentReliable)
    (void)_receiver.Accept(message);
  for (const std::vector<std::uint8_t>& datagram : _link.sent)
    (void)_receiver.Accept(datagram);
}

Game::ShipId SpawnAt(Game::World& _world, float _x, float _z, Game::HullId _hull = Game::HullId::Corvette,
                     Game::FactionId _faction = Game::FACTION_PLAYER)
{
  return _world.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(_hull), _faction);
}

[[nodiscard]] bool Holds(std::span<const Game::EntityId> _set, Game::EntityId _entity)
{
  for (const Game::EntityId entity : _set)
  {
    if (entity == _entity)
      return true;
  }
  return false;
}

// Handles, for the server-side sets: an interest set is a set of references into one World, and
// only the wire deals in identities (ADR 0047).
[[nodiscard]] bool Holds(std::span<const Game::ShipHandle> _set, Game::ShipHandle _handle)
{
  for (const Game::ShipHandle handle : _set)
  {
    if (handle == _handle)
      return true;
  }
  return false;
}

// The same question of the despawn log, which carries a cause per entry now. The cause is matched
// too rather than ignored: a test asking "did this one die" should not be answered yes by a
// docking (ADR 0040).
[[nodiscard]] bool Holds(std::span<const Game::DespawnRecord> _log, Game::ShipHandle _handle,
                         Game::DespawnCause _cause = Game::DespawnCause::Destroyed)
{
  for (const Game::DespawnRecord& record : _log)
  {
    if (record.handle == _handle && record.cause == _cause)
      return true;
  }
  return false;
}

// Who a departed ship was, off the log. A test that plays the publisher's part needs it for the
// same reason the publisher does: the ship is gone, so the world cannot be asked (ADR 0047).
[[nodiscard]] Game::EntityId EntityOfDeparture(const Game::World& _world, Game::ShipHandle _handle)
{
  for (const Game::DespawnRecord& record : _world.DespawnsSince(0))
  {
    if (record.handle == _handle)
      return record.entity;
  }
  return Game::INVALID_ENTITY_ID;
}

// A faction id is one byte, and a byte is a character to anything that prints one -- so a failure
// would report an unprintable glyph rather than "1". Widened for the assertion, never for the wire.
[[nodiscard]] std::uint32_t Faction(Game::FactionId _faction)
{
  return _faction;
}

// What round-to-nearest costs on a wire that carries a position as a 0.125 m lattice step and an
// angle as a sixteenth-bit of a turn: half a step of either. These are the numbers the slice's
// acceptance is stated in (Design/Archive/QuantizedWire-work-order.md 5), and they are what four fields of
// the ship record are asserted against instead of equality.
constexpr float WIRE_POSITION_BOUND_METRES = 0.0625f;
constexpr float WIRE_ANGLE_BOUND_RAD = DirectX::XM_PI / 65536.0f;

// The lattice step itself, for the assertions about what the wire can and cannot represent.
constexpr float WIRE_POSITION_STEP_METRES = 0.125f;

// Per axis, not as a distance: the quantizer rounds each axis independently, so a diagonal error of
// 0.088 m is two axes each inside the bound and not a violation of it.
[[nodiscard]] float WorstAxisError(const Game::WorldPos& _a, const Game::WorldPos& _b)
{
  return std::max(std::fabs(Game::OffsetX(_a, _b)), std::fabs(Game::OffsetZ(_a, _b)));
}

// An angle compared as an angle. The decode's range is (-pi, pi], so a source of exactly -pi comes
// back as +pi: the same bearing and a different number, which a plain subtraction would call a
// failure of 2pi (Design/Archive/QuantizedWire-work-order.md 5).
[[nodiscard]] float AngleError(float _a, float _b)
{
  return std::fabs(DirectX::XMScalarModAngle(_a - _b));
}

// Every datagram the capture holds, in order. Write uses only the unreliable lane, so this is the
// whole of a full snapshot -- FeedBothLanes is for the update path, which also states departures.
void FeedDatagrams(Game::SnapshotReceiver& _receiver, const CaptureTransport& _link)
{
  for (const std::vector<std::uint8_t>& datagram : _link.sent)
    (void)_receiver.Accept(datagram);
}

[[nodiscard]] const Game::ShipSnapshot* Find(const Game::WorldSnapshot& _snapshot, Game::EntityId _entity)
{
  for (const Game::ShipSnapshot& ship : _snapshot.ships)
  {
    if (ship.entity == _entity)
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
    // recorded because they are the argument for interest management: 23 ships per datagram -- 13
    // before the record was quantized -- means a 5,000-ship snapshot is 218 fragments, which is what
    // slice 6 exists to stop sending.
    Assert::AreEqual(23u, Game::ShipsPerSnapshotFragment(), L"the quantized record no longer fits 23 ships in a datagram");
    Assert::IsTrue(Game::ShipsPerSnapshotFragment() < 64, L"the record got suspiciously small");
    Assert::IsTrue(Game::MaxShipsPerOrder() > Game::ShipsPerSnapshotFragment(), L"an order holds fewer ships than a snapshot fragment");
  }

  TEST_METHOD(OneShipRoundTripsFieldForField)
  {
    // Spawned hostile so that every field is compared against a value that is not its default: a
    // faction that round-trips only because both ends default to zero proves nothing.
    Game::World world;
    const Game::ShipId ship = SpawnAt(world, 120.0f, -340.0f, Game::HullId::Frigate, Game::FACTION_VANDAL);
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
    Assert::IsTrue(copy.entity == world.EntityIdOf(ship), L"the handle did not survive");

    // Four fields are quantized and seven are not, and both halves are asserted here: a bound where
    // the wire rounds, and bit equality where it does not. A record whose fields got out of step
    // between the writer and the reader shows up as the exact ones failing, which is why they are
    // still compared against zero (Design/Archive/QuantizedWire-work-order.md 1).
    Assert::IsTrue(WorstAxisError(source.posWorld, copy.posWorld) <= WIRE_POSITION_BOUND_METRES,
                   L"posWorld came back further than half a lattice step");
    Assert::IsTrue(WorstAxisError(source.prevPos, copy.prevPos) <= WIRE_POSITION_BOUND_METRES,
                   L"prevPos came back further than half a lattice step");
    Assert::IsTrue(AngleError(source.headingRad, copy.headingRad) <= WIRE_ANGLE_BOUND_RAD,
                   L"headingRad came back further than half a turns16 step");
    Assert::IsTrue(AngleError(source.prevHeading, copy.prevHeading) <= WIRE_ANGLE_BOUND_RAD,
                   L"prevHeading came back further than half a turns16 step");
    Assert::AreEqual(source.speed, copy.speed, 0.0f, L"speed did not survive");
    Assert::AreEqual(source.accelSample, copy.accelSample, 0.0f, L"accelSample did not survive");
    Assert::AreEqual(source.turnRateRadPerSec, copy.turnRateRadPerSec, 0.0f, L"turnRateRadPerSec did not survive");
    Assert::AreEqual(source.order, copy.order, L"the order state did not survive");
    Assert::AreEqual(Faction(source.factionId), Faction(copy.factionId), L"factionId did not survive");
    Assert::AreEqual(source.hullId, copy.hullId, L"hullId did not survive");
  }

  TEST_METHOD(EveryPositionArrivesWithinHalfALatticeStep)
  {
    // The wire puts a position on a 0.125 m lattice, so round-to-nearest costs at most 6.25 cm on
    // each axis. Swept rather than sampled at one point, because the cases that break a quantizer
    // are the ones a single spawn never visits: a sector border, a negative sector, and an offset
    // that rounds up into the next sector rather than down into this one.
    const float LOCALS[] = {0.0f, 0.0624f, 0.0625f, 0.0626f, 1.0f, 4096.0f, 8191.874f, 8191.9f, 8191.999f};
    const float SECTORS[] = {0.0f, Game::SECTOR_SIZE_METRES, -Game::SECTOR_SIZE_METRES * 3.0f};

    Game::World world;
    std::vector<Game::ShipId> ships;
    for (const float sector : SECTORS)
    {
      for (const float localX : LOCALS)
      {
        for (const float localZ : LOCALS)
          ships.push_back(
            world.SpawnShip(Game::LocalPos(sector + localX, sector + localZ), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette)));
      }
    }

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(world, transport) > 1u, L"the sweep did not fragment, so it is not testing the writer");

    Game::SnapshotReceiver receiver;
    FeedDatagrams(receiver, transport);
    Assert::AreEqual(ships.size(), receiver.Latest().ships.size(), L"the sweep did not come back whole");

    for (const Game::ShipId id : ships)
    {
      const Game::ShipSnapshot* copy = Find(receiver.Latest(), world.EntityIdOf(id));
      Assert::IsTrue(copy != nullptr, L"a swept position went missing");
      Assert::IsTrue(WorstAxisError(world.Ship(id).posWorld, copy->posWorld) <= WIRE_POSITION_BOUND_METRES,
                     L"a swept position came back further than half a lattice step");
    }
  }

  TEST_METHOD(EveryHeadingArrivesWithinHalfATurns16Step)
  {
    // An angle rides 65,536 steps to the circle, so rounding costs at most pi/2^16. The sweep
    // deliberately includes both signs of pi: the decode's range is (-pi, pi], so a source of -pi
    // comes back as +pi, and an assertion that compared numbers rather than angles would call the
    // same bearing a failure of a whole turn.
    const float HEADINGS[] = {0.0f,
                              0.001f,
                              -0.001f,
                              1.0f,
                              -1.0f,
                              3.0f,
                              -3.0f,
                              DirectX::XM_PI,
                              -DirectX::XM_PI,
                              DirectX::XM_PIDIV2,
                              -DirectX::XM_PIDIV2,
                              2.3561945f,
                              -2.3561945f,
                              9.5873799e-5f,
                              -9.5873799e-5f};

    Game::World world;
    std::vector<Game::ShipId> ships;
    for (const float heading : HEADINGS)
    {
      ships.push_back(world.SpawnShip(Game::LocalPos(static_cast<float>(ships.size()) * 400.0f, 0.0f), heading,
                                      static_cast<std::uint32_t>(Game::HullId::Corvette)));
    }

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(world, transport) > 0u, L"nothing was sent");

    Game::SnapshotReceiver receiver;
    FeedDatagrams(receiver, transport);

    for (const Game::ShipId id : ships)
    {
      const Game::ShipSnapshot* copy = Find(receiver.Latest(), world.EntityIdOf(id));
      Assert::IsTrue(copy != nullptr, L"a swept heading went missing");
      Assert::IsTrue(AngleError(world.Ship(id).headingRad, copy->headingRad) <= WIRE_ANGLE_BOUND_RAD,
                     L"a swept heading came back further than half a turns16 step");
      // Spawn sets prevHeading to the same value, so this pins the second angle field as well --
      // the two are written by the same call and would drift together if either were mis-sized.
      Assert::IsTrue(AngleError(world.Ship(id).prevHeading, copy->prevHeading) <= WIRE_ANGLE_BOUND_RAD,
                     L"a swept prevHeading came back further than half a turns16 step");
      Assert::IsTrue(copy->headingRad > -DirectX::XM_PI - WIRE_ANGLE_BOUND_RAD && copy->headingRad <= DirectX::XM_PI + WIRE_ANGLE_BOUND_RAD,
                     L"a decoded heading left the range the decode promises");
    }
  }

  TEST_METHOD(APrevPosArrivesAsAWholeNumberOfStepsFromItsOwnPosition)
  {
    // prevPos is not a second position on the wire: it is an integer step delta from posWorld, which
    // is what makes the pair four bytes instead of twelve and what keeps the reconstruction exact
    // rather than doubly rounded (Design/Archive/QuantizedWire-work-order.md 2.3). Two things follow, and
    // both are asserted: the offset between the decoded pair is a whole number of steps, and it is
    // within one step of the offset the simulation holds. The view divides that offset by one tick
    // to get velocity, so it is the number this field exists for.
    Game::World world;
    const Game::ShipId ship = SpawnAt(world, 0.0f, 0.0f, Game::HullId::Interceptor);
    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::LocalPos(3000.0f, 1500.0f), false, 0.0f);

    float worstDelta = 0.0f;
    float longestTravel = 0.0f;
    for (int tick = 0; tick < 120; ++tick)
    {
      world.Step();

      CaptureTransport transport;
      Game::SnapshotWriter writer;
      Assert::AreEqual(1u, writer.Write(world, transport), L"one ship did not fit in one fragment");
      Game::SnapshotReceiver receiver;
      FeedDatagrams(receiver, transport);
      Assert::AreEqual(static_cast<std::size_t>(1), receiver.Latest().ships.size(), L"the ship did not arrive");

      const Game::ShipState& source = world.Ship(ship);
      const Game::ShipSnapshot& copy = receiver.Latest().ships[0];

      const float wireX = Game::OffsetX(copy.prevPos, copy.posWorld);
      const float wireZ = Game::OffsetZ(copy.prevPos, copy.posWorld);
      Assert::AreEqual(std::round(wireX / WIRE_POSITION_STEP_METRES), wireX / WIRE_POSITION_STEP_METRES, 0.0f,
                       L"the decoded prevPos was not a whole number of steps from posWorld");
      Assert::AreEqual(std::round(wireZ / WIRE_POSITION_STEP_METRES), wireZ / WIRE_POSITION_STEP_METRES, 0.0f,
                       L"the decoded prevPos was not a whole number of steps from posWorld");

      const float trueX = Game::OffsetX(source.prevPos, source.posWorld);
      const float trueZ = Game::OffsetZ(source.prevPos, source.posWorld);
      worstDelta = std::max(worstDelta, std::max(std::fabs(wireX - trueX), std::fabs(wireZ - trueZ)));
      longestTravel = std::max(longestTravel, std::max(std::fabs(trueX), std::fabs(trueZ)));

      Assert::IsTrue(WorstAxisError(source.prevPos, copy.prevPos) <= WIRE_POSITION_BOUND_METRES,
                     L"prevPos itself came back further than half a lattice step");
    }

    // One rounding at each end of the delta, so a tick of travel is right to a step.
    Assert::IsTrue(worstDelta <= WIRE_POSITION_STEP_METRES, L"a tick of travel arrived more than one step wrong");
    // And the ship actually moved, or every assertion above compared zero with zero.
    Assert::IsTrue(longestTravel > 0.4f, L"the Interceptor never got moving, so the delta was never exercised");
  }

  TEST_METHOD(ASectorBorderIsCrossedWithoutAJump)
  {
    // The decode rebuilds prevPos by translating the decoded posWorld, so the sector carry is the
    // simulation's own. If it were not, a ship crossing a border would decode into the sector it had
    // just left and the view would see it jump a sector's width and come back.
    Game::World world;
    const Game::ShipId ship =
      world.SpawnShip(Game::LocalPos(Game::SECTOR_SIZE_METRES - 60.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
    const Game::ShipId order[] = {ship};
    world.IssueMoveOrder(order, Game::LocalPos(Game::SECTOR_SIZE_METRES + 400.0f, 0.0f), false, 0.0f);

    Game::WorldPos previous;
    bool havePrevious = false;
    bool crossed = false;
    float worstStep = 0.0f;
    for (int tick = 0; tick < 400; ++tick)
    {
      world.Step();

      CaptureTransport transport;
      Game::SnapshotWriter writer;
      Assert::AreEqual(1u, writer.Write(world, transport), L"one ship did not fit in one fragment");
      Game::SnapshotReceiver receiver;
      FeedDatagrams(receiver, transport);
      Assert::AreEqual(static_cast<std::size_t>(1), receiver.Latest().ships.size(), L"the ship did not arrive");
      const Game::ShipSnapshot& copy = receiver.Latest().ships[0];

      if (havePrevious)
        worstStep = std::max(worstStep, Game::Distance(previous, copy.posWorld));
      previous = copy.posWorld;
      havePrevious = true;
      crossed = crossed || copy.posWorld.sectorX == 1;

      Assert::IsTrue(Game::Distance(copy.prevPos, copy.posWorld) < 1.0f,
                     L"a record's prevPos was more than a tick of travel from its posWorld");
    }

    Assert::IsTrue(crossed, L"the ship never crossed the border, so nothing was tested");
    // An Interceptor tops out at 34 m/s, so a tick is 0.567 m; anything near a sector's width is the
    // carry going wrong rather than the ship going fast.
    Assert::IsTrue(worstStep < 1.0f, L"a decoded position jumped further than one tick of travel");
  }

  TEST_METHOD(APositionAMillimetreShortOfABorderRoundsIntoTheNextSector)
  {
    // The nearest lattice point to 8,191.999 m is the next sector's origin, not the last step inside
    // this one. Carrying rather than clamping is what keeps the encode monotonic across a border: a
    // quantizer that clamped would put a ship a millimetre from the border 0.124 m back inside it,
    // and the sign of the error would flip at the boundary.
    Game::World world;
    const Game::ShipId ship =
      world.SpawnShip(Game::LocalPos(Game::SECTOR_SIZE_METRES - 0.001f, 0.001f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::AreEqual(1u, writer.Write(world, transport), L"one ship did not fit in one fragment");
    Game::SnapshotReceiver receiver;
    FeedDatagrams(receiver, transport);

    const Game::ShipSnapshot& copy = receiver.Latest().ships[0];
    Assert::IsTrue(world.Ship(ship).posWorld.sectorX == 0, L"the spawn was not where this test needs it");
    Assert::IsTrue(copy.posWorld.sectorX == 1, L"the encode did not carry into the next sector");
    Assert::AreEqual(0.0f, copy.posWorld.localX, 0.0f, L"the carried offset is not the new sector's origin");
    Assert::IsTrue(copy.posWorld.sectorZ == 0, L"a millimetre north changed sector");
    Assert::AreEqual(0.0f, copy.posWorld.localZ, 0.0f, L"a millimetre did not round to the origin");
    Assert::IsTrue(WorstAxisError(world.Ship(ship).posWorld, copy.posWorld) <= WIRE_POSITION_BOUND_METRES,
                   L"the carry moved the ship further than half a step");
  }

  TEST_METHOD(FactionSurvivesTheWire)
  {
    // Both shapes of send, because they are the two the game uses and only one of them is exercised
    // by the round-trip test above.
    Game::World world;
    const Game::ShipId ours = SpawnAt(world, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId theirs = SpawnAt(world, 200.0f, 0.0f, Game::HullId::Interceptor, Game::FACTION_VANDAL);
    world.Step();

    const Game::EntityId ourEntity = world.EntityIdOf(ours);
    const Game::EntityId theirEntity = world.EntityIdOf(theirs);

    CaptureTransport whole;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(world, whole) > 0, L"the full snapshot did not send");
    Game::SnapshotReceiver fromWhole;
    for (const std::vector<std::uint8_t>& datagram : whole.sent)
      (void)fromWhole.Accept(datagram);
    Assert::IsTrue(Find(fromWhole.Latest(), ourEntity) != nullptr, L"the player's ship is missing from the full snapshot");
    Assert::AreEqual(Faction(Game::FACTION_PLAYER), Faction(Find(fromWhole.Latest(), ourEntity)->factionId),
                     L"the player's faction did not survive Write");
    Assert::AreEqual(Faction(Game::FACTION_VANDAL), Faction(Find(fromWhole.Latest(), theirEntity)->factionId),
                     L"the hostile faction did not survive Write");

    world.Step();
    CaptureTransport update;
    // Handles, not ids: the sent list is resolved against this world to build records, which is the
    // one list on this call that is still server-side currency (ADR 0047).
    const Game::ShipHandle both[] = {world.HandleOf(ours), world.HandleOf(theirs)};
    Assert::IsTrue(writer.WriteInterest(world, both, {}, {}, {}, update) > 0, L"the interest update did not send");
    Game::SnapshotReceiver fromUpdate;
    for (const std::vector<std::uint8_t>& datagram : update.sent)
      (void)fromUpdate.Accept(datagram);
    Assert::AreEqual(Faction(Game::FACTION_PLAYER), Faction(Find(fromUpdate.Latest(), ourEntity)->factionId),
                     L"the player's faction did not survive WriteInterest");
    Assert::AreEqual(Faction(Game::FACTION_VANDAL), Faction(Find(fromUpdate.Latest(), theirEntity)->factionId),
                     L"the hostile faction did not survive WriteInterest");
  }

  // A client tapping a structure has to know it is tapping a station before an order is worth
  // sending, and "immovable hull of faction 2" is inference of exactly the kind the wire's whole
  // design forbids (Design/Stations.md 6.2).
  TEST_METHOD(TheStationFlagSurvivesTheWire)
  {
    Game::World world;
    const Game::ShipId ship = SpawnAt(world, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId scenery = SpawnAt(world, 400.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    const Game::ShipId post = SpawnAt(world, 800.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);

    Game::World::StationDesc desc;
    desc.ownerFaction = Game::FACTION_VANGUARD;
    Assert::AreNotEqual(Game::World::INVALID_STATION_ID, world.MakeStation(post, desc), L"the station was not made");
    world.Step();

    const Game::EntityId shipEntity = world.EntityIdOf(ship);
    const Game::EntityId sceneryEntity = world.EntityIdOf(scenery);
    const Game::EntityId postEntity = world.EntityIdOf(post);

    // Both shapes of send: the flag is written by one record writer, but only one of the two paths
    // would be exercised by a round-trip test, and a header read in the wrong place breaks the other.
    CaptureTransport whole;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(world, whole) > 0, L"the full snapshot did not send");
    Game::SnapshotReceiver fromWhole;
    for (const std::vector<std::uint8_t>& datagram : whole.sent)
      (void)fromWhole.Accept(datagram);

    Assert::AreEqual(static_cast<std::uint32_t>(Game::SHIP_FLAG_STATION),
                     static_cast<std::uint32_t>(Find(fromWhole.Latest(), postEntity)->flags), L"the station's flag did not survive Write");
    Assert::AreEqual(static_cast<std::uint32_t>(0), static_cast<std::uint32_t>(Find(fromWhole.Latest(), sceneryEntity)->flags),
                     L"a Structure that is only scenery came back flagged as a station");
    Assert::AreEqual(static_cast<std::uint32_t>(0), static_cast<std::uint32_t>(Find(fromWhole.Latest(), shipEntity)->flags),
                     L"a plain ship came back flagged as a station");

    world.Step();
    CaptureTransport update;
    const Game::ShipHandle all[] = {world.HandleOf(ship), world.HandleOf(scenery), world.HandleOf(post)};
    Assert::IsTrue(writer.WriteInterest(world, all, {}, {}, {}, update) > 0, L"the interest update did not send");
    Game::SnapshotReceiver fromUpdate;
    for (const std::vector<std::uint8_t>& datagram : update.sent)
      (void)fromUpdate.Accept(datagram);

    Assert::AreEqual(static_cast<std::uint32_t>(Game::SHIP_FLAG_STATION),
                     static_cast<std::uint32_t>(Find(fromUpdate.Latest(), postEntity)->flags),
                     L"the station's flag did not survive WriteInterest");
    Assert::AreEqual(static_cast<std::uint32_t>(0), static_cast<std::uint32_t>(Find(fromUpdate.Latest(), sceneryEntity)->flags),
                     L"scenery came back flagged over WriteInterest");
  }

  // The client must not infer its standing, and there is nothing to infer from anyway: an order
  // datagram is fire-and-forget, so a refused dock would otherwise be ships that simply never go
  // (Design/Stations.md 4.3).
  TEST_METHOD(StandingSurvivesTheWire)
  {
    Game::World world;
    const Game::ShipId raider = SpawnAt(world, 0.0f, 0.0f, Game::HullId::Bomber, Game::FACTION_PLAYER);
    const Game::ShipId post = SpawnAt(world, 900.0f, 900.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);

    Game::World::StationDesc desc;
    desc.ownerFaction = Game::FACTION_VANGUARD;
    const Game::World::StationId station = world.MakeStation(post, desc);
    world.Step();

    const Game::ShipHandle held[] = {world.HandleOf(raider), world.HandleOf(post)};
    Game::SnapshotWriter writer;

    // The full-snapshot path first. Both writers stamp KIND_SNAPSHOT and one reader parses both, so
    // a mask written by one and not the other desynchronises the reader on the first full snapshot
    // -- and nothing else in this file would notice, because every other test here uses one path.
    CaptureTransport whole;
    Assert::IsTrue(writer.Write(world, whole, Game::FACTION_PLAYER) > 0, L"the full snapshot did not send");
    Game::SnapshotReceiver fromWhole;
    for (const std::vector<std::uint8_t>& datagram : whole.sent)
      (void)fromWhole.Accept(datagram);
    Assert::IsTrue(fromWhole.IsHostileToMe(Game::FACTION_VANDAL), L"the mask did not survive Write");
    Assert::IsFalse(fromWhole.IsHostileToMe(Game::FACTION_VANGUARD), L"Write reported the Vanguard hostile before it was");

    CaptureTransport atBoot;
    Assert::IsTrue(writer.WriteInterest(world, held, {}, {}, {}, atBoot, Game::FACTION_PLAYER) > 0, L"the update did not send");
    Game::SnapshotReceiver player;
    for (const std::vector<std::uint8_t>& datagram : atBoot.sent)
      (void)player.Accept(datagram);

    Assert::IsTrue(player.IsHostileToMe(Game::FACTION_VANDAL), L"the client was not told the Vandals are hostile to it");
    Assert::IsFalse(player.IsHostileToMe(Game::FACTION_VANGUARD), L"the client was told the Vanguard is hostile before it was");

    world.RecordAggression(world.HandleOf(raider), station);
    world.Step();

    CaptureTransport afterwards;
    Assert::IsTrue(writer.WriteInterest(world, held, {}, {}, {}, afterwards, Game::FACTION_PLAYER) > 0, L"the second update did not send");
    for (const std::vector<std::uint8_t>& datagram : afterwards.sent)
      (void)player.Accept(datagram);
    Assert::IsTrue(player.IsHostileToMe(Game::FACTION_VANGUARD), L"the client was never told the law had turned on it");

    // Directional, and stated per subscriber rather than broadcast: a Vandal-faction client is told
    // about the player, not about itself. One row of the table, never the table.
    world.Step();
    CaptureTransport vandalLink;
    Assert::IsTrue(writer.WriteInterest(world, held, {}, {}, {}, vandalLink, Game::FACTION_VANDAL) > 0, L"the Vandal update did not send");
    Game::SnapshotReceiver vandal;
    for (const std::vector<std::uint8_t>& datagram : vandalLink.sent)
      (void)vandal.Accept(datagram);
    Assert::IsTrue(vandal.IsHostileToMe(Game::FACTION_PLAYER), L"the Vandals were not told the player is hostile to them");
    Assert::IsFalse(vandal.IsHostileToMe(Game::FACTION_VANDAL), L"the Vandals were told they are hostile to themselves");
  }

  // The three ways a ship can leave a client's view, told apart. Hostiles opened this door "the
  // width of one list"; docking is the second cause through it, and the client's explosion is what
  // hangs on the difference (ADR 0040).
  TEST_METHOD(ADockAndADeathDifferOnTheWire)
  {
    Game::World world;
    const Game::ShipId post = SpawnAt(world, 900.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    Game::World::StationDesc desc;
    desc.ownerFaction = Game::FACTION_VANGUARD;
    (void)world.MakeStation(post, desc);

    const Game::ShipId doomed = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId visitor = SpawnAt(world, 20.0f, 0.0f);
    const Game::ShipId leaver = SpawnAt(world, 40.0f, 0.0f);
    world.Step();

    const Game::EntityId doomedEntity = world.EntityIdOf(doomed);
    const Game::EntityId visitorEntity = world.EntityIdOf(visitor);
    const Game::EntityId leaverEntity = world.EntityIdOf(leaver);

    Assert::IsTrue(world.DespawnShip(world.HandleOf(doomed)), L"the despawn failed");
    Assert::IsTrue(world.DespawnShip(world.HandleOf(visitor), Game::DespawnCause::Docked), L"the docking despawn failed");

    // The publisher's split is what the writer is handed; here the test plays that part, so the
    // format is what is under test rather than the split.
    const Game::EntityId left[] = {leaverEntity};
    const Game::EntityId destroyed[] = {doomedEntity};
    const Game::EntityId docked[] = {visitorEntity};

    CaptureTransport link;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.WriteInterest(world, {}, left, destroyed, docked, link) > 0, L"the update did not send");

    Game::SnapshotReceiver receiver;
    FeedBothLanes(receiver, link);

    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Destroyed().size(), L"the destroyed list is the wrong size");
    Assert::IsTrue(receiver.Destroyed()[0] == doomedEntity, L"the wrong ship was reported destroyed");
    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Docked().size(), L"the docked list is the wrong size");
    Assert::IsTrue(receiver.Docked()[0] == visitorEntity, L"the wrong ship was reported docked");

    // A range-leaver is in neither. That is what a leave has always meant and what the causes exist
    // to stop it being read as.
    Assert::IsFalse(Holds(receiver.Destroyed(), leaverEntity), L"a ship that flew out of range was reported destroyed");
    Assert::IsFalse(Holds(receiver.Docked(), leaverEntity), L"a ship that flew out of range was reported docked");
    Assert::IsFalse(Holds(receiver.Docked(), doomedEntity), L"a death was reported as a docking");
    Assert::IsFalse(Holds(receiver.Destroyed(), visitorEntity), L"a docking was reported as a death");

    // All three left the held set, whatever they were told to be.
    Assert::IsTrue(Find(receiver.Latest(), doomedEntity) == nullptr, L"the destroyed ship is still held");
    Assert::IsTrue(Find(receiver.Latest(), visitorEntity) == nullptr, L"the docked ship is still held");
    Assert::IsTrue(Find(receiver.Latest(), leaverEntity) == nullptr, L"the departed ship is still held");

    // Cleared independently, so a consumer that has drawn its explosions has not thereby forgotten
    // the dockings it still owes a log line.
    receiver.ClearDestroyed();
    Assert::IsTrue(receiver.Destroyed().empty(), L"clearing the deaths did not clear them");
    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Docked().size(), L"clearing the deaths cleared the dockings too");
    receiver.ClearDocked();
    Assert::IsTrue(receiver.Docked().empty(), L"clearing the dockings did not clear them");
  }

  TEST_METHOD(ADockOrderRoundTrips)
  {
    Game::World world;
    const Game::ShipId post = SpawnAt(world, 900.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    const Game::ShipId first = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId second = SpawnAt(world, 40.0f, 0.0f);

    Game::DockOrder sent;
    sent.station = world.EntityIdOf(post);
    sent.ships = {world.EntityIdOf(first), world.EntityIdOf(second)};

    CaptureTransport link;
    Assert::IsTrue(Game::WriteDockOrder(sent, link), L"the dock order did not send");
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"a dock order did not take the reliable lane");

    Game::DockOrder read;
    Assert::IsTrue(Game::ReadDockOrder(link.sentReliable[0], read), L"the dock order did not decode");
    Assert::IsTrue(read.station == sent.station, L"the station did not survive the wire");
    Assert::AreEqual(sent.ships.size(), read.ships.size(), L"the ship list changed size");
    for (std::size_t at = 0; at < sent.ships.size(); ++at)
      Assert::IsTrue(read.ships[at] == sent.ships[at], L"a handle did not survive the wire");

    // A move order reader must decline a dock order and the other way round, or the adapter's
    // "try one, then the other" would apply the wrong one.
    Game::MoveOrder asMove;
    Assert::IsFalse(Game::ReadMoveOrder(link.sentReliable[0], asMove), L"a dock order decoded as a move order");

    // One cap for both kinds, deliberately: this order's own header would admit two more.
    Game::DockOrder oversized;
    oversized.station = sent.station;
    oversized.ships.assign(Game::MaxShipsPerOrder() + 1, world.EntityIdOf(first));
    CaptureTransport refused;
    Assert::IsFalse(Game::WriteDockOrder(oversized, refused), L"an oversized dock order was sent");
    Assert::IsTrue(refused.sentReliable.empty(), L"an oversized dock order put bytes on the wire");

    Game::DockOrder empty;
    empty.station = sent.station;
    Assert::IsFalse(Game::WriteDockOrder(empty, refused), L"an empty dock order was sent");
  }

  TEST_METHOD(ADeathAndADepartureDifferOnTheWire)
  {
    // The distinction the client's explosion hangs on. Until now a leave meant both, so a hostile
    // patrol crossing the edge of the interest radius detonated on screen while it was alive and
    // well (Design/Archive/Hostiles.md 4.4).
    //
    // The split is WorldSimulation's, which lives in the executable and has no suite, so the rule is
    // restated here against the same two inputs: the world's despawn log, and the interest set's
    // leaves.
    Game::World world;
    const Game::ShipId doomed = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId departing = SpawnAt(world, 100.0f, 0.0f);
    const Game::EntityId doomedEntity = world.EntityIdOf(doomed);
    const Game::EntityId departingEntity = world.EntityIdOf(departing);
    world.Step();

    Game::InterestSet interest;
    interest.Update(world, Game::LocalPos(0.0f, 0.0f));

    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;
    CaptureTransport link;
    const Game::ShipHandle both[] = {world.HandleOf(doomed), world.HandleOf(departing)};
    Assert::IsTrue(writer.WriteInterest(world, both, {}, {}, {}, link) > 0, L"the first update did not send");
    FeedBothLanes(receiver, link);
    Assert::AreEqual(static_cast<std::size_t>(2), receiver.Latest().ships.size(), L"the client did not take both ships");
    Assert::IsTrue(receiver.Destroyed().empty(), L"an update that killed nothing reported a death");

    // One dies; the other is left alive but carried out of range by moving the viewpoint, which is
    // the case a subscriber actually creates. Both drop out on the same update, which is the point:
    // they arrive in one Left() list and only the log tells them apart.
    Assert::IsTrue(world.DespawnShip(world.HandleOf(doomed)), L"the despawn failed");
    world.Step();
    interest.Update(world, Game::LocalPos(Game::INTEREST_RADIUS_METRES * 3.0f, 0.0f));
    Assert::AreEqual(static_cast<std::size_t>(2), interest.Left().size(), L"both ships should have dropped out on one update");

    // The split the publisher does, done by hand: handles in, ids out. A dead ship's id comes off
    // the despawn log, because the world can no longer be asked who a handle was (ADR 0047).
    std::vector<Game::EntityId> destroyed;
    std::vector<Game::EntityId> left;
    for (const Game::ShipHandle handle : interest.Left())
    {
      if (Holds(world.DespawnsSince(0), handle))
        destroyed.push_back(EntityOfDeparture(world, handle));
      else
        left.push_back(world.EntityIdOf(handle));
    }
    Assert::AreEqual(static_cast<std::size_t>(1), destroyed.size(), L"exactly one of the two died");
    Assert::AreEqual(static_cast<std::size_t>(1), left.size(), L"exactly one of the two merely departed");

    link.sent.clear();
    link.sentReliable.clear();
    Assert::IsTrue(writer.WriteInterest(world, {}, left, destroyed, {}, link) > 0, L"the second update did not send");
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"the departures did not take the reliable lane");
    FeedBothLanes(receiver, link);

    Assert::IsTrue(receiver.Latest().ships.empty(), L"the client kept a ship it was told about losing");
    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Destroyed().size(), L"the update did not state exactly one death");
    Assert::IsTrue(Holds(receiver.Destroyed(), doomedEntity), L"the despawned ship was not reported destroyed");
    Assert::IsFalse(Holds(receiver.Destroyed(), departingEntity), L"a ship that only left the radius was reported destroyed");
  }

  TEST_METHOD(AWorldTooBigForOneDatagramFragmentsAndReassembles)
  {
    // 200 ships against 23 per fragment. The point is not the number but that the count and the
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
      Assert::IsTrue(receiver.Latest().ships[id].entity == world.EntityIdOf(id), L"ships came back in a different order");
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
    sent.ships = {world.EntityIdOf(first), world.EntityIdOf(second)};
    sent.destination = Game::LocalPos(-450.5f, 1200.25f);
    sent.facingRad = 1.25f;
    sent.hasFacing = true;

    CaptureTransport transport;
    Assert::IsTrue(Game::WriteMoveOrder(sent, transport), L"the order did not send");

    Game::MoveOrder got;
    Assert::AreEqual(static_cast<std::size_t>(1), transport.sentReliable.size(), L"the order did not take the reliable lane");
    Assert::IsTrue(transport.sent.empty(), L"the order also went out as a datagram");
    Assert::IsTrue(Game::ReadMoveOrder(transport.sentReliable[0], got), L"the order did not decode");
    Assert::AreEqual(sent.ships.size(), got.ships.size(), L"the order lost ships");
    for (std::size_t at = 0; at < sent.ships.size(); ++at)
      Assert::IsTrue(sent.ships[at] == got.ships[at], L"an ordered ship changed identity");
    Assert::IsTrue(IsSamePosition(sent.destination, got.destination), L"the destination did not survive");
    Assert::AreEqual(sent.facingRad, got.facingRad, 0.0f, L"the facing did not survive");
    Assert::IsTrue(got.hasFacing, L"the has-facing flag did not survive");
  }

  TEST_METHOD(AnOrderForADespawnedShipResolvesToNothing)
  {
    // The reason an order carries neither a ShipId nor an index. Between the click and the order
    // arriving, a ship can die and swap-and-pop can move a stranger into its array index (ADR 0005).
    // What the order carries now is an entity id, which is stronger again: a handle is a reference
    // into one World and an id names the ship itself (ADR 0047).
    Game::World world;
    const Game::ShipId doomed = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId other = SpawnAt(world, 100.0f, 0.0f);
    const Game::EntityId doomedEntity = world.EntityIdOf(doomed);
    const Game::EntityId otherEntity = world.EntityIdOf(other);

    Game::MoveOrder sent;
    sent.ships = {doomedEntity, otherEntity};
    sent.destination = Game::LocalPos(0.0f, 500.0f);

    CaptureTransport transport;
    Assert::IsTrue(Game::WriteMoveOrder(sent, transport), L"the order did not send");
    Assert::IsTrue(world.DespawnShip(world.HandleOf(doomed)), L"the despawn failed");

    Game::MoveOrder got;
    Assert::AreEqual(static_cast<std::size_t>(1), transport.sentReliable.size(), L"the order did not take the reliable lane");
    Assert::IsTrue(transport.sent.empty(), L"the order also went out as a datagram");
    Assert::IsTrue(Game::ReadMoveOrder(transport.sentReliable[0], got), L"the order did not decode");
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.ResolveEntity(got.ships[0]), L"a dead ship's id resolved to something");

    // The survivor is still there, but swap-and-pop moved it into the freed slot, so its ShipId is
    // not the one it had when the order was written -- and its id is. That is ADR 0005 and ADR 0047
    // in one assertion: the identity follows the ship, the array index does not.
    const Game::ShipId resolved = world.ResolveEntity(got.ships[1]);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, resolved, L"the surviving ship's id stopped resolving");
    Assert::AreNotEqual(other, resolved, L"the survivor's index did not move, so this test proves nothing");
    Assert::IsTrue(world.EntityIdOf(resolved) == otherEntity, L"the id resolved to a stranger");
  }

  TEST_METHOD(EveryDepartureSurvivesEveryDatagramBeingLost)
  {
    // The row this slice exists for, and the one that retires finding E1 in
    // Design/MmoScalabilityReview.md: with the datagram lane dropping everything, a client is still
    // told about every leave and every death. Before slice 3b those lists rode in the first snapshot
    // fragment, so this test could not have passed -- a lost fragment was a ghost ship for the rest
    // of the match, and nothing repeated it.
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport::Desc desc;
    desc.dropOneInN = 1; // no datagram survives
    Neuron::LoopbackTransport::Connect(server, client, desc);

    Game::World world;
    const Game::ShipId leaving = SpawnAt(world, 0.0f, 0.0f);
    const Game::ShipId dying = SpawnAt(world, 50.0f, 0.0f);
    const Game::EntityId leavingEntity = world.EntityIdOf(leaving);
    const Game::EntityId dyingEntity = world.EntityIdOf(dying);

    server.AdvanceTo(0);
    client.AdvanceTo(0);

    Game::SnapshotWriter writer;
    const std::array<Game::EntityId, 1> left{leavingEntity};
    const std::array<Game::EntityId, 1> destroyed{dyingEntity};
    (void)writer.WriteInterest(world, {}, left, destroyed, {}, server);
    Assert::AreEqual(0u, writer.RefusedLeaveCount(), L"the lane refused the departure message");

    server.Poll();
    client.Poll();

    Game::SnapshotReceiver receiver;
    std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> datagram{};
    Assert::AreEqual(0u, client.Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size())),
                     L"a datagram survived dropOneInN = 1, so this test is not testing what it claims");

    std::vector<std::uint8_t> message(Neuron::MAX_RELIABLE_BYTES, 0u);
    const std::uint32_t size = client.ReceiveReliable(message.data(), Neuron::MAX_RELIABLE_BYTES);
    Assert::IsTrue(size > 0, L"the departure message did not survive the datagram lane being dead");
    Assert::IsTrue(receiver.Accept(std::span<const std::uint8_t>(message.data(), size)), L"the receiver refused the departure message");

    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Destroyed().size(), L"the client was not told about exactly one death");
    Assert::IsTrue(Holds(receiver.Destroyed(), dyingEntity), L"the client was told the wrong ship died");
  }

  TEST_METHOD(AnOrderSurvivesEveryDatagramBeingLost)
  {
    // The other half of the same finding: a dropped order is a click the player made and the game
    // ignored, and no amount of interpolation covers that up.
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport::Desc desc;
    desc.dropOneInN = 1;
    Neuron::LoopbackTransport::Connect(client, server, desc);
    client.AdvanceTo(0);
    server.AdvanceTo(0);

    Game::MoveOrder order;
    order.ships.push_back(Game::MakeEntityId(0, 3));
    order.destination = Game::LocalPos(120.0f, -40.0f);
    order.hasFacing = true;
    order.facingRad = 0.5f;
    Assert::IsTrue(Game::WriteMoveOrder(order, client), L"the order was refused");

    client.Poll();
    server.Poll();

    std::vector<std::uint8_t> message(Neuron::MAX_RELIABLE_BYTES, 0u);
    const std::uint32_t size = server.ReceiveReliable(message.data(), Neuron::MAX_RELIABLE_BYTES);
    Assert::IsTrue(size > 0, L"the order did not survive the datagram lane being dead");

    Game::MoveOrder received;
    Assert::IsTrue(Game::ReadMoveOrder(std::span<const std::uint8_t>(message.data(), size), received), L"the order did not parse");
    Assert::AreEqual(static_cast<std::size_t>(1), received.ships.size(), L"the order lost its ship");
    Assert::IsTrue(received.hasFacing, L"the order lost its facing");
  }

  TEST_METHOD(ALeaveArrivingBeforeItsShipIsHarmless)
  {
    // The two lanes have no ordering between them, so a departure can overtake the update that would
    // have introduced the ship. Removing a handle the receiver does not hold is a no-op, which is
    // what makes either order safe -- and this is the test that says so rather than the comment.
    Game::World world;
    const Game::ShipId ship = SpawnAt(world, 0.0f, 0.0f);
    const Game::EntityId handle = world.EntityIdOf(ship);

    CaptureTransport link;
    Game::SnapshotWriter writer;
    const std::array<Game::EntityId, 1> left{handle};
    (void)writer.WriteInterest(world, {}, left, {}, {}, link);
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"the departure did not go on the reliable lane");

    Game::SnapshotReceiver receiver;
    Assert::IsTrue(receiver.Accept(link.sentReliable[0]), L"the receiver refused a departure for a ship it never held");
    Assert::IsTrue(receiver.Latest().ships.empty(), L"a departure for an unknown ship added one");
  }

  TEST_METHOD(DeparturesDoNotRideInTheSnapshotAnyMore)
  {
    // The structural half of the change: nothing about a leave is on the datagram lane, so a
    // fragment's size no longer depends on how many ships left this update.
    Game::World world;
    for (int at = 0; at < 3; ++at)
      (void)SpawnAt(world, static_cast<float>(at) * 40.0f, 0.0f);
    const Game::EntityId gone = world.EntityIdOf(0);

    CaptureTransport withDepartures;
    Game::SnapshotWriter a;
    const std::array<Game::EntityId, 1> left{gone};
    (void)a.WriteInterest(world, {}, left, {}, {}, withDepartures);

    CaptureTransport withNone;
    Game::SnapshotWriter b;
    (void)b.WriteInterest(world, {}, {}, {}, {}, withNone);

    Assert::AreEqual(withNone.sent.size(), withDepartures.sent.size(), L"a departure changed how many datagrams an update took");
    Assert::AreEqual(withNone.sent[0].size(), withDepartures.sent[0].size(), L"a departure changed the size of a snapshot fragment");
    Assert::AreEqual(static_cast<std::size_t>(1), withDepartures.sentReliable.size(), L"the departure did not take the reliable lane");
    Assert::IsTrue(withNone.sentReliable.empty(), L"an update with nothing to state sent a departure message anyway");
  }

  TEST_METHOD(ARefusedDepartureIsCounted)
  {
    // Nothing repeats a refused leave, so the gap has to be visible. A number that should be zero
    // is worth more than a comment saying it should be.
    Game::World world;
    const Game::ShipId ship = SpawnAt(world, 0.0f, 0.0f);
    const Game::EntityId handle = world.EntityIdOf(ship);

    CaptureTransport link;
    link.refuseReliable = true;

    Game::SnapshotWriter writer;
    const std::array<Game::EntityId, 1> left{handle};
    (void)writer.WriteInterest(world, {}, left, {}, {}, link);
    Assert::AreEqual(1u, writer.RefusedLeaveCount(), L"a refused departure was not counted");
    Assert::IsTrue(link.sentReliable.empty(), L"a refused lane still captured a message");
  }

  TEST_METHOD(AnOversizedOrderIsRefused)
  {
    Game::MoveOrder tooMany;
    tooMany.ships.assign(Game::MaxShipsPerOrder() + 1, Game::MakeEntityId(0, 1));
    tooMany.destination = Game::LocalPos(0.0f, 0.0f);

    CaptureTransport transport;
    Assert::IsFalse(Game::WriteMoveOrder(tooMany, transport), L"an order larger than the cap was sent");
    Assert::IsTrue(transport.sent.empty(), L"a refused order still put bytes on the datagram lane");
    Assert::IsTrue(transport.sentReliable.empty(), L"a refused order still put bytes on the reliable lane");

    Game::MoveOrder empty;
    Assert::IsFalse(Game::WriteMoveOrder(empty, transport), L"an order with no ships was sent");
  }
};
} // namespace GameLogicTests
