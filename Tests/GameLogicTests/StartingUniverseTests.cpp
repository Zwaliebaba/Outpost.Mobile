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
