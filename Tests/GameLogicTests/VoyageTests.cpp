#include "pch.h"

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GameLogicTests
{
namespace
{
// A galaxy of systems in a line, far enough apart that no gather, interest radius or path island
// spans two of them, linked one to the next.
//
// Hand-built rather than LayOutGalaxy's, and that is what makes these rows readable: the shipped
// galaxy's shortest route between two far systems is fourteen gates and several thousand metres of
// flying per gate, which is a fine thing to measure and a terrible thing to assert against. A chain
// makes "which hop is next" arithmetic -- from system i toward a higher one it is i + 1 -- so a row
// that fails says which door was taken wrongly rather than only that the fleet ended up elsewhere.
inline constexpr float CHAIN_SYSTEM_SPACING_METRES = 60000.0f;
inline constexpr float CHAIN_GATE_STANDOFF_METRES = 400.0f;

[[nodiscard]] Game::GalaxyLayout ChainGalaxy(std::uint32_t _count)
{
  Game::GalaxyLayout galaxy;
  for (std::uint32_t at = 0; at < _count; ++at)
  {
    Game::SystemSite site;
    site.starPos = Game::LocalPos(CHAIN_SYSTEM_SPACING_METRES * static_cast<float>(at), 0.0f);
    site.cellQ = static_cast<std::int32_t>(at);
    site.systemSeed = 0x5461696Cull + at;
    galaxy.systems.push_back(site);
  }
  for (std::uint32_t at = 0; at + 1 < _count; ++at)
    galaxy.links.push_back(Game::GateLink{at, at + 1});
  return galaxy;
}

// Where a chain galaxy's gate stands: CHAIN_GATE_STANDOFF_METRES from its own star, on the side the
// road leaves by. GateSite's rule with the chain's arithmetic in place of the real bearing.
[[nodiscard]] Game::UniversePos ChainGateSite(const Game::GalaxyLayout& _galaxy, std::uint32_t _in, std::uint32_t _toward)
{
  Game::UniversePos where = _galaxy.systems[_in].starPos;
  Game::Translate(where, (_toward > _in) ? CHAIN_GATE_STANDOFF_METRES : -CHAIN_GATE_STANDOFF_METRES, 0.0f);
  return where;
}

// A gate at each end of every link, each naming the other -- SpawnGates' two passes, over a galaxy
// this file built. Facing away from its own star along the road, which is what puts an arriving
// fleet outside the ring rather than on top of the door (GateHeadingRad).
void SpawnChainGates(Game::Universe& _universe, const Game::GalaxyLayout& _galaxy)
{
  std::vector<Game::ShipId> ends;
  for (const Game::GateLink& link : _galaxy.links)
  {
    ends.push_back(_universe.SpawnShip(ChainGateSite(_galaxy, link.systemA, link.systemB), DirectX::XM_PIDIV2,
                                       static_cast<std::uint32_t>(Game::HullId::Stargate), Game::FACTION_VANGUARD));
    ends.push_back(_universe.SpawnShip(ChainGateSite(_galaxy, link.systemB, link.systemA), -DirectX::XM_PIDIV2,
                                       static_cast<std::uint32_t>(Game::HullId::Stargate), Game::FACTION_VANGUARD));
  }
  for (std::size_t at = 0; at + 1 < ends.size(); at += 2)
  {
    Game::Universe::GateDesc toB;
    toB.destination = _universe.EntityIdOf(ends[at + 1]);
    (void)_universe.MakeGate(ends[at], toB);

    Game::Universe::GateDesc toA;
    toA.destination = _universe.EntityIdOf(ends[at]);
    (void)_universe.MakeGate(ends[at + 1], toA);
  }
}

// A chain galaxy, its gates, and the player's fleet standing in system 0 beside the first door.
struct Chain
{
  Game::GalaxyLayout galaxy;
  std::vector<Game::ShipId> fleet;
};

[[nodiscard]] Chain BuildChain(Game::Universe& _universe, std::uint32_t _systems, std::uint32_t _ships = 2)
{
  Chain chain;
  chain.galaxy = ChainGalaxy(_systems);
  _universe.ConfigureGalaxy(chain.galaxy);
  SpawnChainGates(_universe, chain.galaxy);

  // Clear of the first door by more than a crossing radius -- 545 m for a Corvette at a Stargate,
  // which is GateRangeMetres' arithmetic -- so the first hop is a flight to the gate and not an
  // instant crossing off the spawn point. A fleet that starts inside the radius would make every row
  // below a test of the arrival and none of them a test of the approach.
  for (std::uint32_t at = 0; at < _ships; ++at)
  {
    Game::UniversePos where = chain.galaxy.systems[0].starPos;
    Game::Translate(where, 60.0f * static_cast<float>(at), 2000.0f);
    chain.fleet.push_back(_universe.SpawnShip(where, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER));
  }
  (void)_universe.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, chain.fleet);
  return chain;
}

[[nodiscard]] Game::Universe::FleetOrderResult OrderVoyage(Game::Universe& _universe, const Game::UniversePos& _to)
{
  Game::Universe::FleetCommand command;
  command.kind = Game::FleetOrderKind::Voyage;
  command.point = _to;
  return _universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, command);
}

// Where the fleet in slot 0 is, as a system of the galaxy the universe was configured with, or
// INVALID_SYSTEM_INDEX if it has nothing left in space.
[[nodiscard]] std::uint32_t FleetSystem(const Game::Universe& _universe)
{
  const Game::Universe::FleetId id = _universe.FleetInSlot(Game::OWNER_LOCAL, 0);
  if (id == Game::Universe::INVALID_FLEET_ID)
    return Game::INVALID_SYSTEM_INDEX;
  const Game::Universe::Fleet& fleet = _universe.FleetOf(id);
  for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
  {
    const Game::ShipId member = _universe.Resolve(fleet.members[at]);
    if (member != Game::INVALID_SHIP_ID)
      return _universe.SystemOf(_universe.Ships()[member].posUniverse);
  }
  return Game::INVALID_SYSTEM_INDEX;
}

[[nodiscard]] Game::FleetOrderKind FleetOrder(const Game::Universe& _universe)
{
  const Game::Universe::FleetId id = _universe.FleetInSlot(Game::OWNER_LOCAL, 0);
  return (id == Game::Universe::INVALID_FLEET_ID) ? Game::FleetOrderKind::Idle : _universe.FleetOf(id).orderKind;
}

// Steps until the fleet's order is spent, or until the budget runs out. Returns the ticks it took,
// or _budget if the voyage never ended -- so a row asserts on a number rather than on a hang.
[[nodiscard]] std::uint32_t StepUntilIdle(Game::Universe& _universe, std::uint32_t _budget)
{
  for (std::uint32_t tick = 0; tick < _budget; ++tick)
  {
    if (FleetOrder(_universe) == Game::FleetOrderKind::Idle)
      return tick;
    _universe.Step();
  }
  return _budget;
}

// A generous ceiling rather than a measured one: a hop is about a kilometre of flying at a
// Corvette's cruise, so three of them are a few thousand ticks and this is several times that. A
// row that reaches it has failed to arrive, which is what it is for.
inline constexpr std::uint32_t VOYAGE_TICK_BUDGET = 40000;
} // namespace

TEST_CLASS(VoyageTests)
{
public:
  // The title claim: one order, three gates, and the fleet is somewhere it could not have reached
  // through any single door. What makes it work is that the order lives on the FLEET -- every ship
  // that started the voyage was despawned and respawned three times on the way, and the row is the
  // only thing in the universe that survived all of it (ADR 0056, ADR 0069).
  TEST_METHOD(AVoyageCrossesEveryGateOnTheWay)
  {
    Game::Universe universe;
    const Chain chain = BuildChain(universe, 4);

    Assert::AreEqual(static_cast<std::uint32_t>(0), FleetSystem(universe), L"the fleet did not start in system 0");
    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderVoyage(universe, chain.galaxy.systems[3].starPos),
                   L"a voyage across a connected chain was refused");
    Assert::IsTrue(Game::FleetOrderKind::Voyage == FleetOrder(universe), L"the row does not hold the voyage it accepted");

    const std::uint32_t ticks = StepUntilIdle(universe, VOYAGE_TICK_BUDGET);
    Assert::IsTrue(ticks < VOYAGE_TICK_BUDGET, L"the voyage never ended");
    Assert::AreEqual(static_cast<std::uint32_t>(3), FleetSystem(universe), L"the fleet did not end up in the system it was ordered to");
    Assert::IsTrue(Game::FleetOrderKind::Idle == FleetOrder(universe), L"an arrived fleet is still holding its voyage");

    // Whole, and the same ships: a crossing is a despawn and a spawn under one identity, so the
    // fleet that arrives is the fleet that left rather than a copy of it.
    const Game::Universe::Fleet& fleet = universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0));
    Assert::AreEqual(static_cast<std::uint32_t>(chain.fleet.size()), fleet.memberCount, L"the fleet lost a member on the way");
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      Assert::AreNotEqual(Game::INVALID_SHIP_ID, universe.Resolve(fleet.members[at]), L"a member of an arrived fleet does not resolve");
      Assert::AreEqual(static_cast<std::uint32_t>(3), universe.SystemOf(universe.Ships()[universe.Resolve(fleet.members[at])].posUniverse),
                       L"a member arrived in a different system from its fleet");
    }
  }

  // The one hop at a time claim, watched. A fleet under a voyage is never in a system it has no
  // business in and never skips one: the route is the chain, so the systems it stands in are 0, 1,
  // 2, 3 in that order and no other.
  TEST_METHOD(AVoyageVisitsEverySystemOnTheRouteInOrder)
  {
    Game::Universe universe;
    const Chain chain = BuildChain(universe, 4);
    (void)OrderVoyage(universe, chain.galaxy.systems[3].starPos);

    std::vector<std::uint32_t> visited{FleetSystem(universe)};
    for (std::uint32_t tick = 0; tick < VOYAGE_TICK_BUDGET && FleetOrder(universe) != Game::FleetOrderKind::Idle; ++tick)
    {
      universe.Step();
      const std::uint32_t now = FleetSystem(universe);
      if (now != visited.back())
        visited.push_back(now);
    }

    Assert::AreEqual(static_cast<std::size_t>(4), visited.size(), L"the fleet did not stand in exactly the four systems of the chain");
    for (std::uint32_t at = 0; at < visited.size(); ++at)
      Assert::AreEqual(at, visited[at], L"the fleet crossed into a system that is not the next one on the route");
  }

  // Planned from where the fleet IS, which is the property that makes a voyage survive its own save
  // file: nothing about the route is written down, so there is nothing to restore. The reloaded
  // universe is told where the stars are -- that is content, from the seed the header carries -- and
  // the fleet finishes the crossing it was in the middle of.
  TEST_METHOD(AVoyageSurvivesASaveTakenInTheMiddleOfIt)
  {
    Game::Universe universe;
    const Chain chain = BuildChain(universe, 4);
    (void)OrderVoyage(universe, chain.galaxy.systems[3].starPos);

    // Far enough in to have crossed at least one door, so the saved row is a voyage under way
    // rather than one that has not started.
    for (std::uint32_t tick = 0; tick < VOYAGE_TICK_BUDGET && FleetSystem(universe) == 0; ++tick)
      universe.Step();
    Assert::AreEqual(static_cast<std::uint32_t>(1), FleetSystem(universe), L"the fleet did not reach the second system");
    universe.SettleDerivedState();

    std::vector<std::uint8_t> saved;
    Game::WriteUniverseState(universe, saved);

    Game::Universe reloaded;
    Assert::IsTrue(Game::ReadUniverseState(saved, reloaded), L"a universe holding a voyage was refused by its own reader");
    Assert::IsTrue(Game::FleetOrderKind::Voyage == FleetOrder(reloaded), L"the voyage did not survive the state codec");
    reloaded.ConfigureGalaxy(chain.galaxy);

    Assert::IsTrue(StepUntilIdle(reloaded, VOYAGE_TICK_BUDGET) < VOYAGE_TICK_BUDGET, L"a reloaded voyage never ended");
    Assert::AreEqual(static_cast<std::uint32_t>(3), FleetSystem(reloaded), L"a reloaded voyage did not finish where it was going");
  }

  // Ordering a fleet to where it already stands is an arrival, not an error and not a voyage of no
  // hops: the order is accepted, the row is left Idle, and nothing is left holding a destination it
  // has already reached.
  TEST_METHOD(AVoyageToTheSystemItIsInIsAnArrival)
  {
    Game::Universe universe;
    const Chain chain = BuildChain(universe, 4);

    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderVoyage(universe, chain.galaxy.systems[0].starPos),
                   L"a voyage to the fleet's own system was refused");
    Assert::IsTrue(Game::FleetOrderKind::Idle == FleetOrder(universe), L"a voyage to nowhere left an order standing");
  }

  // Two ways there can be no road, and one refusal for both -- and, in each, a standing order that
  // was there before is exactly as it was afterwards. That is the rule every gate in
  // IssueFleetOrder follows and the reason the hop is planned before the row is touched.
  TEST_METHOD(AVoyageWithNoRouteIsRefusedAndChangesNothing)
  {
    // A universe nobody told where the stars are. It is the fail-closed case: a fleet that will not
    // leave beats one sent at a door picked out of an empty table.
    Game::Universe blind;
    const Chain chain = BuildChain(blind, 4);
    blind.ConfigureGalaxy(Game::GalaxyLayout{});
    Assert::IsTrue(Game::Universe::FleetOrderResult::NoRoute == OrderVoyage(blind, chain.galaxy.systems[3].starPos),
                   L"a universe with no galaxy accepted a voyage");

    // A galaxy in two pieces. LinkGates cannot produce one -- the relative neighborhood graph
    // contains a spanning tree (ADR 0055) -- but a hand-built layout can, and the refusal has to be
    // the same one.
    Game::Universe split;
    Game::GalaxyLayout islands = ChainGalaxy(4);
    islands.links.erase(islands.links.begin() + 1); // 0-1 and 2-3, with nothing between
    split.ConfigureGalaxy(islands);
    SpawnChainGates(split, islands);
    std::vector<Game::ShipId> ships{
      split.SpawnShip(islands.systems[0].starPos, 0.0f, static_cast<std::uint32_t>(Game::HullId::Corvette), Game::FACTION_PLAYER)};
    (void)split.FormFleet(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, ships);

    Game::Universe::FleetCommand move;
    move.kind = Game::FleetOrderKind::Move;
    move.point = Game::LocalPos(300.0f, 300.0f);
    (void)split.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, move);

    Assert::IsTrue(Game::Universe::FleetOrderResult::NoRoute == OrderVoyage(split, islands.systems[3].starPos),
                   L"a voyage into another component was accepted");
    Assert::IsTrue(Game::FleetOrderKind::Move == FleetOrder(split), L"a refused voyage replaced the order it should have left alone");
    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered == OrderVoyage(split, islands.systems[1].starPos),
                   L"a voyage inside the fleet's own component was refused");
  }

  // The stand-down (Design/GalaxyMap.md 6.5). A road that stops existing mid-voyage leaves the fleet
  // whole, in a system, with nothing to do -- never mid-crossing and never flying at a door that is
  // not there. Nothing in the shipped game can destroy a gate; the rule is held here so that the day
  // something can, this row is what says what happens.
  TEST_METHOD(AVoyageStandsDownWhenItsRoadIsGone)
  {
    Game::Universe universe;
    const Chain chain = BuildChain(universe, 4);
    (void)OrderVoyage(universe, chain.galaxy.systems[3].starPos);

    for (std::uint32_t tick = 0; tick < VOYAGE_TICK_BUDGET && FleetSystem(universe) == 0; ++tick)
      universe.Step();
    Assert::AreEqual(static_cast<std::uint32_t>(1), FleetSystem(universe), L"the fleet did not reach the second system");

    // The far end of the next hop's road, taken out from under it. The near door is still standing,
    // which is the interesting half: the fleet must refuse a door that leads nowhere rather than
    // fly at it and be despawned into a system that has no gate to put it down at.
    const Game::UniversePos site = ChainGateSite(chain.galaxy, 2, 1);
    for (std::uint32_t at = 0; at < universe.GateCount(); ++at)
    {
      const Game::ShipId structure = universe.Resolve(universe.GateOf(at).structure);
      if (structure != Game::INVALID_SHIP_ID && Game::Distance(universe.Ships()[structure].posUniverse, site) < 1.0f)
        (void)universe.DespawnShip(universe.GateOf(at).structure);
    }

    Assert::IsTrue(StepUntilIdle(universe, VOYAGE_TICK_BUDGET) < VOYAGE_TICK_BUDGET, L"a voyage with no road left never stood down");
    Assert::AreEqual(static_cast<std::uint32_t>(1), FleetSystem(universe), L"a stood-down fleet is not in the system it stopped in");

    const Game::Universe::Fleet& fleet = universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0));
    Assert::AreEqual(static_cast<std::uint32_t>(chain.fleet.size()), fleet.memberCount, L"a stood-down fleet lost a member");
    Assert::IsTrue(Game::INVALID_SHIP_ID == universe.Resolve(fleet.orderGate), L"a stood-down fleet is still holding a door");
  }

  // An explicit order outranks a voyage, exactly as it outranks every other standing order: the
  // fleet stops crossing gates and does what it was told instead. There is nothing left over,
  // because there was never anything but the row.
  TEST_METHOD(AnExplicitOrderEndsAVoyage)
  {
    Game::Universe universe;
    const Chain chain = BuildChain(universe, 4);
    (void)OrderVoyage(universe, chain.galaxy.systems[3].starPos);

    // Interrupted a system into the crossing, so what is being replaced is a voyage under way
    // rather than one that has not begun.
    for (std::uint32_t tick = 0; tick < VOYAGE_TICK_BUDGET && FleetSystem(universe) == 0; ++tick)
      universe.Step();
    const std::uint32_t interrupted = FleetSystem(universe);
    Assert::AreEqual(static_cast<std::uint32_t>(1), interrupted, L"the fleet did not reach the second system");

    // Somewhere in the system it is standing in, so that obeying the move cannot itself carry the
    // fleet across a boundary and make this row pass for the wrong reason.
    const Game::Universe::Fleet& fleet = universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0));
    Game::UniversePos nearby = universe.Ships()[universe.Resolve(fleet.members[0])].posUniverse;
    Game::Translate(nearby, 0.0f, 300.0f);

    Game::Universe::FleetCommand move;
    move.kind = Game::FleetOrderKind::Move;
    move.point = nearby;
    Assert::IsTrue(Game::Universe::FleetOrderResult::Ordered ==
                     universe.IssueFleetOrder(Game::Issuer{Game::OWNER_LOCAL, Game::FACTION_PLAYER}, 0, move),
                   L"a move over a voyage was refused");
    Assert::IsTrue(Game::INVALID_SHIP_ID == universe.Resolve(fleet.orderGate), L"a replaced voyage left its door in the row");

    for (std::uint32_t tick = 0; tick < 3000; ++tick)
      universe.Step();
    Assert::IsTrue(Game::FleetOrderKind::Move == FleetOrder(universe), L"a voyage came back after an explicit order replaced it");
    Assert::AreEqual(interrupted, FleetSystem(universe), L"the fleet crossed a gate after being told to move");
  }

  // A member lost on the tick the fleet crosses, which is the one moment the voyage pass runs on a
  // row the fleet pass has not compacted yet.
  //
  // StepVoyages is the first thing in the tree to lower a fleet order from INSIDE a tick -- every
  // caller before it ran between ticks or after the prune, so "the fleet's position" could be read
  // off members[0] and be a live ship by accident. Here it is not: the ship died last tick, the
  // prune is four passes away, and the approach for the next hop is being laid now. The row exists
  // because the fix (read the first live member instead) is invisible from outside until this
  // happens.
  TEST_METHOD(AVoyageOutlivesAMemberLostOnACrossing)
  {
    Game::Universe universe;
    const Chain chain = BuildChain(universe, 4, 3);
    (void)OrderVoyage(universe, chain.galaxy.systems[3].starPos);

    // Stepped until every live member is inside the door it was sent at, so the NEXT tick is the
    // one that crosses -- and the loss is timed into it.
    bool crossingNow = false;
    for (std::uint32_t tick = 0; tick < VOYAGE_TICK_BUDGET && !crossingNow; ++tick)
    {
      const Game::Universe::Fleet& row = universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0));
      const Game::ShipId gate = universe.Resolve(row.orderGate);
      if (gate != Game::INVALID_SHIP_ID)
      {
        crossingNow = true;
        for (std::uint32_t at = 0; at < row.memberCount && crossingNow; ++at)
        {
          const Game::ShipId member = universe.Resolve(row.members[at]);
          if (member == Game::INVALID_SHIP_ID)
            continue;
          crossingNow =
            Game::Distance(universe.Ships()[member].posUniverse, universe.Ships()[gate].posUniverse) <=
            Game::GateRangeMetres(Game::HullSpecOf(universe.Ships()[gate].hullId), Game::HullSpecOf(universe.Ships()[member].hullId));
        }
      }
      if (!crossingNow)
        universe.Step();
    }
    Assert::IsTrue(crossingNow, L"the fleet never reached the first door");

    // The FIRST entry of the row, which is the one a reader is tempted to call "the fleet".
    const Game::Universe::Fleet& row = universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0));
    Assert::IsTrue(universe.DespawnShip(row.members[0]), L"a live member refused to despawn");
    universe.Step(); // crosses, and names the next door, on a row nothing has compacted

    Assert::IsTrue(StepUntilIdle(universe, VOYAGE_TICK_BUDGET) < VOYAGE_TICK_BUDGET, L"a voyage that lost a member never ended");
    Assert::AreEqual(static_cast<std::uint32_t>(3), FleetSystem(universe), L"a voyage that lost a member did not finish");
    Assert::AreEqual(static_cast<std::uint32_t>(2), universe.FleetOf(universe.FleetInSlot(Game::OWNER_LOCAL, 0)).memberCount,
                     L"the fleet did not arrive with the members it had left");
  }

  // The door for a hop is found by where the road comes OUT, not by where it goes in. A system with
  // two gates in it is the case that separates the two: only one of them arrives in the system the
  // route named, and picking by proximity or by bearing would be a coin toss between them.
  TEST_METHOD(TheGateForAHopIsTheOneThatComesOutInTheRightSystem)
  {
    Game::Universe universe;
    const Chain chain = BuildChain(universe, 4);

    // System 1 holds two doors: one back to 0, one on to 2.
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, universe.GateBetween(1, 0), L"the road back out of system 1 was not found");
    Assert::AreNotEqual(Game::INVALID_SHIP_ID, universe.GateBetween(1, 2), L"the road onward out of system 1 was not found");
    Assert::AreNotEqual(universe.GateBetween(1, 0), universe.GateBetween(1, 2), L"both roads out of system 1 are the same door");

    // A pair with no link between them has no door, whichever way round it is asked, and neither
    // does a system that is not one.
    Assert::AreEqual(Game::INVALID_SHIP_ID, universe.GateBetween(0, 2), L"a door was found for a link that does not exist");
    Assert::AreEqual(Game::INVALID_SHIP_ID, universe.GateBetween(0, 99), L"a door was found to a system that is not one");
    Assert::AreEqual(Game::INVALID_SHIP_ID, universe.GateBetween(1, 1), L"a system has a door to itself");

    // And a universe that was never told where the stars are has no doors at all, which is what
    // makes NoRoute the honest answer there rather than an arbitrary gate.
    Game::Universe blind;
    SpawnChainGates(blind, chain.galaxy);
    Assert::AreEqual(Game::INVALID_SHIP_ID, blind.GateBetween(0, 1), L"a universe with no galaxy found a door anyway");
  }
};
} // namespace GameLogicTests
