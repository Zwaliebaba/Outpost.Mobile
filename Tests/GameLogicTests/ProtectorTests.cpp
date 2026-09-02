#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// The Vanguard's shipped garrison (Design/Archive/Stations.md 8.2): three Corvettes, one every 1.5 s.
constexpr std::uint32_t COMPLEMENT = 3;
constexpr std::uint32_t CADENCE_TICKS = 90;
constexpr std::uint32_t TARGET_CAP = 4;

[[nodiscard]] std::uint32_t Faction(Game::FactionId _faction)
{
  return _faction;
}

Game::Universe::StationId MakeStation(Game::Universe& _universe, float _x, float _z, std::uint32_t _complement = COMPLEMENT)
{
  const Game::ShipId structure =
    _universe.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
  Game::Universe::StationDesc desc;
  desc.ownerFaction = Game::FACTION_VANGUARD;
  desc.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
  desc.protectorComplement = _complement;
  desc.launchEveryTicks = CADENCE_TICKS;
  desc.targetCap = TARGET_CAP;
  return _universe.MakeStation(structure, desc);
}

// The nearest live protector to a position, or a very large number if none is out.
[[nodiscard]] float NearestProtector(const Game::Universe& _universe, const Game::UniversePos& _to)
{
  float nearest = 1.0e30f;
  for (Game::ShipId id = 0; id < _universe.ShipCount(); ++id)
  {
    if (_universe.ProtectorOf(id).active)
      nearest = std::min(nearest, Game::Distance(_universe.Ship(id).posUniverse, _to));
  }
  return nearest;
}

[[nodiscard]] Game::ShipId FirstProtector(const Game::Universe& _universe)
{
  for (Game::ShipId id = 0; id < _universe.ShipCount(); ++id)
  {
    if (_universe.ProtectorOf(id).active)
      return id;
  }
  return Game::INVALID_SHIP_ID;
}
} // namespace

TEST_CLASS(ProtectorTests)
{
public:
  TEST_METHOD(TheStationScramblesItsComplement)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStation(universe, 0.0f, 0.0f);
    const Game::ShipId raider = universe.SpawnShip(Game::LocalPos(1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    // Nothing before the act. The response starts from a *stated* aggression, never from a radius:
    // a ship parked beside a station it has not attacked is unmolested (Design/Archive/Stations.md 8.4).
    for (int tick = 0; tick < 400; ++tick)
      universe.Step();
    Assert::AreEqual(static_cast<std::uint32_t>(0), universe.LaunchedProtectorCount(station),
                     L"a station launched at somebody who had done nothing");

    universe.RecordAggression(universe.HandleOf(raider), station);
    universe.Step();
    Assert::AreEqual(static_cast<std::uint32_t>(1), universe.LaunchedProtectorCount(station),
                     L"the first protector did not launch on the tick after the aggression");

    // On the metronome, and capped. Stepped well past what three launches need.
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 6; ++tick)
      universe.Step();
    Assert::AreEqual(COMPLEMENT, universe.LaunchedProtectorCount(station), L"the garrison did not reach, or overran, its complement");

    // In the owner's faction and on the owner's hull, so IFF and silhouette both read right.
    std::uint32_t seen = 0;
    for (Game::ShipId id = 0; id < universe.ShipCount(); ++id)
    {
      if (!universe.ProtectorOf(id).active)
        continue;
      ++seen;
      Assert::AreEqual(Faction(Game::FACTION_VANGUARD), Faction(universe.Ship(id).factionId), L"a protector flew somebody else's flag");
      Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), universe.Ship(id).hullId, L"a protector flew the wrong hull");
      Assert::IsTrue(universe.ProtectorOf(id).home == station, L"a protector belongs to the wrong station");
    }
    Assert::AreEqual(COMPLEMENT, seen, L"the duty table and the launched count disagree");
  }

  // Complement 0 is the Vandal base: its patrol is not a garrison and it launches nothing, ever.
  TEST_METHOD(AStationWithNoGarrisonLaunchesNothing)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStation(universe, 0.0f, 0.0f, 0);
    const Game::ShipId raider = universe.SpawnShip(Game::LocalPos(900.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    universe.RecordAggression(universe.HandleOf(raider), station);
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 10; ++tick)
      universe.Step();

    Assert::AreEqual(static_cast<std::uint32_t>(0), universe.LaunchedProtectorCount(station),
                     L"a station with no complement launched something");
    Assert::AreEqual(static_cast<std::uint32_t>(2), universe.ShipCount(), L"a ship appeared from somewhere");
  }

  // The chase, and the threshold that keeps it a sequence of ordinary orders rather than a replan
  // every tick.
  TEST_METHOD(AProtectorPursuesItsTarget)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStation(universe, 0.0f, 0.0f, 1);
    const Game::ShipId raider =
      universe.SpawnShip(Game::LocalPos(1200.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
    const Game::ShipHandle raiderHandle = universe.HandleOf(raider);

    universe.RecordAggression(raiderHandle, station);
    universe.Step();
    const Game::ShipId hunter = FirstProtector(universe);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, hunter, L"nothing launched");

    // The raider runs. A fleeing target is the case the replan threshold exists for.
    (void)universe.IssueMoveOrder(std::array{raider}, Game::LocalPos(1200.0f, 4000.0f), false, 0.0f, Game::FACTION_PLAYER);

    const float startDistance = Game::Distance(universe.Ship(hunter).posUniverse, universe.Ship(raider).posUniverse);
    std::uint32_t reaims = 0;
    Game::UniversePos aim = universe.RouteOf(hunter).empty() ? universe.Ship(hunter).posUniverse : universe.RouteOf(hunter).back();

    constexpr int WINDOW = 3000;
    for (int tick = 0; tick < WINDOW; ++tick)
    {
      universe.Step();
      const Game::ShipId live = universe.Resolve(raiderHandle);
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, live, L"the raider vanished");

      const std::span<const Game::UniversePos> route = universe.RouteOf(hunter);
      if (route.empty())
        continue;
      if (!IsSamePosition(route.back(), aim))
      {
        ++reaims;
        aim = route.back();
        continue;
      }

      // Between re-aims the target must be inside the threshold of the point last aimed at. That is
      // the invariant the constant states, checked on every tick rather than inferred from a count.
      //
      // Asked of Universe::PursuitAimedAt rather than of the route's destination, which is where this
      // row used to read it. The two were the same point until a pursuit gained a stand-off and the
      // destination moved up to 224 m short of the target (Design/Archive/Combat.md 8, ADR 0052); measured
      // against the destination now, a Corvette's own 144 m stand-off reads as permanent drift.
      Assert::IsTrue(Game::Distance(universe.PursuitAimedAt(hunter), universe.Ship(live).posUniverse) <= Game::PURSUIT_REPLAN_METRES + 1.0f,
                     L"the hunter let its target drift past the replan threshold without re-aiming");
    }

    Assert::IsTrue(reaims > 0, L"the hunter never re-aimed at a target that ran");
    Assert::IsTrue(reaims < WINDOW / 10, L"the hunter re-planned far too often -- the threshold is not holding");

    const float endDistance = Game::Distance(universe.Ship(hunter).posUniverse, universe.Ship(universe.Resolve(raiderHandle)).posUniverse);
    Assert::IsTrue(endDistance < startDistance, L"the hunter did not close on its target");
  }

  TEST_METHOD(AProtectorStandsDownWhenItsTargetDies)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStation(universe, 0.0f, 0.0f);
    const Game::ShipId raider = universe.SpawnShip(Game::LocalPos(1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    const Game::ShipHandle raiderHandle = universe.HandleOf(raider);

    universe.RecordAggression(raiderHandle, station);
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 4; ++tick)
      universe.Step();
    Assert::AreEqual(COMPLEMENT, universe.LaunchedProtectorCount(station), L"the garrison never reached its complement");

    Assert::IsTrue(universe.DespawnShip(raiderHandle), L"the despawn failed");

    bool home = false;
    for (int tick = 0; tick < 30000 && !home; ++tick)
    {
      universe.Step();
      home = universe.LaunchedProtectorCount(station) == 0;
    }
    Assert::IsTrue(home, L"the garrison never came home");

    // A garrison is not a guest: docking home returns the hull to the complement and writes no
    // ledger row (Design/Archive/Stations.md 8.3).
    Assert::IsTrue(universe.StationOf(station).docked.empty(), L"a protector coming home was written into the ledger");
    Assert::AreEqual(static_cast<std::uint32_t>(1), universe.ShipCount(), L"only the station should be left in the universe");

    // And it stays down. Nothing relaunches at a target list that is empty.
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 6; ++tick)
      universe.Step();
    Assert::AreEqual(static_cast<std::uint32_t>(0), universe.LaunchedProtectorCount(station), L"the station relaunched at nobody");
  }

  // The reserve is bottomless for as long as a target lives, which is safe precisely because a
  // protector drops nothing when destroyed (Design/Archive/Stations.md 8.6) -- there is nothing to farm.
  TEST_METHOD(ALossIsReplaced)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStation(universe, 0.0f, 0.0f);
    const Game::ShipId raider = universe.SpawnShip(Game::LocalPos(2500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    universe.RecordAggression(universe.HandleOf(raider), station);
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 4; ++tick)
      universe.Step();
    Assert::AreEqual(COMPLEMENT, universe.LaunchedProtectorCount(station), L"the garrison never reached its complement");

    const Game::ShipId doomed = FirstProtector(universe);
    Assert::IsTrue(universe.DespawnShip(universe.HandleOf(doomed)), L"the despawn failed");
    Assert::AreEqual(COMPLEMENT - 1, universe.LaunchedProtectorCount(station), L"the loss was not counted");

    // Replaced by the same metronome, and the complement is never exceeded on the way back up.
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 3; ++tick)
    {
      universe.Step();
      Assert::IsTrue(universe.LaunchedProtectorCount(station) <= COMPLEMENT, L"the station overran its complement replacing a loss");
    }
    Assert::AreEqual(COMPLEMENT, universe.LaunchedProtectorCount(station), L"the loss was never replaced");
  }

  // Standing is imperial, the response is local. Both come straight from the owner's brief.
  TEST_METHOD(AggressionIsImperialAndTheResponseIsLocal)
  {
    Game::Universe universe;
    const Game::Universe::StationId attacked = MakeStation(universe, 0.0f, 0.0f);
    const Game::Universe::StationId elsewhere = MakeStation(universe, 4000.0f, 0.0f);
    const Game::ShipId raider = universe.SpawnShip(Game::LocalPos(1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    universe.RecordAggression(universe.HandleOf(raider), attacked);
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 5; ++tick)
      universe.Step();

    Assert::AreEqual(COMPLEMENT, universe.LaunchedProtectorCount(attacked), L"the attacked station did not scramble");
    Assert::AreEqual(static_cast<std::uint32_t>(0), universe.LaunchedProtectorCount(elsewhere),
                     L"a station nobody attacked scrambled: the response is local");

    // The grudge, though, is the whole government's.
    Assert::IsTrue(universe.StandingOf(Game::FACTION_VANGUARD, Game::FACTION_PLAYER) == Game::Standing::Hostile,
                   L"the attacker is not criminal in the Vanguard's eyes");
  }

  TEST_METHOD(AFullTargetListDropsTheNewest)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStation(universe, 0.0f, 0.0f, 1);

    std::vector<Game::ShipHandle> raiders;
    for (std::uint32_t at = 0; at < TARGET_CAP + 2; ++at)
    {
      const Game::ShipId raider = universe.SpawnShip(Game::LocalPos(1500.0f + static_cast<float>(at) * 120.0f, 0.0f), 0.0f,
                                                     static_cast<std::uint32_t>(Game::HullId::Bomber));
      raiders.push_back(universe.HandleOf(raider));
      universe.RecordAggression(raiders.back(), station);
    }

    const std::vector<Game::ShipHandle>& targets = universe.StationOf(station).targets;
    Assert::AreEqual(static_cast<std::size_t>(TARGET_CAP), targets.size(), L"the target list is not capped");
    for (std::uint32_t at = 0; at < TARGET_CAP; ++at)
      Assert::IsTrue(targets[at] == raiders[at], L"the list did not keep the earliest arrivals, in arrival order");

    // Every one of them is still criminal: the standing flip happens whether or not there is room,
    // which is the part that matters.
    Assert::IsTrue(universe.StandingOf(Game::FACTION_VANGUARD, Game::FACTION_PLAYER) == Game::Standing::Hostile,
                   L"an over-cap aggressor escaped the standing flip");

    // Attacking twice does not queue two protectors.
    const std::size_t before = targets.size();
    universe.RecordAggression(raiders[0], station);
    Assert::AreEqual(before, universe.StationOf(station).targets.size(), L"a repeat aggression was listed twice");
  }

  // A protector on its way home picks a new target up and turns round, rather than docking and
  // being relaunched a tick later (Design/Archive/Stations-slice-4.md 2.4).
  TEST_METHOD(AHomewardProtectorTurnsRound)
  {
    Game::Universe universe;
    const Game::Universe::StationId station = MakeStation(universe, 0.0f, 0.0f, 1);
    const Game::ShipId first = universe.SpawnShip(Game::LocalPos(1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    const Game::ShipHandle firstHandle = universe.HandleOf(first);

    universe.RecordAggression(firstHandle, station);
    for (int tick = 0; tick < 600; ++tick)
      universe.Step();
    const Game::ShipId hunter = FirstProtector(universe);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, hunter, L"nothing launched");

    // Its target dies; it turns for home.
    Assert::IsTrue(universe.DespawnShip(firstHandle), L"the despawn failed");
    universe.Step();
    const Game::ShipId homeward = FirstProtector(universe);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, homeward, L"the protector stopped being one the moment its target died");
    Assert::IsTrue(universe.DockingOf(homeward).active, L"a protector with nothing to hunt did not head home");

    // A second aggressor, while it is still on its way in.
    const Game::ShipId second = universe.SpawnShip(Game::LocalPos(-1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    universe.RecordAggression(universe.HandleOf(second), station);
    universe.Step();

    const Game::ShipId turned = FirstProtector(universe);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, turned, L"the protector docked instead of turning round");
    Assert::IsFalse(universe.DockingOf(turned).active, L"the protector is still trying to dock with a target outstanding");
    Assert::IsTrue(universe.ProtectorOf(turned).target == universe.HandleOf(second), L"the protector did not take the new target");
    Assert::IsTrue(universe.StationOf(station).docked.empty(), L"the protector docked on its way past");
  }

  // The replay gate over everything slices 1 to 4 added: a layout's spawns, a dock, an aggression,
  // the scramble and the pursuit -- twice, compared field for field every tick.
  TEST_METHOD(TheSameResponseProducesTheSameRun)
  {
    // What the scene must actually have done, so that the comparison above is comparing something.
    struct Summary
    {
      std::uint32_t peakLaunched = 0;
      std::uint32_t docked = 0;
    };

    const auto play = [](std::vector<Game::UniversePos>& _outTrack, std::vector<float>& _outMotion, std::vector<std::uint32_t>& _outCounts,
                         Summary& _outSummary)
    {
      Game::Universe universe;

      // Content out of the layout function, so the replay covers slice 1's output as spawn input.
      //
      // The first planet is pinned close. At its drawn orbit a Corvette cannot cross to it inside
      // the tick budget below, and a scene where nobody arrives would have the replay comparing two
      // identical nothings -- which is the failure mode the summary at the end exists to catch.
      // Pinning also puts slice 1's pin path in the replay.
      Game::SystemDesc desc;
      desc.pinFirstPlanet = true;
      desc.firstPlanetBearingRad = 0.0f;
      desc.firstPlanetOrbitMetres = 900.0f;
      const Game::SystemLayout layout = Game::LayOutSystem(0x53746174696F6Eull, Game::LocalPos(0.0f, 0.0f), desc);
      std::vector<Game::Universe::StationId> stations;
      for (const Game::PlanetSite& site : layout.planets)
      {
        const Game::ShipId structure =
          universe.SpawnShip(site.posUniverse, 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
        Game::Universe::StationDesc garrison;
        garrison.ownerFaction = Game::FACTION_VANGUARD;
        garrison.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
        garrison.protectorComplement = COMPLEMENT;
        garrison.launchEveryTicks = CADENCE_TICKS;
        garrison.targetCap = TARGET_CAP;
        stations.push_back(universe.MakeStation(structure, garrison));
      }

      const Game::ShipId docker = universe.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

      // The raider is a Vandal and not the player, deliberately. Provoking with the same faction
      // that is docking makes the two halves of the scene contradict each other: the standing flip
      // is empire-wide, so the docker would be turned away at the door by the capture-time re-check
      // and the run would prove only that two refusals agree (Design/Archive/Stations.md 7.3).
      const Game::ShipId raider =
        universe.SpawnShip(Game::LocalPos(200.0f, 600.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber), Game::FACTION_VANDAL);

      const Game::ShipId firstStructure = universe.Resolve(universe.StationOf(stations[0]).structure);
      (void)universe.IssueDockOrder(std::array{docker}, firstStructure, Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER});
      universe.RecordAggression(universe.HandleOf(raider), stations[1]);

      for (int tick = 0; tick < 3000; ++tick)
      {
        universe.Step();
        for (Game::ShipId id = 0; id < universe.ShipCount(); ++id)
        {
          _outTrack.push_back(universe.Ship(id).posUniverse);
          _outMotion.push_back(universe.Ship(id).speed);
          _outMotion.push_back(universe.Ship(id).headingRad);
        }
        _outCounts.push_back(universe.ShipCount());
        for (const Game::Universe::StationId station : stations)
        {
          const std::uint32_t launched = universe.LaunchedProtectorCount(station);
          const std::uint32_t docked = static_cast<std::uint32_t>(universe.StationOf(station).docked.size());
          _outCounts.push_back(launched);
          _outCounts.push_back(docked);
          _outCounts.push_back(static_cast<std::uint32_t>(universe.StationOf(station).targets.size()));
          _outSummary.peakLaunched = std::max(_outSummary.peakLaunched, launched);
          _outSummary.docked += docked;
        }
      }
    };

    std::vector<Game::UniversePos> firstTrack, secondTrack;
    std::vector<float> firstMotion, secondMotion;
    std::vector<std::uint32_t> firstCounts, secondCounts;
    Summary firstSummary, secondSummary;
    play(firstTrack, firstMotion, firstCounts, firstSummary);
    play(secondTrack, secondMotion, secondCounts, secondSummary);

    Assert::AreEqual(firstTrack.size(), secondTrack.size(), L"the two runs produced different numbers of samples");
    for (std::size_t at = 0; at < firstTrack.size(); ++at)
      Assert::IsTrue(IsSamePosition(firstTrack[at], secondTrack[at]), L"a position diverged between two identical runs");
    for (std::size_t at = 0; at < firstMotion.size(); ++at)
      Assert::AreEqual(firstMotion[at], secondMotion[at], 0.0f, L"a ship's motion diverged between two identical runs");
    Assert::AreEqual(firstCounts.size(), secondCounts.size(), L"the two runs counted different numbers of things");
    for (std::size_t at = 0; at < firstCounts.size(); ++at)
      Assert::AreEqual(firstCounts[at], secondCounts[at], L"a spawn, a docking or a target list diverged between two identical runs");

    // The scene has to have actually docked a ship and actually run a response, or the comparison
    // above is proving that two empty runs agree.
    Assert::IsTrue(firstSummary.peakLaunched > 0, L"the replay scene never launched a protector");
    Assert::IsTrue(firstSummary.docked > 0, L"the replay scene never docked a ship");
    Assert::AreEqual(firstSummary.peakLaunched, secondSummary.peakLaunched, L"the two runs scrambled differently");
    Assert::AreEqual(firstSummary.docked, secondSummary.docked, L"the two runs docked differently");
  }
};
} // namespace GameLogicTests
