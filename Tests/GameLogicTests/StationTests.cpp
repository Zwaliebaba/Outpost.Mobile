#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// A faction id is one byte, and a byte is a character to anything that prints one, so a failure
// would report an unprintable glyph rather than "1". Widened for the assertion, never for the wire.
[[nodiscard]] std::uint32_t Faction(Game::FactionId _faction)
{
  return _faction;
}

[[nodiscard]] bool IsHostile(const Game::World& _world, Game::FactionId _owner, Game::FactionId _other)
{
  return _world.StandingOf(_owner, _other) == Game::Standing::Hostile;
}

// A structure made into a station, at a place. The Vanguard's shipped garrison is three Corvettes
// on a 90-tick metronome (Design/Archive/Stations.md 8.2); nothing launches until slice 4, so these are
// carried and read back rather than acted on.
Game::World::StationId MakeVanguardStation(Game::World& _world, float _x, float _z)
{
  const Game::ShipId structure =
    _world.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);

  Game::World::StationDesc desc;
  desc.ownerFaction = Game::FACTION_VANGUARD;
  desc.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
  desc.protectorComplement = 3;
  desc.launchEveryTicks = 90;
  desc.targetCap = 4;
  return _world.MakeStation(structure, desc);
}
} // namespace

TEST_CLASS(StationTests)
{
public:
  // The Vandals were never neutral; the tree just had no word for it until now. This is that
  // sentence, read back out of the table it was authored into.
  TEST_METHOD(TheStandingTableStartsAsAuthored)
  {
    const Game::World world;

    for (std::uint32_t other = 0; other < Game::FACTION_LIMIT; ++other)
    {
      const Game::FactionId them = static_cast<Game::FactionId>(other);
      if (them == Game::FACTION_VANDAL)
        continue;
      Assert::IsTrue(IsHostile(world, Game::FACTION_VANDAL, them), L"the Vandal Collective is neutral to somebody");
      Assert::IsTrue(IsHostile(world, them, Game::FACTION_VANDAL), L"somebody is neutral to the Vandal Collective");
    }

    // "Every other faction" excludes the diagonal: a faction is not its own enemy.
    Assert::IsFalse(IsHostile(world, Game::FACTION_VANDAL, Game::FACTION_VANDAL), L"the Vandals are hostile to themselves");

    // The Vanguard arrives neutral. It is a government, not an enemy, until somebody makes it one.
    Assert::IsFalse(IsHostile(world, Game::FACTION_VANGUARD, Game::FACTION_PLAYER), L"the Vanguard starts hostile to the player");
    Assert::IsFalse(IsHostile(world, Game::FACTION_PLAYER, Game::FACTION_VANGUARD), L"the player starts hostile to the Vanguard");

    // A faction nobody authored is a stranger, and the safe answer for a gate is refusal.
    const Game::FactionId stranger = static_cast<Game::FactionId>(Game::FACTION_LIMIT);
    Assert::IsTrue(IsHostile(world, Game::FACTION_VANGUARD, stranger), L"an unauthored faction was admitted as a friend");
    Assert::IsTrue(IsHostile(world, stranger, Game::FACTION_VANGUARD), L"an unauthored faction was read as neutral");
  }

  TEST_METHOD(AStationIsItsRow)
  {
    Game::World world;
    const Game::ShipId plain = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    const Game::World::StationId station = MakeVanguardStation(world, 900.0f, 900.0f);

    Assert::AreNotEqual(Game::World::INVALID_STATION_ID, station, L"a live structure did not become a station");
    Assert::AreEqual(static_cast<std::uint32_t>(1), world.StationCount(), L"the station table holds the wrong number of rows");

    // The garrison is content, carried verbatim. Nothing reads it until slice 4; a desc that lost it
    // here would leave the composition root unable to say what it means.
    const Game::World::Station& row = world.StationOf(station);
    Assert::AreEqual(Faction(Game::FACTION_VANGUARD), Faction(row.ownerFaction), L"the station forgot its owner");
    Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), row.protectorHullId, L"the station forgot its protector hull");
    Assert::AreEqual(static_cast<std::uint32_t>(3), row.protectorComplement, L"the station forgot its complement");
    Assert::AreEqual(static_cast<std::uint32_t>(90), row.launchEveryTicks, L"the station forgot its cadence");
    Assert::AreEqual(static_cast<std::uint32_t>(4), row.targetCap, L"the station forgot its target cap");
    Assert::IsTrue(row.docked.empty(), L"a fresh station already has somebody inside it");

    // Found by the ship it is, and not found for the ship it is not.
    const Game::ShipId structure = world.Resolve(row.structure);
    Assert::AreEqual(station, world.StationAt(structure), L"the station was not found by its own structure");
    Assert::IsTrue(world.IsStation(structure), L"the structure does not report as a station");
    Assert::AreEqual(Game::World::INVALID_STATION_ID, world.StationAt(plain), L"a plain ship reported as a station");
    Assert::IsFalse(world.IsStation(plain), L"a plain ship reported as a station");

    // A ship that is not live makes no row.
    Assert::AreEqual(Game::World::INVALID_STATION_ID, world.MakeStation(world.ShipCount(), Game::World::StationDesc{}),
                     L"a station was made on a ship that does not exist");
  }

  // The row holds a handle, not an id, so the death of its structure orphans the row instead of
  // retargeting it. Nothing can destroy a station this phase; the user-station design inherits a
  // table that already tolerates one (Design/Archive/Stations.md 6.1).
  TEST_METHOD(ADeadStructureOrphansItsRow)
  {
    Game::World world;
    const Game::World::StationId station = MakeVanguardStation(world, 900.0f, 900.0f);
    const Game::ShipId structure = world.Resolve(world.StationOf(station).structure);

    // A second ship, so that swap-and-pop has something to move into the freed slot. Without one,
    // a row holding a raw id would look correct by accident.
    const Game::ShipId other = world.SpawnShip(Game::LocalPos(50.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
    Assert::AreEqual(static_cast<Game::ShipId>(1), other, L"the second ship did not land where the test assumes");

    Assert::IsTrue(world.DespawnShip(world.HandleOf(structure)), L"the despawn failed");

    // The survivor moved into index 0, which is exactly where the station's structure used to be.
    Assert::AreEqual(Game::INVALID_SHIP_ID, world.Resolve(world.StationOf(station).structure), L"a dead structure still resolves");
    Assert::IsFalse(world.IsStation(0), L"the ship that swap-and-pop moved inherited a station row");
    Assert::AreEqual(Game::World::INVALID_STATION_ID, world.StationAt(0), L"the moved ship was found as a station");
  }

  // Standing is imperial: the attacked station scrambles, but the *grudge* belongs to the whole
  // owning faction, so a second station a kilometre away refuses the attacker without being told.
  TEST_METHOD(AggressionIsImperialAndPermanent)
  {
    Game::World world;
    const Game::World::StationId attacked = MakeVanguardStation(world, 900.0f, 900.0f);
    const Game::World::StationId elsewhere = MakeVanguardStation(world, -900.0f, -900.0f);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    Assert::IsFalse(IsHostile(world, Game::FACTION_VANGUARD, Game::FACTION_PLAYER), L"the Vanguard was hostile before anything happened");

    world.RecordAggression(world.HandleOf(raider), attacked);

    Assert::IsTrue(IsHostile(world, Game::FACTION_VANGUARD, Game::FACTION_PLAYER),
                   L"attacking a station did not make the attacker criminal");
    Assert::AreEqual(Faction(Game::FACTION_VANGUARD), Faction(world.StationOf(elsewhere).ownerFaction),
                     L"the second station changed owner");

    // Directional. The player has no opinion of the Vanguard; the simulation does not invent one.
    Assert::IsFalse(IsHostile(world, Game::FACTION_PLAYER, Game::FACTION_VANGUARD), L"the aggression flipped the wrong direction too");

    // Permanent: no decay, no forgiveness, no payment of dues. A standings-repair mechanic is a
    // later design, and the owner chose permanence over inventing half of one here.
    for (int tick = 0; tick < 1000; ++tick)
      world.Step();
    Assert::IsTrue(IsHostile(world, Game::FACTION_VANGUARD, Game::FACTION_PLAYER), L"a thousand ticks forgave a criminal");
  }

  TEST_METHOD(AnAggressionNamesTheAttackersFaction)
  {
    Game::World world;
    const Game::World::StationId station = MakeVanguardStation(world, 900.0f, 900.0f);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    const Game::ShipHandle raiderHandle = world.HandleOf(raider);

    world.RecordAggression(raiderHandle, station);
    Assert::IsTrue(IsHostile(world, Game::FACTION_VANGUARD, Game::FACTION_PLAYER), L"the aggression did not register");

    // Keyed on the faction and not on the ship: the criminal's whole fleet is criminal, which is
    // what "one subscriber is one faction" means today (Design/Archive/Stations.md 4.2).
    const Game::ShipId innocent = world.SpawnShip(Game::LocalPos(300.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Frigate));
    Assert::IsTrue(IsHostile(world, Game::FACTION_VANGUARD, world.Ship(innocent).factionId),
                   L"a second ship of the criminal faction is clean");

    // A stale handle is a no-op, not a flip against faction zero -- which is the player's, so
    // getting this wrong would make the player criminal every time an attacker died first.
    Game::World second;
    const Game::World::StationId secondStation = MakeVanguardStation(second, 900.0f, 900.0f);
    const Game::ShipId doomed = second.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    const Game::ShipHandle stale = second.HandleOf(doomed);
    Assert::IsTrue(second.DespawnShip(stale), L"the despawn failed");
    second.RecordAggression(stale, secondStation);
    Assert::IsFalse(IsHostile(second, Game::FACTION_VANGUARD, Game::FACTION_PLAYER), L"a dead attacker made the player criminal");
  }

  // The byte the wire states, before it is a byte: their opinion of you, not yours of them.
  TEST_METHOD(TheHostileMaskIsWhatOthersThinkOfYou)
  {
    Game::World world;
    const Game::World::StationId station = MakeVanguardStation(world, 900.0f, 900.0f);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    const std::uint8_t vandalBit = static_cast<std::uint8_t>(1u << Game::FACTION_VANDAL);
    const std::uint8_t vanguardBit = static_cast<std::uint8_t>(1u << Game::FACTION_VANGUARD);

    const std::uint8_t atBoot = world.HostileMaskFor(Game::FACTION_PLAYER);
    Assert::AreEqual(vandalBit, static_cast<std::uint8_t>(atBoot & vandalBit), L"the Vandal bit is clear at boot");
    Assert::AreEqual(static_cast<std::uint8_t>(0), static_cast<std::uint8_t>(atBoot & vanguardBit), L"the Vanguard bit is set at boot");

    world.RecordAggression(world.HandleOf(raider), station);

    const std::uint8_t afterwards = world.HostileMaskFor(Game::FACTION_PLAYER);
    Assert::AreEqual(vanguardBit, static_cast<std::uint8_t>(afterwards & vanguardBit),
                     L"the Vanguard bit did not light after an aggression");

    // The Vandals' own mask never mentions the Vandals, and does mention everyone else.
    const std::uint8_t vandalView = world.HostileMaskFor(Game::FACTION_VANDAL);
    Assert::AreEqual(static_cast<std::uint8_t>(0), static_cast<std::uint8_t>(vandalView & vandalBit),
                     L"the Vandals hold themselves hostile");
    Assert::AreEqual(static_cast<std::uint8_t>(1u << Game::FACTION_PLAYER),
                     static_cast<std::uint8_t>(vandalView & static_cast<std::uint8_t>(1u << Game::FACTION_PLAYER)),
                     L"the player does not appear in the Vandals' mask");
  }
};
} // namespace GameLogicTests
