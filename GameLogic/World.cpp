#include "pch.h"
#include "World.h"

#include "Collision.h"
#include "HullSpec.h"
#include "Movement.h"
#include "SimTuning.h"

#include <cstddef>

using namespace DirectX;

namespace Game
{
ShipId World::SpawnShip(const WorldPos& _posWorld, float _headingRad, std::uint32_t _hullId, FactionId _factionId)
{
  ShipState ship;
  ship.posWorld = _posWorld;
  ship.prevPos = _posWorld;
  ship.headingRad = _headingRad;
  ship.prevHeading = _headingRad;
  ship.hullId = _hullId;
  ship.factionId = _factionId;

  const ShipId id = static_cast<ShipId>(m_ships.size());
  m_ships.push_back(ship);

  std::uint32_t slot = 0;
  if (!m_freeSlots.empty())
  {
    slot = m_freeSlots.back();
    m_freeSlots.pop_back();
  }
  else
  {
    slot = static_cast<std::uint32_t>(m_slots.size());
    m_slots.emplace_back();
  }
  m_slots[slot].ship = id;
  // Generation 0 is the null handle, so a slot's first issue is 1 and a wrap skips back past it.
  if (m_slots[slot].generation == 0)
    m_slots[slot].generation = 1;
  m_shipSlot.push_back(slot);
  m_routes.emplace_back();
  m_patrols.emplace_back();
  m_dockings.emplace_back();

  // Only an immovable can change the static set. A spawn appends, so no existing id moves and
  // nothing already in the store is disturbed -- which is why this needs no id-shift caveat and the
  // despawn below does (Design/MmoScalabilityReview.md U4). The gate is deliberately looser than
  // the store's own filter, which is immovable *and* collidable: a Stargate costs one rebuild it did
  // not need, and a gate that is a superset of the filter cannot miss one that was needed, which is
  // the direction to be wrong in if the two ever drift apart.
  if (HullSpecOf(_hullId).immovable)
    m_staticIndexDirty = true;
  return id;
}

bool World::DespawnShip(ShipHandle _handle, DespawnCause _cause)
{
  const ShipId id = Resolve(_handle);
  if (id == INVALID_SHIP_ID)
    return false;

  // Logged before the slot is retired, so the publisher can tell this death from a departure. A
  // despawn no subscriber held is dropped where the publisher intersects it with what that
  // subscriber knew: you cannot be told of the death of something you never knew about
  // (Design/Archive/Hostiles.md 4.4).
  m_despawnLog.push_back(DespawnRecord{_handle, _cause});

  // Read before anything moves. Two ships can change the static set here: the one being removed, and
  // the one swap-and-pop moves into its place -- because the static store holds ShipIds, and the
  // moved ship's id changes even though the ship does not (ADR 0005). Marking every despawn dirty
  // got that right by brute force and cost a whole-fleet rescan plus a universe-wide replan on every
  // death; these two checks get it right for the cases that are actually it.
  const bool removedWasStatic = HullSpecOf(m_ships[id].hullId).immovable;

  const ShipId last = static_cast<ShipId>(m_ships.size() - 1);
  const bool movedIsStatic = (id != last) && HullSpecOf(m_ships[last].hullId).immovable;
  if (removedWasStatic || movedIsStatic)
    m_staticIndexDirty = true;

  if (id != last)
  {
    m_ships[id] = m_ships[last];
    m_routes[id] = m_routes[last];
    m_patrols[id] = m_patrols[last];
    m_dockings[id] = m_dockings[last];
    m_shipSlot[id] = m_shipSlot[last];
    m_slots[m_shipSlot[id]].ship = id; // the moved ship keeps its slot, and its handles keep working
  }
  m_ships.pop_back();
  m_routes.pop_back();
  m_patrols.pop_back();
  m_dockings.pop_back();
  m_shipSlot.pop_back();

  Slot& freed = m_slots[_handle.slot];
  freed.ship = INVALID_SHIP_ID;
  ++freed.generation;
  if (freed.generation == 0)
    freed.generation = 1;
  m_freeSlots.push_back(_handle.slot);
  return true;
}

Standing World::StandingOf(FactionId _owner, FactionId _other) const noexcept
{
  // Out of range is Hostile, not Neutral. Nobody authored that faction, every caller here is a gate
  // or a warning colour, and the failure directions are not symmetric: a stranger refused a dock is
  // a bug report, a stranger admitted is a hole.
  if (_owner >= FACTION_LIMIT || _other >= FACTION_LIMIT)
    return Standing::Hostile;
  return m_standings.rows[_owner][_other];
}

std::uint8_t World::HostileMaskFor(FactionId _viewer) const noexcept
{
  std::uint8_t mask = 0;
  for (std::uint32_t faction = 0; faction < FACTION_LIMIT; ++faction)
  {
    // Their opinion of the viewer, not the viewer's of them. The client colours a faction that has
    // turned on *it*, and refuses to offer a dock it would be refused (Design/Stations.md 4.3, 9.3).
    if (StandingOf(static_cast<FactionId>(faction), _viewer) == Standing::Hostile)
      mask |= static_cast<std::uint8_t>(1u << faction);
  }
  return mask;
}

void World::RecordAggression(ShipHandle _attacker, StationId _station)
{
  const ShipId attacker = Resolve(_attacker);
  if (attacker == INVALID_SHIP_ID || _station >= m_stations.size())
    return; // an attacker that is already gone, or a station that never was: nothing to judge

  const FactionId attackerFaction = m_ships[attacker].factionId;
  const FactionId owner = m_stations[_station].ownerFaction;
  if (attackerFaction >= FACTION_LIMIT || owner >= FACTION_LIMIT)
    return;

  // Keyed on the faction, not on the ship: one subscriber is one faction today, so "your faction is
  // criminal" and "you are criminal" are the same sentence. The day two players share a faction this
  // widens to per-player rows exactly as ADR 0014's authority gate does (Design/Stations.md 4.2, 12).
  m_standings.rows[owner][attackerFaction] = Standing::Hostile;
}

World::StationId World::MakeStation(ShipId _structure, const StationDesc& _desc)
{
  if (_structure >= m_ships.size())
    return INVALID_STATION_ID;

  Station station;
  station.structure = HandleOf(_structure);
  station.ownerFaction = _desc.ownerFaction;
  station.protectorHullId = _desc.protectorHullId;
  station.protectorComplement = _desc.protectorComplement;
  station.launchEveryTicks = _desc.launchEveryTicks;
  station.targetCap = _desc.targetCap;

  const StationId id = static_cast<StationId>(m_stations.size());
  m_stations.push_back(std::move(station));
  return id;
}

World::StationId World::StationAt(ShipId _id) const noexcept
{
  if (_id >= m_ships.size())
    return INVALID_STATION_ID;

  // Through Resolve rather than by comparing stored ids, because swap-and-pop moves ids and a row
  // holding a raw id would silently name whichever ship arrived in that slot (ADR 0005).
  for (std::size_t at = 0; at < m_stations.size(); ++at)
  {
    if (Resolve(m_stations[at].structure) == _id)
      return static_cast<StationId>(at);
  }
  return INVALID_STATION_ID;
}

const World::Station& World::StationOf(StationId _id) const noexcept
{
  return m_stations[_id];
}

std::span<const DespawnRecord> World::DespawnsSince(std::uint64_t _cursor) const noexcept
{
  if (_cursor <= m_despawnBase)
    return m_despawnLog;
  const std::uint64_t offset = _cursor - m_despawnBase;
  if (offset >= m_despawnLog.size())
    return {};
  return std::span<const DespawnRecord>(m_despawnLog).subspan(static_cast<std::size_t>(offset));
}

void World::TrimDespawnsBefore(std::uint64_t _cursor) noexcept
{
  if (_cursor <= m_despawnBase)
    return;
  const std::uint64_t offset = _cursor - m_despawnBase;
  if (offset >= m_despawnLog.size())
  {
    // Trimming past the head is not an error and must not move the base past it: a cursor ahead of
    // the log names deaths that have not happened, and the next one to happen is still theirs.
    m_despawnBase += m_despawnLog.size();
    m_despawnLog.clear();
    return;
  }
  m_despawnLog.erase(m_despawnLog.begin(), m_despawnLog.begin() + static_cast<std::ptrdiff_t>(offset));
  m_despawnBase = _cursor;
}

ShipHandle World::HandleOf(ShipId _id) const noexcept
{
  if (_id >= m_ships.size())
    return {};
  const std::uint32_t slot = m_shipSlot[_id];
  return ShipHandle{slot, m_slots[slot].generation};
}

ShipId World::Resolve(ShipHandle _handle) const noexcept
{
  if (_handle.generation == 0 || _handle.slot >= m_slots.size())
    return INVALID_SHIP_ID;
  const Slot& slot = m_slots[_handle.slot];
  return (slot.generation == _handle.generation) ? slot.ship : INVALID_SHIP_ID;
}

void World::ConfigureIndex(const SpatialIndex::Desc& _desc)
{
  m_index.Configure(_desc);
  m_staticIndexDirty = true;
}

std::span<const Neighbour> World::NeighboursOf(ShipId _id) const noexcept
{
  if (_id >= m_neighbourCount.size())
    return {};
  return std::span<const Neighbour>(m_neighbours).subspan(m_neighbourStart[_id], m_neighbourCount[_id]);
}

std::span<const WorldPos> World::RouteOf(ShipId _id) const noexcept
{
  if (_id >= m_routes.size())
    return {};
  const Route& route = m_routes[_id];
  return std::span<const WorldPos>(route.waypoint, route.count).subspan(route.cursor);
}

void World::AssignPatrol(ShipId _ship, ShipId _anchorStation, float _ringRadiusMetres, float _cruiseSpeedMetresPerSec)
{
  if (_ship >= m_ships.size() || _anchorStation >= m_ships.size() || _ship == _anchorStation)
    return;

  Patrol& patrol = m_patrols[_ship];
  patrol.anchor = HandleOf(_anchorStation);
  patrol.ringRadiusMetres = _ringRadiusMetres;
  patrol.cruiseSpeedMetresPerSec = _cruiseSpeedMetresPerSec;
  // The nearest point *minus one*, because the pass issues waypointIndex + 1: the first leg then
  // goes to the point the ship is already nearest, rather than back round the ring to zero.
  const std::uint32_t nearest = NearestPatrolRingIndex(m_ships[_anchorStation].posWorld, m_ships[_ship].posWorld);
  patrol.waypointIndex = (nearest + PATROL_RING_WAYPOINTS - 1) % PATROL_RING_WAYPOINTS;
  patrol.active = true;
}

const World::Patrol& World::PatrolOf(ShipId _id) const noexcept
{
  static constexpr Patrol NONE;
  return (_id < m_patrols.size()) ? m_patrols[_id] : NONE;
}

namespace
{
// Where a docking ship is sent: the point on its current bearing from the station, at exactly its
// own dock range.
//
// Its current bearing rather than a fixed approach lane, because there is no bay geometry this
// phase and a lane would be a promise the station cannot keep -- a hull captured at the skin,
// without ceremony, is what docking is until stations have an inside (Design/Stations.md 14). Its
// *own* range rather than a shared one, so a Carrier is not asked to fly to an Interceptor's
// doorstep and shove its way there against the separation pass.
[[nodiscard]] WorldPos DockApproachPoint(const WorldPos& _station, const WorldPos& _ship, float _dockRangeMetres) noexcept
{
  const float dx = OffsetX(_station, _ship);
  const float dz = OffsetZ(_station, _ship);
  const float distance = std::sqrt(dx * dx + dz * dz);

  WorldPos point = _station;
  if (distance <= 0.0001f)
  {
    // Concentric with the station, which cannot happen with a hull in the way but is not worth a
    // divide by zero. Due north is as good as any bearing and is the one PatrolRingPoint calls 0.
    Translate(point, 0.0f, _dockRangeMetres);
    return point;
  }
  Translate(point, dx / distance * _dockRangeMetres, dz / distance * _dockRangeMetres);
  return point;
}
} // namespace

const World::Docking& World::DockingOf(ShipId _id) const noexcept
{
  return m_dockings[_id];
}

World::DockOrderResult World::IssueDockOrder(std::span<const ShipId> _ships, ShipId _station, FactionId _issuerFaction)
{
  const StationId station = StationAt(_station);
  if (station == INVALID_STATION_ID)
    return DockOrderResult::NotAStation;

  // The owner's opinion of the issuer, and the whole order stands or falls on it: an aggressor is
  // not allowed to dock. Refused means nothing changes -- no ship diverts, no intent is set -- and
  // the client's affordance said so before sending, so the silent wire costs nothing.
  if (StandingOf(m_stations[station].ownerFaction, _issuerFaction) == Standing::Hostile)
    return DockOrderResult::RefusedStanding;

  // An order plans a route, so the islands have to be current here for the same reason
  // IssueMoveOrder rebuilds them.
  RebuildStaticIfDirty();

  const ShipHandle stationHandle = m_stations[station].structure;
  const HullSpec& stationHull = HullSpecOf(m_ships[_station].hullId);

  for (const ShipId id : _ships)
  {
    // Somebody else's ship is dropped the way a stale id already is, and the rest of the list still
    // goes (Design/Archive/Hostiles.md 4.3).
    if (id >= m_ships.size() || m_ships[id].factionId != _issuerFaction)
      continue;
    if (id == _station)
      continue; // a station does not dock at itself

    ShipState& ship = m_ships[id];
    m_dockings[id].station = stationHandle;
    m_dockings[id].active = true;

    // An explicit order outranks a standing behavior, the line IssueMoveOrder already has.
    m_patrols[id].active = false;

    ship.order = OrderState::Moving;
    ship.orderHasFacing = false;
    ship.orderSpeedCapMetresPerSec = 0.0f;

    const float approachRange = DockApproachRangeMetres(stationHull, HullSpecOf(ship.hullId));
    const WorldPos approach = DockApproachPoint(m_ships[_station].posWorld, ship.posWorld, approachRange);
    PlanRoute(id, approach, HullSpecOf(ship.hullId).BoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
  }
  return DockOrderResult::Ordered;
}

void World::StepDockings()
{
  RebuildStaticIfDirty();

  m_captureScratch.clear();
  for (ShipId id = 0; id < ShipCount(); ++id)
  {
    Docking& docking = m_dockings[id];
    if (!docking.active)
      continue;

    const ShipId structure = Resolve(docking.station);
    const StationId station = (structure == INVALID_SHIP_ID) ? INVALID_STATION_ID : StationAt(structure);
    if (station == INVALID_STATION_ID)
    {
      docking.active = false; // the station is gone, or is no longer one: stand down where you are
      continue;
    }

    ShipState& ship = m_ships[id];
    const HullSpec& stationHull = HullSpecOf(m_ships[structure].hullId);
    const HullSpec& shipHull = HullSpecOf(ship.hullId);
    if (Distance(m_ships[structure].posWorld, ship.posWorld) <= DockRangeMetres(stationHull, shipHull))
    {
      // Checked again here, and not only at order time. This closes the window between an accepted
      // order and an aggression recorded during the flight: the door is guarded, not just the
      // doorbell (Design/Stations.md 7.3).
      if (StandingOf(m_stations[station].ownerFaction, ship.factionId) == Standing::Hostile)
      {
        docking.active = false;
        continue;
      }
      // Everything the capture needs, taken now: after the walk this ship's id may name another.
      // Hull and faction are the whole of what a ship is today, so they are the whole ledger row --
      // when undocking arrives it spawns a fresh ship from it, with a fresh handle
      // (Design/Stations.md 7.3).
      m_captureScratch.push_back(Capture{HandleOf(id), station, ship.hullId, ship.factionId});
      continue;
    }

    if (ship.order != OrderState::Idle)
      continue; // still flying the last leg; arrival is the order machinery's, not this pass's

    // Re-issued whenever the ship goes Idle short of range -- shoved off by traffic, replanned,
    // blocked -- so docking is patient the way a patrol is, with no arrival logic of its own.
    ship.order = OrderState::Moving;
    ship.orderHasFacing = false;
    ship.orderSpeedCapMetresPerSec = 0.0f;
    const WorldPos approach = DockApproachPoint(m_ships[structure].posWorld, ship.posWorld, DockApproachRangeMetres(stationHull, shipHull));
    PlanRoute(id, approach, shipHull.BoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
  }

  // After the walk, in collection order -- which is array order, which is deterministic. During it
  // would make the visit order depend on who docked, because DespawnShip swap-and-pops four
  // parallel tables. A station id stays valid across this loop: stations do not despawn.
  for (const Capture& capture : m_captureScratch)
  {
    m_stations[capture.station].docked.push_back(DockedShip{capture.hullId, capture.factionId});
    (void)DespawnShip(capture.ship, DespawnCause::Docked);
  }
  m_captureScratch.clear();
}

void World::StepPatrols()
{
  // A patrol issues an order, and an order plans a route, so the islands have to be current here for
  // the same reason IssueMoveOrder rebuilds them: the first leg is planned on the tick after the
  // spawn.
  RebuildStaticIfDirty();

  for (ShipId id = 0; id < ShipCount(); ++id)
  {
    Patrol& patrol = m_patrols[id];
    if (!patrol.active)
      continue;

    ShipState& ship = m_ships[id];
    if (ship.order != OrderState::Idle)
      continue; // still flying the last leg; arrival is the order machinery's, not this pass's

    const ShipId anchor = Resolve(patrol.anchor);
    if (anchor == INVALID_SHIP_ID)
    {
      patrol.active = false; // the station died; the ship stands down where it is
      continue;
    }

    patrol.waypointIndex = (patrol.waypointIndex + 1) % PATROL_RING_WAYPOINTS;
    ship.order = OrderState::Moving;
    ship.orderHasFacing = false;
    ship.orderSpeedCapMetresPerSec = patrol.cruiseSpeedMetresPerSec;
    const WorldPos waypoint = PatrolRingPoint(m_ships[anchor].posWorld, patrol.waypointIndex, patrol.ringRadiusMetres);
    PlanRoute(id, waypoint, HullSpecOf(ship.hullId).BoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
  }
}

void World::PlanRoute(ShipId _id, const WorldPos& _destination, float _requiredClearanceMetres)
{
  ShipState& ship = m_ships[_id];
  Route& route = m_routes[_id];

  const bool complete = m_pathIslands.FindPath(ship.posWorld, _destination, _requiredClearanceMetres, m_routeScratch);
  route.destination = _destination;
  route.requiredClearanceMetres = _requiredClearanceMetres;
  route.count = std::min<std::uint32_t>(MAX_PATH_WAYPOINTS, static_cast<std::uint32_t>(m_routeScratch.size()));
  for (std::uint32_t at = 0; at < route.count; ++at)
    route.waypoint[at] = m_routeScratch[at];
  route.cursor = 0;
  route.legStart = ship.posWorld;
  route.gridVersion = m_pathIslands.Version();
  route.reachesDestination = complete;

  // A grid that declined to build, or a start with nowhere to go, leaves the destination itself as
  // the only waypoint -- which is exactly the behaviour before this phase existed.
  ship.steerTargetPos = (route.count > 0) ? route.waypoint[0] : _destination;
}

void World::AdvanceRoute(ShipId _id)
{
  ShipState& ship = m_ships[_id];
  Route& route = m_routes[_id];
  if (ship.order != OrderState::Moving || route.count == 0)
    return;

  const HullSpec& hull = HullSpecOf(ship.hullId);
  const bool arrived = Distance(ship.posWorld, ship.steerTargetPos) <= ArrivalRadiusMetres(hull);
  if (arrived && route.cursor + 1 < route.count)
  {
    ++route.cursor;
    route.legStart = ship.posWorld;
    ship.steerTargetPos = route.waypoint[route.cursor];
    return;
  }
  if (arrived && !route.reachesDestination)
  {
    PlanRoute(_id, route.destination, route.requiredClearanceMetres); // the rest of a route too long for one list
    return;
  }

  // A route that was planned against architecture that has since changed is not a route any more.
  if (route.gridVersion != m_pathIslands.Version())
  {
    PlanRoute(_id, route.destination, route.requiredClearanceMetres);
    return;
  }

  // And one the ship has been pushed well off. Only where there is a route to be off: with a single
  // waypoint the ship is steering at the destination and there is no leg to deviate from.
  if (route.count > 1)
  {
    const float legX = OffsetX(route.legStart, ship.steerTargetPos);
    const float legZ = OffsetZ(route.legStart, ship.steerTargetPos);
    const float legLengthSquared = legX * legX + legZ * legZ;
    if (legLengthSquared > 1e-4f)
    {
      const float alongX = OffsetX(route.legStart, ship.posWorld);
      const float alongZ = OffsetZ(route.legStart, ship.posWorld);
      const float along = std::clamp((alongX * legX + alongZ * legZ) / legLengthSquared, 0.0f, 1.0f);
      const float offX = alongX - legX * along;
      const float offZ = alongZ - legZ * along;
      if (offX * offX + offZ * offZ > PATH_REPLAN_DEVIATION_METRES * PATH_REPLAN_DEVIATION_METRES)
        PlanRoute(_id, route.destination, route.requiredClearanceMetres);
    }
  }
}

float World::AuthorityOf(ShipId _id) const noexcept
{
  return AvoidanceAuthorityOf(HullSpecOf(m_ships[_id].hullId), m_ships[_id].order);
}

void World::SnapshotPreviousTick() noexcept
{
  // Two lines, hoisted out of StepShip into a pass of their own, and that hoist is the whole of
  // the order-independence property. prevPos was already written here for the view to interpolate
  // between ticks; making it a whole pass turns it into the authoritative start-of-tick state that
  // every neighbour read below sees, so ship 0 and ship 500 see the same world.
  for (ShipState& ship : m_ships)
  {
    ship.prevPos = ship.posWorld;
    ship.prevHeading = ship.headingRad;
  }
}

void World::RebuildStaticIfDirty()
{
  if (!m_staticIndexDirty)
    return;
  {
    m_staticEntries.clear();
    for (ShipId id = 0; id < m_ships.size(); ++id)
    {
      const HullSpec& hull = HullSpecOf(m_ships[id].hullId);
      if (hull.immovable && hull.collidable)
        m_staticEntries.push_back({id, m_ships[id].posWorld, hull.BoundingRadiusMetres()});
    }
    m_index.RebuildStatic(m_staticEntries);

    // The obstacle set is the static store: nothing mobile is ever an obstacle. Ships route around
    // architecture and avoid each other, and keeping the two apart is what keeps both small.
    m_obstacleScratch.clear();
    for (const SpatialIndex::Entry& entry : m_staticEntries)
      m_obstacleScratch.push_back({entry.pos, entry.boundingRadiusMetres});
    m_pathIslands.Rebuild(m_obstacleScratch);
    m_staticIndexDirty = false;
  }
}

void World::RebuildIndex()
{
  RebuildStaticIfDirty();

  // The neighbourhood's extent rides along with the rebuild, which already walks every ship. It is
  // what stops a skirmish between fighters paying the Carrier's query radius because a Carrier
  // exists in the hull table (Design/MmoScalabilityReview.md U2).
  //
  // Maxima over what is *present*, so every one of them is an upper bound on any neighbour a query
  // can return, which is what makes the narrower radius correct rather than merely smaller. The
  // static maximum comes from the static store, which is rebuilt on its own cadence and is walked
  // above by RebuildStaticIfDirty.
  m_extent = NeighbourhoodExtent{};
  for (const SpatialIndex::Entry& entry : m_staticEntries)
    m_extent.largestStaticRadiusMetres = std::max(m_extent.largestStaticRadiusMetres, entry.boundingRadiusMetres);

  m_dynamicEntries.clear();
  for (ShipId id = 0; id < m_ships.size(); ++id)
  {
    const HullSpec& hull = HullSpecOf(m_ships[id].hullId);
    if (!hull.immovable && hull.collidable)
    {
      m_dynamicEntries.push_back({id, m_ships[id].prevPos, hull.BoundingRadiusMetres()});
      m_extent.largestMobileRadiusMetres = std::max(m_extent.largestMobileRadiusMetres, hull.BoundingRadiusMetres());
      m_extent.fastestSpeedMetresPerSec = std::max(m_extent.fastestSpeedMetresPerSec, hull.maxSpeedMetresPerSec);
    }
  }
  m_index.RebuildDynamic(m_dynamicEntries);
}

void World::GatherNeighbours()
{
  const std::uint32_t count = ShipCount();
  m_neighbourStart.assign(static_cast<std::size_t>(count) + 1, 0);
  for (ShipId id = 0; id < count; ++id)
    m_neighbourStart[id + 1] = m_neighbourStart[id] + HullSpecOf(m_ships[id].hullId).neighbourCap;
  m_neighbours.assign(m_neighbourStart[count], Neighbour{});
  m_neighbourCount.assign(count, 0);
  m_gatheredCandidates = 0;
  m_queriedCandidates = 0;

  for (ShipId id = 0; id < count; ++id)
  {
    const ShipState& ship = m_ships[id];
    const HullSpec& hull = HullSpecOf(ship.hullId);
    if (!hull.collidable)
      continue;

    const float queryRadius = QueryRadiusMetres(hull, m_extent);
    m_index.QueryCircle(ship.prevPos, queryRadius, m_queryScratch);
    m_candidateScratch.clear();
    m_queriedCandidates += m_queryScratch.size();
    for (const ShipId other : m_queryScratch)
    {
      if (other == id)
        continue;
      const ShipState& neighbour = m_ships[other];
      const HullSpec& neighbourHull = HullSpecOf(neighbour.hullId);

      // The offsets and the squared distance first, because the pair can be rejected on those
      // alone. The query radius is one circle wide enough for the worst pairing in the
      // neighbourhood; most pairs in it are nowhere near that, and a fighter beside a fighter has
      // no business paying a sqrt, two trigonometric calls and a 40-byte record for a hull it
      // cannot reach. PairRelevanceRadiusMetres is never wider than the query that found this
      // candidate, so nothing the query was right to return is dropped here.
      const float offsetX = OffsetX(ship.prevPos, neighbour.prevPos);
      const float offsetZ = OffsetZ(ship.prevPos, neighbour.prevPos);
      const float distanceSquared = offsetX * offsetX + offsetZ * offsetZ;
      // Clamped to the query, because beyond it the index returned nothing and there is nothing to
      // reject. That clamp is what makes the filter provably behaviour-preserving without asking
      // anything of the query's own formula: the query decides what exists, this decides what is
      // worth a record, and it is never the narrower of the two by accident.
      const float relevance = std::min(PairRelevanceRadiusMetres(hull, neighbourHull), queryRadius);
      if (distanceSquared > relevance * relevance)
        continue;

      Neighbour record;
      record.id = other;
      record.offsetX = offsetX;
      record.offsetZ = offsetZ;
      record.velocityX = std::sin(neighbour.prevHeading) * neighbour.speed;
      record.velocityZ = std::cos(neighbour.prevHeading) * neighbour.speed;
      record.boundingRadiusMetres = neighbourHull.BoundingRadiusMetres();
      record.avoidanceAuthority = AvoidanceAuthorityOf(neighbourHull, neighbour.order);
      record.immovable = neighbourHull.immovable;
      record.distanceSquared = distanceSquared;
      record.proximityMetres = std::sqrt(distanceSquared) - record.boundingRadiusMetres;
      m_candidateScratch.push_back(record);
    }
    m_gatheredCandidates += m_candidateScratch.size();

    // Every candidate from the covering ring, then sorted, and only then truncated. Truncating
    // cell by cell would make cell size part of the replay contract and it could never be retuned
    // again -- not per region, not after profiling. ShipId is the tie-break and is what makes the
    // order total, so std::sort's instability cannot show through (Design/Archive/Collision.md 7).
    std::sort(m_candidateScratch.begin(), m_candidateScratch.end(), [](const Neighbour& _a, const Neighbour& _b)
              { return (_a.proximityMetres != _b.proximityMetres) ? _a.proximityMetres < _b.proximityMetres : _a.id < _b.id; });

    const std::uint32_t take = std::min<std::uint32_t>(hull.neighbourCap, static_cast<std::uint32_t>(m_candidateScratch.size()));
    std::copy_n(m_candidateScratch.begin(), take, m_neighbours.begin() + m_neighbourStart[id]);
    m_neighbourCount[id] = take;
  }
}

namespace
{
[[nodiscard]] Capsule CapsuleAt(const ShipState& _ship, const HullSpec& _hull, float _centreX, float _centreZ) noexcept
{
  return Capsule{
    _centreX, _centreZ, std::sin(_ship.headingRad), std::cos(_ship.headingRad), _hull.capsuleHalfLengthMetres, _hull.capsuleRadiusMetres};
}
} // namespace

void World::ApplySeparation()
{
  const std::uint32_t count = ShipCount();
  m_appliedX.assign(count, 0.0f);
  m_appliedZ.assign(count, 0.0f);

  // More than one solve per tick, because one is not enough for a jam. A compressed line of
  // parallel hulls can only expand from its ends -- its interior is translation-invariant, so
  // every local solver gives every interior ship the same answer, and the same answer everywhere
  // is a translation, which lengthens nothing. The ends' information reaches the middle by
  // diffusion, one ship per solve step, so k steps make it k times faster and nothing else does
  // (SimTuning.h says why at length).
  for (std::uint32_t iteration = 0; iteration < SEPARATION_ITERATIONS; ++iteration)
  {
    m_correctionX.assign(count, 0.0f);
    m_correctionZ.assign(count, 0.0f);
    float largestCorrection = 0.0f;

    for (ShipId id = 0; id < count; ++id)
    {
      const ShipState& ship = m_ships[id];
      const HullSpec& hull = HullSpecOf(ship.hullId);
      if (hull.immovable || !hull.collidable)
        continue;

      const Capsule self = CapsuleAt(ship, hull, 0.0f, 0.0f);
      float correctionX = 0.0f;
      float correctionZ = 0.0f;
      for (const Neighbour& neighbour : NeighboursOf(id))
      {
        const ShipState& other = m_ships[neighbour.id];
        const HullSpec& otherHull = HullSpecOf(other.hullId);
        if (!otherHull.collidable || otherHull.immovable)
          continue; // architecture is pass 5b's, after this one and without the clamp

        const Capsule against = CapsuleAt(other, otherHull, OffsetX(ship.posWorld, other.posWorld), OffsetZ(ship.posWorld, other.posWorld));
        const Contact contact = CapsuleContact(self, against, id, neighbour.id);
        if (!contact.touching)
          continue;

        // Cap what the pair closes, then split it -- never the other way round. Both sides compute
        // this same cap from the same two radii, so the authority ratio survives the clamp instead
        // of being inverted by it.
        const SeparationShares shares = SeparationSharesFor(AuthorityOf(id), false, AuthorityOf(neighbour.id), false);
        const float pairCap = SEPARATION_PAIR_CLOSE_FRACTION * std::min(hull.capsuleRadiusMetres, otherHull.capsuleRadiusMetres);
        const float closed = std::min(contact.overlapMetres * SEPARATION_STIFFNESS, pairCap);
        correctionX += contact.normalX * closed * shares.a;
        correctionZ += contact.normalZ * closed * shares.a;
      }

      // The clamp bounds the tick, not the step, so extra solver steps buy convergence and never
      // extra displacement: the prediction error budget is the same number it was with one step.
      const float limit = SEPARATION_CLAMP_FRACTION * hull.capsuleRadiusMetres;
      float totalX = m_appliedX[id] + correctionX;
      float totalZ = m_appliedZ[id] + correctionZ;
      const float magnitudeSquared = totalX * totalX + totalZ * totalZ;
      if (magnitudeSquared > limit * limit)
      {
        const float scale = limit / std::sqrt(magnitudeSquared);
        totalX *= scale;
        totalZ *= scale;
      }
      m_correctionX[id] = totalX - m_appliedX[id];
      m_correctionZ[id] = totalZ - m_appliedZ[id];
      largestCorrection = std::max(largestCorrection, std::fabs(m_correctionX[id]));
      largestCorrection = std::max(largestCorrection, std::fabs(m_correctionZ[id]));
    }

    for (ShipId id = 0; id < count; ++id)
    {
      Translate(m_ships[id].posWorld, m_correctionX[id], m_correctionZ[id]);
      m_appliedX[id] += m_correctionX[id];
      m_appliedZ[id] += m_correctionZ[id];
    }

    // A maximum rather than a sum, and that is not incidental: max is exact and order-independent
    // over floats where a sum is neither, so where the loop stops cannot depend on array order.
    if (largestCorrection < SEPARATION_SETTLE_METRES)
      break;
  }
}

void World::ApplyBlocking()
{
  const std::uint32_t count = ShipCount();
  m_correctionX.assign(count, 0.0f);
  m_correctionZ.assign(count, 0.0f);

  for (ShipId id = 0; id < count; ++id)
  {
    const ShipState& ship = m_ships[id];
    const HullSpec& hull = HullSpecOf(ship.hullId);
    if (hull.immovable || !hull.collidable)
      continue;

    const Capsule self = CapsuleAt(ship, hull, 0.0f, 0.0f);
    for (const Neighbour& neighbour : NeighboursOf(id))
    {
      const ShipState& other = m_ships[neighbour.id];
      const HullSpec& otherHull = HullSpecOf(other.hullId);
      if (!otherHull.collidable || !otherHull.immovable)
        continue;

      const Capsule against = CapsuleAt(other, otherHull, OffsetX(ship.posWorld, other.posWorld), OffsetZ(ship.posWorld, other.posWorld));
      const Contact contact = CapsuleContact(self, against, id, neighbour.id);
      if (!contact.touching)
        continue;

      // The whole overlap, and no clamp. Hard blocking has to be hard or it is decoration: this
      // runs after ship-to-ship separation precisely so that a column of traffic behind a ship
      // cannot squeeze it through a Structure.
      m_correctionX[id] += contact.normalX * contact.overlapMetres;
      m_correctionZ[id] += contact.normalZ * contact.overlapMetres;
    }
  }

  for (ShipId id = 0; id < count; ++id)
    Translate(m_ships[id].posWorld, m_correctionX[id], m_correctionZ[id]);
}

void World::Step()
{
  StepDockings();
  StepPatrols();
  SnapshotPreviousTick();
  RebuildIndex();
  GatherNeighbours();

  for (ShipId id = 0; id < ShipCount(); ++id)
  {
    ShipState& ship = m_ships[id];
    const HullSpec& hull = HullSpecOf(ship.hullId);
    if (hull.immovable)
      continue;
    // The path follower runs first and reads only this ship: it changes which point is steered at,
    // and SolveOrder, AvoidNeighbours and IntegrateShip are all unchanged by its existence.
    AdvanceRoute(id);
    // Pass 3 reads only this ship and the start-of-tick copies in its neighbour list; pass 4 writes
    // only this ship. Neither can see another ship half-advanced, which is what keeps the whole
    // tick free of array order.
    const MotionIntent intent = AvoidNeighbours(ship, hull, SolveOrder(ship, hull), NeighboursOf(id));
    IntegrateShip(ship, hull, intent);
  }

  ApplySeparation();
  ApplyBlocking();
  ++m_tick;
}

float World::IssueMoveOrder(std::span<const ShipId> _ships, const WorldPos& _point, bool _hasFacing, float _facingRad,
                            FactionId _issuerFaction)
{
  // An order can arrive before the first tick, so the islands a route is planned against have to be
  // current here rather than only at the top of Step.
  RebuildStaticIfDirty();

  std::vector<ShipId> chosen;
  chosen.reserve(_ships.size());
  for (const ShipId id : _ships)
  {
    // Somebody else's ship is dropped the way a stale id already is. The rest of the list is still
    // steered, and an order that loses every ship returns the facing it was given, exactly as an
    // empty list does (Design/Archive/Hostiles.md 4.3).
    if (id < m_ships.size() && m_ships[id].factionId == _issuerFaction)
      chosen.push_back(id);
  }
  if (chosen.empty())
    return _facingRad;

  // Point the formation along the ordered facing, or along the way the group is about to travel.
  // The second case goes through FormationHeading rather than being spelled out here, because the
  // client orients its order marker with the same function on the same inputs -- one definition is
  // what stops the marker and the ships disagreeing about which way an order points now that the
  // answer no longer travels back down the wire (Design/Archive/Collision-slice-2b.md 2.5).
  float heading = _facingRad;
  if (!_hasFacing)
  {
    m_headingScratch.clear();
    m_headingScratch.reserve(chosen.size());
    for (const ShipId id : chosen)
      m_headingScratch.push_back(m_ships[id].posWorld);
    heading = FormationHeading(m_headingScratch, _point, m_ships[chosen[0]].headingRad);
  }

  // Hand out slots in the order the ships already lie across the formation, so they do not have to
  // cross each other on the way in.
  const float rightX = std::cos(heading);
  const float rightZ = -std::sin(heading);
  const auto acrossFormation = [&](ShipId _id)
  { return OffsetX(_point, m_ships[_id].posWorld) * rightX + OffsetZ(_point, m_ships[_id].posWorld) * rightZ; };
  std::sort(chosen.begin(), chosen.end(), [&](ShipId _a, ShipId _b) { return acrossFormation(_a) < acrossFormation(_b); });

  // Sized by the largest hull in the group, so a mixed order spaces itself for the Carrier in it
  // rather than for the fighters. A formation plans once and travels as one thing.
  float largestRadius = 0.0f;
  for (const ShipId id : chosen)
    largestRadius = std::max(largestRadius, HullSpecOf(m_ships[id].hullId).BoundingRadiusMetres());
  const float spacing = SlotSpacingMetres(largestRadius);

  const int count = static_cast<int>(chosen.size());
  const FormationShape shape = static_cast<FormationShape>(std::clamp(FORMATION_SHAPE, 0, 3));
  const float cosH = std::cos(heading);
  const float sinH = std::sin(heading);

  // One clearance for the whole group, the largest hull's, so a mixed order takes one route rather
  // than the fighters threading a gap the Carrier then has to go round.
  const float groupClearance = largestRadius + PATH_CLEARANCE_MARGIN_METRES;

  for (int slot = 0; slot < count; ++slot)
  {
    const ShipId id = chosen[static_cast<size_t>(slot)];
    const XMFLOAT2 local = FormationOffset(slot, count, shape, spacing);
    ShipState& ship = m_ships[id];
    ship.orderFacingRad = heading;
    ship.orderHasFacing = _hasFacing;
    ship.order = OrderState::Moving;
    // An explicit order outranks a standing behavior, and it travels at the hull's own speed: a
    // patrol left running underneath one would take the ship back to its ring the moment the order
    // finished (Design/Archive/Hostiles.md 5.1).
    ship.orderSpeedCapMetresPerSec = 0.0f;
    m_patrols[id].active = false;

    // A later move order is a change of mind: before capture a docking ship is just a ship flying
    // somewhere, and nothing about it should survive being told to go elsewhere. There is no undock
    // and no cancel-into-hold -- cleared intent leaves the ship doing whatever it was last told
    // (Design/Stations.md 7.1).
    m_dockings[id].active = false;

    WorldPos destination = _point;
    Translate(destination, local.x * cosH + local.y * sinH, -local.x * sinH + local.y * cosH);
    PlanRoute(id, destination, groupClearance);
  }
  return heading;
}
} // namespace Game
