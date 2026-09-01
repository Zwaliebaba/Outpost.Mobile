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

Game::ShipId SpawnAt(Game::Universe& _universe, float _x, float _z, Game::HullId _hull = Game::HullId::Corvette,
                     Game::FactionId _faction = Game::FACTION_PLAYER)
{
  return _universe.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(_hull), _faction);
}

// One decoded record by identity. The receiver holds a set, not a list in spawn order, so a row
// that indexed into it would be asserting against whatever the upsert happened to append.
[[nodiscard]] const Game::ShipSnapshot& FindShip(const Game::SnapshotReceiver& _receiver, Game::EntityId _entity)
{
  for (const Game::ShipSnapshot& ship : _receiver.Latest().ships)
  {
    if (ship.entity == _entity)
      return ship;
  }
  Assert::Fail(L"the snapshot held no record for that entity");
  return _receiver.Latest().ships.front();
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

// Handles, for the server-side sets: an interest set is a set of references into one Universe, and
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
// same reason the publisher does: the ship is gone, so the universe cannot be asked (ADR 0047).
[[nodiscard]] Game::EntityId EntityOfDeparture(const Game::Universe& _universe, Game::ShipHandle _handle)
{
  for (const Game::DespawnRecord& record : _universe.DespawnsSince(0))
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
[[nodiscard]] float WorstAxisError(const Game::UniversePos& _a, const Game::UniversePos& _b)
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

[[nodiscard]] const Game::ShipSnapshot* Find(const Game::UniverseSnapshot& _snapshot, Game::EntityId _entity)
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
    // recorded because they are the argument for interest management: 22 ships per datagram -- 13
    // before the record was quantized -- means a 5,000-ship snapshot is 228 fragments, which is what
    // slice 6 exists to stop sending.
    //
    // 22 and not the 23 that stood here through the quantized wire: the fleet status block joined
    // the header, and it is sized against its WORST case -- five fleets, 71 bytes -- rather than
    // against what an update happens to carry, so that this stays one number every caller and every
    // test agree on (Design/Archive/Fleets-slice-5.md 2.2).
    //
    // And 21 rather than that 22 since the record gained a hull fraction: one byte on 47 costs a
    // record a fragment, which is the price of a client being able to draw a damage bar at all
    // (Design/Combat-slice-2.md 2.2). The figure moves with the record by construction -- that is what
    // this row is for -- and what matters is that it is *derived*, so nobody has to remember it.
    Assert::AreEqual(21u, Game::ShipsPerSnapshotFragment(), L"the record plus the fleet block no longer fits 21 ships in a datagram");
    Assert::IsTrue(Game::ShipsPerSnapshotFragment() < 64, L"the record got suspiciously small");
  }

  TEST_METHOD(AShipRecordCarriesItsHullFraction)
  {
    // A fraction rather than the number, because a fraction is what a pip row and a target bar draw
    // (Design/Combat.md 9.1, 10.3). Three readings matter: whole, hurt, and a hull with nothing to
    // lose -- which reads whole, because undamaged is the only honest answer for a station.
    // The damage is done by the fire pass rather than by a setter, because there is no setter and
    // there should not be one: hull points are the pass's to spend (ADR 0052), and a test that
    // reached past it would be asserting about a state the simulation cannot reach.
    Game::Universe universe;
    const Game::ShipId station = SpawnAt(universe, 900.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    const Game::ShipId victim = SpawnAt(universe, 0.0f, 0.0f, Game::HullId::Frigate, Game::FACTION_PLAYER);
    SpawnAt(universe, 0.0f, 120.0f, Game::HullId::Frigate, Game::FACTION_VANDAL);

    CaptureTransport link;
    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;
    Assert::AreEqual(1u, writer.Write(universe, link));
    FeedBothLanes(receiver, link);
    Assert::AreEqual(std::uint8_t{255}, FindShip(receiver, universe.EntityIdOf(victim)).hullFraction, L"a whole hull did not read whole");
    Assert::AreEqual(std::uint8_t{255}, FindShip(receiver, universe.EntityIdOf(station)).hullFraction,
                     L"a hull that cannot be destroyed did not read whole");

    for (int tick = 0; tick < 300; ++tick)
      universe.Step();

    const std::uint32_t points = universe.Ship(victim).hullPoints;
    Assert::IsTrue(points > 0 && points < Game::HullSpecOf(Game::HullId::Frigate).maxHullPoints,
                   L"the victim was not hurt, or not survived");

    link.sent.clear();
    link.sentReliable.clear();
    Assert::AreEqual(1u, writer.Write(universe, link));
    FeedBothLanes(receiver, link);

    // The encoding is what this row is about: 255ths of whole, computed the way the writer does it.
    const std::uint32_t expected = (points * 255u) / Game::HullSpecOf(Game::HullId::Frigate).maxHullPoints;
    Assert::AreEqual(static_cast<std::uint8_t>(expected), FindShip(receiver, universe.EntityIdOf(victim)).hullFraction,
                     L"the fraction on the wire is not what the hull points say");
    Assert::AreEqual(std::uint8_t{255}, FindShip(receiver, universe.EntityIdOf(station)).hullFraction,
                     L"the station lost hull points it does not have");
  }

  TEST_METHOD(AFireBlockRoundTrips)
  {
    // Shooter, target and mount, in the order they were fired. The mount is the only piece of a
    // mount that ever reaches a client, and it is there so the view knows which muzzle to flash.
    CaptureTransport link;
    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;

    const Game::ShotRecord shots[] = {{11, 22, 0}, {11, 22, 1}, {33, 11, 4}};
    Assert::IsTrue(writer.WriteFire(7, shots, link), L"the fire message was not sent");
    Assert::AreEqual(std::size_t{1}, link.sent.size(), L"gunfire did not take the datagram lane");
    Assert::AreEqual(std::size_t{0}, link.sentReliable.size(), L"gunfire took the reliable lane");

    FeedBothLanes(receiver, link);
    const std::span<const Game::FireEvent> fire = receiver.Fire();
    Assert::AreEqual(std::size_t{3}, fire.size(), L"the events did not all arrive");
    Assert::AreEqual(Game::EntityId{11}, fire[0].shooter);
    Assert::AreEqual(Game::EntityId{22}, fire[0].target);
    Assert::AreEqual(0u, fire[0].mount);
    Assert::AreEqual(4u, fire[2].mount, L"the mount index did not survive");

    // Accumulated across a drain until the consumer says it has drawn them, which is what stops two
    // messages in one pump losing the first one's tracers.
    receiver.ClearFire();
    Assert::AreEqual(std::size_t{0}, receiver.Fire().size(), L"clearing did not empty the list");
  }

  TEST_METHOD(AFireBlockOverTheCapKeepsTheNewest)
  {
    // The newest gunfire is the gunfire a player is looking at, so the cap drops from the front.
    std::vector<Game::ShotRecord> shots;
    for (std::uint32_t at = 0; at < Game::MAX_FIRE_EVENTS + 20; ++at)
      shots.push_back(Game::ShotRecord{at + 1, 999, 0});

    CaptureTransport link;
    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;
    Assert::IsTrue(writer.WriteFire(3, shots, link));
    FeedBothLanes(receiver, link);

    const std::span<const Game::FireEvent> fire = receiver.Fire();
    Assert::AreEqual(static_cast<std::size_t>(Game::MAX_FIRE_EVENTS), fire.size(), L"the cap was not applied");
    Assert::AreEqual(shots.back().shooter, fire.back().shooter, L"the newest shot was dropped");
    Assert::AreEqual(shots[20].shooter, fire.front().shooter, L"the oldest shots were not the ones dropped");
  }

  TEST_METHOD(SilenceIsNotSent)
  {
    // An empty message is a datagram spent on nothing, and a quiet tick is most of them.
    CaptureTransport link;
    Game::SnapshotWriter writer;
    Assert::IsFalse(writer.WriteFire(1, {}, link), L"an empty fire message reported a send");
    Assert::AreEqual(std::size_t{0}, link.sent.size(), L"silence went on the wire");
  }

  TEST_METHOD(OneShipRoundTripsFieldForField)
  {
    // Spawned hostile so that every field is compared against a value that is not its default: a
    // faction that round-trips only because both ends default to zero proves nothing.
    Game::Universe universe;
    const Game::ShipId ship = SpawnAt(universe, 120.0f, -340.0f, Game::HullId::Frigate, Game::FACTION_VANDAL);
    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 600.0f), false, 0.0f);
    for (int tick = 0; tick < 30; ++tick)
      universe.Step();

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::AreEqual(1u, writer.Write(universe, transport), L"one ship did not fit in one fragment");

    Game::SnapshotReceiver receiver;
    Assert::IsTrue(receiver.Accept(transport.sent[0]), L"a single-fragment snapshot did not complete");

    const Game::UniverseSnapshot& got = receiver.Latest();
    Assert::AreEqual(universe.Tick(), got.tick, L"the snapshot carries the wrong tick");
    Assert::AreEqual(static_cast<std::size_t>(1), got.ships.size(), L"the wrong number of ships came back");

    const Game::ShipState& source = universe.Ship(ship);
    const Game::ShipSnapshot& copy = got.ships[0];
    Assert::IsTrue(copy.entity == universe.EntityIdOf(ship), L"the handle did not survive");

    // Four fields are quantized and seven are not, and both halves are asserted here: a bound where
    // the wire rounds, and bit equality where it does not. A record whose fields got out of step
    // between the writer and the reader shows up as the exact ones failing, which is why they are
    // still compared against zero (Design/Archive/QuantizedWire-work-order.md 1).
    Assert::IsTrue(WorstAxisError(source.posUniverse, copy.posUniverse) <= WIRE_POSITION_BOUND_METRES,
                   L"posUniverse came back further than half a lattice step");
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

    Game::Universe universe;
    std::vector<Game::ShipId> ships;
    for (const float sector : SECTORS)
    {
      for (const float localX : LOCALS)
      {
        for (const float localZ : LOCALS)
          ships.push_back(
            universe.SpawnShip(Game::LocalPos(sector + localX, sector + localZ), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette)));
      }
    }

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(universe, transport) > 1u, L"the sweep did not fragment, so it is not testing the writer");

    Game::SnapshotReceiver receiver;
    FeedDatagrams(receiver, transport);
    Assert::AreEqual(ships.size(), receiver.Latest().ships.size(), L"the sweep did not come back whole");

    for (const Game::ShipId id : ships)
    {
      const Game::ShipSnapshot* copy = Find(receiver.Latest(), universe.EntityIdOf(id));
      Assert::IsTrue(copy != nullptr, L"a swept position went missing");
      Assert::IsTrue(WorstAxisError(universe.Ship(id).posUniverse, copy->posUniverse) <= WIRE_POSITION_BOUND_METRES,
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

    Game::Universe universe;
    std::vector<Game::ShipId> ships;
    for (const float heading : HEADINGS)
    {
      ships.push_back(universe.SpawnShip(Game::LocalPos(static_cast<float>(ships.size()) * 400.0f, 0.0f), heading,
                                         static_cast<std::uint32_t>(Game::HullId::Corvette)));
    }

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(universe, transport) > 0u, L"nothing was sent");

    Game::SnapshotReceiver receiver;
    FeedDatagrams(receiver, transport);

    for (const Game::ShipId id : ships)
    {
      const Game::ShipSnapshot* copy = Find(receiver.Latest(), universe.EntityIdOf(id));
      Assert::IsTrue(copy != nullptr, L"a swept heading went missing");
      Assert::IsTrue(AngleError(universe.Ship(id).headingRad, copy->headingRad) <= WIRE_ANGLE_BOUND_RAD,
                     L"a swept heading came back further than half a turns16 step");
      // Spawn sets prevHeading to the same value, so this pins the second angle field as well --
      // the two are written by the same call and would drift together if either were mis-sized.
      Assert::IsTrue(AngleError(universe.Ship(id).prevHeading, copy->prevHeading) <= WIRE_ANGLE_BOUND_RAD,
                     L"a swept prevHeading came back further than half a turns16 step");
      Assert::IsTrue(copy->headingRad > -DirectX::XM_PI - WIRE_ANGLE_BOUND_RAD && copy->headingRad <= DirectX::XM_PI + WIRE_ANGLE_BOUND_RAD,
                     L"a decoded heading left the range the decode promises");
    }
  }

  TEST_METHOD(APrevPosArrivesAsAWholeNumberOfStepsFromItsOwnPosition)
  {
    // prevPos is not a second position on the wire: it is an integer step delta from posUniverse, which
    // is what makes the pair four bytes instead of twelve and what keeps the reconstruction exact
    // rather than doubly rounded (Design/Archive/QuantizedWire-work-order.md 2.3). Two things follow, and
    // both are asserted: the offset between the decoded pair is a whole number of steps, and it is
    // within one step of the offset the simulation holds. The view divides that offset by one tick
    // to get velocity, so it is the number this field exists for.
    Game::Universe universe;
    const Game::ShipId ship = SpawnAt(universe, 0.0f, 0.0f, Game::HullId::Interceptor);
    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, Game::LocalPos(3000.0f, 1500.0f), false, 0.0f);

    float worstDelta = 0.0f;
    float longestTravel = 0.0f;
    for (int tick = 0; tick < 120; ++tick)
    {
      universe.Step();

      CaptureTransport transport;
      Game::SnapshotWriter writer;
      Assert::AreEqual(1u, writer.Write(universe, transport), L"one ship did not fit in one fragment");
      Game::SnapshotReceiver receiver;
      FeedDatagrams(receiver, transport);
      Assert::AreEqual(static_cast<std::size_t>(1), receiver.Latest().ships.size(), L"the ship did not arrive");

      const Game::ShipState& source = universe.Ship(ship);
      const Game::ShipSnapshot& copy = receiver.Latest().ships[0];

      const float wireX = Game::OffsetX(copy.prevPos, copy.posUniverse);
      const float wireZ = Game::OffsetZ(copy.prevPos, copy.posUniverse);
      Assert::AreEqual(std::round(wireX / WIRE_POSITION_STEP_METRES), wireX / WIRE_POSITION_STEP_METRES, 0.0f,
                       L"the decoded prevPos was not a whole number of steps from posUniverse");
      Assert::AreEqual(std::round(wireZ / WIRE_POSITION_STEP_METRES), wireZ / WIRE_POSITION_STEP_METRES, 0.0f,
                       L"the decoded prevPos was not a whole number of steps from posUniverse");

      const float trueX = Game::OffsetX(source.prevPos, source.posUniverse);
      const float trueZ = Game::OffsetZ(source.prevPos, source.posUniverse);
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
    // The decode rebuilds prevPos by translating the decoded posUniverse, so the sector carry is the
    // simulation's own. If it were not, a ship crossing a border would decode into the sector it had
    // just left and the view would see it jump a sector's width and come back.
    Game::Universe universe;
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(Game::SECTOR_SIZE_METRES - 60.0f, 0.0f), 0.0f,
                                                 static_cast<std::uint32_t>(Game::HullId::Interceptor));
    const Game::ShipId order[] = {ship};
    universe.IssueMoveOrder(order, Game::LocalPos(Game::SECTOR_SIZE_METRES + 400.0f, 0.0f), false, 0.0f);

    Game::UniversePos previous;
    bool havePrevious = false;
    bool crossed = false;
    float worstStep = 0.0f;
    for (int tick = 0; tick < 400; ++tick)
    {
      universe.Step();

      CaptureTransport transport;
      Game::SnapshotWriter writer;
      Assert::AreEqual(1u, writer.Write(universe, transport), L"one ship did not fit in one fragment");
      Game::SnapshotReceiver receiver;
      FeedDatagrams(receiver, transport);
      Assert::AreEqual(static_cast<std::size_t>(1), receiver.Latest().ships.size(), L"the ship did not arrive");
      const Game::ShipSnapshot& copy = receiver.Latest().ships[0];

      if (havePrevious)
        worstStep = std::max(worstStep, Game::Distance(previous, copy.posUniverse));
      previous = copy.posUniverse;
      havePrevious = true;
      crossed = crossed || copy.posUniverse.sectorX == 1;

      Assert::IsTrue(Game::Distance(copy.prevPos, copy.posUniverse) < 1.0f,
                     L"a record's prevPos was more than a tick of travel from its posUniverse");
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
    Game::Universe universe;
    const Game::ShipId ship = universe.SpawnShip(Game::LocalPos(Game::SECTOR_SIZE_METRES - 0.001f, 0.001f), 0.0f,
                                                 static_cast<std::uint32_t>(Game::HullId::Corvette));

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::AreEqual(1u, writer.Write(universe, transport), L"one ship did not fit in one fragment");
    Game::SnapshotReceiver receiver;
    FeedDatagrams(receiver, transport);

    const Game::ShipSnapshot& copy = receiver.Latest().ships[0];
    Assert::IsTrue(universe.Ship(ship).posUniverse.sectorX == 0, L"the spawn was not where this test needs it");
    Assert::IsTrue(copy.posUniverse.sectorX == 1, L"the encode did not carry into the next sector");
    Assert::AreEqual(0.0f, copy.posUniverse.localX, 0.0f, L"the carried offset is not the new sector's origin");
    Assert::IsTrue(copy.posUniverse.sectorZ == 0, L"a millimetre north changed sector");
    Assert::AreEqual(0.0f, copy.posUniverse.localZ, 0.0f, L"a millimetre did not round to the origin");
    Assert::IsTrue(WorstAxisError(universe.Ship(ship).posUniverse, copy.posUniverse) <= WIRE_POSITION_BOUND_METRES,
                   L"the carry moved the ship further than half a step");
  }

  TEST_METHOD(FactionSurvivesTheWire)
  {
    // Both shapes of send, because they are the two the game uses and only one of them is exercised
    // by the round-trip test above.
    Game::Universe universe;
    const Game::ShipId ours = SpawnAt(universe, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId theirs = SpawnAt(universe, 200.0f, 0.0f, Game::HullId::Interceptor, Game::FACTION_VANDAL);
    universe.Step();

    const Game::EntityId ourEntity = universe.EntityIdOf(ours);
    const Game::EntityId theirEntity = universe.EntityIdOf(theirs);

    CaptureTransport whole;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(universe, whole) > 0, L"the full snapshot did not send");
    Game::SnapshotReceiver fromWhole;
    for (const std::vector<std::uint8_t>& datagram : whole.sent)
      (void)fromWhole.Accept(datagram);
    Assert::IsTrue(Find(fromWhole.Latest(), ourEntity) != nullptr, L"the player's ship is missing from the full snapshot");
    Assert::AreEqual(Faction(Game::FACTION_PLAYER), Faction(Find(fromWhole.Latest(), ourEntity)->factionId),
                     L"the player's faction did not survive Write");
    Assert::AreEqual(Faction(Game::FACTION_VANDAL), Faction(Find(fromWhole.Latest(), theirEntity)->factionId),
                     L"the hostile faction did not survive Write");

    universe.Step();
    CaptureTransport update;
    // Handles, not ids: the sent list is resolved against this universe to build records, which is the
    // one list on this call that is still server-side currency (ADR 0047).
    const Game::ShipHandle both[] = {universe.HandleOf(ours), universe.HandleOf(theirs)};
    Assert::IsTrue(writer.WriteInterest(universe, both, {}, {}, {}, {}, update) > 0, L"the interest update did not send");
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
  // design forbids (Design/Archive/Stations.md 6.2).
  TEST_METHOD(TheStationFlagSurvivesTheWire)
  {
    Game::Universe universe;
    const Game::ShipId ship = SpawnAt(universe, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId scenery = SpawnAt(universe, 400.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    const Game::ShipId post = SpawnAt(universe, 800.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);

    Game::Universe::StationDesc desc;
    desc.ownerFaction = Game::FACTION_VANGUARD;
    Assert::AreNotEqual(Game::Universe::INVALID_STATION_ID, universe.MakeStation(post, desc), L"the station was not made");
    universe.Step();

    const Game::EntityId shipEntity = universe.EntityIdOf(ship);
    const Game::EntityId sceneryEntity = universe.EntityIdOf(scenery);
    const Game::EntityId postEntity = universe.EntityIdOf(post);

    // Both shapes of send: the flag is written by one record writer, but only one of the two paths
    // would be exercised by a round-trip test, and a header read in the wrong place breaks the other.
    CaptureTransport whole;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(universe, whole) > 0, L"the full snapshot did not send");
    Game::SnapshotReceiver fromWhole;
    for (const std::vector<std::uint8_t>& datagram : whole.sent)
      (void)fromWhole.Accept(datagram);

    Assert::AreEqual(static_cast<std::uint32_t>(Game::SHIP_FLAG_STATION),
                     static_cast<std::uint32_t>(Find(fromWhole.Latest(), postEntity)->flags), L"the station's flag did not survive Write");
    Assert::AreEqual(static_cast<std::uint32_t>(0), static_cast<std::uint32_t>(Find(fromWhole.Latest(), sceneryEntity)->flags),
                     L"a Structure that is only scenery came back flagged as a station");
    Assert::AreEqual(static_cast<std::uint32_t>(0), static_cast<std::uint32_t>(Find(fromWhole.Latest(), shipEntity)->flags),
                     L"a plain ship came back flagged as a station");

    universe.Step();
    CaptureTransport update;
    const Game::ShipHandle all[] = {universe.HandleOf(ship), universe.HandleOf(scenery), universe.HandleOf(post)};
    Assert::IsTrue(writer.WriteInterest(universe, all, {}, {}, {}, {}, update) > 0, L"the interest update did not send");
    Game::SnapshotReceiver fromUpdate;
    for (const std::vector<std::uint8_t>& datagram : update.sent)
      (void)fromUpdate.Accept(datagram);

    Assert::AreEqual(static_cast<std::uint32_t>(Game::SHIP_FLAG_STATION),
                     static_cast<std::uint32_t>(Find(fromUpdate.Latest(), postEntity)->flags),
                     L"the station's flag did not survive WriteInterest");
    Assert::AreEqual(static_cast<std::uint32_t>(0), static_cast<std::uint32_t>(Find(fromUpdate.Latest(), sceneryEntity)->flags),
                     L"scenery came back flagged over WriteInterest");
  }

  // A gate is a Structure too, so the client cannot tell one from a station or from scenery by the
  // hull table -- it reads the record's own flag, and the record has to carry one. Without this bit
  // the JUMP verb has nothing to pick (UniverseView::PickGate, Design/Universe-slice-4.md 4).
  TEST_METHOD(AGateIsFlaggedOnTheWire)
  {
    Game::Universe universe;
    const Game::ShipId scenery = SpawnAt(universe, 400.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    const Game::ShipId post = SpawnAt(universe, 900.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    const Game::ShipId road = SpawnAt(universe, 1500.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    const Game::ShipId farSide = SpawnAt(universe, 60000.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    (void)universe.MakeStation(post, Game::Universe::StationDesc{});
    Game::Universe::GateDesc desc;
    desc.destination = universe.EntityIdOf(farSide);
    (void)universe.MakeGate(road, desc);
    universe.Step();

    CaptureTransport whole;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(universe, whole) > 0, L"the full snapshot did not send");
    Game::SnapshotReceiver got;
    for (const std::vector<std::uint8_t>& datagram : whole.sent)
      (void)got.Accept(datagram);

    Assert::AreEqual(static_cast<std::uint32_t>(Game::SHIP_FLAG_GATE),
                     static_cast<std::uint32_t>(Find(got.Latest(), universe.EntityIdOf(road))->flags), L"the gate's flag did not survive");
    // And the three that are not gates stay unflagged as one, or a client would offer JUMP at a
    // station -- or, worse, at a rock.
    Assert::AreEqual(static_cast<std::uint32_t>(Game::SHIP_FLAG_STATION),
                     static_cast<std::uint32_t>(Find(got.Latest(), universe.EntityIdOf(post))->flags), L"a station came back as a gate");
    Assert::AreEqual(static_cast<std::uint32_t>(0), static_cast<std::uint32_t>(Find(got.Latest(), universe.EntityIdOf(scenery))->flags),
                     L"scenery came back as a gate");
  }

  // The client must not infer its standing, and there is nothing to infer from anyway: an order
  // datagram is fire-and-forget, so a refused dock would otherwise be ships that simply never go
  // (Design/Archive/Stations.md 4.3).
  TEST_METHOD(StandingSurvivesTheWire)
  {
    Game::Universe universe;
    const Game::ShipId raider = SpawnAt(universe, 0.0f, 0.0f, Game::HullId::Bomber, Game::FACTION_PLAYER);
    const Game::ShipId post = SpawnAt(universe, 900.0f, 900.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);

    Game::Universe::StationDesc desc;
    desc.ownerFaction = Game::FACTION_VANGUARD;
    const Game::Universe::StationId station = universe.MakeStation(post, desc);
    universe.Step();

    const Game::ShipHandle held[] = {universe.HandleOf(raider), universe.HandleOf(post)};
    Game::SnapshotWriter writer;

    // The full-snapshot path first. Both writers stamp KIND_SNAPSHOT and one reader parses both, so
    // a mask written by one and not the other desynchronises the reader on the first full snapshot
    // -- and nothing else in this file would notice, because every other test here uses one path.
    CaptureTransport whole;
    Assert::IsTrue(writer.Write(universe, whole, Game::FACTION_PLAYER) > 0, L"the full snapshot did not send");
    Game::SnapshotReceiver fromWhole;
    for (const std::vector<std::uint8_t>& datagram : whole.sent)
      (void)fromWhole.Accept(datagram);
    Assert::IsTrue(fromWhole.IsHostileToMe(Game::FACTION_VANDAL), L"the mask did not survive Write");
    Assert::IsFalse(fromWhole.IsHostileToMe(Game::FACTION_VANGUARD), L"Write reported the Vanguard hostile before it was");

    CaptureTransport atBoot;
    Assert::IsTrue(writer.WriteInterest(universe, held, {}, {}, {}, {}, atBoot, Game::FACTION_PLAYER) > 0, L"the update did not send");
    Game::SnapshotReceiver player;
    for (const std::vector<std::uint8_t>& datagram : atBoot.sent)
      (void)player.Accept(datagram);

    Assert::IsTrue(player.IsHostileToMe(Game::FACTION_VANDAL), L"the client was not told the Vandals are hostile to it");
    Assert::IsFalse(player.IsHostileToMe(Game::FACTION_VANGUARD), L"the client was told the Vanguard is hostile before it was");

    universe.RecordAggression(universe.HandleOf(raider), station);
    universe.Step();

    CaptureTransport afterwards;
    Assert::IsTrue(writer.WriteInterest(universe, held, {}, {}, {}, {}, afterwards, Game::FACTION_PLAYER) > 0,
                   L"the second update did not send");
    for (const std::vector<std::uint8_t>& datagram : afterwards.sent)
      (void)player.Accept(datagram);
    Assert::IsTrue(player.IsHostileToMe(Game::FACTION_VANGUARD), L"the client was never told the law had turned on it");

    // Directional, and stated per subscriber rather than broadcast: a Vandal-faction client is told
    // about the player, not about itself. One row of the table, never the table.
    universe.Step();
    CaptureTransport vandalLink;
    Assert::IsTrue(writer.WriteInterest(universe, held, {}, {}, {}, {}, vandalLink, Game::FACTION_VANDAL) > 0,
                   L"the Vandal update did not send");
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
    Game::Universe universe;
    const Game::ShipId post = SpawnAt(universe, 900.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    Game::Universe::StationDesc desc;
    desc.ownerFaction = Game::FACTION_VANGUARD;
    (void)universe.MakeStation(post, desc);

    const Game::ShipId doomed = SpawnAt(universe, 0.0f, 0.0f);
    const Game::ShipId visitor = SpawnAt(universe, 20.0f, 0.0f);
    const Game::ShipId leaver = SpawnAt(universe, 40.0f, 0.0f);
    universe.Step();

    const Game::EntityId doomedEntity = universe.EntityIdOf(doomed);
    const Game::EntityId visitorEntity = universe.EntityIdOf(visitor);
    const Game::EntityId leaverEntity = universe.EntityIdOf(leaver);

    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(doomed)), L"the despawn failed");
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(visitor), Game::DespawnCause::Docked), L"the docking despawn failed");

    // The publisher's split is what the writer is handed; here the test plays that part, so the
    // format is what is under test rather than the split.
    const Game::EntityId left[] = {leaverEntity};
    const Game::EntityId destroyed[] = {doomedEntity};
    const Game::EntityId docked[] = {visitorEntity};

    CaptureTransport link;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.WriteInterest(universe, {}, left, destroyed, docked, {}, link) > 0, L"the update did not send");

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

  TEST_METHOD(AFleetOrderRoundTrips)
  {
    Game::Universe universe;
    const Game::ShipId post = SpawnAt(universe, 900.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);

    // One fixed-size message whatever the fleet's size: there is no ship list in it at all, which is
    // what makes an order stop scaling with the group it moves (ADR 0049).
    Game::FleetOrder sent;
    sent.slot = 3;
    sent.kind = Game::FleetOrderKind::Move;
    sent.point = Game::LocalPos(1200.0f, -640.0f);
    sent.facingRad = 0.75f;
    sent.hasFacing = true;
    sent.station = universe.EntityIdOf(post);
    sent.target = universe.EntityIdOf(SpawnAt(universe, -400.0f, 0.0f, Game::HullId::Interceptor, Game::FACTION_VANDAL));

    CaptureTransport link;
    Assert::IsTrue(Game::WriteFleetOrder(sent, link), L"the fleet order did not send");
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"a fleet order did not take the reliable lane");

    Game::FleetOrder read;
    Assert::IsTrue(Game::ReadFleetOrder(link.sentReliable[0], read), L"the fleet order did not decode");
    Assert::AreEqual(static_cast<std::uint32_t>(sent.slot), static_cast<std::uint32_t>(read.slot), L"the slot did not survive the wire");
    Assert::IsTrue(read.kind == sent.kind, L"the kind did not survive the wire");
    Assert::IsTrue(IsSamePosition(read.point, sent.point), L"the point did not survive the wire");
    Assert::AreEqual(sent.facingRad, read.facingRad, 0.0f, L"the facing did not survive the wire");
    Assert::IsTrue(read.hasFacing == sent.hasFacing, L"the facing flag did not survive the wire");
    Assert::IsTrue(read.station == sent.station, L"the station did not survive the wire");
    Assert::IsTrue(read.target == sent.target, L"the attack target did not survive the wire");

    // Every kind, including the one that is reserved: the byte travels whether or not the
    // simulation will act on it, which is the point of having spent it. Bounded by the LAST kind
    // rather than by a literal, so a kind appended to the enum is covered the day it lands.
    for (std::uint8_t kind = 0; kind <= static_cast<std::uint8_t>(Game::FleetOrderKind::Jump); ++kind)
    {
      Game::FleetOrder each = sent;
      each.kind = static_cast<Game::FleetOrderKind>(kind);
      CaptureTransport each_link;
      Assert::IsTrue(Game::WriteFleetOrder(each, each_link), L"a kind was refused by the writer");
      Game::FleetOrder back;
      Assert::IsTrue(Game::ReadFleetOrder(each_link.sentReliable[0], back), L"a kind did not decode");
      Assert::IsTrue(back.kind == each.kind, L"a kind changed on the wire");
    }

    // The readers of the other kinds on this lane must decline it, or the adapter's
    // try-one-then-the-other would apply the wrong message.
    Game::ComposeOrder asCompose;
    Game::LedgerRequest asRequest;
    Assert::IsFalse(Game::ReadComposeOrder(link.sentReliable[0], asCompose), L"a fleet order decoded as a compose order");
    Assert::IsFalse(Game::ReadLedgerRequest(link.sentReliable[0], asRequest), L"a fleet order decoded as a ledger request");

    // A slot that does not exist and a kind this build has never heard of are content, and content
    // fails closed rather than being passed to a gate that would have to guess.
    CaptureTransport refused;
    Game::FleetOrder badSlot = sent;
    badSlot.slot = static_cast<std::uint8_t>(Game::FLEET_SLOTS);
    Assert::IsFalse(Game::WriteFleetOrder(badSlot, refused), L"a slot past the fifth was sent");
    Assert::IsTrue(refused.sentReliable.empty(), L"a refused fleet order put bytes on the wire");

    // One past the LAST kind, named by the same symbol the reader bounds itself with, so the two
    // move together. Spelled Mine + 1 until Jump was appended after Mine -- at which point this line
    // was corrupting the message into a perfectly valid order and asserting that it would not decode
    // (Design/Universe-slice-2.md 7).
    std::vector<std::uint8_t> corrupt = link.sentReliable[0];
    corrupt[6] = static_cast<std::uint8_t>(Game::FleetOrderKind::Jump) + 1;
    Game::FleetOrder never;
    Assert::IsFalse(Game::ReadFleetOrder(corrupt, never), L"a kind past the last one decoded");
  }

  TEST_METHOD(AFleetRosterRoundTrips)
  {
    Game::Universe universe;
    Game::FleetRoster sent;
    sent.slot = 4;
    for (int at = 0; at < static_cast<int>(Game::MAX_FLEET_SHIPS); ++at)
      sent.members.push_back(universe.EntityIdOf(SpawnAt(universe, static_cast<float>(at) * 40.0f, 0.0f)));

    CaptureTransport link;
    Assert::IsTrue(Game::WriteFleetRoster(sent, link), L"the roster did not send");
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"a roster did not take the reliable lane");

    Game::FleetRoster read;
    Assert::IsTrue(Game::ReadFleetRoster(link.sentReliable[0], read), L"the roster did not decode");
    Assert::AreEqual(static_cast<std::uint32_t>(sent.slot), static_cast<std::uint32_t>(read.slot), L"the slot did not survive the wire");
    Assert::AreEqual(sent.members.size(), read.members.size(), L"the member count did not survive the wire");
    for (std::size_t at = 0; at < sent.members.size(); ++at)
      Assert::IsTrue(read.members[at] == sent.members[at], L"a member id did not survive the wire, or the order changed");

    // An empty roster is a message and not an omission: it is what a fleet with nobody in space
    // states, which is both a composed one and one that has just lost its last ship. What it does
    // NOT mean is that the slot is free -- that is the status block's mask (Design/Archive/Fleets.md 8.1).
    Game::FleetRoster none;
    none.slot = 0;
    CaptureTransport emptyLink;
    Assert::IsTrue(Game::WriteFleetRoster(none, emptyLink), L"an empty roster was refused");
    Game::FleetRoster back = sent; // seeded non-empty, so an empty decode has something to clear
    Assert::IsTrue(Game::ReadFleetRoster(emptyLink.sentReliable[0], back), L"an empty roster did not decode");
    Assert::IsTrue(back.members.empty(), L"an empty roster left the previous membership standing");

    // Content that cannot mean anything fails closed, at the writer where it can and at the reader
    // where it must (AGENTS.md 5).
    CaptureTransport refused;
    Game::FleetRoster badSlot = sent;
    badSlot.slot = static_cast<std::uint8_t>(Game::FLEET_SLOTS);
    Assert::IsFalse(Game::WriteFleetRoster(badSlot, refused), L"a slot past the fifth was sent");
    Game::FleetRoster tooMany = sent;
    tooMany.members.push_back(0u);
    Assert::IsFalse(Game::WriteFleetRoster(tooMany, refused), L"a roster past MAX_FLEET_SHIPS was sent");
    Assert::IsTrue(refused.sentReliable.empty(), L"a refused roster put bytes on the wire");

    // A count past the cap AND the bytes to back it, so the refusal is the gate rather than the
    // buffer running out. Truncating alone would pass whether or not the cap were checked at all.
    std::vector<std::uint8_t> overCount = link.sentReliable[0];
    overCount[2] = static_cast<std::uint8_t>(Game::MAX_FLEET_SHIPS + 1);
    overCount.resize(overCount.size() + 8, 0u);
    Game::FleetRoster never;
    Assert::IsFalse(Game::ReadFleetRoster(overCount, never), L"a count past MAX_FLEET_SHIPS decoded");

    std::vector<std::uint8_t> truncated = link.sentReliable[0];
    truncated.resize(truncated.size() - 3);
    Assert::IsFalse(Game::ReadFleetRoster(truncated, never), L"a truncated roster decoded");

    // And the readers of the kinds that share this lane must decline it.
    Game::FleetOrder asFleetOrder;
    Game::LedgerReply asReply;
    Assert::IsFalse(Game::ReadFleetOrder(link.sentReliable[0], asFleetOrder), L"a roster decoded as a fleet order");
    Assert::IsFalse(Game::ReadLedgerReply(link.sentReliable[0], asReply), L"a roster decoded as a ledger reply");
  }

  TEST_METHOD(ALedgerExchangeRoundTrips)
  {
    Game::Universe universe;
    const Game::ShipId post = SpawnAt(universe, 900.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);

    Game::LedgerRequest asked;
    asked.station = universe.EntityIdOf(post);

    CaptureTransport up;
    Assert::IsTrue(Game::WriteLedgerRequest(asked, up), L"the request did not send");
    Assert::AreEqual(static_cast<std::size_t>(1), up.sentReliable.size(), L"a ledger request did not take the reliable lane");

    Game::LedgerRequest readRequest;
    Assert::IsTrue(Game::ReadLedgerRequest(up.sentReliable[0], readRequest), L"the request did not decode");
    Assert::IsTrue(readRequest.station == asked.station, L"the station did not survive the wire");

    // Every row distinct, so a reply whose counts were shifted by one index fails rather than
    // passing on a coincidence of equal numbers.
    Game::LedgerReply answered;
    answered.station = asked.station;
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
      answered.hullCounts[hull] = hull * 3u + 1u;

    CaptureTransport down;
    Assert::IsTrue(Game::WriteLedgerReply(answered, down), L"the reply did not send");
    Game::LedgerReply readReply;
    Assert::IsTrue(Game::ReadLedgerReply(down.sentReliable[0], readReply), L"the reply did not decode");
    Assert::IsTrue(readReply.station == answered.station, L"the reply named the wrong station");
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
      Assert::AreEqual(answered.hullCounts[hull], readReply.hullCounts[hull], L"a hull's count did not survive the wire");

    // A build whose hull table is a different size reads a ledger of the wrong hulls -- and would
    // then let a player compose from it. The count on the wire is what turns that into a refusal.
    std::vector<std::uint8_t> wrongTable = down.sentReliable[0];
    wrongTable[9] = static_cast<std::uint8_t>(Game::HULL_COUNT - 1);
    Game::LedgerReply never;
    Assert::IsFalse(Game::ReadLedgerReply(wrongTable, never), L"a reply from another hull table decoded");

    std::vector<std::uint8_t> truncated = down.sentReliable[0];
    truncated.resize(truncated.size() - 1);
    Assert::IsFalse(Game::ReadLedgerReply(truncated, never), L"a truncated reply decoded");

    Game::LedgerRequest asRequest;
    Assert::IsFalse(Game::ReadLedgerRequest(down.sentReliable[0], asRequest), L"a reply decoded as a request");
    Assert::IsFalse(Game::ReadLedgerReply(up.sentReliable[0], never), L"a request decoded as a reply");
  }

  TEST_METHOD(AComposeOrderRoundTrips)
  {
    Game::Universe universe;
    const Game::ShipId post = SpawnAt(universe, 900.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);

    Game::ComposeOrder sent;
    sent.station = universe.EntityIdOf(post);
    sent.slot = 1;
    sent.hullCounts[static_cast<std::size_t>(Game::HullId::Corvette)] = 3;
    sent.hullCounts[static_cast<std::size_t>(Game::HullId::Miner)] = 2;

    CaptureTransport link;
    Assert::IsTrue(Game::WriteComposeOrder(sent, link), L"the compose order did not send");
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"a compose order did not take the reliable lane");

    Game::ComposeOrder read;
    Assert::IsTrue(Game::ReadComposeOrder(link.sentReliable[0], read), L"the compose order did not decode");
    Assert::IsTrue(read.station == sent.station, L"the station did not survive the wire");
    Assert::AreEqual(static_cast<std::uint32_t>(sent.slot), static_cast<std::uint32_t>(read.slot), L"the slot did not survive the wire");
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
      Assert::AreEqual(sent.hullCounts[hull], read.hullCounts[hull], L"a hull's count did not survive the wire");

    CaptureTransport refused;
    Game::ComposeOrder badSlot = sent;
    badSlot.slot = static_cast<std::uint8_t>(Game::FLEET_SLOTS);
    Assert::IsFalse(Game::WriteComposeOrder(badSlot, refused), L"a slot past the fifth was sent");
    Assert::IsTrue(refused.sentReliable.empty(), L"a refused compose order put bytes on the wire");

    std::vector<std::uint8_t> wrongTable = link.sentReliable[0];
    wrongTable[14] = static_cast<std::uint8_t>(Game::HULL_COUNT + 1);
    Game::ComposeOrder never;
    Assert::IsFalse(Game::ReadComposeOrder(wrongTable, never), L"an order from another hull table decoded");

    // A draft of a hundred Battleships decodes: how many ships a fleet may hold is ComposeFleet's
    // rule, and a codec enforcing it too would be a second copy of it to keep in step (ADR 0014).
    Game::ComposeOrder greedy = sent;
    greedy.hullCounts[static_cast<std::size_t>(Game::HullId::Battleship)] = 100;
    CaptureTransport greedyLink;
    Assert::IsTrue(Game::WriteComposeOrder(greedy, greedyLink), L"an over-large draft was refused by the codec");
    Assert::IsTrue(Game::ReadComposeOrder(greedyLink.sentReliable[0], never), L"an over-large draft did not decode");

    Game::FleetOrder asFleetOrder;
    Assert::IsFalse(Game::ReadFleetOrder(link.sentReliable[0], asFleetOrder), L"a compose order decoded as a fleet order");
    Assert::IsFalse(Game::ReadComposeOrder(link.sent.empty() ? std::vector<std::uint8_t>{9u} : link.sent[0], never),
                    L"a one-byte message decoded as a compose order");
  }

  TEST_METHOD(ASnapshotCarriesTheFleetHeader)
  {
    // The format's own regression test: records decode with a block in front of them, at both ends
    // of what the block can be. A header that grew without the reader agreeing would put the cursor
    // inside the first record, and every field after it would be garbage that still parsed.
    Game::Universe empty;
    for (int at = 0; at < 4; ++at)
      (void)SpawnAt(empty, static_cast<float>(at) * 60.0f, 0.0f);

    CaptureTransport noFleets;
    Game::SnapshotWriter writer;
    Assert::AreEqual(1u, writer.Write(empty, noFleets), L"the snapshot did not send");
    Game::SnapshotReceiver receiver;
    FeedBothLanes(receiver, noFleets);
    Assert::AreEqual(static_cast<std::size_t>(4), receiver.Latest().ships.size(), L"the records did not decode with no fleets");
    Assert::AreEqual(0u, static_cast<std::uint32_t>(receiver.FleetMask()), L"a universe with no fleets stated one");

    // Five fleets: the widest the block ever is, and the case ShipsPerSnapshotFragment is sized for.
    Game::Universe full;
    std::vector<Game::ShipId> members;
    for (std::uint8_t slot = 0; slot < Game::FLEET_SLOTS; ++slot)
    {
      const Game::ShipId ship = SpawnAt(full, static_cast<float>(slot) * 200.0f, 0.0f);
      members.push_back(ship);
      const Game::ShipId one[] = {ship};
      Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, full.FormFleet(Game::FACTION_PLAYER, slot, one), L"a fleet was refused");
    }

    CaptureTransport fiveFleets;
    Game::SnapshotWriter fullWriter;
    Assert::AreEqual(1u, fullWriter.Write(full, fiveFleets), L"the snapshot did not send");
    Game::SnapshotReceiver fullReceiver;
    FeedBothLanes(fullReceiver, fiveFleets);
    Assert::AreEqual(static_cast<std::size_t>(Game::FLEET_SLOTS), fullReceiver.Latest().ships.size(),
                     L"the records did not decode behind a full fleet block");
    Assert::AreEqual(0x1Fu, static_cast<std::uint32_t>(fullReceiver.FleetMask()), L"five fleets did not all reach the mask");
    for (std::uint8_t slot = 0; slot < Game::FLEET_SLOTS; ++slot)
    {
      Assert::IsTrue(Distance(fullReceiver.FleetStatusOf(slot).position, full.Ship(members[slot]).posUniverse) < 0.1f,
                     L"a fleet's stated position is not where its one member is");
    }
  }

  TEST_METHOD(ADeathAndADepartureDifferOnTheWire)
  {
    // The distinction the client's explosion hangs on. Until now a leave meant both, so a hostile
    // patrol crossing the edge of the interest radius detonated on screen while it was alive and
    // well (Design/Archive/Hostiles.md 4.4).
    //
    // The split is UniverseSimulation's, which lives in the executable and has no suite, so the rule is
    // restated here against the same two inputs: the universe's despawn log, and the interest set's
    // leaves.
    Game::Universe universe;
    const Game::ShipId doomed = SpawnAt(universe, 0.0f, 0.0f);
    const Game::ShipId departing = SpawnAt(universe, 100.0f, 0.0f);
    const Game::EntityId doomedEntity = universe.EntityIdOf(doomed);
    const Game::EntityId departingEntity = universe.EntityIdOf(departing);
    universe.Step();

    Game::InterestSet interest;
    interest.Update(universe, Game::LocalPos(0.0f, 0.0f));

    Game::SnapshotWriter writer;
    Game::SnapshotReceiver receiver;
    CaptureTransport link;
    const Game::ShipHandle both[] = {universe.HandleOf(doomed), universe.HandleOf(departing)};
    Assert::IsTrue(writer.WriteInterest(universe, both, {}, {}, {}, {}, link) > 0, L"the first update did not send");
    FeedBothLanes(receiver, link);
    Assert::AreEqual(static_cast<std::size_t>(2), receiver.Latest().ships.size(), L"the client did not take both ships");
    Assert::IsTrue(receiver.Destroyed().empty(), L"an update that killed nothing reported a death");

    // One dies; the other is left alive but carried out of range by moving the viewpoint, which is
    // the case a subscriber actually creates. Both drop out on the same update, which is the point:
    // they arrive in one Left() list and only the log tells them apart.
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(doomed)), L"the despawn failed");
    universe.Step();
    interest.Update(universe, Game::LocalPos(Game::INTEREST_RADIUS_METRES * 3.0f, 0.0f));
    Assert::AreEqual(static_cast<std::size_t>(2), interest.Left().size(), L"both ships should have dropped out on one update");

    // The split the publisher does, done by hand: handles in, ids out. A dead ship's id comes off
    // the despawn log, because the universe can no longer be asked who a handle was (ADR 0047).
    std::vector<Game::EntityId> destroyed;
    std::vector<Game::EntityId> left;
    for (const Game::ShipHandle handle : interest.Left())
    {
      if (Holds(universe.DespawnsSince(0), handle))
        destroyed.push_back(EntityOfDeparture(universe, handle));
      else
        left.push_back(universe.EntityIdOf(handle));
    }
    Assert::AreEqual(static_cast<std::size_t>(1), destroyed.size(), L"exactly one of the two died");
    Assert::AreEqual(static_cast<std::size_t>(1), left.size(), L"exactly one of the two merely departed");

    link.sent.clear();
    link.sentReliable.clear();
    Assert::IsTrue(writer.WriteInterest(universe, {}, left, destroyed, {}, {}, link) > 0, L"the second update did not send");
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"the departures did not take the reliable lane");
    FeedBothLanes(receiver, link);

    Assert::IsTrue(receiver.Latest().ships.empty(), L"the client kept a ship it was told about losing");
    Assert::AreEqual(static_cast<std::size_t>(1), receiver.Destroyed().size(), L"the update did not state exactly one death");
    Assert::IsTrue(Holds(receiver.Destroyed(), doomedEntity), L"the despawned ship was not reported destroyed");
    Assert::IsFalse(Holds(receiver.Destroyed(), departingEntity), L"a ship that only left the radius was reported destroyed");
  }

  TEST_METHOD(AUniverseTooBigForOneDatagramFragmentsAndReassembles)
  {
    // 200 ships against 23 per fragment. The point is not the number but that the count and the
    // order come back exactly, because a snapshot that reassembles out of order is a universe where
    // ships have swapped places.
    Game::Universe universe;
    for (int at = 0; at < 200; ++at)
      SpawnAt(universe, static_cast<float>(at) * 40.0f, 0.0f);
    universe.Step();

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    const std::uint32_t fragments = writer.Write(universe, transport);
    Assert::IsTrue(fragments > 1, L"200 ships did not fragment");

    Game::SnapshotReceiver receiver;
    bool complete = false;
    for (const std::vector<std::uint8_t>& datagram : transport.sent)
      complete = receiver.Accept(datagram);

    Assert::IsTrue(complete, L"the last fragment did not complete the snapshot");
    Assert::AreEqual(static_cast<std::size_t>(200), receiver.Latest().ships.size(), L"ships went missing across fragments");
    for (Game::ShipId id = 0; id < 200; ++id)
      Assert::IsTrue(receiver.Latest().ships[id].entity == universe.EntityIdOf(id), L"ships came back in a different order");
  }

  TEST_METHOD(ASnapshotMissingAFragmentIsDroppedWhole)
  {
    // Half a universe is worse than a stale one: stale reads as lag, partial reads as ships vanishing.
    Game::Universe universe;
    for (int at = 0; at < 60; ++at)
      SpawnAt(universe, static_cast<float>(at) * 40.0f, 0.0f);
    universe.Step();

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::IsTrue(writer.Write(universe, transport) > 2, L"60 ships did not need three fragments");

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
    Game::Universe universe;
    SpawnAt(universe, 0.0f, 0.0f);
    const Game::ShipId order[] = {0};
    universe.IssueMoveOrder(order, Game::LocalPos(0.0f, 900.0f), false, 0.0f);

    CaptureTransport early;
    Game::SnapshotWriter writer;
    for (int tick = 0; tick < 10; ++tick)
      universe.Step();
    Assert::AreEqual(1u, writer.Write(universe, early), L"the early snapshot did not send");

    CaptureTransport late;
    for (int tick = 0; tick < 10; ++tick)
      universe.Step();
    Assert::AreEqual(1u, writer.Write(universe, late), L"the later snapshot did not send");

    Game::SnapshotReceiver receiver;
    Assert::IsTrue(receiver.Accept(late.sent[0]), L"the later snapshot did not apply");
    const std::uint64_t applied = receiver.Latest().tick;

    Assert::IsFalse(receiver.Accept(early.sent[0]), L"an older snapshot was applied over a newer one");
    Assert::AreEqual(applied, receiver.Latest().tick, L"an older snapshot changed what was current");
  }

  TEST_METHOD(AMalformedDatagramIsRefusedRatherThanRead)
  {
    Game::Universe universe;
    SpawnAt(universe, 0.0f, 0.0f);
    universe.Step();

    CaptureTransport transport;
    Game::SnapshotWriter writer;
    (void)writer.Write(universe, transport);

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
    Game::Universe universe;
    for (int at = 0; at < 60; ++at)
      SpawnAt(universe, static_cast<float>(at) * 40.0f, 0.0f);
    universe.Step();

    CaptureTransport transport;
    transport.refuseFrom = 2;
    Game::SnapshotWriter writer;
    Assert::AreEqual(2u, writer.Write(universe, transport), L"the writer did not stop at the first refusal");
  }

  TEST_METHOD(AnEmptyUniverseStillSendsASnapshot)
  {
    // "No ships" is information. No snapshot at all is indistinguishable from a stalled server.
    Game::Universe universe;
    CaptureTransport transport;
    Game::SnapshotWriter writer;
    Assert::AreEqual(1u, writer.Write(universe, transport), L"an empty universe sent nothing");

    Game::SnapshotReceiver receiver;
    Assert::IsTrue(receiver.Accept(transport.sent[0]), L"an empty snapshot did not complete");
    Assert::IsTrue(receiver.Latest().ships.empty(), L"an empty universe produced ships");
  }

  TEST_METHOD(AnOrderForADespawnedShipResolvesToNothing)
  {
    // The reason an order carries neither a ShipId nor an index. Between the click and the order
    // arriving, a ship can die and swap-and-pop can move a stranger into its array index (ADR 0005).
    // What an order carries is an entity id, which is stronger again: a handle is a reference into
    // one Universe and an id names the ship itself (ADR 0047).
    //
    // An order names a fleet now, so the ids that can go stale between the click and the tick are
    // the ones a kind carries -- a Dock's station and an Attack's target. The property is the same
    // and the surface is smaller, which is what ADR 0049 bought.
    Game::Universe universe;
    const Game::ShipId doomed = SpawnAt(universe, 0.0f, 0.0f, Game::HullId::Interceptor, Game::FACTION_VANDAL);
    const Game::ShipId other = SpawnAt(universe, 100.0f, 0.0f, Game::HullId::Interceptor, Game::FACTION_VANDAL);
    const Game::EntityId doomedEntity = universe.EntityIdOf(doomed);
    const Game::EntityId otherEntity = universe.EntityIdOf(other);

    Game::FleetOrder sent;
    sent.slot = 1;
    sent.kind = Game::FleetOrderKind::Attack;
    sent.target = doomedEntity;

    CaptureTransport transport;
    Assert::IsTrue(Game::WriteFleetOrder(sent, transport), L"the order did not send");
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(doomed)), L"the despawn failed");

    Game::FleetOrder got;
    Assert::AreEqual(static_cast<std::size_t>(1), transport.sentReliable.size(), L"the order did not take the reliable lane");
    Assert::IsTrue(transport.sent.empty(), L"the order also went out as a datagram");
    Assert::IsTrue(Game::ReadFleetOrder(transport.sentReliable[0], got), L"the order did not decode");
    Assert::AreEqual(Game::INVALID_SHIP_ID, universe.ResolveEntity(got.target), L"a dead ship's id resolved to something");

    // The survivor is still there, but swap-and-pop moved it into the freed slot, so its ShipId is
    // not the one it had when the order was written -- and its id is. That is ADR 0005 and ADR 0047
    // in one assertion: the identity follows the ship, the array index does not.
    const Game::ShipId resolved = universe.ResolveEntity(otherEntity);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, resolved, L"the surviving ship's id stopped resolving");
    Assert::AreNotEqual(other, resolved, L"the survivor's index did not move, so this test proves nothing");
    Assert::IsTrue(universe.EntityIdOf(resolved) == otherEntity, L"the id resolved to a stranger");
  }

  TEST_METHOD(EveryDepartureSurvivesEveryDatagramBeingLost)
  {
    // The row this slice exists for, and the one that retires finding E1 in
    // Design/Archive/MmoScalabilityReview.md: with the datagram lane dropping everything, a client is still
    // told about every leave and every death. Before slice 3b those lists rode in the first snapshot
    // fragment, so this test could not have passed -- a lost fragment was a ghost ship for the rest
    // of the match, and nothing repeated it.
    Neuron::LoopbackTransport server;
    Neuron::LoopbackTransport client;
    Neuron::LoopbackTransport::Desc desc;
    desc.dropOneInN = 1; // no datagram survives
    Neuron::LoopbackTransport::Connect(server, client, desc);

    Game::Universe universe;
    const Game::ShipId leaving = SpawnAt(universe, 0.0f, 0.0f);
    const Game::ShipId dying = SpawnAt(universe, 50.0f, 0.0f);
    const Game::EntityId leavingEntity = universe.EntityIdOf(leaving);
    const Game::EntityId dyingEntity = universe.EntityIdOf(dying);

    server.AdvanceTo(0);
    client.AdvanceTo(0);

    Game::SnapshotWriter writer;
    const std::array<Game::EntityId, 1> left{leavingEntity};
    const std::array<Game::EntityId, 1> destroyed{dyingEntity};
    (void)writer.WriteInterest(universe, {}, left, destroyed, {}, {}, server);
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

    Game::FleetOrder order;
    order.slot = 3;
    order.kind = Game::FleetOrderKind::Move;
    order.point = Game::LocalPos(120.0f, -40.0f);
    order.hasFacing = true;
    order.facingRad = 0.5f;
    Assert::IsTrue(Game::WriteFleetOrder(order, client), L"the order was refused");

    client.Poll();
    server.Poll();

    std::vector<std::uint8_t> message(Neuron::MAX_RELIABLE_BYTES, 0u);
    const std::uint32_t size = server.ReceiveReliable(message.data(), Neuron::MAX_RELIABLE_BYTES);
    Assert::IsTrue(size > 0, L"the order did not survive the datagram lane being dead");

    Game::FleetOrder received;
    Assert::IsTrue(Game::ReadFleetOrder(std::span<const std::uint8_t>(message.data(), size), received), L"the order did not parse");
    Assert::AreEqual(3u, static_cast<std::uint32_t>(received.slot), L"the order lost its slot");
    Assert::IsTrue(received.hasFacing, L"the order lost its facing");
  }

  TEST_METHOD(ALeaveArrivingBeforeItsShipIsHarmless)
  {
    // The two lanes have no ordering between them, so a departure can overtake the update that would
    // have introduced the ship. Removing a handle the receiver does not hold is a no-op, which is
    // what makes either order safe -- and this is the test that says so rather than the comment.
    Game::Universe universe;
    const Game::ShipId ship = SpawnAt(universe, 0.0f, 0.0f);
    const Game::EntityId handle = universe.EntityIdOf(ship);

    CaptureTransport link;
    Game::SnapshotWriter writer;
    const std::array<Game::EntityId, 1> left{handle};
    (void)writer.WriteInterest(universe, {}, left, {}, {}, {}, link);
    Assert::AreEqual(static_cast<std::size_t>(1), link.sentReliable.size(), L"the departure did not go on the reliable lane");

    Game::SnapshotReceiver receiver;
    Assert::IsTrue(receiver.Accept(link.sentReliable[0]), L"the receiver refused a departure for a ship it never held");
    Assert::IsTrue(receiver.Latest().ships.empty(), L"a departure for an unknown ship added one");
  }

  TEST_METHOD(DeparturesDoNotRideInTheSnapshotAnyMore)
  {
    // The structural half of the change: nothing about a leave is on the datagram lane, so a
    // fragment's size no longer depends on how many ships left this update.
    Game::Universe universe;
    for (int at = 0; at < 3; ++at)
      (void)SpawnAt(universe, static_cast<float>(at) * 40.0f, 0.0f);
    const Game::EntityId gone = universe.EntityIdOf(0);

    CaptureTransport withDepartures;
    Game::SnapshotWriter a;
    const std::array<Game::EntityId, 1> left{gone};
    (void)a.WriteInterest(universe, {}, left, {}, {}, {}, withDepartures);

    CaptureTransport withNone;
    Game::SnapshotWriter b;
    (void)b.WriteInterest(universe, {}, {}, {}, {}, {}, withNone);

    Assert::AreEqual(withNone.sent.size(), withDepartures.sent.size(), L"a departure changed how many datagrams an update took");
    Assert::AreEqual(withNone.sent[0].size(), withDepartures.sent[0].size(), L"a departure changed the size of a snapshot fragment");
    Assert::AreEqual(static_cast<std::size_t>(1), withDepartures.sentReliable.size(), L"the departure did not take the reliable lane");
    Assert::IsTrue(withNone.sentReliable.empty(), L"an update with nothing to state sent a departure message anyway");
  }

  TEST_METHOD(ARefusedDepartureIsCounted)
  {
    // Nothing repeats a refused leave, so the gap has to be visible. A number that should be zero
    // is worth more than a comment saying it should be.
    Game::Universe universe;
    const Game::ShipId ship = SpawnAt(universe, 0.0f, 0.0f);
    const Game::EntityId handle = universe.EntityIdOf(ship);

    CaptureTransport link;
    link.refuseReliable = true;

    Game::SnapshotWriter writer;
    const std::array<Game::EntityId, 1> left{handle};
    (void)writer.WriteInterest(universe, {}, left, {}, {}, {}, link);
    Assert::AreEqual(1u, writer.RefusedLeaveCount(), L"a refused departure was not counted");
    Assert::IsTrue(link.sentReliable.empty(), L"a refused lane still captured a message");
  }
};
} // namespace GameLogicTests
