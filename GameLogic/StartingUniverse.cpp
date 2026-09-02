#include "pch.h"
#include "StartingUniverse.h"

#include "Patrol.h"

#include <cstddef>
#include <vector>

namespace Game
{
namespace
{
// The player's fleet, abreast on the plane, in slot 1.
//
// Slot 1 and not "no fleet", and that is not a convenience: selection is fleet-grain (ADR 0049), so
// a starting hull in no fleet is a hull the player cannot take hold of.
//
// FormFleet rather than ComposeFleet, because these ships are already in space. Composing is what
// draws hulls out of a station's ledger, and there is no ledger at tick zero.
void SpawnStartingFleet(Universe& _universe)
{
  // Named apart from HullSpec.h's HULL_COUNT, which is in scope here and would be shadowed (C4459).
  constexpr std::size_t STARTING_HULL_COUNT = std::size(STARTING_FLEET);
  std::vector<ShipId> ships;
  ships.reserve(STARTING_HULL_COUNT);
  for (std::size_t at = 0; at < STARTING_HULL_COUNT; ++at)
  {
    const float x = (static_cast<float>(at) - static_cast<float>(STARTING_HULL_COUNT - 1) * 0.5f) * STARTING_FLEET_SPACING_METRES;
    ships.push_back(_universe.SpawnShip(LocalPos(x, 0.0f), 0.0f, static_cast<std::uint32_t>(STARTING_FLEET[at]), FACTION_PLAYER));
  }

  // The single player this build has, which genesis has to name because there is no login to ask
  // (Design/Archive/OwnerKey-work-order.md). A grep for OWNER_LOCAL finds every place that assumes one.
  (void)_universe.FormFleet(Issuer{OWNER_LOCAL, FACTION_PLAYER}, 0, ships);
}

// One Vanguard station in every system of the galaxy, at VanguardStationSite -- the first planet's
// bearing, pulled in toward the star (StartingUniverse.h says why one, and why there).
//
// Every system is laid out here, from its own seed alone, and the layout is not kept: what the
// universe needs is the positions. What a CLIENT needs is its local system's, and it lays that one
// out again for itself from the same seed and gets the same answer (ADR 0055).
void SpawnVanguardStations(const GalaxyLayout& _galaxy, Universe& _universe)
{
  Universe::StationDesc desc;
  desc.ownerFaction = FACTION_VANGUARD;
  desc.protectorHullId = static_cast<std::uint32_t>(VANGUARD_PROTECTOR_HULL);
  desc.protectorComplement = VANGUARD_PROTECTOR_COMPLEMENT;
  desc.launchEveryTicks = VANGUARD_LAUNCH_EVERY_TICKS;
  desc.targetCap = VANGUARD_TARGET_CAP;

  for (const SystemSite& site : _galaxy.systems)
  {
    const SystemLayout system = LayOutGalaxySystem(site, STARTING_GALAXY, GALAXY_PINS);
    const ShipId structure =
      _universe.SpawnShip(VanguardStationSite(system), 0.0f, static_cast<std::uint32_t>(HullId::Structure), FACTION_VANGUARD);
    _universe.MakeStation(structure, desc);
  }
}

// A gate at each end of every link, each naming the other by the identity that already survives
// leaving a universe (ADR 0047, ADR 0056).
//
// On the Stargate hull, which was authored for exactly this -- immovable, indestructible and flown
// through on purpose (HullSpec.h, Design/Archive/Collision.md 18.2) -- so a gate looks like a door
// rather than a second station when the client draws it.
//
// Two passes, and it has to be two: the row carries the far gate's EntityId, and the far gate does
// not exist while the near one is being spawned. So the structures go down first and the rows are
// made once both ends can be named.
void SpawnGates(const GalaxyLayout& _galaxy, Universe& _universe)
{
  // One entry per end of every link, in link order, so the pairing below is arithmetic rather than a
  // search: ends 2i and 2i+1 are the two halves of link i.
  std::vector<ShipId> ends;
  ends.reserve(_galaxy.links.size() * 2u);

  for (const GateLink& link : _galaxy.links)
  {
    const SystemSite& a = _galaxy.systems[link.systemA];
    const SystemSite& b = _galaxy.systems[link.systemB];
    ends.push_back(_universe.SpawnShip(GateSite(a, b, STARTING_GALAXY), GateHeadingRad(a, b), static_cast<std::uint32_t>(HullId::Stargate),
                                       FACTION_VANGUARD));
    ends.push_back(_universe.SpawnShip(GateSite(b, a, STARTING_GALAXY), GateHeadingRad(b, a), static_cast<std::uint32_t>(HullId::Stargate),
                                       FACTION_VANGUARD));
  }

  for (std::size_t at = 0; at + 1 < ends.size(); at += 2)
  {
    Universe::GateDesc toB;
    toB.destination = _universe.EntityIdOf(ends[at + 1]);
    (void)_universe.MakeGate(ends[at], toB);

    Universe::GateDesc toA;
    toA.destination = _universe.EntityIdOf(ends[at]);
    (void)_universe.MakeGate(ends[at + 1], toA);
  }
}

// The rival, and the patrol that circles it.
void SpawnHostileBase(Universe& _universe)
{
  const UniversePos anchor = LocalPos(HOSTILE_BASE_EAST_METRES, HOSTILE_BASE_NORTH_METRES);
  const ShipId station = _universe.SpawnShip(anchor, 0.0f, static_cast<std::uint32_t>(HullId::Structure), FACTION_VANDAL);

  Universe::StationDesc base;
  base.ownerFaction = FACTION_VANDAL;
  base.protectorComplement = 0;
  _universe.MakeStation(station, base);

  for (std::uint32_t at = 0; at < HOSTILE_PATROL_COUNT; ++at)
  {
    // Spread evenly over the ring -- 0, 4, 8 of twelve -- and headed along it, so the first leg does
    // not begin with a turn. The geometry is Patrol.h's, because the universe steers by the same
    // function and an author doing its own arithmetic would put them somewhere it then walks away
    // from.
    const std::uint32_t index = at * PATROL_RING_WAYPOINTS / HOSTILE_PATROL_COUNT;
    const ShipId ship = _universe.SpawnShip(PatrolRingPoint(anchor, index, HOSTILE_PATROL_RING_METRES), PatrolRingHeadingRad(index),
                                            static_cast<std::uint32_t>(HullId::Interceptor), FACTION_VANDAL);
    _universe.AssignPatrol(ship, station, HOSTILE_PATROL_RING_METRES, HOSTILE_PATROL_CRUISE_MPS);
  }
}
} // namespace

void BuildStartingUniverse(const GalaxyLayout& _galaxy, ShardId _shard, Universe& _outUniverse)
{
  // Before anything spawns, because a ship minted under the wrong shard carries the wrong identity
  // for the rest of its life (ADR 0047).
  _outUniverse.ConfigureShard(_shard);

  // This order is the contract, not a preference. Ship ids are handed out in spawn order, so
  // reordering these four renumbers every ship in the file -- and the fleet is first so that it
  // keeps the ids it has always had.
  SpawnStartingFleet(_outUniverse);
  SpawnVanguardStations(_galaxy, _outUniverse);
  SpawnGates(_galaxy, _outUniverse);
  SpawnHostileBase(_outUniverse);

  // Settled, because the next thing a caller does with this is write it, and a universe that has
  // spawned 197 things and never ticked is not at rest. Without this the file is a faithful record
  // of an intermediate state: identical in its bytes, and one tick later a different universe
  // (Universe::SettleDerivedState).
  _outUniverse.SettleDerivedState();
}
} // namespace Game
