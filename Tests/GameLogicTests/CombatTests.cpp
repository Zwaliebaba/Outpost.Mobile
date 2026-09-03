#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// One owner per faction, which is what every row here means: these suites were written when the key
// WAS the faction, and this keeps each of them saying exactly what it said (Design/Archive/OwnerKey-work-order.md).
[[nodiscard]] Game::Issuer IssuerFor(Game::FactionId _faction)
{
  return Game::Issuer{(_faction == Game::FACTION_PLAYER) ? Game::OWNER_LOCAL : Game::OwnerId{_faction} + 1u, _faction};
}

// A fighter's gun does 3 every half second into 60 points, so a peer kills one in about ten seconds
// -- the first of Design/Archive/Combat.md 13's five pacing targets, and the one every other number is
// balanced against.
constexpr float PEER_KILL_SECONDS = 10.0f;

Game::ShipId Spawn(Game::Universe& _universe, float _x, float _z, float _headingRad, Game::HullId _hull, Game::FactionId _faction)
{
  return _universe.SpawnShip(Game::LocalPos(_x, _z), _headingRad, static_cast<std::uint32_t>(_hull), _faction);
}

// A fleet of one, which is what most of these rows want: a ship that can be given an order.
Game::Universe::FleetId FleetOfOne(Game::Universe& _universe, Game::ShipId _ship, Game::FactionId _faction = Game::FACTION_PLAYER,
                                   std::uint8_t _slot = 0)
{
  const Game::ShipId members[] = {_ship};
  return _universe.FormFleet(IssuerFor(_faction), _slot, std::span<const Game::ShipId>(members, 1));
}

Game::Universe::FleetOrderResult OrderAttack(Game::Universe& _universe, Game::ShipId _target,
                                             Game::FactionId _faction = Game::FACTION_PLAYER, std::uint8_t _slot = 0)
{
  Game::Universe::FleetCommand command;
  command.kind = Game::FleetOrderKind::Attack;
  command.target = _target;
  return _universe.IssueFleetOrder(IssuerFor(_faction), _slot, command);
}

void Run(Game::Universe& _universe, int _ticks)
{
  for (int at = 0; at < _ticks; ++at)
    _universe.Step();
}

// Ticks until the universe loses a ship, or _limit if it never does.
[[nodiscard]] int TicksUntilALoss(Game::Universe& _universe, int _limit)
{
  const std::uint32_t started = _universe.ShipCount();
  for (int at = 0; at < _limit; ++at)
  {
    _universe.Step();
    if (_universe.ShipCount() < started)
      return at + 1;
  }
  return _limit;
}

// Two ships facing each other down the +Z axis, which puts each dead ahead of the other and so
// satisfies a bow mount's arc without anybody having to turn.
void FacingPair(Game::Universe& _universe, Game::HullId _mine, Game::HullId _theirs, float _apartMetres)
{
  Spawn(_universe, 0.0f, 0.0f, 0.0f, _mine, Game::FACTION_PLAYER);
  Spawn(_universe, 0.0f, _apartMetres, DirectX::XM_PI, _theirs, Game::FACTION_VANDAL);
}
} // namespace

TEST_CLASS(CombatTests)
{
public:
  TEST_METHOD(AFighterDiesUnderAPeersGun)
  {
    Game::Universe universe;
    FacingPair(universe, Game::HullId::Interceptor, Game::HullId::Interceptor, 100.0f);
    Assert::AreEqual(60u, universe.Ship(0).hullPoints, L"a fighter spawns whole");

    const int ticks = TicksUntilALoss(universe, 2000);
    const float seconds = static_cast<float>(ticks) / Game::TICK_HZ;
    Assert::IsTrue(
      seconds > PEER_KILL_SECONDS * 0.8f && seconds < PEER_KILL_SECONDS * 1.2f,
      std::format(L"a fighter under a peer's gun lasted {:.1f} s against a {:.0f} s target", seconds, PEER_KILL_SECONDS).c_str());
  }

  TEST_METHOD(TheSameBattleProducesTheSameRun)
  {
    // The replay gate, pointed at the pass that decides who dies. Two universes built the same way and
    // stepped together, compared state for state on every tick rather than at the end -- because a
    // divergence that cancels out is still a divergence.
    Game::Universe first;
    Game::Universe second;
    for (Game::Universe* universe : {&first, &second})
    {
      Spawn(*universe, 0.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
      Spawn(*universe, 0.0f, 120.0f, DirectX::XM_PI, Game::HullId::Corvette, Game::FACTION_VANDAL);
      Spawn(*universe, 40.0f, 90.0f, DirectX::XM_PI, Game::HullId::Interceptor, Game::FACTION_VANDAL);
    }

    for (int at = 0; at < 900; ++at)
    {
      first.Step();
      second.Step();
      Assert::AreEqual(first.ShipCount(), second.ShipCount(), L"the two battles lost different ships");
      for (Game::ShipId id = 0; id < first.ShipCount(); ++id)
      {
        Assert::AreEqual(first.Ship(id).hullPoints, second.Ship(id).hullPoints, L"hull points diverged");
        Assert::IsTrue(IsSamePosition(first.Ship(id).posUniverse, second.Ship(id).posUniverse), L"positions diverged");
        Assert::AreEqual(first.Ship(id).headingRad, second.Ship(id).headingRad, L"headings diverged");
      }
    }
  }

  TEST_METHOD(AMutualKillTakesBothShips)
  {
    // Two Bombers, each with a 90-point cannon against the other's 150 points, firing on the same
    // ticks because they were spawned facing at the same range. The second shot lands on both in the
    // same walk, and both deaths are collected before either is applied -- so whichever order the
    // array is walked in, neither ship gets to outlive the shot it had already fired.
    Game::Universe universe;
    FacingPair(universe, Game::HullId::Bomber, Game::HullId::Bomber, 150.0f);
    Run(universe, 800);
    Assert::AreEqual(0u, universe.ShipCount(), L"a mutual kill left a survivor");
  }

  TEST_METHOD(AKillingShotStillRousesTheFleet)
  {
    // The ordering that is the whole reason the pass is four loops and not one. A Battleship's heavy
    // turret does 70 to a 60-point Interceptor, so the first shot that lands kills it outright --
    // and RecordHostileAct resolves its victim, so a death applied before the act would rouse
    // nobody at all.
    Game::Universe universe;
    const Game::ShipId member = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Interceptor, Game::FACTION_PLAYER);
    const Game::ShipId escort = Spawn(universe, 30.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    Spawn(universe, 0.0f, 120.0f, DirectX::XM_PI, Game::HullId::Battleship, Game::FACTION_VANDAL);

    const Game::ShipId members[] = {member, escort};
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0,
                                                                             std::span<const Game::ShipId>(members, 2)));

    (void)TicksUntilALoss(universe, 600);
    const Game::Universe::FleetId fleet = universe.FleetInSlot(Game::OWNER_LOCAL, 0);
    Assert::AreNotEqual(Game::Universe::INVALID_FLEET_ID, fleet, L"the fleet retired with its escort still flying");
    Assert::IsTrue(universe.FleetOf(fleet).alertTicks > 0, L"a member killed outright roused nobody");
  }

  TEST_METHOD(NobodyShootsAFriend)
  {
    Game::Universe universe;
    const Game::ShipId mine = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId yours = Spawn(universe, 0.0f, 60.0f, DirectX::XM_PI, Game::HullId::Corvette, Game::FACTION_PLAYER);
    Run(universe, 600);
    Assert::AreEqual(240u, universe.Ship(mine).hullPoints, L"a friend was shot");
    Assert::AreEqual(240u, universe.Ship(yours).hullPoints, L"a friend was shot");

    (void)FleetOfOne(universe, mine);
    Assert::IsTrue(OrderAttack(universe, yours) == Game::Universe::FleetOrderResult::RefusedFriendly,
                   L"an attack order naming your own ship was accepted");
  }

  TEST_METHOD(OpportunisticFireNeedsAHostileStanding)
  {
    // The one priority that is a sense rather than a statement, and the gate on it. A Vanguard hull
    // in the same envelope is neutral at boot and is left alone; the Vandal on the other beam is not.
    //
    // The two of them are put on opposite sides of the player and 230 m apart deliberately. The
    // Vandal Collective holds *every* other faction hostile, the Vanguard included (DEFAULT_STANDINGS),
    // so a Vandal within reach of the neutral shoots it -- correctly -- and the row would then be
    // measuring the Vandal's opinion rather than the player's.
    Game::Universe universe;
    Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId neutral = Spawn(universe, 0.0f, 80.0f, DirectX::XM_PI, Game::HullId::Corvette, Game::FACTION_VANGUARD);
    const Game::ShipId hostile = Spawn(universe, 0.0f, -150.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_VANDAL);
    Run(universe, 300);

    Assert::AreEqual(240u, universe.Ship(neutral).hullPoints, L"a neutral inside the envelope was shot");
    Assert::IsTrue(universe.Ship(hostile).hullPoints < 240u, L"a standing hostile inside the envelope was not shot");
  }

  TEST_METHOD(AnOrderedAttackShootsANeutral)
  {
    // A stated target is fired on whatever the standing table says, because an ordered attack on a
    // neutral is the player spending their own standing -- which is what makes it a decision.
    Game::Universe universe;
    const Game::ShipId mine = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId neutral = Spawn(universe, 0.0f, 80.0f, DirectX::XM_PI, Game::HullId::Corvette, Game::FACTION_VANGUARD);

    (void)FleetOfOne(universe, mine);
    Assert::IsTrue(OrderAttack(universe, neutral) == Game::Universe::FleetOrderResult::Ordered);
    Run(universe, 120);
    Assert::IsTrue(universe.Ship(neutral).hullPoints < 240u, L"a stated neutral target was not shot");
  }

  TEST_METHOD(AMoveOrderHoldsTheGuns)
  {
    // The travel hold. A Move order means leave: while the member is still flying it, the held
    // target and the opportunistic sense are skipped, so a hostile inside the envelope is passed
    // rather than raked -- and once the ship has ARRIVED it is Idle at its slot and stands guard
    // exactly as before, which is what the second half of this row pins. The bystander is a Miner
    // because it is unarmed: a hostile that shot back would rouse the defense, which outranks the
    // hold on purpose and would turn the row into a measurement of the leash.
    Game::Universe universe;
    const Game::ShipId mine = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId bystander = Spawn(universe, 0.0f, 80.0f, 0.0f, Game::HullId::Miner, Game::FACTION_VANDAL);

    (void)FleetOfOne(universe, mine);
    Game::Universe::FleetCommand command;
    command.kind = Game::FleetOrderKind::Move;
    command.point = Game::LocalPos(100.0f, 160.0f); // arrives 128 m from the bystander: inside a LightTurret's 180 m
    Assert::IsTrue(universe.IssueFleetOrder(IssuerFor(Game::FACTION_PLAYER), 0, command) == Game::Universe::FleetOrderResult::Ordered);

    Run(universe, 120);
    Assert::AreEqual(200u, universe.Ship(bystander).hullPoints, L"a travelling fleet raked a bystander in passing");

    Run(universe, 1200);
    Assert::IsTrue(universe.Ship(bystander).hullPoints < 200u, L"an arrived fleet no longer stands guard");
  }

  TEST_METHOD(TheDefenseDoesNotSuspendATravelOrder)
  {
    // The travel hold's other half (owner decision, 2026-09-02): a stated act against a fleet that
    // is FLYING an explicit order no longer turns its combatants around. The threat still stands in
    // the row -- the guns answer it over the shoulder and the alert burns -- but the move is
    // obeyed. Without this, a fleet ordered out of a fight was dragged back by the first shot that
    // followed it, and a Move was only ever one landed hit away from behaving like an Attack.
    Game::Universe universe;
    const Game::ShipId mine = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId shooter = Spawn(universe, 0.0f, -60.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_VANDAL);

    (void)FleetOfOne(universe, mine);
    Game::Universe::FleetCommand command;
    command.kind = Game::FleetOrderKind::Move;
    command.point = Game::LocalPos(0.0f, 1500.0f);
    Assert::IsTrue(universe.IssueFleetOrder(IssuerFor(Game::FACTION_PLAYER), 0, command) == Game::Universe::FleetOrderResult::Ordered);

    Run(universe, 1200);
    Assert::IsTrue(universe.Ship(mine).hullPoints < 240u, L"the shooter never landed a hit -- the row would measure nothing");
    Assert::IsTrue(universe.Ship(shooter).hullPoints < 240u, L"the fleeing fleet never answered over its shoulder");
    Assert::IsTrue(Game::OffsetZ(Game::LocalPos(0.0f, 0.0f), universe.Ship(mine).posUniverse) > 500.0f,
                   L"a landed hit dragged a travelling fleet back into the fight");
  }

  TEST_METHOD(AStationDiscardsDamageAndStillJudges)
  {
    // Design/Archive/Stations.md 8.5's standing rule, implemented rather than restated: "however it
    // models damage, a Vanguard station's is discarded". The act is stated all the same, which is
    // the half ADR 0041 left this design to call.
    Game::Universe universe;
    const Game::ShipId structure = Spawn(universe, 0.0f, 600.0f, 0.0f, Game::HullId::Structure, Game::FACTION_VANGUARD);
    Game::Universe::StationDesc desc;
    desc.ownerFaction = Game::FACTION_VANGUARD;
    desc.protectorHullId = static_cast<std::uint32_t>(Game::HullId::Corvette);
    desc.protectorComplement = 2;
    const Game::Universe::StationId station = universe.MakeStation(structure, desc);

    const Game::ShipId raider = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Frigate, Game::FACTION_PLAYER);
    (void)FleetOfOne(universe, raider);
    Assert::IsTrue(OrderAttack(universe, structure) == Game::Universe::FleetOrderResult::Ordered);
    Assert::IsTrue(universe.StandingOf(Game::FACTION_VANGUARD, Game::FACTION_PLAYER) == Game::Standing::Neutral);

    Run(universe, 1200);
    Assert::AreEqual(0u, universe.Ship(structure).hullPoints, L"an indestructible hull held points to lose");
    Assert::IsTrue(universe.StandingOf(Game::FACTION_VANGUARD, Game::FACTION_PLAYER) == Game::Standing::Hostile,
                   L"shooting a station did not provoke the law");
    Assert::IsTrue(universe.LaunchedProtectorCount(station) > 0, L"the provoked station launched nothing");
  }

  TEST_METHOD(TheStandOffKeepsTheGunsBearing)
  {
    // Where a pursuit is AIMED, which is the stand-off itself rather than how far along it has flown
    // by any particular tick. A Corvette's light turrets reach 180 m, so it holds 144.
    Game::Universe universe;
    const Game::ShipId hunter = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId quarry = Spawn(universe, 0.0f, 900.0f, 0.0f, Game::HullId::Hauler, Game::FACTION_VANDAL);
    (void)FleetOfOne(universe, hunter);
    Assert::IsTrue(OrderAttack(universe, quarry) == Game::Universe::FleetOrderResult::Ordered);
    universe.Step();

    const std::span<const Game::UniversePos> route = universe.RouteOf(hunter);
    Assert::IsFalse(route.empty(), L"the pursuit planned no route at all");
    const float held = Game::Distance(route.back(), universe.Ship(quarry).posUniverse);
    Assert::IsTrue(held > 138.0f && held < 150.0f, std::format(L"a Corvette aimed to hold {:.1f} m rather than 144 m", held).c_str());
  }

  TEST_METHOD(AFixedGunHullIsSentAtItsQuarry)
  {
    // The other half of the same rule, and what makes an Interceptor fly a pass without one line
    // written for attack runs: a hull with no traversing mount takes no stand-off at all.
    Game::Universe universe;
    const Game::ShipId fighter = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Interceptor, Game::FACTION_PLAYER);
    const Game::ShipId quarry = Spawn(universe, 0.0f, 900.0f, 0.0f, Game::HullId::Hauler, Game::FACTION_VANDAL);
    (void)FleetOfOne(universe, fighter);
    Assert::IsTrue(OrderAttack(universe, quarry) == Game::Universe::FleetOrderResult::Ordered);
    universe.Step();

    const std::span<const Game::UniversePos> route = universe.RouteOf(fighter);
    Assert::IsFalse(route.empty());
    Assert::IsTrue(Game::Distance(route.back(), universe.Ship(quarry).posUniverse) < 1.0f, L"a bow-fixed hull was given a stand-off");
    Assert::AreEqual(0.0f, Game::EngageStandoffMetres(Game::HullSpecOf(Game::HullId::Interceptor)));
    Assert::IsTrue(Game::EngageStandoffMetres(Game::HullSpecOf(Game::HullId::Frigate)) > 223.0f, L"a Frigate should hold 224 m");
  }

  TEST_METHOD(APursuitReplansOnlyWhenTheTargetMoves)
  {
    // The regression the stand-off would otherwise have introduced. PursueTarget measured a target's
    // drift against the route's destination, which was the target's own position until this design
    // moved it 144 m away; measured against the destination, that constant offset reads as drift on
    // every tick and re-plans an A* sixty times a second (Universe.h, Route::pursuitAimedAt).
    Game::Universe universe;
    const Game::ShipId hunter = Spawn(universe, 0.0f, 0.0f, 0.0f, Game::HullId::Corvette, Game::FACTION_PLAYER);
    const Game::ShipId quarry = Spawn(universe, 0.0f, 900.0f, 0.0f, Game::HullId::Hauler, Game::FACTION_VANDAL);
    (void)FleetOfOne(universe, hunter);
    Assert::IsTrue(OrderAttack(universe, quarry) == Game::Universe::FleetOrderResult::Ordered);

    Run(universe, 120);
    const std::uint64_t before = universe.RoutePlanCount();
    Run(universe, 300);
    const std::uint64_t plans = universe.RoutePlanCount() - before;
    Assert::IsTrue(plans < 30, std::format(L"a pursuit re-planned {} times in 300 ticks against a target holding station", plans).c_str());
  }

  TEST_METHOD(ATurretLosesACrossingFighter)
  {
    // Design/Archive/Combat.md 4's tactical sentence as arithmetic rather than as a claim: a target crossing
    // at v subtends v/r rad/s, so a HeavyTurret at 18 deg/s holds a 34 m/s fighter beyond about
    // 108 m and cannot hold it inside that. Closing under the guns of a capital is a real tactic,
    // and it falls out of three authored numbers with no mechanism built for it.
    const Game::DeviceSpec& heavy = Game::DeviceSpecOf(Game::DeviceId::HeavyTurret);
    const float fastest = Game::HullSpecOf(Game::HullId::Interceptor).maxSpeedMetresPerSec;
    const float holdsBeyond = fastest / heavy.traverseRadPerSec;
    Assert::IsTrue(holdsBeyond > 105.0f && holdsBeyond < 112.0f,
                   std::format(L"a HeavyTurret holds a fighter beyond {:.1f} m", holdsBeyond).c_str());
  }

  TEST_METHOD(TheHullTableArmsWhatItShould)
  {
    // The rows, read back. The two static_asserts in HullSpec.h cover the invariants; this covers the
    // authoring, so that a table edit that disarms the fleet fails here rather than in a play test.
    Assert::IsTrue(Game::HullSpecOf(Game::HullId::Miner).MountCount() == 0, L"the Miner is unarmed until the mining design");
    Assert::IsTrue(Game::HullSpecOf(Game::HullId::Hauler).MountCount() == 0, L"the Hauler is unarmed");
    Assert::IsTrue(Game::HullSpecOf(Game::HullId::Battleship).MountCount() == 5, L"the Battleship carries five");

    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    {
      const Game::HullSpec& spec = Game::HullSpecOf(hull);
      if (spec.immovable)
      {
        Assert::AreEqual(0u, spec.MountCount(), L"an immovable hull is armed");
        Assert::AreEqual(0u, spec.maxHullPoints, L"an immovable hull is destructible");
      }
      else
      {
        Assert::IsTrue(spec.maxHullPoints > 0u, L"a mobile hull cannot be destroyed");
      }
    }
  }

  TEST_METHOD(NoHullOutRangesItsOwnSenses)
  {
    // The gunnery term of the query radius, checked the way the pair filter is: over every subset of
    // the hull table, because the query's numbers are maxima over what is *present* and the worst
    // case is whichever universe makes those maxima smallest. Without the term, a skirmish of
    // Interceptors alone narrows the query to 137.1 m while a LightGun reaches 163.5 m.
    constexpr std::uint32_t SUBSETS = 1u << Game::HULL_COUNT;
    for (std::uint32_t mask = 1; mask < SUBSETS; ++mask)
    {
      Game::NeighbourhoodExtent extent;
      for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
      {
        if ((mask & (1u << hull)) == 0)
          continue;
        const Game::HullSpec& spec = Game::HULL_SPECS[hull];
        if (!spec.collidable)
          continue;
        if (spec.immovable)
          extent.largestStaticRadiusMetres = std::max(extent.largestStaticRadiusMetres, spec.BoundingRadiusMetres());
        else
        {
          extent.largestMobileRadiusMetres = std::max(extent.largestMobileRadiusMetres, spec.BoundingRadiusMetres());
          extent.fastestSpeedMetresPerSec = std::max(extent.fastestSpeedMetresPerSec, spec.maxSpeedMetresPerSec);
        }
      }

      for (std::uint32_t shooter = 0; shooter < Game::HULL_COUNT; ++shooter)
      {
        if ((mask & (1u << shooter)) == 0 || !Game::HULL_SPECS[shooter].collidable)
          continue;
        const Game::HullSpec& spec = Game::HULL_SPECS[shooter];
        if (spec.MountCount() == 0)
          continue;
        const float query = Game::QueryRadiusMetres(spec, extent);
        const float reach = spec.LongestMountRangeMetres() + extent.largestMobileRadiusMetres;
        Assert::IsTrue(query + 0.001f >= reach,
                       std::format(L"hull {} queries {:.1f} m and shoots {:.1f} m", shooter, query, reach).c_str());
      }
    }
  }

  TEST_METHOD(TheGunsSurviveTheRoundTrip)
  {
    Game::Universe universe;
    FacingPair(universe, Game::HullId::Frigate, Game::HullId::Frigate, 200.0f);
    Run(universe, 200);

    std::vector<std::uint8_t> bytes;
    Game::WriteUniverseState(universe, bytes);
    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(bytes, reloaded), L"a mid-battle universe did not reload");
    Assert::AreEqual(universe.ShipCount(), reloaded.ShipCount());

    for (Game::ShipId id = 0; id < universe.ShipCount(); ++id)
    {
      Assert::AreEqual(universe.Ship(id).hullPoints, reloaded.Ship(id).hullPoints, L"hull points were lost");
      for (std::uint32_t at = 0; at < Game::MAX_MOUNTS; ++at)
      {
        Assert::AreEqual(universe.MountsOf(id).mount[at].aimBearingRad, reloaded.MountsOf(id).mount[at].aimBearingRad, L"an aim was lost");
        Assert::AreEqual(universe.MountsOf(id).mount[at].cooldownTicks, reloaded.MountsOf(id).mount[at].cooldownTicks,
                         L"a cooldown was lost");
      }
    }

    // And it goes on producing the same battle, which is what the fields being carried is *for*.
    for (int at = 0; at < 300; ++at)
    {
      universe.Step();
      reloaded.Step();
      Assert::AreEqual(universe.ShipCount(), reloaded.ShipCount(), L"the reloaded battle lost a different ship");
      for (Game::ShipId id = 0; id < universe.ShipCount(); ++id)
        Assert::AreEqual(universe.Ship(id).hullPoints, reloaded.Ship(id).hullPoints, L"the reloaded battle diverged");
    }
  }
};
} // namespace GameLogicTests
