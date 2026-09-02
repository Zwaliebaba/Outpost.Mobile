#include "pch.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// The shipped bounds are GalaxyDesc's own defaults (Design/Archive/Universe.md 10), so a test that wants
// them constructs one and asks. Spelling them here instead would prove something about a galaxy
// nobody plays the first time the density is retuned.
constexpr std::uint64_t GALAXY_SEED = 0x46726F6E74696572ull; // "Frontier"
constexpr std::uint64_t HOME_SEED = 0x53797331ull;           // "Sys1", the starting system

// A hash of the same cell, which is what the block partition is measured against. It balances
// perfectly and keeps nothing together, which is the reason Design/CrossShard.md 2 rejects it.
[[nodiscard]] std::uint32_t HashShard(const Game::SystemSite& _site, std::uint32_t _count) noexcept
{
  std::uint64_t hash = 1469598103934665603ull;
  for (const std::uint64_t value : {static_cast<std::uint64_t>(_site.cellQ + 1000), static_cast<std::uint64_t>(_site.cellR + 1000)})
  {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return static_cast<std::uint32_t>(hash % _count);
}

[[nodiscard]] bool SameSite(const Game::SystemSite& _a, const Game::SystemSite& _b) noexcept
{
  return _a.cellQ == _b.cellQ && _a.cellR == _b.cellR && _a.systemSeed == _b.systemSeed && _a.pin == _b.pin &&
         IsSamePosition(_a.starPos, _b.starPos);
}

[[nodiscard]] bool Holds(const Game::GalaxyLayout& _galaxy, const Game::SystemSite& _site) noexcept
{
  return std::any_of(_galaxy.systems.begin(), _galaxy.systems.end(), [&](const Game::SystemSite& s) { return SameSite(s, _site); });
}

// The one pin the game ships: the starting system, at the lattice origin, laid out from the seed
// and the description the composition root already boots on.
[[nodiscard]] Game::SystemPin ShippedPin() noexcept
{
  Game::SystemPin pin;
  pin.cellQ = 0;
  pin.cellR = 0;
  pin.systemSeed = HOME_SEED;
  pin.desc.pinFirstPlanet = true;
  pin.desc.firstPlanetBearingRad = -23.0f * (DirectX::XM_PI / 180.0f);
  pin.desc.firstPlanetOrbitMetres = 3500.0f;
  return pin;
}

// How many pieces the gate graph comes in, by union-find over the links.
[[nodiscard]] std::size_t ComponentCount(const Game::GalaxyLayout& _galaxy)
{
  const std::size_t count = _galaxy.systems.size();
  std::vector<std::size_t> parent(count);
  std::iota(parent.begin(), parent.end(), 0u);

  const auto find = [&parent](std::size_t _x)
  {
    while (parent[_x] != _x)
    {
      parent[_x] = parent[parent[_x]];
      _x = parent[_x];
    }
    return _x;
  };

  for (const Game::GateLink& link : _galaxy.links)
    parent[find(link.systemA)] = find(link.systemB);

  std::size_t roots = 0;
  for (std::size_t at = 0; at < count; ++at)
    roots += (find(at) == at) ? 1u : 0u;
  return roots;
}
} // namespace

TEST_CLASS(GalaxyLayoutTests)
{
public:
  TEST_METHOD(ThePartitionIsAPureFunctionOfTheCell)
  {
    // Every participant -- each shard, and every client -- must agree on which shard owns a system
    // and must agree WITHOUT being told (Design/CrossShard.md 2). That is only true if this is a
    // function of the cell and the count, so it is asked twice about the same site and asked about
    // sites built by hand rather than by a layout.
    Game::GalaxyDesc desc = Game::STARTING_GALAXY;
    desc.shardCount = 4;
    for (std::int32_t q = -5; q <= 5; ++q)
    {
      for (std::int32_t r = -5; r <= 5; ++r)
      {
        Game::SystemSite site;
        site.cellQ = q;
        site.cellR = r;
        const Game::ShardId once = Game::ShardOfSystem(site, desc);
        Assert::AreEqual(static_cast<std::uint32_t>(once), static_cast<std::uint32_t>(Game::ShardOfSystem(site, desc)),
                         L"the partition answered differently the second time it was asked");
        Assert::IsTrue(once < desc.shardCount, L"the partition named a shard that does not exist");

        // The star's position plays no part: two systems in one cell are one shard's whatever the
        // jitter did to them, which is what makes a client's answer the server's.
        Game::SystemSite moved = site;
        Game::Translate(moved.starPos, 12345.0f, -6789.0f);
        Assert::AreEqual(static_cast<std::uint32_t>(once), static_cast<std::uint32_t>(Game::ShardOfSystem(moved, desc)),
                         L"the partition read the star's position rather than its cell");
      }
    }
  }

  TEST_METHOD(OneShardHoldsEverything)
  {
    // The shipped galaxy, and the reason this slice changes nothing that ships: at one shard the
    // partition is the identity and every system is shard 0's.
    Game::GalaxyDesc desc = Game::STARTING_GALAXY;
    Assert::AreEqual(1u, desc.shardCount, L"the shipped galaxy is no longer one shard");
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, desc, Game::GALAXY_PINS);
    for (const Game::SystemSite& site : galaxy.systems)
      Assert::AreEqual(0u, static_cast<std::uint32_t>(Game::ShardOfSystem(site, desc)), L"a system left shard 0 in a one-shard galaxy");
    Assert::AreEqual(1u, Game::OccupiedShardCount(galaxy.systems, desc), L"one shard did not hold the whole galaxy");
  }

  TEST_METHOD(ThePartitionKeepsNeighboursTogetherFarBetterThanAHash)
  {
    // The design's one claim about the partition, measured rather than asserted: contiguity is the
    // requirement and cheapness is not, because a partition that scatters neighbours turns most
    // gates into wire crossings and that is the cost the whole cross-shard design bounds
    // (Design/CrossShard.md 2). The floors below are what the block split measured on 2026-09-02;
    // they are a floor and not an equality, so a later partition may beat them and may not lose to
    // them silently.
    struct Row
    {
      std::uint32_t shards;
      std::uint32_t keptAtLeast;
    };
    constexpr Row ROWS[] = {{2, 61}, {3, 55}, {4, 48}};

    for (const Row& row : ROWS)
    {
      Game::GalaxyDesc desc = Game::STARTING_GALAXY;
      desc.shardCount = row.shards;
      const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, desc, Game::GALAXY_PINS);

      std::uint32_t kept = 0;
      std::uint32_t hashKept = 0;
      for (const Game::GateLink& link : galaxy.links)
      {
        const Game::SystemSite& a = galaxy.systems[link.systemA];
        const Game::SystemSite& b = galaxy.systems[link.systemB];
        if (Game::ShardOfSystem(a, desc) == Game::ShardOfSystem(b, desc))
          ++kept;
        if (HashShard(a, row.shards) == HashShard(b, row.shards))
          ++hashKept;
      }

      Assert::IsTrue(kept >= row.keptAtLeast, std::format(L"at {} shards the partition kept {} of {} links whole, against a floor of {}",
                                                          row.shards, kept, galaxy.links.size(), row.keptAtLeast)
                                                .c_str());
      // The comparison is the argument, not the number: a hash balances perfectly and keeps nothing
      // together, which is why it lost.
      Assert::IsTrue(kept > hashKept * 2u,
                     std::format(L"at {} shards the partition kept {} links against a hash's {}", row.shards, kept, hashKept).c_str());
      Assert::AreEqual(row.shards, Game::OccupiedShardCount(galaxy.systems, desc), L"a shard was left with no system at all");
    }
  }

  TEST_METHOD(OccupancyIsMeasuredAndNotAssumed)
  {
    // Occupancy is NOT monotonic in the count -- the shipped galaxy fills every shard at ten and
    // leaves one empty at nine, because a band's edges move with the count and the systems inside it
    // do not. That is why UniverseGen asks the drawn layout rather than a ceiling, and this row is
    // what says the ceiling would have been a lie (Design/CrossShard-slice-1.md 6).
    Game::GalaxyDesc desc = Game::STARTING_GALAXY;
    Assert::AreEqual(11u, Game::MaxShardCount(desc), L"the column ceiling moved");

    const auto occupiedAt = [](std::uint32_t _count)
    {
      Game::GalaxyDesc at = Game::STARTING_GALAXY;
      at.shardCount = _count;
      const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(Game::STARTING_GALAXY_SEED, Game::UniversePos{}, at, Game::GALAXY_PINS);
      return Game::OccupiedShardCount(galaxy.systems, at);
    };

    Assert::AreEqual(8u, occupiedAt(9u), L"nine shards no longer leaves one empty");
    Assert::AreEqual(10u, occupiedAt(10u), L"ten shards no longer fills every one");
    Assert::IsTrue(occupiedAt(9u) < 9u && occupiedAt(10u) == 10u,
                   L"occupancy has become monotonic, and the tool's per-count check could be a ceiling after all");
  }

  // A seed means one galaxy forever, which is the reason the layout may be content rather than wire
  // data: both halves call this and neither tells the other what came out (ADR 0037, ADR 0055).
  TEST_METHOD(TheGalaxyIsAFunctionOfItsSeed)
  {
    const Game::GalaxyDesc desc;
    const Game::UniversePos origin = Game::LocalPos(0.0f, 0.0f);
    const Game::SystemPin pins[] = {ShippedPin()};

    const Game::GalaxyLayout first = Game::LayOutGalaxy(GALAXY_SEED, origin, desc, pins);
    const Game::GalaxyLayout second = Game::LayOutGalaxy(GALAXY_SEED, origin, desc, pins);

    Assert::IsFalse(first.systems.empty(), L"the shipped description laid out no systems at all");
    Assert::AreEqual(first.systems.size(), second.systems.size(), L"two calls with one seed produced different system counts");
    for (std::size_t at = 0; at < first.systems.size(); ++at)
      Assert::IsTrue(SameSite(first.systems[at], second.systems[at]), L"two calls with one seed produced different systems");
    Assert::AreEqual(first.links.size(), second.links.size(), L"two calls with one seed produced different gate graphs");

    // Adjacent seeds, because adjacent is the case a weak seeding procedure fails --
    // UniverseLayoutTests makes the same check for the same reason.
    const Game::GalaxyLayout neighbour = Game::LayOutGalaxy(GALAXY_SEED + 1u, origin, desc, pins);
    bool anyDiffers = neighbour.systems.size() != first.systems.size();
    for (std::size_t at = 0; at < first.systems.size() && at < neighbour.systems.size(); ++at)
      anyDiffers = anyDiffers || !SameSite(first.systems[at], neighbour.systems[at]);
    Assert::IsTrue(anyDiffers, L"two adjacent seeds produced the same galaxy");
  }

  // The property the fixed per-cell draw spend buys, and the reason density may be a balance knob:
  // a cell's stream belongs to the cell rather than to the census, so raising the density reveals
  // cells without moving, reseeding or removing any system a lower density already had.
  TEST_METHOD(RaisingTheDensityLeavesTheSurvivorsAlone)
  {
    const Game::UniversePos origin = Game::LocalPos(0.0f, 0.0f);
    const Game::SystemPin pins[] = {ShippedPin()};

    for (int step = 4; step < 17; ++step)
    {
      Game::GalaxyDesc sparse;
      sparse.density = static_cast<float>(step) * 0.05f;
      Game::GalaxyDesc dense = sparse;
      dense.density = static_cast<float>(step + 1) * 0.05f;

      const Game::GalaxyLayout thin = Game::LayOutGalaxy(GALAXY_SEED, origin, sparse, pins);
      const Game::GalaxyLayout thick = Game::LayOutGalaxy(GALAXY_SEED, origin, dense, pins);

      Assert::IsTrue(thick.systems.size() >= thin.systems.size(), L"a denser galaxy held fewer systems");
      for (const Game::SystemSite& was : thin.systems)
        Assert::IsTrue(Holds(thick, was), L"raising the density moved, reseeded or removed a system that already existed");
    }
  }

  // The pin overwrites its cell and does not skip its draws. If it skipped them, every cell after it
  // would take another cell's stream and one seed would mean two galaxies depending on a table.
  TEST_METHOD(APinnedSystemHoldsItsCellAndSeed)
  {
    const Game::UniversePos origin = Game::LocalPos(0.0f, 0.0f);
    const Game::SystemPin pins[] = {ShippedPin()};

    // Occupied at any density, including a density that would have refused every cell.
    for (int step = 0; step <= 10; ++step)
    {
      Game::GalaxyDesc desc;
      desc.density = static_cast<float>(step) * 0.1f;
      const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED, origin, desc, pins);

      const auto home =
        std::find_if(galaxy.systems.begin(), galaxy.systems.end(), [](const Game::SystemSite& s) { return s.cellQ == 0 && s.cellR == 0; });
      Assert::IsTrue(home != galaxy.systems.end(), L"the pinned system was not in the galaxy");
      Assert::AreEqual(0u, home->pin, L"the pinned system does not name its pin");
      Assert::AreEqual(HOME_SEED, home->systemSeed, L"the pinned system did not take its authored seed");
      Assert::AreEqual(0.0f, Game::Distance(origin, home->starPos), 0.0f,
                       L"the pinned system took the jitter: the framed opening shot moved");
    }

    // And adding the pin moves nothing the lattice drew.
    const Game::GalaxyDesc desc;
    const Game::GalaxyLayout without = Game::LayOutGalaxy(GALAXY_SEED, origin, desc, {});
    const Game::GalaxyLayout with = Game::LayOutGalaxy(GALAXY_SEED, origin, desc, pins);
    for (const Game::SystemSite& drawn : without.systems)
    {
      if (drawn.cellQ == 0 && drawn.cellR == 0)
        continue; // the pinned cell is the one the pin is allowed to change
      Assert::IsTrue(Holds(with, drawn), L"adding a pin skipped a draw: an unpinned system moved");
    }
  }

  // The bound the jitter buys, and it is a proof over the construction rather than a sample: the
  // assertion holds for every seed tried because it holds for every seed there is.
  TEST_METHOD(SystemsKeepTheirDistance)
  {
    const Game::GalaxyDesc desc;
    const Game::UniversePos origin = Game::LocalPos(0.0f, 0.0f);
    const Game::SystemPin pins[] = {ShippedPin()};
    const float bound = Game::MinimumStarSeparationMetres(desc);

    Assert::IsTrue(bound > 0.0f, L"the jitter is wide enough that two stars may share a point");

    for (std::uint64_t seed = 0; seed < 64; ++seed)
    {
      const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED + seed, origin, desc, pins);
      for (std::size_t a = 0; a < galaxy.systems.size(); ++a)
      {
        for (std::size_t b = a + 1; b < galaxy.systems.size(); ++b)
        {
          const float apart = Game::Distance(galaxy.systems[a].starPos, galaxy.systems[b].starPos);
          Assert::IsTrue(apart >= bound - 1.0f, L"two stars came closer than the jitter bound allows");
        }
      }
    }
  }

  // The theorem the graph was chosen for: the relative neighborhood graph contains the minimum
  // spanning tree, so a galaxy is connected for every seed by construction and no repair pass --
  // which would be a rejection loop by another name -- is needed (ADR 0055).
  TEST_METHOD(TheGateGraphConnectsEverySystem)
  {
    const Game::GalaxyDesc desc;
    const Game::UniversePos origin = Game::LocalPos(0.0f, 0.0f);
    const Game::SystemPin pins[] = {ShippedPin()};

    for (std::uint64_t seed = 0; seed < 64; ++seed)
    {
      const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED + seed, origin, desc, pins);
      Assert::AreEqual(static_cast<std::size_t>(1), ComponentCount(galaxy), L"a galaxy came out in more than one piece");
    }
  }

  // The rule blocks on strictly closer, and a tie therefore leaves the link alone. Three systems in
  // a 36-48-60 triangle scaled to whole metres a float holds exactly: for the pair (A, B), the
  // blocker C is exactly as far away as the pair is long. Blocking on a tie cuts two of these three
  // links and strands A -- which is the case that cannot be reached by rolling a lattice, and is why
  // LinkGates is exposed for a test to put systems where it wants them.
  TEST_METHOD(ATieLeavesTheLinkAlone)
  {
    const auto siteAt = [](float _x, float _z)
    {
      Game::SystemSite site;
      site.starPos = Game::LocalPos(_x, _z);
      return site;
    };
    const Game::SystemSite triangle[] = {siteAt(0.0f, 0.0f), siteAt(60000.0f, 0.0f), siteAt(36000.0f, 48000.0f)};

    std::vector<Game::GateLink> links;
    Game::LinkGates(triangle, links);
    Assert::AreEqual(static_cast<std::size_t>(3), links.size(), L"a tie cut a link: the rule must block only on strictly closer");
  }

  // The link list is a function of the layout and not of the loop that found it, which is what lets
  // a save file and a client compare two galaxies without sorting first.
  TEST_METHOD(TheGateGraphIsOrderedAndUnique)
  {
    const Game::GalaxyDesc desc;
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED, Game::LocalPos(0.0f, 0.0f), desc, {});

    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    std::pair<std::uint32_t, std::uint32_t> previous{0u, 0u};
    bool first = true;
    for (const Game::GateLink& link : galaxy.links)
    {
      Assert::IsTrue(link.systemA < link.systemB, L"a link was not emitted low index first, or named a system twice");
      Assert::IsTrue(link.systemB < galaxy.systems.size(), L"a link named a system that does not exist");
      Assert::IsTrue(seen.insert({link.systemA, link.systemB}).second, L"a link appeared twice");

      const std::pair<std::uint32_t, std::uint32_t> here{link.systemA, link.systemB};
      if (!first)
        Assert::IsTrue(previous < here, L"the links are not in ascending order");
      previous = here;
      first = false;
    }
  }

  // Two bounds at once, and the second is what the lattice pitch is actually for. PathIslands
  // declines to build past its ceiling quietly -- the symptom is ships that stop routing, a long
  // way from this file -- and two systems whose architecture could reach each other would merge
  // into one island that is certainly past it.
  TEST_METHOD(EverySystemFitsItsOwnPathIsland)
  {
    const Game::GalaxyDesc desc;

    // Measured against the GATES, not the planets: a gate stands further out than any orbit, so it
    // is the gates that decide a system's static span. Counting only orbits is what let
    // Design/Archive/Universe.md 10 specify a gate ring of 8 000 m -- 532 cells against a ceiling of 512
    // (Design/Archive/Universe-slice-3.md 7).
    const float widestMetres = std::max(desc.gateRingMetres, desc.systemBounds.maxOrbitMetres);
    const float systemSpanMetres = 2.0f * widestMetres + 2.0f * Game::PATH_GRID_MARGIN_METRES;
    const int cells = static_cast<int>(std::ceil(systemSpanMetres / Game::PATH_CELL_SIZE_METRES));
    Assert::IsTrue(cells < Game::PATH_GRID_MAX_CELLS_PER_AXIS,
                   L"a system is wider than one path grid may be: the grid would decline and ships would stop routing");

    const float clearMetres = Game::MinimumStarSeparationMetres(desc) - 2.0f * widestMetres;
    Assert::IsTrue(clearMetres > 2.0f * Game::PATH_GRID_MARGIN_METRES,
                   L"two systems' architecture can reach each other: their islands would merge");
  }

  // Every position the simulation stores satisfies UniversePos's invariant, and a galaxy is the
  // first thing in this tree big enough that its own extent crosses sectors on its own.
  TEST_METHOD(AGalaxyHoldsTheSectorInvariant)
  {
    const Game::GalaxyDesc desc;
    const Game::UniversePos corner = Game::LocalPos(Game::SECTOR_SIZE_METRES - 1.0f, Game::SECTOR_SIZE_METRES - 1.0f);
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED, corner, desc, {});

    for (const Game::SystemSite& site : galaxy.systems)
    {
      Assert::IsTrue(site.starPos.localX >= 0.0f && site.starPos.localX < Game::SECTOR_SIZE_METRES,
                     L"a star's local X is outside its sector");
      Assert::IsTrue(site.starPos.localZ >= 0.0f && site.starPos.localZ < Game::SECTOR_SIZE_METRES,
                     L"a star's local Z is outside its sector");
    }

    // The origin crossed a boundary to get here; the galaxy around it must still be the same shape.
    const Game::GalaxyLayout atOrigin = Game::LayOutGalaxy(GALAXY_SEED, Game::LocalPos(0.0f, 0.0f), desc, {});
    Assert::AreEqual(atOrigin.systems.size(), galaxy.systems.size(), L"anchoring the galaxy elsewhere changed how many systems it has");
    for (std::size_t at = 0; at + 1 < galaxy.systems.size(); ++at)
    {
      const float here = Game::Distance(galaxy.systems[at].starPos, galaxy.systems[at + 1].starPos);
      const float there = Game::Distance(atOrigin.systems[at].starPos, atOrigin.systems[at + 1].starPos);
      Assert::AreEqual(there, here, 1.0f, L"a galaxy anchored near a sector boundary is a different shape");
    }
  }

  // A drawn system draws its own count and its own planets, from its own seed and nothing else --
  // so a system can be laid out on demand, by either half, without the galaxy walk being re-run.
  TEST_METHOD(AnUnpinnedSystemDrawsItsOwnPlanets)
  {
    const Game::GalaxyDesc desc;
    const Game::SystemPin pins[] = {ShippedPin()};
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED, Game::LocalPos(0.0f, 0.0f), desc, pins);

    for (const Game::SystemSite& site : galaxy.systems)
    {
      const Game::SystemLayout system = Game::LayOutGalaxySystem(site, desc, pins);
      Assert::IsTrue(IsSamePosition(site.starPos, system.starPos), L"a system was laid out around the wrong star");

      if (site.pin == Game::INVALID_PIN_INDEX)
      {
        Assert::IsTrue(system.planets.size() >= desc.minPlanetCount && system.planets.size() <= desc.maxPlanetCount,
                       L"a drawn system has a planet count outside its band");
        for (const Game::PlanetSite& planet : system.planets)
        {
          const float orbit = Game::Distance(site.starPos, planet.posUniverse);
          Assert::IsTrue(orbit >= desc.systemBounds.minOrbitMetres - 1.0f && orbit <= desc.systemBounds.maxOrbitMetres + 1.0f,
                         L"a planet was laid outside its orbit band");
          Assert::IsTrue(planet.radiusMetres >= desc.systemBounds.minRadiusMetres &&
                           planet.radiusMetres <= desc.systemBounds.maxRadiusMetres,
                         L"a planet was given a radius outside its band");
        }
      }

      const Game::SystemLayout again = Game::LayOutGalaxySystem(site, desc, pins);
      Assert::AreEqual(system.planets.size(), again.planets.size(), L"one site laid out twice produced two systems");
      for (std::size_t at = 0; at < system.planets.size(); ++at)
        Assert::AreEqual(system.planets[at].bodySeed, again.planets[at].bodySeed, L"one site laid out twice produced two systems");
    }
  }

  // The pinned system is the system the game boots into today, reached through the galaxy: what a
  // pin means is that its contents are its author's, not the lattice's.
  TEST_METHOD(ThePinnedSystemIsTheOneTheGameBootsInto)
  {
    const Game::GalaxyDesc desc;
    const Game::UniversePos origin = Game::LocalPos(0.0f, 0.0f);
    const Game::SystemPin pins[] = {ShippedPin()};
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED, origin, desc, pins);

    const auto home = std::find_if(galaxy.systems.begin(), galaxy.systems.end(),
                                   [](const Game::SystemSite& s) { return s.pin != Game::INVALID_PIN_INDEX; });
    Assert::IsTrue(home != galaxy.systems.end(), L"the galaxy holds no pinned system");

    const Game::SystemLayout viaGalaxy = Game::LayOutGalaxySystem(*home, desc, pins);
    const Game::SystemLayout viaBoot = Game::LayOutSystem(pins[0].systemSeed, home->starPos, pins[0].desc);

    Assert::AreEqual(viaBoot.planets.size(), viaGalaxy.planets.size(), L"the pinned system is not the system the game boots into");
    for (std::size_t at = 0; at < viaBoot.planets.size(); ++at)
    {
      Assert::AreEqual(viaBoot.planets[at].bodySeed, viaGalaxy.planets[at].bodySeed,
                       L"the pinned system's worlds are not the shipped ones");
      Assert::AreEqual(viaBoot.planets[at].bearingRad, viaGalaxy.planets[at].bearingRad, 0.0f,
                       L"the pinned system's worlds are not where the shipped ones are");
    }
  }

  // The walk is one fixed order over every candidate cell, exactly once. It is the assumption every
  // other property here rests on: a cell visited twice would draw twice, and a cell skipped would
  // shift the stream of every cell after it.
  TEST_METHOD(TheWalkVisitsEveryCellExactlyOnce)
  {
    for (std::uint32_t rings = 0; rings <= 7; ++rings)
    {
      Game::GalaxyDesc desc;
      desc.ringCount = rings;
      desc.density = 1.0f; // every cell occupied, so the systems are the cells

      const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED, Game::LocalPos(0.0f, 0.0f), desc, {});
      Assert::AreEqual(static_cast<std::size_t>(Game::GalaxyCellCount(rings)), galaxy.systems.size(),
                       L"the walk did not visit every candidate cell");

      std::set<std::pair<std::int32_t, std::int32_t>> cells;
      for (const Game::SystemSite& site : galaxy.systems)
        Assert::IsTrue(cells.insert({site.cellQ, site.cellR}).second, L"the spiral walk visited a cell twice");
    }
  }

  // SystemAt is the client's "which system am I in", and it is here rather than in the composition
  // root so that this row can exist at all (Design/Archive/Universe-slice-4b.md 4).
  //
  // The property is total: EVERY system's own star answers with that system, over the shipped
  // galaxy, so no cell has a neighbour close enough to steal its own centre. A rule that got one
  // system wrong would put the wrong worlds on screen for anybody standing in it.
  TEST_METHOD(EverySystemOwnsItsOwnStar)
  {
    const Game::SystemPin pins[] = {ShippedPin()};
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED, Game::LocalPos(0.0f, 0.0f), Game::GalaxyDesc{}, pins);
    Assert::IsTrue(galaxy.systems.size() > 1, L"a one-system galaxy would prove nothing here");

    for (std::size_t at = 0; at < galaxy.systems.size(); ++at)
      Assert::AreEqual(static_cast<std::uint32_t>(at), Game::SystemAt(galaxy.systems, galaxy.systems[at].starPos),
                       L"a system's own star did not resolve to that system");
  }

  // And total over the part of a system a player can actually be in: every planet site and every
  // gate site of every system resolves to the system it belongs to.
  //
  // This is the row that would catch a gate ring pushed out past half the lattice pitch -- a gate
  // is the furthest from its star that anything authored gets, and a gate on the wrong side of the
  // midpoint would flip the scenery the moment a fleet arrived at it.
  TEST_METHOD(EveryPlaceInASystemResolvesToIt)
  {
    const Game::SystemPin pins[] = {ShippedPin()};
    const Game::GalaxyDesc desc;
    const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(GALAXY_SEED, Game::LocalPos(0.0f, 0.0f), desc, pins);

    for (std::size_t at = 0; at < galaxy.systems.size(); ++at)
    {
      const std::uint32_t index = static_cast<std::uint32_t>(at);
      const Game::SystemLayout system = Game::LayOutGalaxySystem(galaxy.systems[at], desc, pins);
      for (const Game::PlanetSite& site : system.planets)
        Assert::AreEqual(index, Game::SystemAt(galaxy.systems, site.posUniverse), L"a planet resolved to the wrong system");
    }

    for (const Game::GateLink& link : galaxy.links)
    {
      const Game::SystemSite& a = galaxy.systems[link.systemA];
      const Game::SystemSite& b = galaxy.systems[link.systemB];
      Assert::AreEqual(link.systemA, Game::SystemAt(galaxy.systems, Game::GateSite(a, b, desc)), L"a gate resolved to the wrong system");
      Assert::AreEqual(link.systemB, Game::SystemAt(galaxy.systems, Game::GateSite(b, a, desc)), L"a gate resolved to the wrong system");
    }
  }

  // The answer is a fact about the point, not about the order it was asked in, and a tie is
  // resolved rather than left to the machine.
  //
  // Two stars, a point exactly between them: the lower index wins, both ways round. That is what
  // makes the scenery stable for a camera parked on the midline instead of rebuilding every frame
  // on whichever way the last rounding fell.
  TEST_METHOD(ATieKeepsTheLowerSystem)
  {
    Game::SystemSite left;
    left.starPos = Game::LocalPos(-40000.0f, 0.0f);
    Game::SystemSite right;
    right.starPos = Game::LocalPos(40000.0f, 0.0f);

    const Game::SystemSite forwards[] = {left, right};
    const Game::SystemSite backwards[] = {right, left};
    const Game::UniversePos midpoint = Game::LocalPos(0.0f, 0.0f);

    Assert::AreEqual(0u, Game::SystemAt(forwards, midpoint), L"a tie did not keep the lower index");
    Assert::AreEqual(0u, Game::SystemAt(backwards, midpoint), L"a tie did not keep the lower index");

    // Either side of the midline, though, it answers by distance and not by index.
    Assert::AreEqual(0u, Game::SystemAt(forwards, Game::LocalPos(-1.0f, 0.0f)), L"a point nearer the left star did not pick it");
    Assert::AreEqual(1u, Game::SystemAt(forwards, Game::LocalPos(1.0f, 0.0f)), L"a point nearer the right star did not pick it");
  }

  // An empty galaxy answers 0 rather than reading past the end of a span. It cannot come out of
  // LayOutGalaxy -- ring 0 is always laid -- but a caller holding a default-constructed layout is
  // one boot-order change away, and the fail-closed answer costs a branch.
  TEST_METHOD(AnEmptyGalaxyAnswersZero)
  {
    Assert::AreEqual(0u, Game::SystemAt({}, Game::LocalPos(0.0f, 0.0f)), L"an empty galaxy did not answer zero");
  }
};
} // namespace GameLogicTests
