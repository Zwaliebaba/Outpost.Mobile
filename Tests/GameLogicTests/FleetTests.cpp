#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// A faction id is one byte, and a byte is a character to anything that prints one, so a failure
// would report an unprintable glyph rather than "1". Widened for the assertion, never for the wire
// -- StationTests' helper, for StationTests' reason.
[[nodiscard]] std::uint32_t Faction(Game::FactionId _faction)
{
  return _faction;
}

// _count ships of one faction, spaced far enough apart that nothing in this file is measuring
// avoidance by accident.
std::vector<Game::ShipId> SpawnLine(Game::World& _world, int _count, Game::FactionId _faction = Game::FACTION_PLAYER, float _z = 0.0f)
{
  std::vector<Game::ShipId> ships;
  for (int at = 0; at < _count; ++at)
  {
    ships.push_back(_world.SpawnShip(Game::LocalPos(static_cast<float>(at) * 300.0f - 1200.0f, _z), 0.0f,
                                     static_cast<std::uint32_t>(Game::HullId::Corvette), _faction));
  }
  return ships;
}

// The ships a fleet names right now, resolved back to ids. A stale handle inside the count resolves
// to INVALID_SHIP_ID rather than being skipped, so a row that kept one shows up as a wrong list and
// not as a short one.
std::vector<Game::ShipId> MembersOf(const Game::World& _world, Game::World::FleetId _fleet)
{
  const Game::World::Fleet& row = _world.FleetOf(_fleet);
  std::vector<Game::ShipId> ids;
  for (std::uint32_t at = 0; at < row.memberCount; ++at)
    ids.push_back(_world.Resolve(row.members[at]));
  return ids;
}

// Something in most of the tables the tick already walks, and no fleet in any of them: architecture,
// a station with a garrison, a move order under way, a ship on patrol, and a docking in flight.
void BuildFleetlessScene(Game::World& _world)
{
  const Game::ShipId post =
    _world.SpawnShip(Game::LocalPos(-260.0f, 700.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);

  Game::World::StationDesc desc;
  desc.ownerFaction = Game::FACTION_VANGUARD;
  desc.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
  desc.protectorComplement = 2;
  (void)_world.MakeStation(post, desc);

  const std::vector<Game::ShipId> ordered = SpawnLine(_world, 3, Game::FACTION_PLAYER, -400.0f);
  (void)_world.IssueMoveOrder(ordered, Game::LocalPos(300.0f, 900.0f), true, 0.4f);

  const Game::ShipId guard =
    _world.SpawnShip(Game::PatrolRingPoint(Game::LocalPos(-260.0f, 700.0f), 0, 420.0f), Game::PatrolRingHeadingRad(0),
                     static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANGUARD);
  _world.AssignPatrol(guard, post, 420.0f, 11.0f);

  const Game::ShipId visitor =
    _world.SpawnShip(Game::LocalPos(-260.0f, 1120.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER);
  const Game::ShipId dockOrder[] = {visitor};
  (void)_world.IssueDockOrder(dockOrder, post, Game::FACTION_PLAYER);
}

// A station at a place, facing a way. The heading is which way is out: the launch fan is centred on
// it, so a test that cares where ships appear says so here.
Game::World::StationId MakeStationAt(Game::World& _world, float _x, float _z, float _headingRad = 0.0f,
                                     Game::FactionId _owner = Game::FACTION_VANGUARD)
{
  const Game::ShipId structure =
    _world.SpawnShip(Game::LocalPos(_x, _z), _headingRad, static_cast<std::uint32_t>(Game::HullId::Structure), _owner);
  Game::World::StationDesc desc;
  desc.ownerFaction = _owner;
  return _world.MakeStation(structure, desc);
}

// Ships parked at a station's door and told to dock, so that a ledger has something in it. They
// spawn inside capture range, so the one tick below puts them inside.
void DockShips(Game::World& _world, Game::World::StationId _station, Game::HullId _hull, int _count,
               Game::FactionId _faction = Game::FACTION_PLAYER)
{
  const Game::ShipId structure = _world.Resolve(_world.StationOf(_station).structure);
  const Game::WorldPos stationPos = _world.Ship(structure).posWorld;
  const float range = Game::DockApproachRangeMetres(Game::HullSpecOf(_world.Ship(structure).hullId), Game::HullSpecOf(_hull));

  std::vector<Game::ShipId> ships;
  for (int at = 0; at < _count; ++at)
  {
    const float bearingRad = static_cast<float>(at) * 0.6f;
    Game::WorldPos pos = stationPos;
    Game::Translate(pos, std::sin(bearingRad) * range, std::cos(bearingRad) * range);
    ships.push_back(_world.SpawnShip(pos, bearingRad, static_cast<std::uint32_t>(_hull), _faction));
  }
  (void)_world.IssueDockOrder(ships, structure, _faction);
  _world.Step();
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
[[nodiscard]] int Code(Game::World::ComposeResult _result)
{
  return static_cast<int>(_result);
}

[[nodiscard]] int Code(Game::World::ComposeResult _result, int)
{
  return static_cast<int>(_result);
}

// The overlap between two ships as the simulation itself computes it, so that a launch cannot be
// declared clear by measuring something the narrow phase does not (SeparationTests' helper).
[[nodiscard]] float OverlapBetween(const Game::World& _world, Game::ShipId _a, Game::ShipId _b)
{
  const Game::ShipState& first = _world.Ship(_a);
  const Game::ShipState& second = _world.Ship(_b);
  const Game::HullSpec& firstHull = Game::HullSpecOf(first.hullId);
  const Game::HullSpec& secondHull = Game::HullSpecOf(second.hullId);

  const Game::Capsule self{
    0.0f, 0.0f, std::sin(first.headingRad), std::cos(first.headingRad), firstHull.capsuleHalfLengthMetres, firstHull.capsuleRadiusMetres};
  const Game::Capsule against{Game::OffsetX(first.posWorld, second.posWorld),
                              Game::OffsetZ(first.posWorld, second.posWorld),
                              std::sin(second.headingRad),
                              std::cos(second.headingRad),
                              secondHull.capsuleHalfLengthMetres,
                              secondHull.capsuleRadiusMetres};
  const Game::Contact contact = Game::CapsuleContact(self, against, _a, _b);
  return contact.touching ? contact.overlapMetres : 0.0f;
}

// A fleet's live members as ship ids, for a test that wants to look at the hulls.
std::vector<Game::ShipId> LiveMembers(const Game::World& _world, Game::World::FleetId _fleet)
{
  std::vector<Game::ShipId> ids;
  const Game::World::Fleet& row = _world.FleetOf(_fleet);
  for (std::uint32_t at = 0; at < row.memberCount; ++at)
  {
    const Game::ShipId id = _world.Resolve(row.members[at]);
    if (id != Game::INVALID_SHIP_ID)
      ids.push_back(id);
  }
  return ids;
}
} // namespace

TEST_CLASS(FleetTests)
{
public:
  TEST_METHOD(AFleetIsFormedFromLiveShips)
  {
    Game::World world;
    const std::vector<Game::ShipId> ships = SpawnLine(world, 3);

    const Game::World::FleetId fleet = world.FormFleet(Game::FACTION_PLAYER, 0, ships);
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, fleet, L"a fleet of three live ships was refused");
    Assert::AreEqual(1u, world.FleetCount(), L"the table did not gain a row");

    const Game::World::Fleet& row = world.FleetOf(fleet);
    Assert::AreEqual(3u, row.memberCount, L"the fleet does not hold what it was formed from");
    Assert::AreEqual(Faction(Game::FACTION_PLAYER), Faction(row.ownerFaction), L"the fleet has the wrong owner");
    Assert::AreEqual(0u, static_cast<std::uint32_t>(row.slot), L"the fleet is in the wrong slot");

    // In the order handed in. Nothing reads that order yet; slice 3's formation solve will, which is
    // why it is pinned here rather than discovered there.
    Assert::IsTrue(MembersOf(world, fleet) == ships, L"the members are not the ships that were handed in");

    Assert::AreEqual(fleet, world.FleetInSlot(Game::FACTION_PLAYER, 0), L"the slot does not name the fleet");
    for (const Game::ShipId ship : ships)
      Assert::AreEqual(fleet, world.FleetAt(ship), L"a member does not find its fleet");

    // An id past the end reads back an empty fleet rather than past the end of the table.
    Assert::AreEqual(0u, world.FleetOf(Game::World::INVALID_FLEET_ID).memberCount, L"a stale id read something");
  }

  TEST_METHOD(TheSixthFleetIsRefused)
  {
    Game::World world;
    const std::vector<Game::ShipId> ships = SpawnLine(world, 7);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(world.HandleOf(ship));

    for (std::uint32_t slot = 0; slot < Game::FLEET_SLOTS; ++slot)
    {
      const Game::ShipId one[] = {ships[slot]};
      Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, static_cast<std::uint8_t>(slot), one),
                          L"one of the five slots refused a fleet");
    }
    Assert::AreEqual(Game::FLEET_SLOTS, world.FleetCount(), L"five fleets did not make five rows");

    // A sixth fleet has nowhere to be: there is no slot past the fifth, and every one of them is
    // held. Both refusals leave the table exactly as it was.
    const Game::ShipId spare[] = {ships[5]};
    Assert::AreEqual(Game::World::INVALID_FLEET_ID,
                     world.FormFleet(Game::FACTION_PLAYER, static_cast<std::uint8_t>(Game::FLEET_SLOTS), spare),
                     L"a slot past the fifth was accepted");
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 2, spare), L"an occupied slot was accepted");
    Assert::AreEqual(Game::FLEET_SLOTS, world.FleetCount(), L"a refused form changed the table");

    // Retiring one frees its slot, which is the whole of what "five active fleets" means: a slot is
    // held from the form until the last ship has left space, and not a tick longer.
    Assert::IsTrue(world.DespawnShip(handles[2]), L"the despawn failed");
    world.Step();
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FleetInSlot(Game::FACTION_PLAYER, 2), L"the retired slot is still held");

    // Through the handle, because the despawn above moved somebody's id (ADR 0005).
    const Game::ShipId refill[] = {world.Resolve(handles[6])};
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 2, refill), L"the freed slot refused a fleet");
  }

  TEST_METHOD(TheNinthShipIsRefused)
  {
    Game::World world;
    const std::vector<Game::ShipId> ships = SpawnLine(world, 9);

    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 0, ships), L"nine ships made a fleet");
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 0, {}), L"an empty fleet was formed");
    Assert::AreEqual(0u, world.FleetCount(), L"a refused form made a row");

    const std::span<const Game::ShipId> eight(ships.data(), Game::MAX_FLEET_SHIPS);
    const Game::World::FleetId full = world.FormFleet(Game::FACTION_PLAYER, 0, eight);
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, full, L"eight ships were refused");
    Assert::AreEqual(Game::MAX_FLEET_SHIPS, world.FleetOf(full).memberCount, L"the full fleet came out short");

    // The ninth ship was refused as a member, so it is still nobody's.
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FleetAt(ships[8]), L"the refused ship was claimed anyway");
  }

  TEST_METHOD(AFleetTakesOnlyItsOwnersShips)
  {
    Game::World world;
    std::vector<Game::ShipId> ships = SpawnLine(world, 2);
    const Game::ShipId stranger =
      world.SpawnShip(Game::LocalPos(600.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_VANDAL);
    ships.push_back(stranger);

    // Refused whole rather than filtered. IssueMoveOrder drops a stranger from the order because an
    // order of two ships and an order of three are the same kind of thing; a fleet of two where
    // three were asked for is a fleet nobody asked for, and the size is one of the rules.
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 0, ships),
                     L"a foreign ship was taken into a fleet");
    Assert::AreEqual(0u, world.FleetCount(), L"a refused form made a row");
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FleetAt(ships[0]), L"a refused form claimed a ship anyway");

    // The table is faction-generic: the Vandals may hold slots, and one faction's slot 0 is not
    // another's.
    const Game::ShipId theirs[] = {stranger};
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_VANDAL, 0, theirs), L"a Vandal fleet was refused");
    const Game::ShipId ours[] = {ships[0]};
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 0, ours),
                        L"two factions cannot both hold slot 0");
    Assert::AreEqual(2u, world.FleetCount(), L"the two fleets did not make two rows");
  }

  TEST_METHOD(AShipBelongsToOneFleet)
  {
    Game::World world;
    const std::vector<Game::ShipId> ships = SpawnLine(world, 3);

    const Game::ShipId first[] = {ships[0], ships[1]};
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 0, first), L"the first fleet was refused");

    // The fleet-only model, held at the only place that makes a membership.
    const Game::ShipId poached[] = {ships[1], ships[2]};
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 1, poached), L"a ship joined a second fleet");

    // The same defect arriving from inside one call: it would spend two of the eight places on one
    // ship, and the fleet would be a member short of the size it was asked for.
    const Game::ShipId twice[] = {ships[2], ships[2]};
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 1, twice), L"one ship filled two places");
    Assert::AreEqual(1u, world.FleetCount(), L"a refused form made a row");
  }

  TEST_METHOD(ALossPrunesTheFleet)
  {
    Game::World world;
    const std::vector<Game::ShipId> ships = SpawnLine(world, 4);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(world.HandleOf(ship));

    const Game::World::FleetId fleet = world.FormFleet(Game::FACTION_PLAYER, 3, ships);
    Assert::AreEqual(4u, world.FleetOf(fleet).memberCount, L"the fleet did not form with four");

    Assert::IsTrue(world.DespawnShip(handles[1]), L"the despawn failed");
    // Not before the tick: the pass is where a fleet learns, and it learns on the tick the ship left
    // rather than the tick after, which is why it runs last in the standing-intent slot.
    world.Step();

    const Game::World::FleetId after = world.FleetInSlot(Game::FACTION_PLAYER, 3);
    Assert::AreEqual(3u, world.FleetOf(after).memberCount, L"the dead member is still counted");

    const std::vector<Game::ShipId> survivors = {world.Resolve(handles[0]), world.Resolve(handles[2]), world.Resolve(handles[3])};
    Assert::IsTrue(MembersOf(world, after) == survivors, L"pruning lost or reordered the survivors");

    // The vacated entry is a null handle rather than a stale one nobody looks at, which is what
    // makes a saved row equal the row it was saved from.
    Assert::AreEqual(0u, world.FleetOf(after).members[3].generation, L"the vacated entry kept a stale handle");
  }

  TEST_METHOD(MembersSurviveSwapAndPop)
  {
    Game::World world;
    // The bystander is spawned first, so despawning it moves the LAST ship into its index -- and the
    // last ship is a member. That is exactly the retarget ADR 0005 exists to prevent, at fleet
    // grain: a row holding raw ids would come out of this naming a stranger.
    const Game::ShipId bystander =
      world.SpawnShip(Game::LocalPos(900.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
    const std::vector<Game::ShipId> ships = SpawnLine(world, 3);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(world.HandleOf(ship));

    const Game::World::FleetId fleet = world.FormFleet(Game::FACTION_PLAYER, 0, ships);
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, fleet, L"the fleet was refused");

    Assert::IsTrue(world.DespawnShip(world.HandleOf(bystander)), L"the despawn failed");
    world.Step();

    const Game::World::FleetId after = world.FleetInSlot(Game::FACTION_PLAYER, 0);
    Assert::AreEqual(3u, world.FleetOf(after).memberCount, L"a bystander's death cost the fleet a member");

    const std::vector<Game::ShipId> moved = {world.Resolve(handles[0]), world.Resolve(handles[1]), world.Resolve(handles[2])};
    Assert::AreNotEqual(ships[2], moved[2], L"nothing moved, so this test proved nothing");
    Assert::IsTrue(MembersOf(world, after) == moved, L"the fleet names different ships than it was formed with");
    for (const Game::ShipId ship : moved)
      Assert::AreEqual(after, world.FleetAt(ship), L"a moved member no longer finds its fleet");
  }

  TEST_METHOD(TheLastLossRetiresTheFleet)
  {
    Game::World world;
    const std::vector<Game::ShipId> ships = SpawnLine(world, 3);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(world.HandleOf(ship));

    const Game::ShipId doomed[] = {ships[0]};
    const Game::ShipId keeper[] = {ships[1], ships[2]};
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 0, doomed), L"fleet 1 was refused");
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world.FormFleet(Game::FACTION_PLAYER, 4, keeper), L"fleet 5 was refused");
    Assert::AreEqual(2u, world.FleetCount(), L"two fleets did not make two rows");

    Assert::IsTrue(world.DespawnShip(handles[0]), L"the despawn failed");
    world.Step();

    Assert::AreEqual(1u, world.FleetCount(), L"the emptied fleet was not retired");
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FleetInSlot(Game::FACTION_PLAYER, 0), L"the retired slot is still held");

    // The survivor keeps its identity across a retirement that moved its row, which is why an owner
    // and a slot -- and never an index -- is what anything holds across a tick.
    const Game::World::FleetId survivor = world.FleetInSlot(Game::FACTION_PLAYER, 4);
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, survivor, L"the surviving fleet lost its slot");
    Assert::AreEqual(2u, world.FleetOf(survivor).memberCount, L"the surviving fleet lost a member");
    Assert::IsTrue(MembersOf(world, survivor) == std::vector<Game::ShipId>({world.Resolve(handles[1]), world.Resolve(handles[2])}),
                   L"the surviving fleet names different ships");
  }

  TEST_METHOD(TheFleetTableSurvivesTheRoundTrip)
  {
    Game::World original;
    const std::vector<Game::ShipId> ships = SpawnLine(original, 6);
    std::vector<Game::ShipHandle> handles;
    for (const Game::ShipId ship : ships)
      handles.push_back(original.HandleOf(ship));

    const Game::ShipId first[] = {ships[0], ships[1], ships[2]};
    const Game::ShipId second[] = {ships[3], ships[4], ships[5]};
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, original.FormFleet(Game::FACTION_PLAYER, 1, first), L"fleet 2 was refused");
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, original.FormFleet(Game::FACTION_PLAYER, 3, second), L"fleet 4 was refused");

    // One of them pruned before the save, so the file carries a row whose count is not the count it
    // was formed with and whose tail has been cleared.
    Assert::IsTrue(original.DespawnShip(handles[4]), L"the despawn failed");
    original.Step();

    std::vector<std::uint8_t> saved;
    Game::WriteWorldState(original, saved);

    Game::World reloaded;
    Assert::IsTrue(Game::ReadWorldState(saved, reloaded), L"the state did not load");
    Assert::AreEqual(original.FleetCount(), reloaded.FleetCount(), L"the fleet count did not survive");

    for (std::uint32_t slot = 0; slot < Game::FLEET_SLOTS; ++slot)
    {
      const Game::World::FleetId here = original.FleetInSlot(Game::FACTION_PLAYER, static_cast<std::uint8_t>(slot));
      const Game::World::FleetId there = reloaded.FleetInSlot(Game::FACTION_PLAYER, static_cast<std::uint8_t>(slot));
      Assert::IsTrue((here == Game::World::INVALID_FLEET_ID) == (there == Game::World::INVALID_FLEET_ID),
                     L"a slot changed occupancy across the reload");
      if (here == Game::World::INVALID_FLEET_ID)
        continue;
      Assert::IsTrue(MembersOf(original, here) == MembersOf(reloaded, there), L"a fleet's members did not survive the reload");
      Assert::AreEqual(Faction(original.FleetOf(here).ownerFaction), Faction(reloaded.FleetOf(there).ownerFaction),
                       L"an owner did not survive the reload");
    }

    const Game::World::FleetId pruned = reloaded.FleetInSlot(Game::FACTION_PLAYER, 3);
    Assert::AreEqual(2u, reloaded.FleetOf(pruned).memberCount, L"the pruned fleet came back the wrong size");
    Assert::AreEqual(0u, reloaded.FleetOf(pruned).members[2].generation, L"a vacated entry came back holding something");

    // Truncated refuses and changes nothing, which is what failing closed means for anything that
    // parses content (AGENTS.md 5).
    Game::World untouched;
    (void)SpawnLine(untouched, 1);
    saved.resize(saved.size() - 1);
    Assert::IsFalse(Game::ReadWorldState(saved, untouched), L"a truncated state loaded");
    Assert::AreEqual(1u, untouched.ShipCount(), L"a refused load changed the world");
    Assert::AreEqual(0u, untouched.FleetCount(), L"a refused load changed the world");
  }

  TEST_METHOD(AFleetlessWorldTicksAsToday)
  {
    // What "as today" means in full is the rest of GameLogicTests passing unchanged: every scene in
    // the suite has an empty fleet table and a pass that visits nothing. This is the half a test can
    // hold -- that the new pass neither grows a fleet in a world nobody formed one in, nor changes
    // what such a world does.
    Game::World first;
    Game::World second;
    BuildFleetlessScene(first);
    BuildFleetlessScene(second);

    for (int tick = 0; tick < 240; ++tick)
    {
      first.Step();
      second.Step();
      Assert::AreEqual(0u, first.FleetCount(), L"a world nobody formed a fleet in grew one");
    }

    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
    Game::WriteWorldState(first, before);
    Game::WriteWorldState(second, after);
    Assert::IsTrue(before == after, L"two identical fleetless worlds diverged");
  }

  TEST_METHOD(TheSameFleetProducesTheSameRun)
  {
    // The replay gate over the row: the same forms and the same losses in two worlds, compared as
    // whole states every tick rather than once at the end, so a divergence says when.
    Game::World first;
    Game::World second;

    for (Game::World* world : {&first, &second})
    {
      BuildFleetlessScene(*world);
      const std::vector<Game::ShipId> wing = SpawnLine(*world, 5, Game::FACTION_PLAYER, 1500.0f);
      const Game::ShipId three[] = {wing[0], wing[1], wing[2]};
      const Game::ShipId two[] = {wing[3], wing[4]};
      Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world->FormFleet(Game::FACTION_PLAYER, 0, three), L"fleet 1 was refused");
      Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, world->FormFleet(Game::FACTION_PLAYER, 4, two), L"fleet 5 was refused");
      (void)world->IssueMoveOrder(three, Game::LocalPos(1200.0f, 2400.0f), false, 0.0f);
    }

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < 120; ++tick)
    {
      first.Step();
      second.Step();
      Game::WriteWorldState(first, a);
      Game::WriteWorldState(second, b);
      Assert::IsTrue(a == b, L"two identical runs diverged before the losses");
    }

    // A loss in each, at the same point in the run, so the prune and the retire are both replayed.
    for (Game::World* world : {&first, &second})
    {
      const Game::World::Fleet& row = world->FleetOf(world->FleetInSlot(Game::FACTION_PLAYER, 4));
      const Game::ShipHandle one = row.members[0];
      const Game::ShipHandle other = row.members[1];
      Assert::IsTrue(world->DespawnShip(one), L"the despawn failed");
      Assert::IsTrue(world->DespawnShip(other), L"the despawn failed");
    }

    for (int tick = 0; tick < 120; ++tick)
    {
      first.Step();
      second.Step();
      Game::WriteWorldState(first, a);
      Game::WriteWorldState(second, b);
      Assert::IsTrue(a == b, L"two identical runs diverged after the losses");
    }

    Assert::AreEqual(1u, first.FleetCount(), L"the emptied fleet was not retired during the run");
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, first.FleetInSlot(Game::FACTION_PLAYER, 4), L"the emptied slot is still held");
  }

  // --- compose (slice 2) -------------------------------------------------------------------------

  TEST_METHOD(AFleetIsComposedFromTheLedger)
  {
    Game::World world;
    const Game::World::StationId station = MakeStationAt(world, 0.0f, 0.0f);
    DockShips(world, station, Game::HullId::Corvette, 3);
    DockShips(world, station, Game::HullId::Frigate, 2);
    DockShips(world, station, Game::HullId::Miner, 1);
    Assert::AreEqual(6u, static_cast<std::uint32_t>(world.StationOf(station).docked.size()), L"the ledger did not fill");

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Frigate, 2);
    Want(counts, Game::HullId::Corvette, 1);
    Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(world.ComposeFleet(station, 2, counts, Game::FACTION_PLAYER)),
                     L"a compose the ledger covers was refused");

    const Game::World::FleetId fleet = world.FleetInSlot(Game::FACTION_PLAYER, 2);
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, fleet, L"the slot is not held from the tick it was composed on");

    const Game::World::Fleet& row = world.FleetOf(fleet);
    Assert::AreEqual(0u, row.memberCount, L"a composed fleet starts with ships in space");
    Assert::AreEqual(3u, row.manifestCount, L"the manifest is not what was asked for");

    // Ascending hull id: Corvette (2) then the two Frigates (4). It is the launch order, and the
    // launch order is the order members join in.
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), row.manifest[0], L"the manifest is not in ascending hull id");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Frigate), row.manifest[1], L"the manifest is not in ascending hull id");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Frigate), row.manifest[2], L"the manifest is not in ascending hull id");

    // The rows left the ledger, and only those rows.
    const std::vector<Game::World::DockedShip>& ledger = world.StationOf(station).docked;
    Assert::AreEqual(3u, static_cast<std::uint32_t>(ledger.size()), L"the drawn rows are still in the ledger");
    std::uint32_t corvettes = 0, miners = 0;
    for (const Game::World::DockedShip& docked : ledger)
    {
      corvettes += (docked.hullId == static_cast<std::uint32_t>(Game::HullId::Corvette)) ? 1u : 0u;
      miners += (docked.hullId == static_cast<std::uint32_t>(Game::HullId::Miner)) ? 1u : 0u;
    }
    Assert::AreEqual(2u, corvettes, L"the wrong number of Corvettes was drawn");
    Assert::AreEqual(1u, miners, L"a hull nobody asked for was drawn");
  }

  TEST_METHOD(ComposeRefusesWhatIsNotThere)
  {
    Game::World world;
    const Game::World::StationId station = MakeStationAt(world, 0.0f, 0.0f);
    DockShips(world, station, Game::HullId::Corvette, 3);
    // A neutral third party's ships in the same ledger. Whose is docked where is the station's
    // business, so they are not the player's to compose with.
    const Game::FactionId other = static_cast<Game::FactionId>(3);
    DockShips(world, station, Game::HullId::Frigate, 2, other);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 4);
    Assert::AreEqual(Code(Game::World::ComposeResult::NotDocked), Code(world.ComposeFleet(station, 0, counts, Game::FACTION_PLAYER)),
                     L"a fleet was composed out of ships that are not there");

    counts = ZeroCounts();
    Want(counts, Game::HullId::Frigate, 1);
    Assert::AreEqual(Code(Game::World::ComposeResult::NotDocked), Code(world.ComposeFleet(station, 0, counts, Game::FACTION_PLAYER)),
                     L"a fleet was composed out of somebody else's ships");

    // A count against a hull id the table does not have. No ledger can hold one, so it is NotDocked
    // rather than a silent truncation of what was asked for.
    std::vector<std::uint32_t> overlong(Game::HULL_COUNT + 2, 0u);
    overlong[Game::HULL_COUNT + 1] = 1;
    Assert::AreEqual(Code(Game::World::ComposeResult::NotDocked), Code(world.ComposeFleet(station, 0, overlong, Game::FACTION_PLAYER)),
                     L"a hull past the table was composed");

    Assert::AreEqual(0u, world.FleetCount(), L"a refused compose made a fleet");
    Assert::AreEqual(5u, static_cast<std::uint32_t>(world.StationOf(station).docked.size()), L"a refused compose touched the ledger");
  }

  TEST_METHOD(AHostilePortRefusesComposition)
  {
    Game::World world;
    const Game::World::StationId station = MakeStationAt(world, 0.0f, 0.0f);
    DockShips(world, station, Game::HullId::Corvette, 3);

    // Turn the law: the same ships, the same ledger, and now an owner that holds the player hostile.
    const Game::ShipId criminal = world.SpawnShip(Game::LocalPos(2000.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    world.RecordAggression(world.HandleOf(criminal), station);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 2);
    Assert::AreEqual(Code(Game::World::ComposeResult::RefusedStanding), Code(world.ComposeFleet(station, 0, counts, Game::FACTION_PLAYER)),
                     L"an aggressor assembled a battle group in the port it attacked");
    Assert::AreEqual(0u, world.FleetCount(), L"a refused compose made a fleet");
    Assert::AreEqual(3u, static_cast<std::uint32_t>(world.StationOf(station).docked.size()), L"a refused compose touched the ledger");
  }

  TEST_METHOD(ComposeRefusesABadSlotOrAnOverfullFleet)
  {
    Game::World world;
    const Game::World::StationId station = MakeStationAt(world, 0.0f, 0.0f);
    DockShips(world, station, Game::HullId::Corvette, 9);

    std::vector<std::uint32_t> nine = ZeroCounts();
    Want(nine, Game::HullId::Corvette, 9);
    Assert::AreEqual(Code(Game::World::ComposeResult::TooMany), Code(world.ComposeFleet(station, 0, nine, Game::FACTION_PLAYER)),
                     L"nine ships made a fleet");
    Assert::AreEqual(Code(Game::World::ComposeResult::TooMany), Code(world.ComposeFleet(station, 0, ZeroCounts(), Game::FACTION_PLAYER)),
                     L"an empty compose made a fleet");

    std::vector<std::uint32_t> two = ZeroCounts();
    Want(two, Game::HullId::Corvette, 2);
    Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(world.ComposeFleet(station, 1, two, Game::FACTION_PLAYER)),
                     L"a compose the ledger covers was refused");
    Assert::AreEqual(Code(Game::World::ComposeResult::SlotTaken), Code(world.ComposeFleet(station, 1, two, Game::FACTION_PLAYER)),
                     L"one slot took two fleets");
    Assert::AreEqual(Code(Game::World::ComposeResult::SlotTaken),
                     Code(world.ComposeFleet(station, static_cast<std::uint8_t>(Game::FLEET_SLOTS), two, Game::FACTION_PLAYER)),
                     L"a slot past the fifth was accepted");

    Assert::AreEqual(Code(Game::World::ComposeResult::NotAStation),
                     Code(world.ComposeFleet(world.StationCount(), 2, two, Game::FACTION_PLAYER)), L"a row that is not a station composed");
    Assert::AreEqual(1u, world.FleetCount(), L"a refused compose made a fleet");
  }

  // --- launch ------------------------------------------------------------------------------------

  TEST_METHOD(TheManifestEmptiesOnTheMetronome)
  {
    Game::World world;
    const Game::World::StationId station = MakeStationAt(world, 0.0f, 0.0f);
    DockShips(world, station, Game::HullId::Corvette, 2);
    DockShips(world, station, Game::HullId::Frigate, 2);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 2);
    Want(counts, Game::HullId::Frigate, 2);
    Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(world.ComposeFleet(station, 0, counts, Game::FACTION_PLAYER)),
                     L"the compose was refused");

    std::uint32_t launched = 0;
    for (int tick = 0; tick < 4 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS); ++tick)
    {
      world.Step();
      const Game::World::FleetId fleet = world.FleetInSlot(Game::FACTION_PLAYER, 0);
      Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, fleet, L"the fleet retired mid-launch");
      const Game::World::Fleet& row = world.FleetOf(fleet);

      // Nothing is created and nothing is lost: a launch moves one hull from one count to the other.
      Assert::AreEqual(4u, row.memberCount + row.manifestCount, L"the composed set changed size during the launch");

      // One per FLEET_LAUNCH_EVERY_TICKS, and the first on the tick after the compose.
      const std::uint32_t due = 1u + static_cast<std::uint32_t>(tick) / Game::FLEET_LAUNCH_EVERY_TICKS;
      Assert::AreEqual(due, row.memberCount, L"the metronome is not launching one hull per cadence");
      launched = row.memberCount;
    }
    Assert::AreEqual(4u, launched, L"the manifest did not empty");

    // Ascending hull id, which is the order ComposeFleet filled the manifest in.
    const std::vector<Game::ShipId> members = LiveMembers(world, world.FleetInSlot(Game::FACTION_PLAYER, 0));
    Assert::AreEqual(4u, static_cast<std::uint32_t>(members.size()), L"a member was lost");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), world.Ship(members[0]).hullId, L"launched out of order");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), world.Ship(members[1]).hullId, L"launched out of order");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Frigate), world.Ship(members[2]).hullId, L"launched out of order");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Frigate), world.Ship(members[3]).hullId, L"launched out of order");
  }

  TEST_METHOD(ALaunchNeverJams)
  {
    Game::World world;
    const Game::World::StationId station = MakeStationAt(world, 0.0f, 0.0f);
    DockShips(world, station, Game::HullId::Corvette, static_cast<int>(Game::MAX_FLEET_SHIPS));

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, Game::MAX_FLEET_SHIPS);
    Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(world.ComposeFleet(station, 0, counts, Game::FACTION_PLAYER)),
                     L"the compose was refused");

    // The whole launch, and long enough after it for the formation to settle.
    const int ticks = static_cast<int>(Game::MAX_FLEET_SHIPS * Game::FLEET_LAUNCH_EVERY_TICKS) + 900;
    float worst = 0.0f;
    for (int tick = 0; tick < ticks; ++tick)
    {
      world.Step();
      const std::vector<Game::ShipId> members = LiveMembers(world, world.FleetInSlot(Game::FACTION_PLAYER, 0));
      for (std::size_t a = 0; a < members.size(); ++a)
      {
        for (std::size_t b = a + 1; b < members.size(); ++b)
          worst = std::max(worst, OverlapBetween(world, members[a], members[b]));
      }
    }
    Assert::AreEqual(0.0f, worst, 1e-2f, L"two hulls of one launch ended up inside each other");
    Assert::AreEqual(Game::MAX_FLEET_SHIPS, world.FleetOf(world.FleetInSlot(Game::FACTION_PLAYER, 0)).memberCount,
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
    Game::World world;
    const Game::World::StationId station = MakeStationAt(world, 0.0f, 0.0f);
    DockShips(world, station, Game::HullId::Corvette, 3);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 3);
    Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(world.ComposeFleet(station, 4, counts, Game::FACTION_PLAYER)),
                     L"the compose was refused");

    // The door goes before a single hull is out. Launching is the only way out of a ledger, so the
    // manifest can never become ships.
    Assert::IsTrue(world.DespawnShip(world.StationOf(station).structure), L"the structure would not despawn");

    world.Step();
    const Game::World::FleetId stranded = world.FleetInSlot(Game::FACTION_PLAYER, 4);
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, stranded, L"the fleet vanished a tick early");
    Assert::AreEqual(0u, world.FleetOf(stranded).manifestCount, L"a manifest with no door left is still waiting to launch");

    world.Step();
    Assert::AreEqual(0u, world.FleetCount(), L"a fleet nothing can ever fill is still holding a slot");
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FleetInSlot(Game::FACTION_PLAYER, 4), L"the slot is still held");
  }

  TEST_METHOD(ADockDismantlesTheFleet)
  {
    Game::World world;
    const Game::World::StationId station = MakeStationAt(world, 0.0f, 0.0f);
    DockShips(world, station, Game::HullId::Corvette, 3);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 3);
    Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(world.ComposeFleet(station, 0, counts, Game::FACTION_PLAYER)),
                     L"the compose was refused");
    Assert::AreEqual(0u, static_cast<std::uint32_t>(world.StationOf(station).docked.size()), L"the ledger kept the composed rows");

    for (int tick = 0; tick < 3 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 60; ++tick)
      world.Step();
    Assert::AreEqual(3u, world.FleetOf(world.FleetInSlot(Game::FACTION_PLAYER, 0)).memberCount, L"the fleet is not out");

    // Send it home. Docking dismantles: each capture is a ledger row and a member pruned, and the
    // slot frees when the last of them is inside.
    const Game::ShipId structure = world.Resolve(world.StationOf(station).structure);
    const std::vector<Game::ShipId> members = LiveMembers(world, world.FleetInSlot(Game::FACTION_PLAYER, 0));
    (void)world.IssueDockOrder(members, structure, Game::FACTION_PLAYER);

    for (int tick = 0; tick < 3000 && world.FleetCount() != 0; ++tick)
      world.Step();

    Assert::AreEqual(0u, world.FleetCount(), L"the fleet did not dismantle when its last ship docked");
    Assert::AreEqual(Game::World::INVALID_FLEET_ID, world.FleetInSlot(Game::FACTION_PLAYER, 0), L"the slot is still held");
    Assert::AreEqual(3u, static_cast<std::uint32_t>(world.StationOf(station).docked.size()), L"the ledger did not get its rows back");

    // And the slot composes again from the rows that came home, which is the whole loop.
    Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(world.ComposeFleet(station, 0, counts, Game::FACTION_PLAYER)),
                     L"the freed slot would not compose again");
  }

  TEST_METHOD(TheManifestSurvivesTheRoundTrip)
  {
    Game::World original;
    const Game::World::StationId station = MakeStationAt(original, 0.0f, 0.0f, 0.7f);
    DockShips(original, station, Game::HullId::Corvette, 2);
    DockShips(original, station, Game::HullId::Frigate, 2);

    std::vector<std::uint32_t> counts = ZeroCounts();
    Want(counts, Game::HullId::Corvette, 2);
    Want(counts, Game::HullId::Frigate, 2);
    Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(original.ComposeFleet(station, 3, counts, Game::FACTION_PLAYER)),
                     L"the compose was refused");

    // Saved mid-launch, so the file carries a part-empty manifest and a cooldown part-way down.
    for (int tick = 0; tick < static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 20; ++tick)
      original.Step();
    const Game::World::Fleet& before = original.FleetOf(original.FleetInSlot(Game::FACTION_PLAYER, 3));
    Assert::AreEqual(2u, before.memberCount, L"the scene is not mid-launch");
    Assert::IsTrue(before.launchCooldownTicks > 0, L"the cooldown is not part-way down, so this proves less than it should");

    std::vector<std::uint8_t> saved;
    Game::WriteWorldState(original, saved);
    Game::World reloaded;
    Assert::IsTrue(Game::ReadWorldState(saved, reloaded), L"the state did not load");

    const Game::World::FleetId there = reloaded.FleetInSlot(Game::FACTION_PLAYER, 3);
    Assert::AreNotEqual(Game::World::INVALID_FLEET_ID, there, L"the fleet did not survive the reload");
    const Game::World::Fleet& after = reloaded.FleetOf(there);
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
      Game::WriteWorldState(original, a);
      Game::WriteWorldState(reloaded, b);
      Assert::IsTrue(a == b, L"a reloaded world launched a different fleet");
    }
  }

  TEST_METHOD(TheSameComposeProducesTheSameRun)
  {
    Game::World first;
    Game::World second;
    for (Game::World* world : {&first, &second})
    {
      BuildFleetlessScene(*world);
      const Game::World::StationId station = MakeStationAt(*world, 4000.0f, 0.0f, 1.1f);
      DockShips(*world, station, Game::HullId::Interceptor, 2);
      DockShips(*world, station, Game::HullId::Corvette, 2);
      DockShips(*world, station, Game::HullId::Hauler, 1);

      std::vector<std::uint32_t> counts = ZeroCounts();
      Want(counts, Game::HullId::Interceptor, 2);
      Want(counts, Game::HullId::Corvette, 2);
      Want(counts, Game::HullId::Hauler, 1);
      Assert::AreEqual(Code(Game::World::ComposeResult::Composed), Code(world->ComposeFleet(station, 0, counts, Game::FACTION_PLAYER)),
                       L"the compose was refused");
    }

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    for (int tick = 0; tick < 5 * static_cast<int>(Game::FLEET_LAUNCH_EVERY_TICKS) + 300; ++tick)
    {
      first.Step();
      second.Step();
      Game::WriteWorldState(first, a);
      Game::WriteWorldState(second, b);
      Assert::IsTrue(a == b, L"two identical composes diverged");
    }
    Assert::AreEqual(5u, first.FleetOf(first.FleetInSlot(Game::FACTION_PLAYER, 0)).memberCount, L"the launch did not finish");
  }
};
} // namespace GameLogicTests
