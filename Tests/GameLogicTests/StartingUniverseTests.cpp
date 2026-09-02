#include "pch.h"

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// The galaxy the shipped universe is built on, laid out the way its one author does it.
[[nodiscard]] Game::GalaxyLayout ShippedGalaxy()
{
  return Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, Game::STARTING_GALAXY, Game::GALAXY_PINS);
}
} // namespace

// Genesis, provable for the first time.
//
// It used to be four private methods on OutpostApp, in an executable with no suite, exercised only
// by launching the game and looking at it. Slice 5b moved it here because a tool and a program both
// need it (ADR 0058) -- and this file is the second reason the move was worth making
// (Design/Archive/Universe-slice-5b.md 7).
TEST_CLASS(StartingUniverseTests)
{
public:
  // The census, pinned. Not because these numbers are sacred but because a change to any of them is
  // a change to the game every player gets, and it should be a change somebody made on purpose.
  TEST_METHOD(TheShippedUniverseIsItsCensus)
  {
    const Game::GalaxyLayout galaxy = ShippedGalaxy();
    Assert::AreEqual(static_cast<std::size_t>(54), galaxy.systems.size(), L"the shipped galaxy is not 54 systems");
    Assert::AreEqual(static_cast<std::size_t>(68), galaxy.links.size(), L"the shipped galaxy is not 68 links");

    Game::Universe universe;
    Game::BuildStartingUniverse(galaxy, 0, universe);

    // 3 fleet + 164 Vanguard stations + 136 gates + 1 hostile station + 3 patrol.
    Assert::AreEqual(307u, universe.ShipCount(), L"the shipped universe is not 307 ships");
    Assert::AreEqual(136u, universe.GateCount(), L"the shipped universe is not 136 gates -- two per link");
    Assert::AreEqual(165u, universe.StationCount(), L"the shipped universe is not 165 stations -- 164 Vanguard and one Vandal");
    Assert::AreEqual(1u, universe.FleetCount(), L"the shipped universe does not hold exactly the player's fleet");
    Assert::AreEqual(static_cast<std::uint32_t>(galaxy.links.size()) * 2u, universe.GateCount(),
                     L"the gate count and the link count have come apart");
  }

  // A pure function of its arguments: the same galaxy builds the same universe, to the byte. This is
  // what lets a tool write a universe that a program then runs -- two processes, two builds, one
  // answer (ADR 0058).
  TEST_METHOD(TheShippedUniverseIsAFunctionOfItsGalaxy)
  {
    const Game::GalaxyLayout galaxy = ShippedGalaxy();

    Game::Universe first;
    Game::BuildStartingUniverse(galaxy, 0, first);
    Game::Universe second;
    Game::BuildStartingUniverse(galaxy, 0, second);

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    Game::WriteUniverseState(first, a);
    Game::WriteUniverseState(second, b);
    Assert::IsTrue(a == b, L"two builds of one galaxy disagreed at tick zero");

    // And they go on agreeing. A build that differed only in something Step reads -- a route, a
    // patrol's first leg -- would pass the line above and fail here.
    for (int tick = 0; tick < 200; ++tick)
    {
      first.Step();
      second.Step();
    }
    Game::WriteUniverseState(first, a);
    Game::WriteUniverseState(second, b);
    Assert::IsTrue(a == b, L"two builds of one galaxy diverged while running");
  }

  // The shard is an argument, and it reaches the identities. A dedicated server generating its own
  // region passes its own, and this is the row that says so is true rather than intended (ADR 0047).
  TEST_METHOD(TheShardReachesEveryIdentity)
  {
    const Game::GalaxyLayout galaxy = ShippedGalaxy();
    Game::Universe universe;
    Game::BuildStartingUniverse(galaxy, 7, universe);

    Assert::AreEqual(static_cast<std::uint32_t>(7), static_cast<std::uint32_t>(universe.Shard()), L"the shard argument was ignored");
    for (std::uint32_t id = 0; id < universe.ShipCount(); ++id)
    {
      Assert::AreEqual(static_cast<std::uint32_t>(7), static_cast<std::uint32_t>(Game::EntityShardOf(universe.EntityIdOf(id))),
                       L"a ship was minted under the wrong shard");
    }
  }

  // Every gate leads somewhere, and the somewhere is a live gate. A gate whose destination does not
  // resolve strands a fleet at the door; one that resolves to something that is not a gate lands it
  // inside a station.
  TEST_METHOD(EveryGateLeadsToALiveGate)
  {
    const Game::GalaxyLayout galaxy = ShippedGalaxy();
    Game::Universe universe;
    Game::BuildStartingUniverse(galaxy, 0, universe);

    std::uint32_t checked = 0;
    for (std::uint32_t id = 0; id < universe.ShipCount(); ++id)
    {
      if (!universe.IsGate(id))
        continue;

      const Game::EntityId destination = universe.GateOf(universe.GateAt(id)).destination;
      const Game::ShipId farSide = universe.ResolveEntity(destination);
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, farSide, L"a gate's destination did not resolve to a live ship");
      Assert::IsTrue(universe.IsGate(farSide), L"a gate's destination resolved to something that is not a gate");
      Assert::AreNotEqual(id, farSide, L"a gate led to itself");
      ++checked;
    }
    Assert::AreEqual(universe.GateCount(), checked, L"the sweep did not visit every gate");
  }

  // The path grid took the whole galaxy without declining a single island.
  //
  // This is the row that would catch a content change nothing else can see: PathIslands declines to
  // build past its ceiling QUIETLY, and the symptom is ships that stop routing, a long way from
  // whichever constant grew (ADR 0033). GalaxyLayoutTests proves the bound against the LAYOUT; this
  // proves it against the universe that was actually built out of it.
  TEST_METHOD(TheShippedUniverseDeclinesNoIsland)
  {
    const Game::GalaxyLayout galaxy = ShippedGalaxy();
    Game::Universe universe;
    Game::BuildStartingUniverse(galaxy, 0, universe);
    universe.Step(); // the islands are rebuilt on the first tick that needs them

    Assert::AreEqual(static_cast<std::size_t>(0), universe.DeclinedPathIslandCount(),
                     L"the shipped universe declined a path island -- something has outgrown the grid ceiling");
    Assert::IsTrue(universe.PathIslandCount() > 1, L"a galaxy of 54 systems came out as one island");
  }

  // The player's three hulls, in slot 1, in the order the fleet bar draws them. Selection is
  // fleet-grain (ADR 0049), so a starting hull in no fleet is a hull nobody can take hold of.
  TEST_METHOD(TheStartingFleetIsInSlotOne)
  {
    const Game::GalaxyLayout galaxy = ShippedGalaxy();
    Game::Universe universe;
    Game::BuildStartingUniverse(galaxy, 0, universe);

    // The fleet is spawned first, so its ships are ids 0..2 -- which is the ordering contract
    // BuildStartingUniverse states, and this row is what holds it.
    for (std::uint32_t at = 0; at < static_cast<std::uint32_t>(std::size(Game::STARTING_FLEET)); ++at)
    {
      Assert::AreEqual(static_cast<std::uint32_t>(Game::STARTING_FLEET[at]), universe.Ship(at).hullId,
                       L"the starting fleet is not the hulls it ships with, in order");
      Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, universe.FleetAt(at), L"a starting hull is in no fleet");
      Assert::AreEqual(static_cast<std::uint32_t>(Game::FACTION_PLAYER), static_cast<std::uint32_t>(universe.Ship(at).factionId),
                       L"a starting hull is not the player's");
    }
  }

  // The tool's whole contract, end to end: generate, write a save file, read it back in another
  // universe, and run both. What the tool writes is what the game runs (ADR 0058).
  TEST_METHOD(APartitionedGalaxyIsTheSameGalaxyCutUp)
  {
    // The census is conserved and nothing is counted twice: every shard's universe together is the
    // one galaxy the layout drew (Design/CrossShard-slice-1.md 4.4). At one shard it must also be
    // the SAME universe the shipped build makes, byte for byte, which is the claim that this slice
    // changed nothing that ships.
    const Game::GalaxyLayout shipped = ShippedGalaxy();
    Game::Universe whole;
    Game::BuildStartingUniverse(shipped, 0, whole);
    std::vector<std::uint8_t> wholeBytes;
    Game::WriteUniverseState(whole, wholeBytes);

    for (const std::uint32_t count : {1u, 2u, 4u})
    {
      Game::GalaxyDesc desc = Game::STARTING_GALAXY;
      desc.shardCount = count;
      const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, desc, Game::GALAXY_PINS);

      std::vector<Game::Universe> shards(count);
      Game::BuildStartingGalaxy(galaxy, desc, shards);

      std::uint32_t ships = 0;
      std::uint32_t gates = 0;
      std::uint32_t stations = 0;
      std::uint32_t fleets = 0;
      for (std::uint32_t at = 0; at < count; ++at)
      {
        Assert::AreEqual(at, static_cast<std::uint32_t>(shards[at].Shard()), L"a shard was not configured with its own index");
        ships += shards[at].ShipCount();
        gates += shards[at].GateCount();
        stations += shards[at].StationCount();
        fleets += shards[at].FleetCount();
      }
      Assert::AreEqual(whole.ShipCount(), ships, L"the shards together do not hold the galaxy's ships");
      Assert::AreEqual(whole.GateCount(), gates, L"the shards together do not hold the galaxy's gates");
      Assert::AreEqual(whole.StationCount(), stations, L"the shards together do not hold the galaxy's stations");
      Assert::AreEqual(1u, fleets, L"the player's fleet is in more or fewer than one shard");

      if (count == 1u)
      {
        std::vector<std::uint8_t> oneShard;
        Game::WriteUniverseState(shards[0], oneShard);
        Assert::IsTrue(wholeBytes == oneShard, L"the one-shard partitioned build is not the shipped build");
      }
    }
  }

  TEST_METHOD(AGateOutOfAShardNamesTheShardItLeadsTo)
  {
    // What the handoff design's test for "this gate leads out" actually reads
    // (Design/CrossShard.md 3): EntityShardOf(destination) != Shard(). A crossing gate must name an
    // entity that really exists in the shard it names -- an identity minted in another universe,
    // which is why every shard is built in one pass and not one at a time.
    Game::GalaxyDesc desc = Game::STARTING_GALAXY;
    desc.shardCount = 4;
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, desc, Game::GALAXY_PINS);

    std::vector<Game::Universe> shards(desc.shardCount);
    Game::BuildStartingGalaxy(galaxy, desc, shards);

    std::uint32_t leadingOut = 0;
    for (std::uint32_t at = 0; at < desc.shardCount; ++at)
    {
      for (std::uint32_t gate = 0; gate < shards[at].GateCount(); ++gate)
      {
        const Game::EntityId destination = shards[at].GateOf(gate).destination;
        const std::uint32_t other = Game::EntityShardOf(destination);
        if (other == static_cast<std::uint32_t>(shards[at].Shard()))
        {
          Assert::AreNotEqual(Game::INVALID_SHIP_ID, shards[at].ResolveEntity(destination), L"a gate inside a shard leads nowhere");
          continue;
        }
        ++leadingOut;
        Assert::IsTrue(other < desc.shardCount, L"a gate leads to a shard that does not exist");
        Assert::AreNotEqual(Game::INVALID_SHIP_ID, shards[other].ResolveEntity(destination),
                            L"a gate leading out names an entity its own shard does not hold");
      }
    }
    // Twenty of the sixty-eight links cross at four shards, so forty ends lead out. Stated rather
    // than merely non-zero, because a partition that quietly stopped cutting anything would pass a
    // non-zero check by keeping every link whole.
    Assert::AreEqual(40u, leadingOut, L"the number of gate ends leading out of their shard moved");
  }

  TEST_METHOD(AGeneratedUniverseSurvivesTheSaveFile)
  {
    const Game::GalaxyLayout galaxy = ShippedGalaxy();
    Game::Universe generated;
    Game::BuildStartingUniverse(galaxy, 0, generated);

    Game::SaveHeader header;
    header.galaxySeed = Game::STARTING_GALAXY_SEED;
    header.shard = generated.Shard();

    std::vector<std::uint8_t> file;
    Game::WriteSaveFile(generated, header, file);

    Game::Universe loaded;
    Game::SaveHeader read;
    Assert::IsTrue(Game::ReadSaveFile(file, read, loaded), L"the tool's own output was refused");
    Assert::AreEqual(Game::STARTING_GALAXY_SEED, read.galaxySeed, L"the galaxy seed did not survive the file");
    Assert::AreEqual(generated.ShipCount(), loaded.ShipCount(), L"the universe that came back is a different size");

    for (int tick = 0; tick < 120; ++tick)
    {
      generated.Step();
      loaded.Step();
    }
    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    Game::WriteUniverseState(generated, a);
    Game::WriteUniverseState(loaded, b);
    Assert::IsTrue(a == b, L"a universe written and reloaded diverged from the one that wrote it");
  }
};
} // namespace GameLogicTests
