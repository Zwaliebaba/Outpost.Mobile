#include "pch.h"

#include <chrono>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// A seeded generator spelled out here rather than taken from <random>, because the standard
// library's engines are not required to produce the same sequence across implementations and this
// suite has to mean the same thing on every machine that runs it.
class IndexRandom
{
public:
  explicit IndexRandom(std::uint32_t _seed) noexcept
    : m_state(_seed | 1u)
  {
  }

  [[nodiscard]] std::uint32_t Next() noexcept
  {
    m_state ^= m_state << 13;
    m_state ^= m_state >> 17;
    m_state ^= m_state << 5;
    return m_state;
  }

  [[nodiscard]] float NextFloat(float _min, float _max) noexcept
  {
    return _min + (_max - _min) * (static_cast<float>(Next() >> 8) / static_cast<float>(1u << 24));
  }

private:
  std::uint32_t m_state;
};

// Every collidable hull in the table, so a configuration spans the full 72:1 size range rather
// than the range whoever wrote the test happened to think of.
std::vector<Game::SpatialIndex::Entry> RandomEntries(IndexRandom& _random, std::uint32_t _count, float _spreadMetres)
{
  std::vector<Game::SpatialIndex::Entry> entries;
  entries.reserve(_count);
  for (std::uint32_t i = 0; i < _count; ++i)
  {
    std::uint32_t hull = _random.Next() % Game::HULL_COUNT;
    while (!Game::HULL_SPECS[hull].collidable)
      hull = (hull + 1) % Game::HULL_COUNT;
    entries.push_back({i,
                       Game::LocalPos(_random.NextFloat(-_spreadMetres, _spreadMetres), _random.NextFloat(-_spreadMetres, _spreadMetres)),
                       Game::HULL_SPECS[hull].BoundingRadiusMetres()});
  }
  return entries;
}

std::vector<Game::ShipId> BruteForce(const std::vector<Game::SpatialIndex::Entry>& _entries, const Game::WorldPos& _centre,
                                     float _radiusMetres)
{
  std::vector<Game::ShipId> found;
  for (const Game::SpatialIndex::Entry& entry : _entries)
  {
    const float span = _radiusMetres + entry.boundingRadiusMetres;
    if (Game::DistanceSquared(_centre, entry.pos) <= span * span)
      found.push_back(entry.id);
  }
  return found;
}

std::wstring Describe(const std::vector<Game::ShipId>& _expected, const std::vector<Game::ShipId>& _actual)
{
  return std::format(L"the index returned {} entities where a linear scan found {}", _actual.size(), _expected.size());
}
} // namespace

TEST_CLASS(SpatialIndexTests)
{
public:
  TEST_METHOD(TheIndexAgreesWithALinearScan)
  {
    // The test that catches every cell-ring off-by-one. A missed contact does not assert or crash;
    // it reads as a tuning problem, which is why this is the gate rather than a play-test
    // (Design/Archive/Collision.md 16).
    IndexRandom random(12345u);
    for (int configuration = 0; configuration < 24; ++configuration)
    {
      const std::vector<Game::SpatialIndex::Entry> entries = RandomEntries(random, 240, 1500.0f);
      Game::SpatialIndex index;
      index.Configure({});
      index.RebuildDynamic(entries);

      std::vector<Game::ShipId> actual;
      for (int probe = 0; probe < 24; ++probe)
      {
        const Game::WorldPos centre = Game::LocalPos(random.NextFloat(-1600.0f, 1600.0f), random.NextFloat(-1600.0f, 1600.0f));
        const float radius = random.NextFloat(1.0f, 700.0f);
        index.QueryCircle(centre, radius, actual);

        std::vector<Game::ShipId> expected = BruteForce(entries, centre, radius);
        std::vector<Game::ShipId> sorted = actual;
        std::sort(sorted.begin(), sorted.end());
        std::sort(expected.begin(), expected.end());
        Assert::IsTrue(sorted == expected, Describe(expected, sorted).c_str());
      }
    }
  }

  TEST_METHOD(AQueryIsWiderThanOneCell)
  {
    // A hardcoded 3x3 neighbourhood passes every small-radius case and fails silently on the large
    // ones, which is exactly the shape of failure that survives a review. These radii are chosen to
    // span several cells at the configured size, on purpose.
    IndexRandom random(999u);
    const std::vector<Game::SpatialIndex::Entry> entries = RandomEntries(random, 400, 2500.0f);

    Game::SpatialIndex index;
    Game::SpatialIndex::Desc desc;
    desc.baseCellSizeMetres = 32.0f; // deliberately far smaller than the queries below
    index.Configure(desc);
    index.RebuildDynamic(entries);

    std::vector<Game::ShipId> actual;
    for (const float radius : {60.0f, 200.0f, 650.0f, 1400.0f})
    {
      const Game::WorldPos centre = Game::LocalPos(random.NextFloat(-500.0f, 500.0f), random.NextFloat(-500.0f, 500.0f));
      index.QueryCircle(centre, radius, actual);

      std::vector<Game::ShipId> expected = BruteForce(entries, centre, radius);
      std::vector<Game::ShipId> sorted = actual;
      std::sort(sorted.begin(), sorted.end());
      std::sort(expected.begin(), expected.end());
      Assert::IsTrue(sorted == expected, Describe(expected, sorted).c_str());
    }
  }

  TEST_METHOD(CellSizeCannotChangeTheAnswer)
  {
    // The claim that makes cell size a performance knob rather than a contract value. If this ever
    // fails, retuning the index per region stops being safe and the sort-then-truncate rule in the
    // sense pass has lost its point.
    IndexRandom random(4242u);
    const std::vector<Game::SpatialIndex::Entry> entries = RandomEntries(random, 300, 2000.0f);

    Game::SpatialIndex coarse;
    Game::SpatialIndex fine;
    Game::SpatialIndex::Desc coarseDesc;
    coarseDesc.baseCellSizeMetres = 512.0f;
    Game::SpatialIndex::Desc fineDesc;
    fineDesc.baseCellSizeMetres = 40.0f;
    coarse.Configure(coarseDesc);
    fine.Configure(fineDesc);
    coarse.RebuildDynamic(entries);
    fine.RebuildDynamic(entries);

    std::vector<Game::ShipId> fromCoarse;
    std::vector<Game::ShipId> fromFine;
    for (int probe = 0; probe < 40; ++probe)
    {
      const Game::WorldPos centre = Game::LocalPos(random.NextFloat(-2200.0f, 2200.0f), random.NextFloat(-2200.0f, 2200.0f));
      const float radius = random.NextFloat(5.0f, 900.0f);
      coarse.QueryCircle(centre, radius, fromCoarse);
      fine.QueryCircle(centre, radius, fromFine);
      std::sort(fromCoarse.begin(), fromCoarse.end());
      std::sort(fromFine.begin(), fromFine.end());
      Assert::IsTrue(fromCoarse == fromFine, L"two cell sizes returned different sets");
    }
  }

  TEST_METHOD(TheStaticStoreIsQueriedAlongsideTheDynamicOne)
  {
    // The static/dynamic split is the larger of the two wins and it is invisible from the query
    // side, which is the property worth pinning: a caller must never have to know which store an
    // entity is in.
    const float structureRadius = Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres();
    // 260 m out with a 251.77 m bounding radius, so its skin reaches 8.23 m from the origin: inside
    // a 30 m query and outside a 5 m one. A query that only tested centres would fail both ways.
    const std::vector<Game::SpatialIndex::Entry> statics = {{100, Game::LocalPos(260.0f, 0.0f), structureRadius}};
    const std::vector<Game::SpatialIndex::Entry> dynamics = {{7, Game::LocalPos(20.0f, 0.0f), 3.51f}};

    Game::SpatialIndex index;
    index.Configure({});
    index.RebuildStatic(statics);
    index.RebuildDynamic(dynamics);

    std::vector<Game::ShipId> found;
    index.QueryCircle(Game::LocalPos(0.0f, 0.0f), 30.0f, found);
    Assert::AreEqual(size_t{2}, found.size(), L"a 30 m query missed either the fighter beside it or the Structure whose skin reaches it");

    index.QueryCircle(Game::LocalPos(0.0f, 0.0f), 5.0f, found);
    Assert::AreEqual(size_t{0}, found.size(), L"a query found something it does not reach");

    // A rebuild of one store must not disturb the other -- the whole reason they are separate is
    // that the static one is not touched sixty times a second.
    index.RebuildDynamic({});
    index.QueryCircle(Game::LocalPos(260.0f, 0.0f), 1.0f, found);
    Assert::AreEqual(size_t{1}, found.size(), L"rebuilding the dynamic store dropped the static one");
  }

  TEST_METHOD(LevelSelectionPutsAHullWhereItFits)
  {
    // Configured with one level, everything is level 0. That is what phase 2 ships, and the
    // function exists now so that adding the second level later is a Desc change rather than a
    // structural one.
    Game::SpatialIndex single;
    single.Configure({});
    for (const Game::HullSpec& spec : Game::HULL_SPECS)
      Assert::AreEqual(0u, single.LevelForRadius(spec.BoundingRadiusMetres()), L"one configured level should hold every hull");

    Game::SpatialIndex tiered;
    Game::SpatialIndex::Desc desc;
    desc.baseCellSizeMetres = 64.0f;
    desc.dynamicLevelCount = 3; // 64 m, 128 m, 256 m
    tiered.Configure(desc);
    Assert::AreEqual(64.0f, tiered.CellSizeMetres(0), 1e-4f, L"level 0 is not the base cell size");
    Assert::AreEqual(256.0f, tiered.CellSizeMetres(2), 1e-4f, L"levels are not powers of two from the base");

    // A hull belongs in the first level whose cell is at least twice its bounding radius, and
    // anything larger than the coarsest level saturates rather than indexing past the end.
    Assert::AreEqual(0u, tiered.LevelForRadius(Game::HullSpecOf(Game::HullId::Interceptor).BoundingRadiusMetres()),
                     L"an Interceptor is not fine");
    Assert::AreEqual(0u, tiered.LevelForRadius(Game::HullSpecOf(Game::HullId::Hauler).BoundingRadiusMetres()), L"a Hauler is not fine");
    Assert::AreEqual(1u, tiered.LevelForRadius(Game::HullSpecOf(Game::HullId::Battleship).BoundingRadiusMetres()),
                     L"a Battleship is misplaced");
    Assert::AreEqual(2u, tiered.LevelForRadius(Game::HullSpecOf(Game::HullId::Carrier).BoundingRadiusMetres()), L"a Carrier is misplaced");
    Assert::AreEqual(2u, tiered.LevelForRadius(Game::HullSpecOf(Game::HullId::Structure).BoundingRadiusMetres()),
                     L"an oversized hull did not saturate");

    // And the tiered index must still answer the same question as the flat one.
    IndexRandom random(31337u);
    const std::vector<Game::SpatialIndex::Entry> entries = RandomEntries(random, 300, 1200.0f);
    tiered.RebuildDynamic(entries);
    std::vector<Game::ShipId> actual;
    for (int probe = 0; probe < 24; ++probe)
    {
      const Game::WorldPos centre = Game::LocalPos(random.NextFloat(-1300.0f, 1300.0f), random.NextFloat(-1300.0f, 1300.0f));
      const float radius = random.NextFloat(1.0f, 700.0f);
      tiered.QueryCircle(centre, radius, actual);
      std::vector<Game::ShipId> expected = BruteForce(entries, centre, radius);
      std::vector<Game::ShipId> sorted = actual;
      std::sort(sorted.begin(), sorted.end());
      std::sort(expected.begin(), expected.end());
      Assert::IsTrue(sorted == expected, Describe(expected, sorted).c_str());
    }
  }

  TEST_METHOD(TheSameInputProducesTheSameOrder)
  {
    // Not merely the same set: the same sequence. The sense pass sorts and truncates, so a query
    // whose order depended on an allocator would put a different neighbour over the cap on a
    // machine that allocated differently -- and that is a replay failure that reproduces nowhere.
    IndexRandom random(777u);
    const std::vector<Game::SpatialIndex::Entry> entries = RandomEntries(random, 200, 900.0f);

    std::vector<Game::ShipId> first;
    std::vector<Game::ShipId> second;
    {
      Game::SpatialIndex index;
      index.Configure({});
      index.RebuildDynamic(entries);
      index.QueryCircle(Game::LocalPos(0.0f, 0.0f), 600.0f, first);
    }
    {
      Game::SpatialIndex index;
      index.Configure({});
      index.RebuildDynamic(entries);
      index.QueryCircle(Game::LocalPos(0.0f, 0.0f), 600.0f, second);
    }
    Assert::IsTrue(first == second, L"two identically built indexes returned the same set in different orders");
  }

  TEST_METHOD(IndexCostIsRecorded)
  {
    // Gates nothing. It is what the decision about a second dynamic level is made from, and what
    // makes a later regression visible as a number rather than as a feeling (Collision.md 15).
    for (const std::uint32_t count : {100u, 1000u, 5000u})
    {
      for (const float cellSize : {64.0f, 128.0f, 256.0f, 512.0f})
      {
        IndexRandom random(2024u);
        // Density held roughly constant across N, so the numbers compare: a fixed box would make
        // the 5,000-ship row a measurement of crowding rather than of scale.
        const float spread = 400.0f * std::sqrt(static_cast<float>(count) / 100.0f);
        const std::vector<Game::SpatialIndex::Entry> entries = RandomEntries(random, count, spread);

        Game::SpatialIndex index;
        Game::SpatialIndex::Desc desc;
        desc.baseCellSizeMetres = cellSize;
        index.Configure(desc);

        constexpr int REPEATS = 20;
        const auto rebuildStart = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < REPEATS; ++repeat)
          index.RebuildDynamic(entries);
        const auto rebuildEnd = std::chrono::steady_clock::now();

        std::vector<Game::ShipId> found;
        std::size_t returned = 0;
        const auto queryStart = std::chrono::steady_clock::now();
        for (const Game::SpatialIndex::Entry& entry : entries)
        {
          index.QueryCircle(entry.pos, 250.0f, found);
          returned += found.size();
        }
        const auto queryEnd = std::chrono::steady_clock::now();

        const double rebuildMs = std::chrono::duration<double, std::milli>(rebuildEnd - rebuildStart).count() / REPEATS;
        const double sweepMs = std::chrono::duration<double, std::milli>(queryEnd - queryStart).count();
        Logger::WriteMessage(std::format(L"N={:>5}  cell={:>4.0f}m   rebuild {:>7.3f} ms   250 m sweep {:>8.3f} ms   "
                                         L"{:.1f} hits/query\n",
                                         count, cellSize, rebuildMs, sweepMs, static_cast<double>(returned) / count)
                               .c_str());
      }
    }
  }

  TEST_METHOD(GatheredCandidatesTrackLocalDensity)
  {
    // The number slice 11 is about. The index's query is over-inclusive on purpose, so what a tick
    // pays for is not what the query returns but what survives the pair filter -- and that number
    // should follow how crowded a ship's own neighbourhood is, not how big the fleet is or what
    // hulls exist in the table (Design/Archive/MmoScalabilityReview.md U2).
    //
    // Density is what varies here; the fleet size does not. A fixed count in a shrinking box is the
    // only way to read the two apart.
    constexpr std::uint32_t SHIPS = 400;
    double previousPerShip = 0.0;
    for (const float spread : {4000.0f, 2000.0f, 1000.0f})
    {
      IndexRandom random(7u);
      Game::World world;
      for (std::uint32_t at = 0; at < SHIPS; ++at)
      {
        const float x = random.NextFloat(-spread, spread);
        const float z = random.NextFloat(-spread, spread);
        (void)world.SpawnShip(Game::LocalPos(x, z), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
      }
      world.Step();

      const double queried = static_cast<double>(world.QueriedCandidateCount()) / SHIPS;
      const double gathered = static_cast<double>(world.GatheredCandidateCount()) / SHIPS;
      Logger::WriteMessage(std::format(L"spread {:>5.0f} m   queried {:>7.1f}/ship   gathered {:>7.1f}/ship   "
                                       L"query radius {:.0f} m\n",
                                       spread, queried, gathered,
                                       Game::QueryRadiusMetres(Game::HullSpecOf(Game::HullId::Interceptor), world.Extent()))
                             .c_str());

      Assert::IsTrue(gathered <= queried, L"the pair filter kept more candidates than the index returned");
      Assert::IsTrue(gathered > previousPerShip, L"packing the same fleet into a quarter of the area gathered no more per ship");
      previousPerShip = gathered;
    }

    // And the radius the whole thing rests on: a world of nothing but Interceptors must not be
    // asking for the Carrier's circle.
    Game::World fighters;
    (void)fighters.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
    fighters.Step();
    const Game::HullSpec& interceptor = Game::HullSpecOf(Game::HullId::Interceptor);
    Assert::IsTrue(Game::QueryRadiusMetres(interceptor, fighters.Extent()) < Game::QueryRadiusMetres(interceptor),
                   L"a world of fighters still asked for the whole hull table's query radius");
  }
};
} // namespace GameLogicTests
