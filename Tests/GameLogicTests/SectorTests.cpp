#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
// The sector pair behind WorldPos: the carry, the invariant it maintains, and the two consumers
// that index on a position rather than merely measuring between two (Design/Collision-slice-8.md).
//
// Everything the game ships sits in sector zero, so these are the only tests that will ever build a
// position outside it. That is deliberate: they are what stands between "day-one content never
// notices" being an argument and it being a fact.
TEST_CLASS(SectorTests)
{
public:
  TEST_METHOD(CrossingTheFarEdgeCarriesIntoTheNextSector)
  {
    Game::WorldPos pos = Game::LocalPos(0.0f, 0.0f);
    Game::Translate(pos, Game::SECTOR_SIZE_METRES + 5.0f, 0.0f);

    Assert::AreEqual(static_cast<std::int64_t>(1), pos.sectorX, L"crossing the far edge did not advance the sector");
    Assert::AreEqual(5.0f, pos.localX, 0.0f, L"the local offset is not what was left over, exactly");
  }

  TEST_METHOD(CrossingBelowZeroCarriesTheOtherWay)
  {
    // The case a truncating cast gets wrong: floor(-5 / 8192) is -1, not 0, so a step west from the
    // sector's own origin belongs to the sector before it and not to this one at a negative offset.
    Game::WorldPos pos = Game::LocalPos(0.0f, 0.0f);
    Game::Translate(pos, -5.0f, -5.0f);

    Assert::AreEqual(static_cast<std::int64_t>(-1), pos.sectorX, L"stepping west of the origin did not go back a sector");
    Assert::AreEqual(static_cast<std::int64_t>(-1), pos.sectorZ, L"stepping south of the origin did not go back a sector");
    Assert::AreEqual(Game::SECTOR_SIZE_METRES - 5.0f, pos.localX, 0.0f, L"the local offset did not wrap to the far edge");
    Assert::AreEqual(Game::SECTOR_SIZE_METRES - 5.0f, pos.localZ, 0.0f, L"the local offset did not wrap to the far edge");
  }

  TEST_METHOD(TheLocalOffsetAlwaysStaysInsideItsSector)
  {
    // The invariant every other test here leans on, checked over displacements that cross one
    // boundary, many boundaries, and several in a single call.
    Game::WorldPos pos = Game::LocalPos(0.0f, 0.0f);
    for (const float step : {1.0f, -2.5f, Game::SECTOR_SIZE_METRES, -Game::SECTOR_SIZE_METRES * 3.0f, 17.25f, -0.125f})
    {
      Game::Translate(pos, step, -step);
      Assert::IsTrue(pos.localX >= 0.0f && pos.localX < Game::SECTOR_SIZE_METRES, L"localX left its sector");
      Assert::IsTrue(pos.localZ >= 0.0f && pos.localZ < Game::SECTOR_SIZE_METRES, L"localZ left its sector");
    }
  }

  TEST_METHOD(TheCarryIsExactAndReversible)
  {
    // A power-of-two sector size makes the division an exponent adjustment, so the carry adds and
    // removes exactly the same quantity. Were it 10000 this would come back a few ulps off, and a
    // recorded game would diverge from its replay for no reason anybody could see.
    const Game::WorldPos start = Game::LocalPos(123.5f, -4321.25f);
    Game::WorldPos pos = start;
    // Not named "far": the Windows SDK still defines that as a 16-bit memory-model keyword, so a
    // variable called it compiles here and nowhere that includes minwindef.h.
    const float farMetres = Game::SECTOR_SIZE_METRES * 5.0f + 137.75f;

    Game::Translate(pos, farMetres, -farMetres);
    Game::Translate(pos, -farMetres, farMetres);

    Assert::IsTrue(IsSamePosition(start, pos), L"translating out and back did not land on the same position");
  }

  TEST_METHOD(DistanceDoesNotCareWhereTheBoundaryIs)
  {
    // Ten metres is ten metres whether the pair sits mid-sector or straddles a border.
    Game::WorldPos insideA = Game::LocalPos(4000.0f, 0.0f);
    Game::WorldPos insideB = insideA;
    Game::Translate(insideB, 10.0f, 0.0f);

    Game::WorldPos straddleA = Game::LocalPos(Game::SECTOR_SIZE_METRES - 5.0f, 0.0f);
    Game::WorldPos straddleB = straddleA;
    Game::Translate(straddleB, 10.0f, 0.0f);

    Assert::AreEqual(static_cast<std::int64_t>(1), straddleB.sectorX, L"the second position did not end up over the border");
    Assert::AreEqual(Game::Distance(insideA, insideB), Game::Distance(straddleA, straddleB), 0.0f,
                     L"the same ten metres measured differently across a sector boundary");
    Assert::AreEqual(10.0f, Game::Distance(straddleA, straddleB), 1e-3f, L"a cross-boundary distance is not the distance");
  }

  TEST_METHOD(LerpLandsOnTheBoundaryBetweenAdjacentSectors)
  {
    Game::WorldPos before = Game::LocalPos(Game::SECTOR_SIZE_METRES - 10.0f, 0.0f);
    Game::WorldPos after = before;
    Game::Translate(after, 20.0f, 0.0f);

    const Game::WorldPos middle = Game::Lerp(before, after, 0.5f);

    Assert::AreEqual(0.0f, Game::Distance(before, middle) - 10.0f, 1e-3f, L"the midpoint is not halfway from the start");
    Assert::AreEqual(0.0f, Game::Distance(middle, after) - 10.0f, 1e-3f, L"the midpoint is not halfway to the end");
  }

  TEST_METHOD(TheIndexFindsANeighbourOverTheBorder)
  {
    // The reason CellOfAxis counts cells from the universe origin rather than from inside a sector:
    // two entries five metres apart either side of a border have to land in adjacent cells, and a
    // per-sector cell index would put them in the same one and in the wrong ring.
    Game::SpatialIndex index;
    index.Configure(Game::SpatialIndex::Desc{});

    Game::WorldPos west = Game::LocalPos(Game::SECTOR_SIZE_METRES - 5.0f, 0.0f);
    Game::WorldPos east = west;
    Game::Translate(east, 10.0f, 0.0f);
    Assert::AreEqual(static_cast<std::int64_t>(1), east.sectorX, L"the second entry did not end up in the next sector");

    const Game::SpatialIndex::Entry entries[] = {{0, west, 10.0f}, {1, east, 10.0f}};
    index.RebuildDynamic(entries);

    std::vector<Game::ShipId> found;
    index.QueryCircle(west, 50.0f, found);

    Assert::AreEqual(static_cast<std::size_t>(2), found.size(), L"a query beside a sector border did not see across it");
  }

  TEST_METHOD(APathIsFoundAcrossASectorBoundary)
  {
    // The grid's own extent is swept as offsets from the first obstacle, so a set of obstacles
    // straddling a border gets a bounding box the width of the obstacles and not of a sector.
    Game::WorldPos obstacle = Game::LocalPos(Game::SECTOR_SIZE_METRES, 0.0f);

    Game::PathGrid grid;
    const Game::PathGrid::Obstacle obstacles[] = {{obstacle, 120.0f}};
    grid.Rebuild(obstacles);
    Assert::IsTrue(grid.HasObstacles(), L"the grid declined to build across a sector boundary");

    Game::WorldPos from = obstacle;
    Game::Translate(from, -600.0f, 0.0f);
    Game::WorldPos to = obstacle;
    Game::Translate(to, 600.0f, 0.0f);
    Assert::AreNotEqual(from.sectorX, to.sectorX, L"the two endpoints did not end up in different sectors");

    std::vector<Game::WorldPos> route;
    Assert::IsTrue(grid.FindPath(from, to, 20.0f, route), L"no route was found across a sector boundary");
    Assert::IsTrue(route.size() >= 2, L"a route across a boundary came back with nothing in it");

    // Contiguous: no waypoint jumps a sector's width, which is what a lost carry would look like.
    Game::WorldPos at = from;
    for (const Game::WorldPos& waypoint : route)
    {
      Assert::IsTrue(Game::Distance(at, waypoint) < Game::SECTOR_SIZE_METRES * 0.5f, L"a route waypoint jumped most of a sector");
      at = waypoint;
    }
    Assert::AreEqual(0.0f, Game::Distance(route.back(), to), 1e-3f, L"the route does not end at the destination");
  }

  TEST_METHOD(AWorldStepsTheSameWhereverItSits)
  {
    // The whole behaviour-neutrality claim in one test: the same encounter, once at the origin and
    // once several sectors away, has to produce the same relative motion tick for tick. Measured as
    // offsets from each run's own start, because that is the only thing the two runs share.
    const auto run = [](const Game::WorldPos& _origin)
    {
      Game::World world;
      Game::WorldPos left = _origin;
      Game::Translate(left, -200.0f, 0.0f);
      Game::WorldPos right = _origin;
      Game::Translate(right, 200.0f, 0.0f);

      const Game::ShipId a = world.SpawnShip(left, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
      const Game::ShipId b = world.SpawnShip(right, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));
      const Game::ShipId both[] = {a, b};

      Game::WorldPos target = _origin;
      Game::Translate(target, 0.0f, 900.0f);
      world.IssueMoveOrder(both, target, false, 0.0f);

      for (int tick = 0; tick < 600; ++tick)
        world.Step();

      std::vector<float> offsets;
      for (const Game::ShipState& ship : world.Ships())
      {
        offsets.push_back(Game::OffsetX(_origin, ship.posWorld));
        offsets.push_back(Game::OffsetZ(_origin, ship.posWorld));
      }
      return offsets;
    };

    Game::WorldPos distant = Game::LocalPos(0.0f, 0.0f);
    Game::Translate(distant, Game::SECTOR_SIZE_METRES * 4.0f, Game::SECTOR_SIZE_METRES * -7.0f);

    const std::vector<float> atOrigin = run(Game::LocalPos(0.0f, 0.0f));
    const std::vector<float> farAway = run(distant);

    Assert::AreEqual(atOrigin.size(), farAway.size(), L"the two runs ended with different numbers of ships");
    for (std::size_t i = 0; i < atOrigin.size(); ++i)
      Assert::AreEqual(atOrigin[i], farAway[i], 0.0f, L"the same encounter played out differently four sectors from the origin");
  }
};
} // namespace GameLogicTests
