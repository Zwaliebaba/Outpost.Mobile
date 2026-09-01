#include "pch.h"

#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// The shipped bounds are SystemDesc's own defaults (Design/Archive/Stations-slice-1.md 2.3), so a test that
// wants them constructs one and asks. Spelling them here instead would prove something about a
// system nobody plays the first time the game retuned an orbit.
constexpr std::uint64_t LAYOUT_SEED = 0x53746174696F6Eull; // "Station"

[[nodiscard]] bool SameSite(const Game::PlanetSite& _a, const Game::PlanetSite& _b) noexcept
{
  return _a.posUniverse.sectorX == _b.posUniverse.sectorX && _a.posUniverse.sectorZ == _b.posUniverse.sectorZ &&
         _a.posUniverse.localX == _b.posUniverse.localX && _a.posUniverse.localZ == _b.posUniverse.localZ &&
         _a.radiusMetres == _b.radiusMetres && _a.bearingRad == _b.bearingRad && _a.bodySeed == _b.bodySeed;
}
} // namespace

TEST_CLASS(UniverseLayoutTests)
{
public:
  // A seed means one system forever, which is the whole reason the layout may be content rather
  // than wire data: both halves call this and neither tells the other what came out.
  TEST_METHOD(TheLayoutIsAFunctionOfItsSeed)
  {
    const Game::SystemDesc desc;
    const Game::UniversePos star = Game::LocalPos(0.0f, 0.0f);

    const Game::SystemLayout first = Game::LayOutSystem(LAYOUT_SEED, star, desc);
    const Game::SystemLayout second = Game::LayOutSystem(LAYOUT_SEED, star, desc);

    Assert::AreEqual(static_cast<std::size_t>(desc.planetCount), first.planets.size(), L"the layout did not produce the planets asked for");
    Assert::AreEqual(first.planets.size(), second.planets.size(), L"two calls with one seed produced different planet counts");
    for (std::size_t at = 0; at < first.planets.size(); ++at)
      Assert::IsTrue(SameSite(first.planets[at], second.planets[at]), L"two calls with one seed produced different planets");

    // Adjacent seeds, because adjacent is the case a weak seeding procedure fails: PCG's seeding
    // decorrelates them and a lesser one would leave the first draw near-identical.
    const Game::SystemLayout neighbour = Game::LayOutSystem(LAYOUT_SEED + 1u, star, desc);
    bool anyDiffers = false;
    for (std::size_t at = 0; at < first.planets.size(); ++at)
      anyDiffers = anyDiffers || !SameSite(first.planets[at], neighbour.planets[at]);
    Assert::IsTrue(anyDiffers, L"two adjacent seeds produced the same system");
  }

  // The pin overwrites planet 0 and does not skip its draws. If it skipped them, every planet after
  // it would take another planet's stream and one seed would mean two systems depending on a bool.
  TEST_METHOD(PinningTheFirstPlanetLeavesTheRestAlone)
  {
    const Game::UniversePos star = Game::LocalPos(0.0f, 0.0f);
    const Game::SystemDesc loose;

    Game::SystemDesc pinned;
    pinned.pinFirstPlanet = true;
    pinned.firstPlanetBearingRad = -0.4014257f; // -23 degrees, the framed opening shot
    pinned.firstPlanetOrbitMetres = 3500.0f;

    const Game::SystemLayout free = Game::LayOutSystem(LAYOUT_SEED, star, loose);
    const Game::SystemLayout held = Game::LayOutSystem(LAYOUT_SEED, star, pinned);

    Assert::AreEqual(free.planets.size(), held.planets.size(), L"pinning changed how many planets there are");
    for (std::size_t at = 1; at < free.planets.size(); ++at)
      Assert::IsTrue(SameSite(free.planets[at], held.planets[at]), L"pinning planet 0 moved a planet that was not pinned");

    Assert::AreEqual(pinned.firstPlanetBearingRad, held.planets[0].bearingRad, 1e-6f, L"the pinned planet did not take its bearing");
    Assert::AreEqual(pinned.firstPlanetOrbitMetres, Game::Distance(star, held.planets[0].posUniverse), 0.5f,
                     L"the pinned planet did not take its orbit");
    Assert::AreEqual(free.planets[0].radiusMetres, held.planets[0].radiusMetres, 0.0f,
                     L"pinning consumed a draw: the pinned planet's radius came from a different one");
    Assert::AreEqual(free.planets[0].bodySeed, held.planets[0].bodySeed, L"pinning consumed a draw: the pinned planet's look changed");
  }

  // The path grid declines to build past PATH_GRID_MAX_CELLS_PER_AXIS cells, and it declines
  // quietly -- the symptom is ships that stop routing, a long way from this file. So the shipped
  // bounds are held under the ceiling by a test rather than by an arithmetic somebody once did.
  TEST_METHOD(TheLayoutRespectsTheGridCeiling)
  {
    const Game::SystemDesc desc;

    // Worst case: two stations diametrically opposed at the outermost orbit, and the grid's own
    // margin at each end.
    const float spanMetres = 2.0f * desc.maxOrbitMetres + 2.0f * Game::PATH_GRID_MARGIN_METRES;
    const int cells = static_cast<int>(std::ceil(spanMetres / Game::PATH_CELL_SIZE_METRES));

    Assert::IsTrue(cells < Game::PATH_GRID_MAX_CELLS_PER_AXIS,
                   L"the shipped system is wider than one path grid may be: the grid would decline and ships would stop routing");
  }

  // Half a slot between adjacent bearings is what the jitter bound buys, and this is the separation
  // that follows from it. It is a proof over the bounds, not a sample: the assertion holds for every
  // seed tried because it holds for every seed there is.
  TEST_METHOD(PlanetsKeepTheirDistance)
  {
    const Game::SystemDesc desc;
    const Game::UniversePos star = Game::LocalPos(0.0f, 0.0f);

    // Chord of the guaranteed half-slot at the innermost orbit. At the shipped numbers -- three
    // planets, 2 500 m -- that is 2 500 m, which is Design/Archive/Stations.md 5.3's "closest two stations".
    const float minSeparationMetres = 2.0f * desc.minOrbitMetres * std::sin(DirectX::XM_PI / (2.0f * static_cast<float>(desc.planetCount)));

    for (std::uint64_t seed = 0; seed < 256; ++seed)
    {
      const Game::SystemLayout layout = Game::LayOutSystem(LAYOUT_SEED + seed, star, desc);
      for (std::size_t a = 0; a < layout.planets.size(); ++a)
      {
        const float orbit = Game::Distance(star, layout.planets[a].posUniverse);
        Assert::IsTrue(orbit >= desc.minOrbitMetres - 0.5f && orbit <= desc.maxOrbitMetres + 0.5f,
                       L"a planet was laid outside its orbit band");
        Assert::IsTrue(layout.planets[a].radiusMetres >= desc.minRadiusMetres && layout.planets[a].radiusMetres <= desc.maxRadiusMetres,
                       L"a planet was given a radius outside its band");

        for (std::size_t b = a + 1; b < layout.planets.size(); ++b)
        {
          const float apart = Game::Distance(layout.planets[a].posUniverse, layout.planets[b].posUniverse);
          Assert::IsTrue(apart >= minSeparationMetres - 0.5f, L"two planets came closer than the bearing slots allow");
        }
      }
    }
  }

  // Every position the simulation stores satisfies UniversePos's invariant, and a system anchored one
  // metre inside a sector's far corner is where a layout that wrote localX directly would break it.
  TEST_METHOD(ALayoutHoldsTheSectorInvariant)
  {
    const Game::SystemDesc desc;
    const Game::UniversePos star = Game::LocalPos(Game::SECTOR_SIZE_METRES - 1.0f, Game::SECTOR_SIZE_METRES - 1.0f);
    const Game::SystemLayout layout = Game::LayOutSystem(LAYOUT_SEED, star, desc);

    for (const Game::PlanetSite& site : layout.planets)
    {
      Assert::IsTrue(site.posUniverse.localX >= 0.0f && site.posUniverse.localX < Game::SECTOR_SIZE_METRES,
                     L"a planet's local X is outside its sector");
      Assert::IsTrue(site.posUniverse.localZ >= 0.0f && site.posUniverse.localZ < Game::SECTOR_SIZE_METRES,
                     L"a planet's local Z is outside its sector");
    }

    // The star crossed a boundary to get here; the planets around it must still be the same
    // distance from it as they are anywhere else.
    const Game::SystemLayout atOrigin = Game::LayOutSystem(LAYOUT_SEED, Game::LocalPos(0.0f, 0.0f), desc);
    for (std::size_t at = 0; at < layout.planets.size(); ++at)
    {
      const float here = Game::Distance(star, layout.planets[at].posUniverse);
      const float there = Game::Distance(Game::LocalPos(0.0f, 0.0f), atOrigin.planets[at].posUniverse);
      Assert::AreEqual(there, here, 0.5f, L"a system laid out near a sector boundary is a different shape");
    }
  }

  // An empty system is well formed. Nothing ships one, and the loop below is what a caller that
  // asked for one would otherwise walk off the end of.
  TEST_METHOD(ASystemMayHaveNoPlanets)
  {
    Game::SystemDesc desc;
    desc.planetCount = 0;

    const Game::UniversePos star = Game::LocalPos(120.0f, -340.0f);
    const Game::SystemLayout layout = Game::LayOutSystem(LAYOUT_SEED, star, desc);

    Assert::IsTrue(layout.planets.empty(), L"a system asked for no planets produced some");
    Assert::IsTrue(IsSamePosition(star, layout.starPos), L"the layout moved the star it was anchored on");
  }
};
} // namespace GameLogicTests
