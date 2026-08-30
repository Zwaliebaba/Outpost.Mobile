#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// The Vanguard's shipped garrison (Design/Stations.md 8.2): three Corvettes, one every 1.5 s.
constexpr std::uint32_t COMPLEMENT = 3;
constexpr std::uint32_t CADENCE_TICKS = 90;
constexpr std::uint32_t TARGET_CAP = 4;

[[nodiscard]] std::uint32_t Faction(Game::FactionId _faction)
{
  return _faction;
}

Game::World::StationId MakeStation(Game::World& _world, float _x, float _z, std::uint32_t _complement = COMPLEMENT)
{
  const Game::ShipId structure =
    _world.SpawnShip(Game::LocalPos(_x, _z), 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
  Game::World::StationDesc desc;
  desc.ownerFaction = Game::FACTION_VANGUARD;
  desc.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
  desc.protectorComplement = _complement;
  desc.launchEveryTicks = CADENCE_TICKS;
  desc.targetCap = TARGET_CAP;
  return _world.MakeStation(structure, desc);
}

// The nearest live protector to a position, or a very large number if none is out.
[[nodiscard]] float NearestProtector(const Game::World& _world, const Game::WorldPos& _to)
{
  float nearest = 1.0e30f;
  for (Game::ShipId id = 0; id < _world.ShipCount(); ++id)
  {
    if (_world.ProtectorOf(id).active)
      nearest = std::min(nearest, Game::Distance(_world.Ship(id).posWorld, _to));
  }
  return nearest;
}

[[nodiscard]] Game::ShipId FirstProtector(const Game::World& _world)
{
  for (Game::ShipId id = 0; id < _world.ShipCount(); ++id)
  {
    if (_world.ProtectorOf(id).active)
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
    Game::World world;
    const Game::World::StationId station = MakeStation(world, 0.0f, 0.0f);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    // Nothing before the act. The response starts from a *stated* aggression, never from a radius:
    // a ship parked beside a station it has not attacked is unmolested (Design/Stations.md 8.4).
    for (int tick = 0; tick < 400; ++tick)
      world.Step();
    Assert::AreEqual(static_cast<std::uint32_t>(0), world.LaunchedProtectorCount(station),
                     L"a station launched at somebody who had done nothing");

    world.RecordAggression(world.HandleOf(raider), station);
    world.Step();
    Assert::AreEqual(static_cast<std::uint32_t>(1), world.LaunchedProtectorCount(station),
                     L"the first protector did not launch on the tick after the aggression");

    // On the metronome, and capped. Stepped well past what three launches need.
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 6; ++tick)
      world.Step();
    Assert::AreEqual(COMPLEMENT, world.LaunchedProtectorCount(station), L"the garrison did not reach, or overran, its complement");

    // In the owner's faction and on the owner's hull, so IFF and silhouette both read right.
    std::uint32_t seen = 0;
    for (Game::ShipId id = 0; id < world.ShipCount(); ++id)
    {
      if (!world.ProtectorOf(id).active)
        continue;
      ++seen;
      Assert::AreEqual(Faction(Game::FACTION_VANGUARD), Faction(world.Ship(id).factionId), L"a protector flew somebody else's flag");
      Assert::AreEqual(static_cast<std::uint32_t>(Game::HullId::Corvette), world.Ship(id).hullId, L"a protector flew the wrong hull");
      Assert::IsTrue(world.ProtectorOf(id).home == station, L"a protector belongs to the wrong station");
    }
    Assert::AreEqual(COMPLEMENT, seen, L"the duty table and the launched count disagree");
  }

  // Complement 0 is the Vandal base: its patrol is not a garrison and it launches nothing, ever.
  TEST_METHOD(AStationWithNoGarrisonLaunchesNothing)
  {
    Game::World world;
    const Game::World::StationId station = MakeStation(world, 0.0f, 0.0f, 0);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(900.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    world.RecordAggression(world.HandleOf(raider), station);
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 10; ++tick)
      world.Step();

    Assert::AreEqual(static_cast<std::uint32_t>(0), world.LaunchedProtectorCount(station),
                     L"a station with no complement launched something");
    Assert::AreEqual(static_cast<std::uint32_t>(2), world.ShipCount(), L"a ship appeared from somewhere");
  }

  // The chase, and the threshold that keeps it a sequence of ordinary orders rather than a replan
  // every tick.
  TEST_METHOD(AProtectorPursuesItsTarget)
  {
    Game::World world;
    const Game::World::StationId station = MakeStation(world, 0.0f, 0.0f, 1);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(1200.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Interceptor));
    const Game::ShipHandle raiderHandle = world.HandleOf(raider);

    world.RecordAggression(raiderHandle, station);
    world.Step();
    const Game::ShipId hunter = FirstProtector(world);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, hunter, L"nothing launched");

    // The raider runs. A fleeing target is the case the replan threshold exists for.
    (void)world.IssueMoveOrder(std::array{raider}, Game::LocalPos(1200.0f, 4000.0f), false, 0.0f, Game::FACTION_PLAYER);

    const float startDistance = Game::Distance(world.Ship(hunter).posWorld, world.Ship(raider).posWorld);
    std::uint32_t reaims = 0;
    Game::WorldPos aim = world.RouteOf(hunter).empty() ? world.Ship(hunter).posWorld : world.RouteOf(hunter).back();

    constexpr int WINDOW = 3000;
    for (int tick = 0; tick < WINDOW; ++tick)
    {
      world.Step();
      const Game::ShipId live = world.Resolve(raiderHandle);
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, live, L"the raider vanished");

      const std::span<const Game::WorldPos> route = world.RouteOf(hunter);
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
      Assert::IsTrue(Game::Distance(aim, world.Ship(live).posWorld) <= Game::PURSUIT_REPLAN_METRES + 1.0f,
                     L"the hunter let its target drift past the replan threshold without re-aiming");
    }

    Assert::IsTrue(reaims > 0, L"the hunter never re-aimed at a target that ran");
    Assert::IsTrue(reaims < WINDOW / 10, L"the hunter re-planned far too often -- the threshold is not holding");

    const float endDistance = Game::Distance(world.Ship(hunter).posWorld, world.Ship(world.Resolve(raiderHandle)).posWorld);
    Assert::IsTrue(endDistance < startDistance, L"the hunter did not close on its target");
  }

  TEST_METHOD(AProtectorStandsDownWhenItsTargetDies)
  {
    Game::World world;
    const Game::World::StationId station = MakeStation(world, 0.0f, 0.0f);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    const Game::ShipHandle raiderHandle = world.HandleOf(raider);

    world.RecordAggression(raiderHandle, station);
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 4; ++tick)
      world.Step();
    Assert::AreEqual(COMPLEMENT, world.LaunchedProtectorCount(station), L"the garrison never reached its complement");

    Assert::IsTrue(world.DespawnShip(raiderHandle), L"the despawn failed");

    bool home = false;
    for (int tick = 0; tick < 30000 && !home; ++tick)
    {
      world.Step();
      home = world.LaunchedProtectorCount(station) == 0;
    }
    Assert::IsTrue(home, L"the garrison never came home");

    // A garrison is not a guest: docking home returns the hull to the complement and writes no
    // ledger row (Design/Stations.md 8.3).
    Assert::IsTrue(world.StationOf(station).docked.empty(), L"a protector coming home was written into the ledger");
    Assert::AreEqual(static_cast<std::uint32_t>(1), world.ShipCount(), L"only the station should be left in the world");

    // And it stays down. Nothing relaunches at a target list that is empty.
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 6; ++tick)
      world.Step();
    Assert::AreEqual(static_cast<std::uint32_t>(0), world.LaunchedProtectorCount(station), L"the station relaunched at nobody");
  }

  // The reserve is bottomless for as long as a target lives, which is safe precisely because a
  // protector drops nothing when destroyed (Design/Stations.md 8.6) -- there is nothing to farm.
  TEST_METHOD(ALossIsReplaced)
  {
    Game::World world;
    const Game::World::StationId station = MakeStation(world, 0.0f, 0.0f);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(2500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    world.RecordAggression(world.HandleOf(raider), station);
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 4; ++tick)
      world.Step();
    Assert::AreEqual(COMPLEMENT, world.LaunchedProtectorCount(station), L"the garrison never reached its complement");

    const Game::ShipId doomed = FirstProtector(world);
    Assert::IsTrue(world.DespawnShip(world.HandleOf(doomed)), L"the despawn failed");
    Assert::AreEqual(COMPLEMENT - 1, world.LaunchedProtectorCount(station), L"the loss was not counted");

    // Replaced by the same metronome, and the complement is never exceeded on the way back up.
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 3; ++tick)
    {
      world.Step();
      Assert::IsTrue(world.LaunchedProtectorCount(station) <= COMPLEMENT, L"the station overran its complement replacing a loss");
    }
    Assert::AreEqual(COMPLEMENT, world.LaunchedProtectorCount(station), L"the loss was never replaced");
  }

  // Standing is imperial, the response is local. Both come straight from the owner's brief.
  TEST_METHOD(AggressionIsImperialAndTheResponseIsLocal)
  {
    Game::World world;
    const Game::World::StationId attacked = MakeStation(world, 0.0f, 0.0f);
    const Game::World::StationId elsewhere = MakeStation(world, 4000.0f, 0.0f);
    const Game::ShipId raider = world.SpawnShip(Game::LocalPos(1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));

    world.RecordAggression(world.HandleOf(raider), attacked);
    for (int tick = 0; tick < static_cast<int>(CADENCE_TICKS) * 5; ++tick)
      world.Step();

    Assert::AreEqual(COMPLEMENT, world.LaunchedProtectorCount(attacked), L"the attacked station did not scramble");
    Assert::AreEqual(static_cast<std::uint32_t>(0), world.LaunchedProtectorCount(elsewhere),
                     L"a station nobody attacked scrambled: the response is local");

    // The grudge, though, is the whole government's.
    Assert::IsTrue(world.StandingOf(Game::FACTION_VANGUARD, Game::FACTION_PLAYER) == Game::Standing::Hostile,
                   L"the attacker is not criminal in the Vanguard's eyes");
  }

  TEST_METHOD(AFullTargetListDropsTheNewest)
  {
    Game::World world;
    const Game::World::StationId station = MakeStation(world, 0.0f, 0.0f, 1);

    std::vector<Game::ShipHandle> raiders;
    for (std::uint32_t at = 0; at < TARGET_CAP + 2; ++at)
    {
      const Game::ShipId raider = world.SpawnShip(Game::LocalPos(1500.0f + static_cast<float>(at) * 120.0f, 0.0f), 0.0f,
                                                  static_cast<std::uint32_t>(Game::HullId::Bomber));
      raiders.push_back(world.HandleOf(raider));
      world.RecordAggression(raiders.back(), station);
    }

    const std::vector<Game::ShipHandle>& targets = world.StationOf(station).targets;
    Assert::AreEqual(static_cast<std::size_t>(TARGET_CAP), targets.size(), L"the target list is not capped");
    for (std::uint32_t at = 0; at < TARGET_CAP; ++at)
      Assert::IsTrue(targets[at] == raiders[at], L"the list did not keep the earliest arrivals, in arrival order");

    // Every one of them is still criminal: the standing flip happens whether or not there is room,
    // which is the part that matters.
    Assert::IsTrue(world.StandingOf(Game::FACTION_VANGUARD, Game::FACTION_PLAYER) == Game::Standing::Hostile,
                   L"an over-cap aggressor escaped the standing flip");

    // Attacking twice does not queue two protectors.
    const std::size_t before = targets.size();
    world.RecordAggression(raiders[0], station);
    Assert::AreEqual(before, world.StationOf(station).targets.size(), L"a repeat aggression was listed twice");
  }

  // A protector on its way home picks a new target up and turns round, rather than docking and
  // being relaunched a tick later (Design/Archive/Stations-slice-4.md 2.4).
  TEST_METHOD(AHomewardProtectorTurnsRound)
  {
    Game::World world;
    const Game::World::StationId station = MakeStation(world, 0.0f, 0.0f, 1);
    const Game::ShipId first = world.SpawnShip(Game::LocalPos(1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    const Game::ShipHandle firstHandle = world.HandleOf(first);

    world.RecordAggression(firstHandle, station);
    for (int tick = 0; tick < 600; ++tick)
      world.Step();
    const Game::ShipId hunter = FirstProtector(world);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, hunter, L"nothing launched");

    // Its target dies; it turns for home.
    Assert::IsTrue(world.DespawnShip(firstHandle), L"the despawn failed");
    world.Step();
    const Game::ShipId homeward = FirstProtector(world);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, homeward, L"the protector stopped being one the moment its target died");
    Assert::IsTrue(world.DockingOf(homeward).active, L"a protector with nothing to hunt did not head home");

    // A second aggressor, while it is still on its way in.
    const Game::ShipId second = world.SpawnShip(Game::LocalPos(-1500.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber));
    world.RecordAggression(world.HandleOf(second), station);
    world.Step();

    const Game::ShipId turned = FirstProtector(world);
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, turned, L"the protector docked instead of turning round");
    Assert::IsFalse(world.DockingOf(turned).active, L"the protector is still trying to dock with a target outstanding");
    Assert::IsTrue(world.ProtectorOf(turned).target == world.HandleOf(second), L"the protector did not take the new target");
    Assert::IsTrue(world.StationOf(station).docked.empty(), L"the protector docked on its way past");
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

    const auto play = [](std::vector<Game::WorldPos>& _outTrack, std::vector<float>& _outMotion, std::vector<std::uint32_t>& _outCounts,
                         Summary& _outSummary)
    {
      Game::World world;

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
      std::vector<Game::World::StationId> stations;
      for (const Game::PlanetSite& site : layout.planets)
      {
        const Game::ShipId structure =
          world.SpawnShip(site.posWorld, 0.0f, static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_VANGUARD);
        Game::World::StationDesc garrison;
        garrison.ownerFaction = Game::FACTION_VANGUARD;
        garrison.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
        garrison.protectorComplement = COMPLEMENT;
        garrison.launchEveryTicks = CADENCE_TICKS;
        garrison.targetCap = TARGET_CAP;
        stations.push_back(world.MakeStation(structure, garrison));
      }

      const Game::ShipId docker = world.SpawnShip(Game::LocalPos(0.0f, 0.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette));

      // The raider is a Vandal and not the player, deliberately. Provoking with the same faction
      // that is docking makes the two halves of the scene contradict each other: the standing flip
      // is empire-wide, so the docker would be turned away at the door by the capture-time re-check
      // and the run would prove only that two refusals agree (Design/Stations.md 7.3).
      const Game::ShipId raider =
        world.SpawnShip(Game::LocalPos(200.0f, 600.0f), 0.0f, static_cast<std::uint32_t>(Game::HullId::Bomber), Game::FACTION_VANDAL);

      const Game::ShipId firstStructure = world.Resolve(world.StationOf(stations[0]).structure);
      (void)world.IssueDockOrder(std::array{docker}, firstStructure, Game::FACTION_PLAYER);
      world.RecordAggression(world.HandleOf(raider), stations[1]);

      for (int tick = 0; tick < 3000; ++tick)
      {
        world.Step();
        for (Game::ShipId id = 0; id < world.ShipCount(); ++id)
        {
          _outTrack.push_back(world.Ship(id).posWorld);
          _outMotion.push_back(world.Ship(id).speed);
          _outMotion.push_back(world.Ship(id).headingRad);
        }
        _outCounts.push_back(world.ShipCount());
        for (const Game::World::StationId station : stations)
        {
          const std::uint32_t launched = world.LaunchedProtectorCount(station);
          const std::uint32_t docked = static_cast<std::uint32_t>(world.StationOf(station).docked.size());
          _outCounts.push_back(launched);
          _outCounts.push_back(docked);
          _outCounts.push_back(static_cast<std::uint32_t>(world.StationOf(station).targets.size()));
          _outSummary.peakLaunched = std::max(_outSummary.peakLaunched, launched);
          _outSummary.docked += docked;
        }
      }
    };

    std::vector<Game::WorldPos> firstTrack, secondTrack;
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
