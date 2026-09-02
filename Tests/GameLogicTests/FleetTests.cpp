#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// One owner per faction, which is what every row here means: these suites were written when the key
// WAS the faction, and this keeps each of them saying exactly what it said (Design/Archive/OwnerKey-work-order.md).
[[nodiscard]] Game::Issuer IssuerFor(Game::FactionId _faction)
{
  return Game::Issuer{(_faction == Game::FACTION_PLAYER) ? Game::OWNER_LOCAL : Game::OwnerId{_faction} + 1u, _faction};
}

// A faction id is one byte, and a byte is a character to anything that prints one, so a failure
// would report an unprintable glyph rather than "1". Widened for the assertion, never for the wire
// -- StationTests' helper, for StationTests' reason.
[[nodiscard]] std::uint32_t Faction(Game::FactionId _faction)
{
  return _faction;
}

// _count ships of one faction, spaced far enough apart that nothing in this file is measuring
// avoidance by accident.
std::vector<Game::ShipId> SpawnLine(Game::Universe& _universe, int _count, Game::FactionId _faction = Game::FACTION_PLAYER, float _z = 0.0f)
{
  std::vector<Game::ShipId> ships;
  for (int at = 0; at < _count; ++at)
  {
    ships.push_back(_universe.SpawnShip(Game::LocalPos(static_cast<float>(at) * 300.0f - 1200.0f, _z), 0.0f,
                                        static_cast<std::uint32_t>(Game::HullId::Corvette), _faction));
  }
  return ships;
}

// The ships a fleet names right now, resolved back to ids. A stale handle inside the count resolves
// to INVALID_SHIP_ID rather than being skipped, so a row that kept one shows up as a wrong list and
// not as a short one.
std::vector<Game::ShipId> MembersOf(const Game::Universe& _universe, Game::Universe::FleetId _fleet)
{
  const Game::Universe::Fleet& row = _universe.FleetOf(_fleet);
  std::vector<Game::ShipId> ids;
  for (std::uint32_t at = 0; at < row.memberCount; ++at)
    ids.push_back(_universe.Resolve(row.members[at]));
  return ids;
}

// Something in most of the tables the tick already walks, and no fleet in any of them: architecture,
// a station with a garrison, a move order under way, a ship on patrol, and a docking in flight.
void BuildFleetlessScene(Game::Universe& _universe)
{
  const Game::ShipId post =
    _universe.SpawnShip(Game::LocalPos(-260.0f, 700.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);

  Game::Universe::StationDesc desc;
  desc.ownerFaction = Game::FACTION_VANGUARD;
  desc.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
  desc.protectorComplement = 2;
  (void)_universe.MakeStation(post, desc);

  const std::vector<Game::ShipId> ordered = SpawnLine(_universe, 3, Game::FACTION_PLAYER, -400.0f);
  (void)_universe.IssueMoveOrder(ordered, Game::LocalPos(300.0f, 900.0f), true, 0.4f);

  const Game::ShipId guard =
    _universe.SpawnShip(Game::PatrolRingPoint(Game::LocalPos(-260.0f, 700.0f), 0, 420.0f), Game::PatrolRingHeadingRad(0),
                        static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANGUARD);
  _universe.AssignPatrol(guard, post, 420.0f, 11.0f);

  const Game::ShipId visitor =
    _universe.SpawnShip(Game::LocalPos(-260.0f, 1120.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
  const Game::ShipId dockOrder[] = {visitor};
  (void)_universe.IssueDockOrder(dockOrder, post, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER});
}

// A station at a place, facing a way. The heading is which way is out: the launch fan is centred on
// it, so a test that cares where ships appear says so here.
Game::Universe::StationId MakeStationAt(Game::Universe& _universe, float _x, float _z, float _headingRad = 0.0f,
                                        Game::FactionId _owner = Game::FACTION_VANGUARD)
{
  const Game::ShipId structure =
    _universe.SpawnShip(Game::LocalPos(_x, _z), _headingRad, static_cast<std::uint32_t>(Game::HullId::Structure), _owner);
  Game::Universe::StationDesc desc;
  desc.ownerFaction = _owner;
  return _universe.MakeStation(structure, desc);
}

// Ships parked at a station's door and told to dock, so that a ledger has something in it. They
// spawn inside capture range, so the one tick below puts them inside.
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
  (void)_universe.IssueDockOrder(ships, structure, IssuerFor(_faction));
  _universe.Step();
}

// The same as DockShips, told who is docking rather than only what they are. Two owners in one
// faction is the case the faction-keyed helper above cannot express.
void DockShipsFor(Game::Universe& _universe, Game::Universe::StationId _station, Game::HullId _hull, int _count,
                  const Game::Issuer& _issuer)
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
    ships.push_back(_universe.SpawnShip(pos, bearingRad, static_cast<std::uint32_t>(_hull), _issuer.faction));
  }
  (void)_universe.IssueDockOrder(ships, structure, _issuer);
  _universe.Step();
}

std::vector<std::uint32_t> ZeroCounts()
{
  return std::vector<std::uint32_t>(Game::HULL_COUNT, 0u);
}

void Want(std::vector<std::uint32_t>& _counts, Game::HullId _hull, std::uint32_t _many)
{
  _counts[static_cast<std::size_t>(_hull)] = _many;
}

// A ComposeResult is an enum class, and the framework can only print what it has a ToString for.
// Widened for the assertion so a failure names the refusal rather than an unprintable glyph.
[[nodiscard]] int Code(Game::Universe::ComposeResult _result)
{
  return static_cast<int>(_result);
}

[[nodiscard]] int Code(Game::Universe::FleetOrderResult _result)
{
  return static_cast<int>(_result);
}

// The overlap between two ships as the simulation itself computes it, so that a launch cannot be
// declared clear by measuring something the narrow phase does not (SeparationTests' helper).
[[nodiscard]] float OverlapBetween(const Game::Universe& _universe, Game::ShipId _a, Game::ShipId _b)
{
  const Game::ShipState& first = _universe.Ship(_a);
  const Game::ShipState& second = _universe.Ship(_b);
  const Game::HullSpec& firstHull = Game::HullSpecOf(first.hullId);
  const Game::HullSpec& secondHull = Game::HullSpecOf(second.hullId);

  const Game::Capsule self{
    0.0f, 0.0f, std::sin(first.headingRad), std::cos(first.headingRad), firstHull.capsuleHalfLengthMetres, firstHull.capsuleRadiusMetres};
  const Game::Capsule against{Game::OffsetX(first.posUniverse, second.posUniverse),
                              Game::OffsetZ(first.posUniverse, second.posUniverse),
                              std::sin(second.headingRad),
                              std::cos(second.headingRad),
                              secondHull.capsuleHalfLengthMetres,
                              secondHull.capsuleRadiusMetres};
  const Game::Contact contact = Game::CapsuleContact(self, against, _a, _b);
  return contact.touching ? contact.overlapMetres : 0.0f;
}

// A fleet's live members as ship ids, for a test that wants to look at the hulls.
std::vector<Game::ShipId> LiveMembers(const Game::Universe& _universe, Game::Universe::FleetId _fleet)
{
  std::vector<Game::ShipId> ids;
  const Game::Universe::Fleet& row = _universe.FleetOf(_fleet);
  for (std::uint32_t at = 0; at < row.memberCount; ++at)
  {
    const Game::ShipId id = _universe.Resolve(row.members[at]);
    if (id != Game::INVALID_SHIP_ID)
      ids.push_back(id);
  }
  return ids;
}

// A fleet of _count Corvettes, composed at a station and flown all the way out of it. Every test
// below that orders a fleet starts from one, because an order is only interesting once there is
// something in space to give it to.
Game::Universe::FleetId LaunchedFleet(Game::Universe& _universe, Game::Universe::StationId _station, std::uint8_t _slot, int _count,
                                      Game::HullId _hull = Game::HullId::Corvette)
{
  DockShips(_universe, _station, _hull, _count);
  std::vector<std::uint32_t> counts = ZeroCounts();
  Want(counts, _hull, static_cast<std::uint32_t>(_count));
  Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                   Code(_universe.ComposeFleet(_station, _slot, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                   L"the compose was refused");
  for (int tick = 0; tick < _count * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 400; ++tick)
    _universe.Step();
  const Game::Universe::FleetId fleet = _universe.FleetInSlot(Game::OWNER_LOCAL, _slot);
  Assert::AreEqual(static_cast<std::uint32_t>(_count), _universe.FleetOf(fleet).memberCount, L"the fleet did not finish launching");
  return fleet;
}

Game::Universe::FleetCommand MoveTo(const Game::UniversePos& _point, bool _hasFacing = false, float _facingRad = 0.0f)
{
  Game::Universe::FleetCommand command;
  command.kind = Game::FleetOrderKind::Move;
  command.point = _point;
  command.hasFacing = _hasFacing;
  command.facingRad = _facingRad;
  return command;
}

Game::Universe::FleetCommand AttackOf(Game::ShipId _target)
{
  Game::Universe::FleetCommand command;
  command.kind = Game::FleetOrderKind::Attack;
  command.target = _target;
  return command;
}

// A fleet of one Corvette and one Miner, launched: the smallest mix that can tell a combatant from
// a hull that carries ore, which is the whole of what the defense turns on.
Game::Universe::FleetId MixedFleet(Game::Universe& _universe, Game::Universe::StationId _station, std::uint8_t _slot)
{
  DockShips(_universe, _station, Game::HullId::Corvette, 1);
  DockShips(_universe, _station, Game::HullId::Miner, 1);
  std::vector<std::uint32_t> counts = ZeroCounts();
  Want(counts, Game::HullId::Corvette, 1);
  Want(counts, Game::HullId::Miner, 1);
  Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                   Code(_universe.ComposeFleet(_station, _slot, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                   L"the compose was refused");
  for (int tick = 0; tick < 2 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 400; ++tick)
    _universe.Step();
  const Game::Universe::FleetId fleet = _universe.FleetInSlot(Game::OWNER_LOCAL, _slot);
  Assert::AreEqual(2u, _universe.FleetOf(fleet).memberCount, L"the fleet did not finish launching");
  return fleet;
}

// A fleet with nothing in it that shoots. It exists for the rows that want a defense's *posture*
// held still while something else is measured: an armed fleet now kills what it is roused against,
// and a row about the alert would otherwise be measuring how fast a Corvette shoots (ADR 0052).
Game::Universe::FleetId UnarmedFleet(Game::Universe& _universe, Game::Universe::StationId _station, std::uint8_t _slot)
{
  DockShips(_universe, _station, Game::HullId::Miner, 1);
  std::vector<std::uint32_t> counts = ZeroCounts();
  Want(counts, Game::HullId::Miner, 1);
  Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                   Code(_universe.ComposeFleet(_station, _slot, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                   L"the compose was refused");
  for (int tick = 0; tick < static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 400; ++tick)
    _universe.Step();
  const Game::Universe::FleetId fleet = _universe.FleetInSlot(Game::OWNER_LOCAL, _slot);
  Assert::AreEqual(1u, _universe.FleetOf(fleet).memberCount, L"the fleet did not finish launching");
  return fleet;
}

// The fleet's members split the way the defense splits them.
Game::ShipId CombatantOf(const Game::Universe& _universe, Game::Universe::FleetId _fleet)
{
  for (const Game::ShipId member : LiveMembers(_universe, _fleet))
  {
    if (Game::HullSpecOf(_universe.Ship(member).hullId).combatant)
      return member;
  }
  return Game::INVALID_SHIP_ID;
}

Game::ShipId NonCombatantOf(const Game::Universe& _universe, Game::Universe::FleetId _fleet)
{
  for (const Game::ShipId member : LiveMembers(_universe, _fleet))
  {
    if (!Game::HullSpecOf(_universe.Ship(member).hullId).combatant)
      return member;
  }
  return Game::INVALID_SHIP_ID;
}
} // namespace

TEST_CLASS(FleetTests)
{
public:
  TEST_METHOD(AFleetIsFormedFromLiveShips)
  {
    Game::Universe universe;
    const std::vector<Game::ShipId> ships = SpawnLine(universe, 3);

    const Game::Universe::FleetId fleet = universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, fleet, L"a fleet of three live ships was refused");
    Assert::AreEqual(1u, universe.FleetCount(), L"the table did not gain a row");

    const Game::Universe::Fleet& row = universe.FleetOf(fleet);
    Assert::AreEqual(3u, row.memberCount, L"the fleet does not hold what it was formed from");
    Assert::AreEqual(Faction(Game::FACTION_PLAYER), Faction(row.ownerFaction), L"the fleet has the wrong owner");
    Assert::AreEqual(0u, static_cast<std::uint32_t>(row.slot), L"the fleet is in the wrong slot");

    // In the order handed in. Nothing reads that order yet; slice 3's formation solve will, which is
    // why it is pinned here rather than discovered there.
    Assert::IsTrue(MembersOf(universe, fleet) == ships, L"the members are not the ships that were handed in");

    Assert::AreEqual(fleet, universe.FleetInSlot(Game::OWNER_LOCAL, 0), L"the slot does not name the fleet");
    for (const Game::ShipId ship : ships)
      Assert::AreEqual(fleet, universe.FleetAt(ship), L"a member does not find its fleet");

    // An id past the end reads back an empty fleet rather than past the end of the table.
    Assert::AreEqual(0u, universe.FleetOf(Game::Universe::INVALID_FLEET_ID).memberCount, L"a stale id read something");
  }

  TEST_METHOD(TheSixthFleetIsRefused)
  {
    Game::Universe universe;
    const std::vector<Game::ShipId> ships = SpawnLine(universe, 7);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(universe.HandleOf(ship));

    for (std::uint32_t slot = 0; slot < Game::FLEET_SLOTS; ++slot)
    {
      const Game::ShipId one[] = {ships[slot]};
      Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                          universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, static_cast<std::uint8_t>(slot), one),
                          L"one of the five slots refused a fleet");
    }
    Assert::AreEqual(Game::FLEET_SLOTS, universe.FleetCount(), L"five fleets did not make five rows");

    // A sixth fleet has nowhere to be: there is no slot past the fifth, and every one of them is
    // held. Both refusals leave the table exactly as it was.
    const Game::ShipId spare[] = {ships[5]};
    Assert::AreEqual(
      Game::Universe::INVALID_FLEET_ID,
      universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, static_cast<std::uint8_t>(Game::FLEET_SLOTS), spare),
      L"a slot past the fifth was accepted");
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 2, spare),
                     L"an occupied slot was accepted");
    Assert::AreEqual(Game::FLEET_SLOTS, universe.FleetCount(), L"a refused form changed the table");

    // Retiring one frees its slot, which is the whole of what "five active fleets" means: a slot is
    // held from the form until the last ship has left space, and not a tick longer.
    Assert::IsTrue(universe.DespawnShip(handles[2]), L"the despawn failed");
    universe.Step();
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FleetInSlot(Game::OWNER_LOCAL, 2), L"the retired slot is still held");

    // Through the handle, because the despawn above moved somebody's id (ADR 0005).
    const Game::ShipId refill[] = {universe.Resolve(handles[6])};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                        universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 2, refill),
                        L"the freed slot refused a fleet");
  }

  TEST_METHOD(TheNinthShipIsRefused)
  {
    Game::Universe universe;
    const std::vector<Game::ShipId> ships = SpawnLine(universe, 9);

    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships),
                     L"nine ships made a fleet");
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, {}),
                     L"an empty fleet was formed");
    Assert::AreEqual(0u, universe.FleetCount(), L"a refused form made a row");

    const std::span<const Game::ShipId> eight(ships.data(), Game::MAX_FLEET_SHIPS);
    const Game::Universe::FleetId full = universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, eight);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, full, L"eight ships were refused");
    Assert::AreEqual(Game::MAX_FLEET_SHIPS, universe.FleetOf(full).memberCount, L"the full fleet came out short");

    // The ninth ship was refused as a member, so it is still nobody's.
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FleetAt(ships[8]), L"the refused ship was claimed anyway");
  }

  TEST_METHOD(AFleetTakesOnlyItsOwnersShips)
  {
    Game::Universe universe;
    std::vector<Game::ShipId> ships = SpawnLine(universe, 2);
    const Game::ShipId stranger =
      universe.SpawnShip(Game::LocalPos(600.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    ships.push_back(stranger);

    // Refused whole rather than filtered. IssueMoveOrder drops a stranger from the order because an
    // order of two ships and an order of three are the same kind of thing; a fleet of two where
    // three were asked for is a fleet nobody asked for, and the size is one of the rules.
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships),
                     L"a foreign ship was taken into a fleet");
    Assert::AreEqual(0u, universe.FleetCount(), L"a refused form made a row");
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FleetAt(ships[0]), L"a refused form claimed a ship anyway");

    // The table is faction-generic: the Vandals may hold slots, and one faction's slot 0 is not
    // another's.
    const Game::ShipId theirs[] = {stranger};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                        universe.FormFleet(Game::Issuer{Game::OwnerId{2}, Game::FACTION_VANDAL}, 0, theirs), L"a Vandal fleet was refused");
    const Game::ShipId ours[] = {ships[0]};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                        universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ours),
                        L"two factions cannot both hold slot 0");
    Assert::AreEqual(2u, universe.FleetCount(), L"the two fleets did not make two rows");
  }

  TEST_METHOD(AShipBelongsToOneFleet)
  {
    Game::Universe universe;
    const std::vector<Game::ShipId> ships = SpawnLine(universe, 3);

    const Game::ShipId first[] = {ships[0], ships[1]};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                        universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, first),
                        L"the first fleet was refused");

    // The fleet-only model, held at the only place that makes a membership.
    const Game::ShipId poached[] = {ships[1], ships[2]};
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID,
                     universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 1, poached),
                     L"a ship joined a second fleet");

    // The same defect arriving from inside one call: it would spend two of the eight places on one
    // ship, and the fleet would be a member short of the size it was asked for.
    const Game::ShipId twice[] = {ships[2], ships[2]};
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 1, twice),
                     L"one ship filled two places");
    Assert::AreEqual(1u, universe.FleetCount(), L"a refused form made a row");
  }

  TEST_METHOD(ALossPrunesTheFleet)
  {
    Game::Universe universe;
    const std::vector<Game::ShipId> ships = SpawnLine(universe, 4);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(universe.HandleOf(ship));

    const Game::Universe::FleetId fleet = universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 3, ships);
    Assert::AreEqual(4u, universe.FleetOf(fleet).memberCount, L"the fleet did not form with four");

    Assert::IsTrue(universe.DespawnShip(handles[1]), L"the despawn failed");
    // Not before the tick: the pass is where a fleet learns, and it learns on the tick the ship left
    // rather than the tick after, which is why it runs last in the standing-intent slot.
    universe.Step();

    const Game::Universe::FleetId after = universe.FleetInSlot(Game::OWNER_LOCAL, 3);
    Assert::AreEqual(3u, universe.FleetOf(after).memberCount, L"the dead member is still counted");

    const std::vector<Game::ShipId> survivors = {universe.Resolve(handles[0]), universe.Resolve(handles[2]), universe.Resolve(handles[3])};
    Assert::IsTrue(MembersOf(universe, after) == survivors, L"pruning lost or reordered the survivors");

    // The vacated entry is a null handle rather than a stale one nobody looks at, which is what
    // makes a saved row equal the row it was saved from.
    Assert::AreEqual(0u, universe.FleetOf(after).members[3].generation, L"the vacated entry kept a stale handle");
  }

  TEST_METHOD(MembersSurviveSwapAndPop)
  {
    Game::Universe universe;
    // The bystander is spawned first, so despawning it moves the LAST ship into its index -- and the
    // last ship is a member. That is exactly the retarget ADR 0005 exists to prevent, at fleet
    // grain: a row holding raw ids would come out of this naming a stranger.
    const Game::ShipId bystander =
      universe.SpawnShip(Game::LocalPos(900.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
    const std::vector<Game::ShipId> ships = SpawnLine(universe, 3);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(universe.HandleOf(ship));

    const Game::Universe::FleetId fleet = universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, fleet, L"the fleet was refused");

    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(bystander)), L"the despawn failed");
    universe.Step();

    const Game::Universe::FleetId after = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreEqual(3u, universe.FleetOf(after).memberCount, L"a bystander's death cost the fleet a member");

    const std::vector<Game::ShipId> moved = {universe.Resolve(handles[0]), universe.Resolve(handles[1]), universe.Resolve(handles[2])};
    Assert::AreNotEqual(ships[2], moved[2], L"nothing moved, so this test proved nothing");
    Assert::IsTrue(MembersOf(universe, after) == moved, L"the fleet names different ships than it was formed with");
    for (const Game::ShipId ship : moved)
      Assert::AreEqual(after, universe.FleetAt(ship), L"a moved member no longer finds its fleet");
  }

  TEST_METHOD(TheLastLossRetiresTheFleet)
  {
    Game::Universe universe;
    const std::vector<Game::ShipId> ships = SpawnLine(universe, 3);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(universe.HandleOf(ship));

    const Game::ShipId doomed[] = {ships[0]};
    const Game::ShipId keeper[] = {ships[1], ships[2]};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                        universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, doomed), L"fleet 1 was refused");
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                        universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 4, keeper), L"fleet 5 was refused");
    Assert::AreEqual(2u, universe.FleetCount(), L"two fleets did not make two rows");

    Assert::IsTrue(universe.DespawnShip(handles[0]), L"the despawn failed");
    universe.Step();

    Assert::AreEqual(1u, universe.FleetCount(), L"the emptied fleet was not retired");
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FleetInSlot(Game::OWNER_LOCAL, 0), L"the retired slot is still held");

    // The survivor keeps its identity across a retirement that moved its row, which is why an owner
    // and a slot -- and never an index -- is what anything holds across a tick.
    const Game::Universe::FleetId survivor = universe.FleetInSlot(Game::OWNER_LOCAL, 4);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, survivor, L"the surviving fleet lost its slot");
    Assert::AreEqual(2u, universe.FleetOf(survivor).memberCount, L"the surviving fleet lost a member");
    Assert::IsTrue(MembersOf(universe, survivor) == std::vector<Game::ShipId>({universe.Resolve(handles[1]), universe.Resolve(handles[2])}),
                   L"the surviving fleet names different ships");
  }

  TEST_METHOD(TheFleetTableSurvivesTheRoundTrip)
  {
    Game::Universe original;
    const std::vector<Game::ShipId> ships = SpawnLine(original, 6);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(original.HandleOf(ship));

    const Game::ShipId first[] = {ships[0], ships[1], ships[2]};
    const Game::ShipId second[] = {ships[3], ships[4], ships[5]};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                        original.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 1, first), L"fleet 2 was refused");
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                        original.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 3, second), L"fleet 4 was refused");

    // One of them pruned before the save, so the file carries a row whose count is not the count it
    // was formed with and whose tail has been cleared.
    Assert::IsTrue(original.DespawnShip(handles[4]), L"the despawn failed");
    original.Step();

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(original, saved);

    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");
    Assert::AreEqual(original.FleetCount(), reloaded.FleetCount(), L"the fleet count did not survive");

    for (std::uint32_t slot = 0; slot < Game::FLEET_SLOTS; ++slot)
    {
      const Game::Universe::FleetId here = original.FleetInSlot(Game::OWNER_LOCAL, static_cast<std::uint8_t>(slot));
      const Game::Universe::FleetId there = reloaded.FleetInSlot(Game::OWNER_LOCAL, static_cast<std::uint8_t>(slot));
      Assert::IsTrue((here == Game::Universe::INVALID_FLEET_ID) == (there == Game::Universe::INVALID_FLEET_ID),
                     L"a slot changed occupancy across the reload");
      if (here == Game::Universe::INVALID_FLEET_ID)
        continue;
      Assert::IsTrue(MembersOf(original, here) == MembersOf(reloaded, there), L"a fleet's members did not survive the reload");
      Assert::AreEqual(Faction(original.FleetOf(here).ownerFaction), Faction(reloaded.FleetOf(there).ownerFaction),
                       L"an owner did not survive the reload");
    }

    const Game::Universe::FleetId pruned = reloaded.FleetInSlot(Game::OWNER_LOCAL, 3);
    Assert::AreEqual(2u, reloaded.FleetOf(pruned).memberCount, L"the pruned fleet came back the wrong size");
    Assert::AreEqual(0u, reloaded.FleetOf(pruned).members[2].generation, L"a vacated entry came back holding something");

    // Truncated refuses and changes nothing, which is what failing closed means for anything that
    // parses content (AGENTS.md 5).
    Game::Universe untouched;
    (void)SpawnLine(untouched, 1);
    saved.resize(saved.size() - 1);
    Assert::IsFalse(Game::ReadUniverseState(saved, untouched), L"a truncated state loaded");
    Assert::AreEqual(1u, untouched.ShipCount(), L"a refused load changed the universe");
    Assert::AreEqual(0u, untouched.FleetCount(), L"a refused load changed the universe");
  }

  TEST_METHOD(AFleetlessUniverseTicksAsToday)
  {
    // What "as today" means in full is the rest of GameLogicTests passing unchanged: every scene in
    // the suite has an empty fleet table and a pass that visits nothing. This is the half a test can
    // hold -- that the new pass neither grows a fleet in a universe nobody formed one in, nor changes
    // what such a universe does.
    Game::Universe first;
    Game::Universe second;
    BuildFleetlessScene(first);
    BuildFleetlessScene(second);

    for (int tick = 0; tick < 240; ++tick)
    {
      first.Step();
      second.Step();
      Assert::AreEqual(0u, first.FleetCount(), L"a universe nobody formed a fleet in grew one");
    }

    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
    Game::WriteUniverseState(first, before);
    Game::WriteUniverseState(second, after);
    Assert::IsTrue(before == after, L"two identical fleetless universes diverged");
  }

  TEST_METHOD(TheSameFleetProducesTheSameRun)
  {
    // The replay gate over the row: the same forms and the same losses in two universes, compared as
    // whole states every tick rather than once at the end, so a divergence says when.
    Game::Universe first;
    Game::Universe second;

    for (Game::Universe* universe : {&first, &second})
    {
      BuildFleetlessScene(*universe);
      const std::vector<Game::ShipId> wing = SpawnLine(*universe, 5, Game::FACTION_PLAYER, 1500.0f);
      const Game::ShipId three[] = {wing[0], wing[1], wing[2]};
      const Game::ShipId two[] = {wing[3], wing[4]};
      Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                          universe->FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, three), L"fleet 1 was refused");
      Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID,
                          universe->FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 4, two), L"fleet 5 was refused");
      (void)universe->IssueMoveOrder(three, Game::LocalPos(1200.0f, 2400.0f), false, 0.0f);
    }

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < 120; ++tick)
    {
      first.Step();
      second.Step();
      Game::WriteUniverseState(first, a);
      Game::WriteUniverseState(second, b);
      Assert::IsTrue(a == b, L"two identical runs diverged before the losses");
    }

    // A loss in each, at the same point in the run, so the prune and the retire are both replayed.
    for (Game::Universe* universe : {&first, &second})
    {
      const Game::Universe::Fleet& row = universe->FleetOf(universe->FleetInSlot(Game::OWNER_LOCAL, 4));
      const Game::ShipHandle one = row.members[0];
      const Game::ShipHandle other = row.members[1];
      Assert::IsTrue(universe->DespawnShip(one), L"the despawn failed");
      Assert::IsTrue(universe->DespawnShip(other), L"the despawn failed");
    }

    for (int tick = 0; tick < 120; ++tick)
    {
      first.Step();
      second.Step();
      Game::WriteUniverseState(first, a);
      Game::WriteUniverseState(second, b);
      Assert::IsTrue(a == b, L"two identical runs diverged after the losses");
    }

    Assert::AreEqual(1u, first.FleetCount(), L"the emptied fleet was not retired during the run");
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, first.FleetInSlot(Game::OWNER_LOCAL, 4), L"the emptied slot is still held");
  }

  // --- compose (slice 2) -------------------------------------------------------------------------

  TEST_METHOD(AFleetIsComposedFromTheLedger)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 3);
    DockShips(universe, station, Game::HullId::Frigate, 2);
    DockShips(universe, station, Game::HullId::Miner, 1);
    Assert::AreEqual(6u, static_cast<std::uint32_t>(universe.StationOf(station).docked.size()), L"the ledger did not fill");

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Frigate, 2);
    Want(counts, Game::HullId::Corvette, 1);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 2, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"a compose the ledger covers was refused");

    const Game::Universe::FleetId fleet = universe.FleetInSlot(Game::OWNER_LOCAL, 2);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, fleet, L"the slot is not held from the tick it was composed on");

    const Game::Universe::Fleet& row = universe.FleetOf(fleet);
    Assert::AreEqual(0u, row.memberCount, L"a composed fleet starts with ships in space");
    Assert::AreEqual(3u, row.manifestCount, L"the manifest is not what was asked for");

    // Ascending hull id: Corvette (2) then the two Frigates (4). It is the launch order, and the
    // launch order is the order members join in.
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), row.manifest[0], L"the manifest is not in ascending hull id");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Frigate), row.manifest[1], L"the manifest is not in ascending hull id");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Frigate), row.manifest[2], L"the manifest is not in ascending hull id");

    // The rows left the ledger, and only those rows.
    const std::vector<Game::Universe::DockedShip>& ledger = universe.StationOf(station).docked;
    Assert::AreEqual(3u, static_cast<std::uint32_t>(ledger.size()), L"the drawn rows are still in the ledger");
    std::uint32_t corvettes = 0, miners = 0;
    for (const Game::Universe::DockedShip& docked : ledger)
    {
      corvettes += (docked.hullId == static_cast<std::uint32_t>(Game::HullId::Corvette)) ? 1u : 0u;
      miners += (docked.hullId == static_cast<std::uint32_t>(Game::HullId::Miner)) ? 1u : 0u;
    }
    Assert::AreEqual(2u, corvettes, L"the wrong number of Corvettes was drawn");
    Assert::AreEqual(1u, miners, L"a hull nobody asked for was drawn");
  }

  TEST_METHOD(ComposeRefusesWhatIsNotThere)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 3);
    // A neutral third party's ships in the same ledger. Whose is docked where is the station's
    // business, so they are not the player's to compose with.
    const Game::FactionId other = static_cast<Game::FactionId>(3);
    DockShips(universe, station, Game::HullId::Frigate, 2, other);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 4);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::NotDocked),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"a fleet was composed out of ships that are not there");

    counts = ZeroCounts();
    Want(counts, Game::HullId::Frigate, 1);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::NotDocked),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"a fleet was composed out of somebody else's ships");

    // A count against a hull id the table does not have. No ledger can hold one, so it is NotDocked
    // rather than a silent truncation of what was asked for.
    std::vector<std::uint32_t> overlong(Game::HULL_COUNT + 2, 0u);
    overlong[Game::HULL_COUNT + 1] = 1;
    Assert::AreEqual(Code(Game::Universe::ComposeResult::NotDocked),
                     Code(universe.ComposeFleet(station, 0, overlong, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"a hull past the table was composed");

    Assert::AreEqual(0u, universe.FleetCount(), L"a refused compose made a fleet");
    Assert::AreEqual(5u, static_cast<std::uint32_t>(universe.StationOf(station).docked.size()), L"a refused compose touched the ledger");
  }

  TEST_METHOD(AHostilePortRefusesComposition)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 3);

    // Turn the law: the same ships, the same ledger, and now an owner that holds the player hostile.
    const Game::ShipId criminal = universe.SpawnShip(Game::LocalPos(2000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    universe.RecordAggression(universe.HandleOf(criminal), station);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 2);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::RefusedStanding),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"an aggressor assembled a battle group in the port it attacked");
    Assert::AreEqual(0u, universe.FleetCount(), L"a refused compose made a fleet");
    Assert::AreEqual(3u, static_cast<std::uint32_t>(universe.StationOf(station).docked.size()), L"a refused compose touched the ledger");
  }

  TEST_METHOD(ComposeRefusesABadSlotOrAnOverfullFleet)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 9);

    std::vector<std::uint32_t> nine = ZeroCounts();
    Want(nine, Game::HullId::Corvette, 9);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::TooMany),
                     Code(universe.ComposeFleet(station, 0, nine, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"nine ships made a fleet");
    Assert::AreEqual(Code(Game::Universe::ComposeResult::TooMany),
                     Code(universe.ComposeFleet(station, 0, ZeroCounts(), Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"an empty compose made a fleet");

    std::vector<std::uint32_t> two = ZeroCounts();
    Want(two, Game::HullId::Corvette, 2);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 1, two, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"a compose the ledger covers was refused");
    Assert::AreEqual(Code(Game::Universe::ComposeResult::SlotTaken),
                     Code(universe.ComposeFleet(station, 1, two, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"one slot took two fleets");
    Assert::AreEqual(Code(Game::Universe::ComposeResult::SlotTaken),
                     Code(universe.ComposeFleet(station, static_cast<std::uint8_t>(Game::FLEET_SLOTS), two,
                                                Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"a slot past the fifth was accepted");

    Assert::AreEqual(Code(Game::Universe::ComposeResult::NotAStation),
                     Code(universe.ComposeFleet(universe.StationCount(), 2, two, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"a row that is not a station composed");
    Assert::AreEqual(1u, universe.FleetCount(), L"a refused compose made a fleet");
  }

  // --- launch ------------------------------------------------------------------------------------

  TEST_METHOD(TheManifestEmptiesOnTheMetronome)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 2);
    DockShips(universe, station, Game::HullId::Frigate, 2);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 2);
    Want(counts, Game::HullId::Frigate, 2);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"the compose was refused");

    std::uint32_t launched = 0;
    for (int tick = 0; tick < 4 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS); ++tick)
    {
      universe.Step();
      const Game::Universe::FleetId fleet = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
      Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, fleet, L"the fleet retired mid-launch");
      const Game::Universe::Fleet& row = universe.FleetOf(fleet);

      // Nothing is created and nothing is lost: a launch moves one hull from one count to the other.
      Assert::AreEqual(4u, row.memberCount + row.manifestCount, L"the composed set changed size during the launch");

      // One per FLEET_LAUNCH_EVERY_TICKS, and the first on the tick after the compose.
      const std::uint32_t due = 1u + static_cast<std::uint32_t>(tick) / Game::FLEET_LAUNCH_EVERY_TICKS;
      Assert::AreEqual(due, row.memberCount, L"the metronome is not launching one hull per cadence");
      launched = row.memberCount;
    }
    Assert::AreEqual(4u, launched, L"the manifest did not empty");

    // Ascending hull id, which is the order ComposeFleet filled the manifest in.
    const std::vector<Game::ShipId> members = LiveMembers(universe, universe.FleetInSlot(Game::OWNER_LOCAL, 0));
    Assert::AreEqual(4u, static_cast<std::uint32_t>(members.size()), L"a member was lost");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), universe.Ship(members[0]).hullId, L"launched out of order");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), universe.Ship(members[1]).hullId, L"launched out of order");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Frigate), universe.Ship(members[2]).hullId, L"launched out of order");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Frigate), universe.Ship(members[3]).hullId, L"launched out of order");
  }

  TEST_METHOD(ALaunchNeverJams)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, static_cast<int>(Game::MAX_FLEET_SHIPS));

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, Game::MAX_FLEET_SHIPS);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"the compose was refused");

    // The whole launch, and long enough after it for the formation to settle.
    const int ticks = static_cast<int>(Game::MAX_FLEET_SHIPS * Game::FLEET_LAUNCH_EVERY_TICKS) + 900;
    float worst = 0.0f;
    for (int tick = 0; tick < ticks; ++tick)
    {
      universe.Step();
      const std::vector<Game::ShipId> members = LiveMembers(universe, universe.FleetInSlot(Game::OWNER_LOCAL, 0));
      for (std::size_t a = 0; a < members.size(); ++a)
      {
        for (std::size_t b = a + 1; b < members.size(); ++b)
          worst = std::max(worst, OverlapBetween(universe, members[a], members[b]));
      }
    }
    Assert::AreEqual(0.0f, worst, 1e-2f, L"two hulls of one launch ended up inside each other");
    Assert::AreEqual(Game::MAX_FLEET_SHIPS, universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0)).memberCount,
                     L"the launch did not finish");
  }

  TEST_METHOD(TheWidestLaunchFanStaysUnderATurn)
  {
    // The fan is one slot spacing between consecutive spawn points on the circle they appear on, so
    // its width is (MAX_FLEET_SHIPS - 1) steps. A fan that wrapped past a full turn would put two
    // launches back on one bearing, which is the one way the construction could fail -- and the
    // widest composition the hull table allows is what has to be checked, not the shipped one.
    const Game::HullSpec& stationHull = Game::HullSpecOf(Game::HullId::Structure);
    float widestFanRad = 0.0f;
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    {
      const Game::HullSpec& spec = Game::HullSpecOf(hull);
      if (spec.immovable)
        continue;
      const float radius = spec.BoundingRadiusMetres();
      const float standoff = stationHull.BoundingRadiusMetres() + radius + Game::AVOID_MARGIN_METRES;
      const float step = Game::SlotSpacingMetres(radius) / standoff;
      widestFanRad = std::max(widestFanRad, static_cast<float>(Game::MAX_FLEET_SHIPS - 1) * step);
    }
    Assert::IsTrue(widestFanRad < DirectX::XM_2PI, L"a full fleet of the widest hull fans past a full turn and reuses a bearing");
  }

  TEST_METHOD(AStrandedManifestFreesItsSlot)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 3);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 3);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 4, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"the compose was refused");

    // The door goes before a single hull is out. Launching is the only way out of a ledger, so the
    // manifest can never become ships.
    Assert::IsTrue(universe.DespawnShip(universe.StationOf(station).structure), L"the structure would not despawn");

    universe.Step();
    const Game::Universe::FleetId stranded = universe.FleetInSlot(Game::OWNER_LOCAL, 4);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, stranded, L"the fleet vanished a tick early");
    Assert::AreEqual(0u, universe.FleetOf(stranded).manifestCount, L"a manifest with no door left is still waiting to launch");

    universe.Step();
    Assert::AreEqual(0u, universe.FleetCount(), L"a fleet nothing can ever fill is still holding a slot");
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FleetInSlot(Game::OWNER_LOCAL, 4), L"the slot is still held");
  }

  TEST_METHOD(ADockDismantlesTheFleet)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 3);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 3);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"the compose was refused");
    Assert::AreEqual(0u, static_cast<std::uint32_t>(universe.StationOf(station).docked.size()), L"the ledger kept the composed rows");

    for (int tick = 0; tick < 3 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 60; ++tick)
      universe.Step();
    Assert::AreEqual(3u, universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0)).memberCount, L"the fleet is not out");

    // Send it home. Docking dismantles: each capture is a ledger row and a member pruned, and the
    // slot frees when the last of them is inside.
    const Game::ShipId structure = universe.Resolve(universe.StationOf(station).structure);
    const std::vector<Game::ShipId> members = LiveMembers(universe, universe.FleetInSlot(Game::OWNER_LOCAL, 0));
    (void)universe.IssueDockOrder(members, structure, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER});

    for (int tick = 0; tick < 3000 && universe.FleetCount() != 0; ++tick)
      universe.Step();

    Assert::AreEqual(0u, universe.FleetCount(), L"the fleet did not dismantle when its last ship docked");
    Assert::AreEqual(Game::Universe::INVALID_FLEET_ID, universe.FleetInSlot(Game::OWNER_LOCAL, 0), L"the slot is still held");
    Assert::AreEqual(3u, static_cast<std::uint32_t>(universe.StationOf(station).docked.size()), L"the ledger did not get its rows back");

    // And the slot composes again from the rows that came home, which is the whole loop.
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"the freed slot would not compose again");
  }

  TEST_METHOD(TheManifestSurvivesTheRoundTrip)
  {
    Game::Universe original;
    const Game::Universe::StationId station = MakeStationAt(original, 0.0f, 0.0f, 0.7f);
    DockShips(original, station, Game::HullId::Corvette, 2);
    DockShips(original, station, Game::HullId::Frigate, 2);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 2);
    Want(counts, Game::HullId::Frigate, 2);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(original.ComposeFleet(station, 3, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"the compose was refused");

    // Saved mid-launch, so the file carries a part-empty manifest and a cooldown part-way down.
    for (int tick = 0; tick < static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 20; ++tick)
      original.Step();
    const Game::Universe::Fleet& before = original.FleetOf(original.FleetInSlot(Game::OWNER_LOCAL, 3));
    Assert::AreEqual(2u, before.memberCount, L"the scene is not mid-launch");
    Assert::IsTrue(before.launchCooldownTicks > 0, L"the cooldown is not part-way down, so this proves less than it should");

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(original, saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");

    const Game::Universe::FleetId there = reloaded.FleetInSlot(Game::OWNER_LOCAL, 3);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, there, L"the fleet did not survive the reload");
    const Game::Universe::Fleet& after = reloaded.FleetOf(there);
    Assert::AreEqual(before.manifestCount, after.manifestCount, L"the manifest count did not survive");
    Assert::AreEqual(before.launchCooldownTicks, after.launchCooldownTicks, L"the cooldown did not survive");
    Assert::IsTrue(before.launchStructure == after.launchStructure, L"the manifest lost its door");
    for (std::uint32_t at = 0; at < before.manifestCount; ++at)
      Assert::AreEqual(before.manifest[at], after.manifest[at], L"a manifest hull did not survive");
    for (std::uint32_t at = after.manifestCount; at < Game::MAX_FLEET_SHIPS; ++at)
      Assert::AreEqual(0u, after.manifest[at], L"a launched entry came back holding something");

    // And it resumes the same launch, which is the part a field-by-field comparison cannot see.
    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < 3 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS); ++tick)
    {
      original.Step();
      reloaded.Step();
      Game::WriteUniverseState(original, a);
      Game::WriteUniverseState(reloaded, b);
      Assert::IsTrue(a == b, L"a reloaded universe launched a different fleet");
    }
  }

  TEST_METHOD(TheSameComposeProducesTheSameRun)
  {
    Game::Universe first;
    Game::Universe second;
    for (Game::Universe* universe : {&first, &second})
    {
      BuildFleetlessScene(*universe);
      const Game::Universe::StationId station = MakeStationAt(*universe, 4000.0f, 0.0f, 1.1f);
      DockShips(*universe, station, Game::HullId::Interceptor, 2);
      DockShips(*universe, station, Game::HullId::Corvette, 2);
      DockShips(*universe, station, Game::HullId::Hauler, 1);

      std::vector<std::uint32_t> counts = ZeroCounts();
      Want(counts, Game::HullId::Interceptor, 2);
      Want(counts, Game::HullId::Corvette, 2);
      Want(counts, Game::HullId::Hauler, 1);
      Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                       Code(universe->ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                       L"the compose was refused");
    }

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < 5 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 300; ++tick)
    {
      first.Step();
      second.Step();
      Game::WriteUniverseState(first, a);
      Game::WriteUniverseState(second, b);
      Assert::IsTrue(a == b, L"two identical composes diverged");
    }
    Assert::AreEqual(5u, first.FleetOf(first.FleetInSlot(Game::OWNER_LOCAL, 0)).memberCount, L"the launch did not finish");
  }

  // --- orders at fleet grain (slice 3) -----------------------------------------------------------

  TEST_METHOD(AFleetOrderLowersToAFormation)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = LaunchedFleet(universe, station, 0, 3);

    const Game::UniversePos point = Game::LocalPos(1500.0f, 1000.0f);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(point, true, 1.2f))),
                     L"the fleet order was refused");
    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Move, L"the standing order was not kept");

    // About 1.6 km from the rally, which is 54 s at a Corvette's 30 m/s. The budget is that with
    // room for the acceleration and the formation, not a round number.
    for (int tick = 0; tick < 4500; ++tick)
      universe.Step();

    // Arrived, and in formation about the point: the wedge is spaced off the largest hull, so three
    // Corvettes sit within a couple of slot spacings of it.
    const float spacing = Game::SlotSpacingMetres(Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres());
    for (const Game::ShipId member : LiveMembers(universe, universe.FleetInSlot(Game::OWNER_LOCAL, 0)))
    {
      Assert::IsTrue(Game::Distance(universe.Ship(member).posUniverse, point) < 3.0f * spacing, L"a member did not arrive about the point");
      // The ordered facing was honored: every hull ends up pointing the same way.
      Assert::AreEqual(1.2f, universe.Ship(member).headingRad, 0.05f, L"a member did not settle on the ordered facing");
    }
  }

  TEST_METHOD(AFleetOrderNamesAFleetNotShips)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId mine = LaunchedFleet(universe, station, 1, 2);

    // A slot nobody holds, and a slot that does not exist.
    const Game::UniversePos point = Game::LocalPos(2000.0f, 0.0f);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::NoSuchFleet),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(point))),
                     L"an empty slot took an order");
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::NoSuchFleet),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER},
                                                   static_cast<std::uint8_t>(Game::FLEET_SLOTS), MoveTo(point))),
                     L"a slot past the fifth took an order");

    // And somebody else's slot 1. The gate is one comparison, and this is the thing it refuses.
    // Another owner as well as another faction, since the gate compares the owner now
    // (Design/Archive/OwnerKey-work-order.md).
    const Game::FactionId other = static_cast<Game::FactionId>(3);
    const Game::Issuer theirIssuer{Game::OwnerId{7}, other};
    const Game::ShipId theirs =
      universe.SpawnShip(Game::LocalPos(-3000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), other);
    const Game::ShipId theirShips[] = {theirs};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(theirIssuer, 1, theirShips),
                        L"the other faction's fleet was refused");

    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered), Code(universe.IssueFleetOrder(theirIssuer, 1, MoveTo(point))),
                     L"the owner could not order its own slot");
    Assert::IsTrue(universe.FleetOf(mine).orderKind == Game::FleetOrderKind::Idle, L"one owner's order reached another owner's slot");
  }

  TEST_METHOD(AFleetCruisesAtItsSlowestMember)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Interceptor, 2);
    DockShips(universe, station, Game::HullId::Hauler, 1);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Interceptor, 2);
    Want(counts, Game::HullId::Hauler, 1);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"the compose was refused");
    for (int tick = 0; tick < 3 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 300; ++tick)
      universe.Step();

    const float haulerTop = Game::HullSpecOf(Game::HullId::Hauler).maxSpeedMetresPerSec;
    Assert::IsTrue(haulerTop < Game::HullSpecOf(Game::HullId::Interceptor).maxSpeedMetresPerSec,
                   L"the scene has no slow ship in it, so this proves nothing");

    Assert::AreEqual(
      Code(Game::Universe::FleetOrderResult::Ordered),
      Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(Game::LocalPos(3000.0f, 0.0f)))),
      L"the fleet order was refused");
    universe.Step();
    for (const Game::ShipId member : LiveMembers(universe, universe.FleetInSlot(Game::OWNER_LOCAL, 0)))
    {
      Assert::AreEqual(haulerTop, universe.Ship(member).orderSpeedCapMetresPerSec, 0.0f, L"a member is not held to the fleet's pace");
      Assert::IsTrue(universe.Ship(member).speed <= haulerTop + 0.001f, L"a member outran the fleet");
    }

    // And it survives a dock order, whose approach the dock pass re-issues -- zeroing the cap each
    // time it does. A cap written once at lowering would be gone by the second tick.
    const Game::ShipId structure = universe.Resolve(universe.StationOf(station).structure);
    Game::Universe::FleetCommand dock;
    dock.kind = Game::FleetOrderKind::Dock;
    dock.station = structure;
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, dock)),
                     L"the dock order was refused");
    for (int tick = 0; tick < 200; ++tick)
    {
      universe.Step();
      const Game::Universe::FleetId fleet = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
      if (fleet == Game::Universe::INVALID_FLEET_ID)
        break;
      for (const Game::ShipId member : LiveMembers(universe, fleet))
        Assert::AreEqual(haulerTop, universe.Ship(member).orderSpeedCapMetresPerSec, 0.0f, L"a docking fleet lost its cruise cap");
    }
  }

  TEST_METHOD(AFleetDockOrderCarriesTheDockGates)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = LaunchedFleet(universe, station, 0, 2);
    const Game::ShipId structure = universe.Resolve(universe.StationOf(station).structure);

    // A record that is not a station at all.
    Game::Universe::FleetCommand dock;
    dock.kind = Game::FleetOrderKind::Dock;
    dock.station = LiveMembers(universe, fleet).front();
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::NotAStation),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, dock)),
                     L"a fleet was told to dock at a ship");
    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Idle, L"a refused order was written to the row");

    // A station whose owner has turned. The dock gate's own refusal, surfaced with a name.
    const Game::ShipId criminal =
      universe.SpawnShip(Game::LocalPos(-4000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    universe.RecordAggression(universe.HandleOf(criminal), station);
    dock.station = structure;
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::RefusedStanding),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, dock)),
                     L"an aggressor was let back into the port it attacked");
    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Idle, L"a refused order was written to the row");
  }

  TEST_METHOD(ADockOrderAtFleetGrainDismantlesIt)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    (void)LaunchedFleet(universe, station, 2, 2);
    const Game::ShipId structure = universe.Resolve(universe.StationOf(station).structure);

    Game::Universe::FleetCommand dock;
    dock.kind = Game::FleetOrderKind::Dock;
    dock.station = structure;
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 2, dock)),
                     L"the dock order was refused");

    for (int tick = 0; tick < 3000 && universe.FleetCount() != 0; ++tick)
      universe.Step();

    Assert::AreEqual(0u, universe.FleetCount(), L"a fleet ordered home did not dismantle");
    Assert::AreEqual(2u, static_cast<std::uint32_t>(universe.StationOf(station).docked.size()), L"the ledger did not get its rows back");
  }

  TEST_METHOD(StopHaltsAFleet)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = LaunchedFleet(universe, station, 0, 3);

    Assert::AreEqual(
      Code(Game::Universe::FleetOrderResult::Ordered),
      Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(Game::LocalPos(6000.0f, 6000.0f)))),
      L"the fleet order was refused");
    for (int tick = 0; tick < 200; ++tick)
      universe.Step();
    for (const Game::ShipId member : LiveMembers(universe, fleet))
      Assert::IsTrue(universe.Ship(member).speed > 1.0f, L"the fleet never got under way, so stopping it proves nothing");

    Game::Universe::FleetCommand stop;
    stop.kind = Game::FleetOrderKind::Stop;
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, stop)),
                     L"the stop was refused");

    // A brake, and the row holds no order at all afterwards: stopping is asking for Idle.
    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Idle, L"stopping left a standing order behind");
    for (const Game::ShipId member : LiveMembers(universe, fleet))
    {
      Assert::AreEqual(Game::OrderState::Idle, universe.Ship(member).order, L"a member is still under orders");
      Assert::AreEqual(0.0f, universe.Ship(member).orderSpeedCapMetresPerSec, 0.0f, L"a stopped member kept a cruise cap");
      Assert::IsFalse(universe.DockingOf(member).active, L"a stopped member is still trying to dock");
    }

    // And it stays stopped: nothing in the pass re-issues an order the fleet no longer has.
    for (int tick = 0; tick < 300; ++tick)
      universe.Step();
    for (const Game::ShipId member : LiveMembers(universe, fleet))
      Assert::AreEqual(Game::OrderState::Idle, universe.Ship(member).order, L"a stopped fleet started moving again");
    Assert::AreEqual(1u, universe.FleetCount(), L"stopping retired the fleet");
  }

  TEST_METHOD(TheMineKindIsStillReserved)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = LaunchedFleet(universe, station, 0, 2);

    // The byte is spent -- that is what reserving one is for -- and refused until there is a mining
    // design and something in the universe to mine. Attack stopped being one of these when the defense
    // landed and gave it a chassis to share.
    Game::Universe::FleetCommand mine;
    mine.kind = Game::FleetOrderKind::Mine;
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Unsupported),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, mine)),
                     L"a kind with no design behind it was accepted");
    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Idle, L"a refused kind was written to the row");
  }

  TEST_METHOD(AFleetIsPatient)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = LaunchedFleet(universe, station, 0, 3);

    Assert::AreEqual(
      Code(Game::Universe::FleetOrderResult::Ordered),
      Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(Game::LocalPos(1200.0f, 0.0f)))),
      L"the fleet order was refused");
    for (int tick = 0; tick < 3500; ++tick)
      universe.Step();

    const std::vector<Game::ShipId> members = LiveMembers(universe, fleet);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId member : members)
      handles.push_back(universe.HandleOf(member));
    const Game::UniversePos slot = universe.Ship(members[0]).posUniverse;
    Assert::AreEqual(Game::OrderState::Idle, universe.Ship(members[0]).order, L"the fleet has not settled, so this proves nothing");

    // Something barges through the formation: a Carrier holds its line far harder than a Corvette,
    // so the member is shoved off the slot it had arrived on.
    const Game::ShipId barge = universe.SpawnShip(slot, 0.0f, static_cast<std::uint32_t>(Game::HullId::Carrier));
    const Game::ShipHandle bargeHandle = universe.HandleOf(barge);
    for (int tick = 0; tick < 120; ++tick)
      universe.Step();
    const float shoved = Game::Distance(universe.Ship(universe.Resolve(handles[0])).posUniverse, slot);
    Assert::IsTrue(shoved > 2.0f * Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Corvette)),
                   L"nothing shoved the member, so patience has nothing to be patient about");

    // The barge leaves, and the fleet re-forms: the member is re-issued the leg it already had,
    // rather than the whole formation being solved again.
    Assert::IsTrue(universe.DespawnShip(bargeHandle), L"the barge would not despawn");
    for (int tick = 0; tick < 900; ++tick)
      universe.Step();

    const Game::ShipId returned = universe.Resolve(handles[0]);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, returned, L"the shoved member is gone");
    Assert::IsTrue(Game::Distance(universe.Ship(returned).posUniverse, slot) < shoved,
                   L"a shoved member never came back to the slot its order left it on");
    Assert::IsTrue(Game::Distance(universe.Ship(returned).posUniverse, slot) <
                     4.0f * Game::ArrivalRadiusMetres(Game::HullSpecOf(Game::HullId::Corvette)),
                   L"a shoved member did not re-form");

    // The others were not re-planned out from under it.
    for (std::size_t at = 1; at < handles.size(); ++at)
      Assert::AreEqual(Game::OrderState::Idle, universe.Ship(universe.Resolve(handles[at])).order, L"an untouched member was re-ordered");
  }

  TEST_METHOD(AFleetOrderedIntoAWallSettles)
  {
    // Patience re-issues the leg of a member that stopped short of its order, and a destination the
    // geometry forbids is short of it for ever. What stops that being an A* a tick is that patience
    // reads the member's ROUTE destination, which AdvanceRoute has already moved to where the ship
    // stands (ADR 0042) -- not the fleet's order point, which is still inside the wall. This is the
    // test that says so: patience rewritten to re-derive the point from the fleet would pass every
    // other assertion in this file and fail only here.
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = LaunchedFleet(universe, station, 0, 2);

    // Into the middle of the station itself, which is a tap a player is entitled to make.
    const Game::ShipId structure = universe.Resolve(universe.StationOf(station).structure);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0,
                                                   MoveTo(universe.Ship(structure).posUniverse))),
                     L"the fleet order was refused");

    for (int tick = 0; tick < 1200; ++tick)
      universe.Step();

    // Settled: as close as the geometry allows, and staying there.
    //
    // Counted rather than watched, and that difference is the whole test. The loop it guards against
    // is invisible from outside a tick: the ship would be set Moving at the top of Step and put back
    // to Idle before the tick ended, because the point it was re-aimed at is the one it is already
    // standing on. Sampling the order state finds a settled fleet either way; only the planner's own
    // count says whether an A* ran.
    const std::uint64_t before = universe.RoutePlanCount();
    for (int tick = 0; tick < 300; ++tick)
      universe.Step();
    const std::uint64_t plans = universe.RoutePlanCount() - before;

    Assert::IsTrue(universe.FleetOf(fleet).memberCount > 0, L"the fleet is gone, so this proves nothing");
    for (const Game::ShipId member : LiveMembers(universe, fleet))
      Assert::AreEqual(Game::OrderState::Idle, universe.Ship(member).order, L"a member never settled");
    Assert::AreEqual(static_cast<std::uint64_t>(0), plans, L"a fleet ordered somewhere it cannot reach never stops re-planning");
  }

  TEST_METHOD(ALateLaunchJoinsTheStandingOrder)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    DockShips(universe, station, Game::HullId::Corvette, 4);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 4);
    Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                     Code(universe.ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                     L"the compose was refused");

    // Two out, two still inside, and the order arrives mid-launch -- which the design allows.
    for (int tick = 0; tick < static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 5; ++tick)
      universe.Step();
    Assert::AreEqual(2u, universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0)).memberCount, L"the scene is not mid-launch");

    const Game::UniversePos point = Game::LocalPos(1500.0f, 1000.0f);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(point))),
                     L"the fleet order was refused");

    // Long enough for the two hulls still inside to launch on the metronome and then fly the leg.
    for (int tick = 0; tick < 5500; ++tick)
      universe.Step();

    const Game::Universe::FleetId fleet = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreEqual(4u, universe.FleetOf(fleet).memberCount, L"the launch did not finish under a standing order");
    const float spacing = Game::SlotSpacingMetres(Game::HullSpecOf(Game::HullId::Corvette).BoundingRadiusMetres());
    for (const Game::ShipId member : LiveMembers(universe, fleet))
    {
      Assert::IsTrue(Game::Distance(universe.Ship(member).posUniverse, point) < 4.0f * spacing,
                     L"a hull born after the order flew to the rally instead of joining it");
    }
  }

  TEST_METHOD(TheStandingOrderSurvivesTheRoundTrip)
  {
    Game::Universe original;
    const Game::Universe::StationId station = MakeStationAt(original, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = LaunchedFleet(original, station, 3, 2);

    const Game::UniversePos point = Game::LocalPos(2600.0f, -1400.0f);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(original.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 3, MoveTo(point, true, -0.8f))),
                     L"the order was refused");
    for (int tick = 0; tick < 200; ++tick)
      original.Step();

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(original, saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");

    const Game::Universe::Fleet& before = original.FleetOf(fleet);
    const Game::Universe::Fleet& after = reloaded.FleetOf(reloaded.FleetInSlot(Game::OWNER_LOCAL, 3));
    Assert::IsTrue(before.orderKind == after.orderKind, L"the standing order's kind did not survive");
    Assert::IsTrue(IsSamePosition(before.orderPoint, after.orderPoint), L"the standing order's point did not survive");
    Assert::AreEqual(before.orderFacingRad, after.orderFacingRad, 0.0f, L"the standing order's facing did not survive");
    Assert::IsTrue(before.orderHasFacing == after.orderHasFacing, L"the standing order's facing flag did not survive");

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < 600; ++tick)
    {
      original.Step();
      reloaded.Step();
      Game::WriteUniverseState(original, a);
      Game::WriteUniverseState(reloaded, b);
      Assert::IsTrue(a == b, L"a reloaded universe flew a different order");
    }
  }

  TEST_METHOD(TheSameFleetOrderProducesTheSameRun)
  {
    Game::Universe first;
    Game::Universe second;
    for (Game::Universe* universe : {&first, &second})
    {
      BuildFleetlessScene(*universe);
      const Game::Universe::StationId station = MakeStationAt(*universe, 5000.0f, 0.0f, 1.4f);
      DockShips(*universe, station, Game::HullId::Interceptor, 2);
      DockShips(*universe, station, Game::HullId::Frigate, 1);

      std::vector<std::uint32_t> counts = ZeroCounts();
      Want(counts, Game::HullId::Interceptor, 2);
      Want(counts, Game::HullId::Frigate, 1);
      Assert::AreEqual(Code(Game::Universe::ComposeResult::Composed),
                       Code(universe->ComposeFleet(station, 0, counts, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER})),
                       L"the compose was refused");
      for (int tick = 0; tick < 60; ++tick)
        universe->Step();
      Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                       Code(universe->IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0,
                                                      MoveTo(Game::LocalPos(6800.0f, 1600.0f), true, 0.3f))),
                       L"the fleet order was refused");
    }

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < 900; ++tick)
    {
      first.Step();
      second.Step();
      Game::WriteUniverseState(first, a);
      Game::WriteUniverseState(second, b);
      Assert::IsTrue(a == b, L"two identical fleet orders diverged");
    }
    Assert::AreEqual(3u, first.FleetOf(first.FleetInSlot(Game::OWNER_LOCAL, 0)).memberCount, L"the fleet lost a member");
  }

  // --- the defense (slice 4) ---------------------------------------------------------------------

  TEST_METHOD(TheHullTableSaysWhoFights)
  {
    // Authored, and the list is the design's. A hull that cannot move cannot answer an attack either.
    const bool fights[Game::HULL_COUNT] = {true, true, true, false, true, false, true, true, false, false};
    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    {
      const Game::HullSpec& spec = Game::HullSpecOf(hull);
      Assert::IsTrue(spec.combatant == fights[hull], L"a hull answers an attack when it should not, or the other way round");
      if (spec.immovable)
        Assert::IsFalse(spec.combatant, L"something that cannot move is expected to give chase");
    }
  }

  TEST_METHOD(AHostileActRousesTheDefense)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);
    const Game::ShipId corvette = CombatantOf(universe, fleet);
    const Game::ShipId miner = NonCombatantOf(universe, fleet);

    const Game::UniversePos point = Game::LocalPos(1400.0f, 0.0f);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(point))),
                     L"the fleet order was refused");
    for (int tick = 0; tick < 300; ++tick)
      universe.Step();

    // Somebody shoots the Miner. Something in this tree can now actually do that -- the fire pass
    // calls this itself (ADR 0052) -- but the socket is still what this row drives, because an act
    // stated by hand is the only way to put a fleet in a known posture on a known tick.
    const Game::ShipId raider = universe.SpawnShip(universe.Ship(miner).posUniverse, 0.0f,
                                                   static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    const Game::ShipHandle raiderHandle = universe.HandleOf(raider);
    universe.RecordHostileAct(raiderHandle, universe.HandleOf(miner));

    Assert::AreEqual(Game::FLEET_ALERT_TICKS, universe.FleetOf(fleet).alertTicks, L"the act did not light the alert");
    Assert::IsTrue(universe.FleetOf(fleet).threat == raiderHandle, L"the fleet is not roused against its attacker");

    for (int tick = 0; tick < 200; ++tick)
      universe.Step();

    // The Corvette turns on the raider; the Miner keeps flying the order it was given, and does not
    // flee -- fleeing is a judgment about where safety is, which is a sense this design has none of.
    //
    // "Turns on" is read as gunfire rather than as distance, and that change is this phase's rather
    // than this row's. The raider was spawned on top of the Miner, which is well inside a Corvette's
    // 180 m turrets, so the combatant is already where its guns bear and a pursuit that closed
    // further would be one that had learned nothing (Design/Combat.md 8). What says it turned is
    // that the raider is losing hull points, and nothing but this fleet is shooting at it.
    Assert::IsTrue(universe.Ship(raider).hullPoints < Game::HullSpecOf(Game::HullId::Interceptor).maxHullPoints,
                   L"the combatant did not turn its guns on the attacker");
    Assert::AreEqual(0.0f, universe.Ship(corvette).orderSpeedCapMetresPerSec, 0.0f, L"a chase was held to the fleet's cruising pace");
    Assert::AreEqual(Game::OrderState::Moving, universe.Ship(miner).order, L"the non-combatant abandoned its order");
    Assert::IsTrue(Game::Distance(universe.Ship(miner).posUniverse, point) < Game::Distance(universe.Ship(corvette).posUniverse, point),
                   L"the non-combatant is not the one still going where it was sent");
  }

  TEST_METHOD(AnActOnAShipInNoFleetIsIgnored)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);

    const Game::ShipId loose =
      universe.SpawnShip(Game::LocalPos(-3000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
    const Game::ShipId raider =
      universe.SpawnShip(Game::LocalPos(-3040.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    universe.RecordHostileAct(universe.HandleOf(raider), universe.HandleOf(loose));

    Assert::AreEqual(0u, universe.FleetOf(fleet).alertTicks, L"a loose ship's misfortune roused somebody else's fleet");
    Assert::IsTrue(universe.FleetOf(fleet).threat.generation == 0, L"a fleet took a threat it was never given");

    // And a victim that is not a live ship at all changes nothing either.
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(loose)), L"the despawn failed");
    universe.RecordHostileAct(universe.HandleOf(raider), Game::ShipHandle{});
    Assert::AreEqual(0u, universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0)).alertTicks, L"a null victim roused a fleet");
  }

  TEST_METHOD(TheDefenseHoldsItsGround)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);
    const Game::ShipId corvette = CombatantOf(universe, fleet);
    const Game::ShipId miner = NonCombatantOf(universe, fleet);

    // A raider that keeps striking as it runs, so the ALERT never goes out and the leash is the only
    // thing that can release the combatant. That is the situation the leash is actually for: no hull
    // in the table covers a kilometer in the ten seconds one act buys, so a hit-and-run is released
    // by the alert, and a running fight is what the leash bounds (Fleets.md 7.2's amendment).
    const Game::ShipId raider = universe.SpawnShip(universe.Ship(miner).posUniverse, 0.0f,
                                                   static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    const Game::ShipHandle raiderHandle = universe.HandleOf(raider);
    const Game::ShipHandle minerHandle = universe.HandleOf(miner);
    universe.RecordHostileAct(raiderHandle, minerHandle);
    const Game::UniversePos anchor = universe.FleetOf(fleet).threatAnchorPos;

    Game::UniversePos bolt = anchor;
    Game::Translate(bolt, 0.0f, 4.0f * Game::FLEET_ENGAGE_RANGE_METRES);
    const Game::ShipId raiderShips[] = {raider};
    (void)universe.IssueMoveOrder(raiderShips, bolt, false, 0.0f, Game::FACTION_VANDAL);

    // Followed as the fight goes on, because every act re-states it: the ground the leash is measured
    // from is where the LAST act happened, not where the first one did.
    Game::UniversePos lastAnchor = anchor;
    bool released = false;
    for (int tick = 0; tick < 6000 && !released; ++tick)
    {
      lastAnchor = universe.FleetOf(fleet).threatAnchorPos;
      universe.Step();
      released = universe.FleetOf(fleet).threat.generation == 0;
      if (!released && tick % 120 == 0)
        universe.RecordHostileAct(raiderHandle, minerHandle);
    }

    Assert::IsTrue(released, L"the defense followed a raider off the ground it struck");
    Assert::IsTrue(universe.FleetOf(fleet).alertTicks > 0, L"the alert ran out first, so the leash is not what released it");
    Assert::IsTrue(Game::Distance(universe.Ship(raider).posUniverse, lastAnchor) > Game::FLEET_ENGAGE_RANGE_METRES,
                   L"the raider was released before it broke the leash");
    Assert::IsTrue(IsSamePosition(universe.FleetOf(fleet).threatAnchorPos, Game::UniversePos{}),
                   L"a stood-down fleet is still holding the ground it was defending");

    // With no standing order to go back to, the combatant stops rather than flying on to where its
    // quarry used to be. Stopping is the Idle below; letting go is the gap opening after it, which
    // is what a combatant still shadowing would not do.
    const float gapAtRelease = Game::Distance(universe.Ship(corvette).posUniverse, universe.Ship(raider).posUniverse);
    for (int tick = 0; tick < 900; ++tick)
      universe.Step();
    Assert::AreEqual(Game::OrderState::Idle, universe.Ship(corvette).order, L"a released combatant kept chasing");
    Assert::IsTrue(Game::Distance(universe.Ship(corvette).posUniverse, universe.Ship(raider).posUniverse) > gapAtRelease + 300.0f,
                   L"a released combatant is still keeping station on its quarry");
  }

  TEST_METHOD(ACombatantReturnsToItsOrder)
  {
    // What standing down has to mean when the fleet was in the middle of something. Pursuit
    // overwrote the combatant's route destination with the target's position, so patience -- which
    // re-issues a member to its own route destination -- would send it back to where its quarry used
    // to be. The stand-down re-lowers the standing order instead, and this is the test that says the
    // difference is real (Fleets.md 7.2's amendment).
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);
    const Game::ShipId corvette = CombatantOf(universe, fleet);
    const Game::ShipId miner = NonCombatantOf(universe, fleet);

    const Game::UniversePos point = Game::LocalPos(1600.0f, 0.0f);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(point))),
                     L"the fleet order was refused");
    for (int tick = 0; tick < 200; ++tick)
      universe.Step();

    // Struck once, and the raider then sits still well inside the leash: only the alert running out
    // can end this, which is what puts the stand-down on a schedule the test can wait for.
    Game::UniversePos ambush = universe.Ship(miner).posUniverse;
    Game::Translate(ambush, -300.0f, 0.0f);
    const Game::ShipId raider =
      universe.SpawnShip(ambush, 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    universe.RecordHostileAct(universe.HandleOf(raider), universe.HandleOf(miner));

    for (std::uint32_t tick = 0; tick < Game::FLEET_ALERT_TICKS / 2; ++tick)
      universe.Step();
    Assert::IsTrue(Game::Distance(universe.Ship(corvette).posUniverse, universe.Ship(raider).posUniverse) < 400.0f,
                   L"the combatant never turned on the raider, so this proves nothing");

    for (std::uint32_t tick = 0; tick < Game::FLEET_ALERT_TICKS; ++tick)
      universe.Step();
    Assert::IsTrue(universe.FleetOf(fleet).threat.generation == 0, L"the alert never burned out");
    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Move, L"the defense ate the standing order");

    // And it goes to the fleet's order, not back to the raider it was shadowing.
    for (int tick = 0; tick < 4000; ++tick)
      universe.Step();
    const float spacing = Game::SlotSpacingMetres(Game::HullSpecOf(Game::HullId::Miner).BoundingRadiusMetres());
    Assert::IsTrue(Game::Distance(universe.Ship(corvette).posUniverse, point) < 4.0f * spacing,
                   L"a combatant that stood down did not resume its fleet's order");
    Assert::IsTrue(Game::Distance(universe.Ship(corvette).posUniverse, universe.Ship(raider).posUniverse) > 1000.0f,
                   L"a combatant that stood down went back to where its quarry was");
  }

  TEST_METHOD(TheAlertDecays)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = UnarmedFleet(universe, station, 0);
    const Game::ShipId miner = NonCombatantOf(universe, fleet);

    // A raider that stays put well inside the leash: only the alert can end this engagement, which
    // is what makes it one of the three things being engaged is.
    //
    // That isolation is why this row's fleet is the Miner alone. Once combatants shoot (ADR 0052) a
    // Corvette kills a 60-point Interceptor in about four and a half seconds, and the engagement
    // would then end because the threat DIED -- a fourth thing, and a true one, but not the thing
    // this row exists to measure. A fleet with nothing armed in it cannot end a fight any way but by
    // the clock, which is the reading this row has always wanted.
    const Game::ShipId raider = universe.SpawnShip(universe.Ship(miner).posUniverse, 0.0f,
                                                   static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    const Game::ShipHandle raiderHandle = universe.HandleOf(raider);
    universe.RecordHostileAct(raiderHandle, universe.HandleOf(miner));

    for (std::uint32_t tick = 0; tick < Game::FLEET_ALERT_TICKS - 1; ++tick)
      universe.Step();
    Assert::IsTrue(universe.FleetOf(fleet).alertTicks > 0, L"the alert went out early");
    Assert::IsTrue(universe.FleetOf(fleet).threat.generation != 0, L"the fleet stood down while it was still under attack");

    // A second act refills it, which is what makes a running fight hold the button lit.
    universe.RecordHostileAct(raiderHandle, universe.HandleOf(miner));
    Assert::AreEqual(Game::FLEET_ALERT_TICKS, universe.FleetOf(fleet).alertTicks, L"a fresh act did not refill the alert");

    for (std::uint32_t tick = 0; tick < Game::FLEET_ALERT_TICKS + 1; ++tick)
      universe.Step();
    Assert::AreEqual(0u, universe.FleetOf(fleet).alertTicks, L"the alert never went out");
    Assert::IsTrue(universe.FleetOf(fleet).threat.generation == 0, L"the posture outlived the alert that bounds it");
  }

  TEST_METHOD(AnExplicitOrderStandsTheDefenseDown)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);
    const Game::ShipId miner = NonCombatantOf(universe, fleet);

    const Game::ShipId raider = universe.SpawnShip(universe.Ship(miner).posUniverse, 0.0f,
                                                   static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    const Game::ShipHandle raiderHandle = universe.HandleOf(raider);
    universe.RecordHostileAct(raiderHandle, universe.HandleOf(miner));
    for (int tick = 0; tick < 60; ++tick)
      universe.Step();
    Assert::IsTrue(universe.FleetOf(fleet).threat.generation != 0, L"the fleet was never roused");

    // An explicit order outranks the standing behavior. The alert keeps burning: the button should
    // not stop glowing because the player gave an order.
    const std::uint32_t burning = universe.FleetOf(fleet).alertTicks;
    Assert::AreEqual(
      Code(Game::Universe::FleetOrderResult::Ordered),
      Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(Game::LocalPos(1400.0f, 0.0f)))),
      L"the fleet order was refused");
    Assert::IsTrue(universe.FleetOf(fleet).threat.generation == 0, L"an explicit order did not stand the defense down");
    Assert::AreEqual(burning, universe.FleetOf(fleet).alertTicks, L"an order put the alert out");

    // And the next act rouses it again, with a fresh anchor.
    for (int tick = 0; tick < 200; ++tick)
      universe.Step();
    universe.RecordHostileAct(raiderHandle, universe.HandleOf(NonCombatantOf(universe, fleet)));
    Assert::IsTrue(universe.FleetOf(fleet).threat == raiderHandle, L"a fresh act did not re-rouse the defense");
    Assert::IsFalse(IsSamePosition(universe.FleetOf(fleet).threatAnchorPos, Game::LocalPos(0.0f, 0.0f)),
                    L"the anchor is not where the second act happened");
  }

  TEST_METHOD(AnOrderedAttackAimsTheCombatants)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);
    const Game::ShipId corvette = CombatantOf(universe, fleet);
    const Game::ShipId miner = NonCombatantOf(universe, fleet);

    const Game::ShipId quarry =
      universe.SpawnShip(Game::LocalPos(1800.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    const Game::ShipHandle quarryHandle = universe.HandleOf(quarry);
    const Game::ShipHandle corvetteHandle = universe.HandleOf(corvette);
    const Game::ShipHandle minerHandle = universe.HandleOf(miner);
    const Game::UniversePos minerWas = universe.Ship(miner).posUniverse;

    Assert::AreEqual(
      Code(Game::Universe::FleetOrderResult::NoSuchTarget),
      Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, AttackOf(universe.ShipCount()))),
      L"a stale target was accepted");
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, AttackOf(quarry))),
                     L"the attack order was refused");

    // About 1.8 km at a Corvette's 30 m/s is a minute; the budget is that with room to spare.
    //
    // The closing is measured *while there is something to measure it against*, and the ids are held
    // as handles, because a combatant that arrives now also shoots: a Corvette kills a 60-point
    // Interceptor in about four and a half seconds, the order completes with its target
    // (Design/Archive/Fleets.md 6.5), and a raw ShipId kept across that is an index past the end
    // (ADR 0005, ADR 0052). Before combat this row could hold one for 4,500 ticks and read it back.
    float closest = 1.0e30f;
    for (int tick = 0; tick < 4500; ++tick)
    {
      universe.Step();
      const Game::ShipId live = universe.Resolve(quarryHandle);
      if (live == Game::INVALID_SHIP_ID)
        break;
      closest =
        std::min(closest, Game::Distance(universe.Ship(universe.Resolve(corvetteHandle)).posUniverse, universe.Ship(live).posUniverse));
    }

    // The Corvette closed to where its guns bear. The Miner holds where the order found it.
    Assert::IsTrue(closest < 250.0f, L"the combatant did not close on its target");
    const Game::ShipId minerNow = universe.Resolve(minerHandle);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, minerNow, L"the non-combatant was lost");
    Assert::IsTrue(Game::Distance(universe.Ship(minerNow).posUniverse, minerWas) < 60.0f, L"the non-combatant was dragged into the attack");
    Assert::AreEqual(Game::OrderState::Idle, universe.Ship(minerNow).order, L"the non-combatant did not hold");
  }

  TEST_METHOD(AnOrderedAttackHasNoLeash)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);
    const Game::ShipId corvette = CombatantOf(universe, fleet);

    const Game::ShipId quarry =
      universe.SpawnShip(Game::LocalPos(600.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Miner), Game::FACTION_VANDAL);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, AttackOf(quarry))),
                     L"the attack order was refused");

    // Far past anything a defense would let go of. An ordered pursuit is a decision the player made,
    // where the defense is a reaction the fleet had, so it runs until the target is gone.
    Game::UniversePos escape = Game::LocalPos(600.0f, 0.0f);
    Game::Translate(escape, 0.0f, 6.0f * Game::FLEET_ENGAGE_RANGE_METRES);
    const Game::ShipId quarryShips[] = {quarry};
    (void)universe.IssueMoveOrder(quarryShips, escape, false, 0.0f, Game::FACTION_VANDAL);

    // A Miner runs at 24 m/s, so two kilometers is 83 s. The Corvette is faster, which is what
    // makes "it kept coming" a measurement rather than a tie.
    for (int tick = 0; tick < 6000; ++tick)
      universe.Step();

    Assert::IsTrue(Game::Distance(universe.Ship(quarry).posUniverse, Game::LocalPos(600.0f, 0.0f)) > 2.0f * Game::FLEET_ENGAGE_RANGE_METRES,
                   L"the quarry never ran, so this proves nothing");
    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Attack, L"the attack order lapsed");
    Assert::IsTrue(Game::Distance(universe.Ship(corvette).posUniverse, universe.Ship(quarry).posUniverse) < 400.0f,
                   L"an ordered pursuit gave up at a range a defense would have");
  }

  TEST_METHOD(AnOrderedAttackEndsWithItsTarget)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);
    const Game::ShipId corvette = CombatantOf(universe, fleet);

    const Game::ShipId quarry =
      universe.SpawnShip(Game::LocalPos(1800.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, AttackOf(quarry))),
                     L"the attack order was refused");
    for (int tick = 0; tick < 200; ++tick)
      universe.Step();
    Assert::AreEqual(Game::OrderState::Moving, universe.Ship(corvette).order, L"the combatant never set off");

    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(quarry)), L"the despawn failed");
    universe.Step();

    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Idle, L"the order outlived its target");
    for (const Game::ShipId member : LiveMembers(universe, fleet))
      Assert::AreEqual(Game::OrderState::Idle, universe.Ship(member).order, L"the fleet flew on to where its quarry used to be");
  }

  TEST_METHOD(TheDefenseOutranksAStandingAttack)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(universe, station, 0);
    const Game::ShipId corvette = CombatantOf(universe, fleet);
    const Game::ShipId miner = NonCombatantOf(universe, fleet);

    const Game::ShipId quarry =
      universe.SpawnShip(Game::LocalPos(2400.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    Assert::AreEqual(Code(Game::Universe::FleetOrderResult::Ordered),
                     Code(universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, AttackOf(quarry))),
                     L"the attack order was refused");
    for (int tick = 0; tick < 200; ++tick)
      universe.Step();

    // And then somebody else shoots the Miner. The defense takes the combatants for as long as it
    // lasts; the standing order is what they go back to.
    const Game::ShipId ambusher =
      universe.SpawnShip(universe.Ship(miner).posUniverse, 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber), Game::FACTION_VANDAL);
    const Game::ShipHandle ambusherHandle = universe.HandleOf(ambusher);
    universe.RecordHostileAct(ambusherHandle, universe.HandleOf(miner));
    for (int tick = 0; tick < 200; ++tick)
      universe.Step();

    Assert::IsTrue(Game::Distance(universe.Ship(corvette).posUniverse, universe.Ship(ambusher).posUniverse) <
                     Game::Distance(universe.Ship(corvette).posUniverse, universe.Ship(quarry).posUniverse),
                   L"the combatant kept chasing its order while it was being shot at");
    Assert::IsTrue(universe.FleetOf(fleet).orderKind == Game::FleetOrderKind::Attack, L"the defense ate the standing order");

    // The ambusher dies, the alert burns out, and the fleet goes back to the target it was ordered
    // at rather than standing about.
    Assert::IsTrue(universe.DespawnShip(ambusherHandle), L"the despawn failed");
    for (int tick = 0; tick < 4500; ++tick)
      universe.Step();
    Assert::IsTrue(universe.FleetOf(fleet).threat.generation == 0, L"the fleet is still hunting a ship that is gone");
    Assert::IsTrue(Game::Distance(universe.Ship(corvette).posUniverse, universe.Ship(quarry).posUniverse) < 400.0f,
                   L"the combatant did not go back to the target it was ordered at");
  }

  TEST_METHOD(TwoOwnersInOneFactionAreTwoPlayers)
  {
    // The door slice 3 exists to open, and the only row that would notice if it closed again. Two
    // owners in the SAME faction: allies who shoot at the same people, dock at the same stations and
    // are held hostile by the same enemies, and who nonetheless have their own hulls and their own
    // five slots. This was unrepresentable while the key was the faction -- the two would have
    // shared one ledger and one slot table, or one of them would have had to be a different faction
    // and therefore a different side in every fight (Design/Archive/OwnerKey-work-order.md).
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStationAt(universe, 0.0f, 0.0f);

    constexpr Game::OwnerId ALICE = 11;
    constexpr Game::OwnerId BOB = 12;
    const Game::Issuer alice{ALICE, Game::FACTION_PLAYER};
    const Game::Issuer bob{BOB, Game::FACTION_PLAYER};

    // Each docks their own hulls at the one station, through the one faction they share.
    DockShipsFor(universe, station, Game::HullId::Corvette, 2, alice);
    DockShipsFor(universe, station, Game::HullId::Frigate, 3, bob);

    // Each sees their own rows and none of the other's, out of the same ledger.
    std::vector<std::uint32_t> hers = ZeroCounts();
    std::vector<std::uint32_t> his = ZeroCounts();
    universe.LedgerFor(station, alice, hers);
    universe.LedgerFor(station, bob, his);
    Assert::AreEqual(2u, hers[static_cast<std::size_t>(Game::HullId::Corvette)], L"an owner cannot see their own hulls");
    Assert::AreEqual(0u, hers[static_cast<std::size_t>(Game::HullId::Frigate)], L"an owner can see another owner's hulls");
    Assert::AreEqual(3u, his[static_cast<std::size_t>(Game::HullId::Frigate)], L"the second owner cannot see their own hulls");
    Assert::AreEqual(0u, his[static_cast<std::size_t>(Game::HullId::Corvette)], L"the second owner can see the first's hulls");

    // Neither can compose out of the other's rows, though they are one faction at one station.
    std::vector<std::uint32_t> wantFrigate = ZeroCounts();
    Want(wantFrigate, Game::HullId::Frigate, 1);
    Assert::IsTrue(universe.ComposeFleet(station, 0, wantFrigate, alice) == Game::Universe::ComposeResult::NotDocked,
                   L"one owner composed a fleet out of another owner's ledger");

    // And both hold slot 0 at once, which is the whole of "five fleets each".
    std::vector<std::uint32_t> wantCorvette = ZeroCounts();
    Want(wantCorvette, Game::HullId::Corvette, 1);
    Assert::IsTrue(universe.ComposeFleet(station, 0, wantCorvette, alice) == Game::Universe::ComposeResult::Composed,
                   L"the first owner could not take their own slot 0");
    Assert::IsTrue(universe.ComposeFleet(station, 0, wantFrigate, bob) == Game::Universe::ComposeResult::Composed,
                   L"the second owner was refused a slot the first one holds");

    const Game::Universe::FleetId hersFleet = universe.FleetInSlot(ALICE, 0);
    const Game::Universe::FleetId hisFleet = universe.FleetInSlot(BOB, 0);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, hersFleet, L"the first owner holds no slot 0");
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, hisFleet, L"the second owner holds no slot 0");
    Assert::AreNotEqual(hersFleet, hisFleet, L"two owners' slot 0 resolved to one fleet");

    // Neither can order the other's, which is the authority gate reading the owner and not the
    // faction -- the two are identical here, and that is the point of the row.
    Assert::IsTrue(universe.IssueFleetOrder(bob, 0, MoveTo(Game::LocalPos(500.0f, 0.0f))) == Game::Universe::FleetOrderResult::Ordered,
                   L"an owner could not order their own fleet");
    Assert::IsTrue(universe.FleetOf(hersFleet).orderKind == Game::FleetOrderKind::Idle,
                   L"one owner's order reached another owner's fleet in the same faction");
  }

  TEST_METHOD(TheThreatSurvivesTheRoundTrip)
  {
    Game::Universe original;
    const Game::Universe::StationId station = MakeStationAt(original, 0.0f, 0.0f);
    const Game::Universe::FleetId fleet = MixedFleet(original, station, 1);
    const Game::ShipId miner = NonCombatantOf(original, fleet);

    const Game::ShipId raider = original.SpawnShip(original.Ship(miner).posUniverse, 0.0f,
                                                   static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    original.RecordHostileAct(original.HandleOf(raider), original.HandleOf(miner));
    for (int tick = 0; tick < 90; ++tick)
      original.Step();

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(original, saved);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"the state did not load");

    const Game::Universe::Fleet& before = original.FleetOf(fleet);
    const Game::Universe::Fleet& after = reloaded.FleetOf(reloaded.FleetInSlot(Game::OWNER_LOCAL, 1));
    Assert::IsTrue(before.threat == after.threat, L"the threat did not survive the reload");
    Assert::IsTrue(IsSamePosition(before.threatAnchorPos, after.threatAnchorPos), L"the anchor did not survive the reload");
    Assert::AreEqual(before.alertTicks, after.alertTicks, L"the alert did not survive the reload");
    Assert::IsTrue(before.alertTicks > 0, L"the scene is not mid-defense, so this proves nothing");

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < 900; ++tick)
    {
      original.Step();
      reloaded.Step();
      Game::WriteUniverseState(original, a);
      Game::WriteUniverseState(reloaded, b);
      Assert::IsTrue(a == b, L"a reloaded universe fought a different fight");
    }
  }

  TEST_METHOD(TheSameDefenseProducesTheSameRun)
  {
    Game::Universe first;
    Game::Universe second;
    for (Game::Universe* universe : {&first, &second})
    {
      BuildFleetlessScene(*universe);
      const Game::Universe::StationId station = MakeStationAt(*universe, 5000.0f, 0.0f, 1.4f);
      const Game::Universe::FleetId fleet = MixedFleet(*universe, station, 0);
      Assert::AreEqual(
        Code(Game::Universe::FleetOrderResult::Ordered),
        Code(universe->IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, MoveTo(Game::LocalPos(6800.0f, 900.0f)))),
        L"the fleet order was refused");
      for (int tick = 0; tick < 120; ++tick)
        universe->Step();

      const Game::ShipId victim = NonCombatantOf(*universe, fleet);
      const Game::ShipId raider = universe->SpawnShip(universe->Ship(victim).posUniverse, 0.4f,
                                                      static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
      universe->RecordHostileAct(universe->HandleOf(raider), universe->HandleOf(victim));
    }

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < static_cast<int>(Game::FLEET_ALERT_TICKS) + 600; ++tick)
    {
      first.Step();
      second.Step();
      Game::WriteUniverseState(first, a);
      Game::WriteUniverseState(second, b);
      Assert::IsTrue(a == b, L"two identical defenses diverged");
    }
    Assert::IsTrue(first.FleetOf(first.FleetInSlot(Game::OWNER_LOCAL, 0)).threat.generation == 0,
                   L"the alert never burned out, so the stand-down was never replayed");
  }
};
} // namespace GameLogicTests
