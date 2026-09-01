#include "pch.h"
#include "Universe.h"

#include "Collision.h"
#include "HullSpec.h"
#include "Movement.h"
#include "SimTuning.h"

#include <cmath>
#include <cstddef>

using namespace DirectX;

namespace Game
{
namespace
{
// Defined further down with the docking geometry it belongs to, and declared here because the jump
// pass's approach reaches it first. Shared rather than copied: two approach points would be two
// chances to disagree about where a structure's doorstep is.
[[nodiscard]] UniversePos DockApproachPoint(const UniversePos& _station, const UniversePos& _ship, float _dockRangeMetres) noexcept;
} // namespace

ShipId Universe::SpawnShip(const UniversePos& _posUniverse, float _headingRad, std::uint32_t _hullId, FactionId _factionId)
{
  // The serial is taken here rather than inside SpawnShipAs so that there is exactly one place that
  // mints, and it is the one place that can be sure the id is not already here.
  return SpawnShipAs(MakeEntityId(m_shard, m_nextEntitySerial), _posUniverse, _headingRad, _hullId, _factionId);
}

ShipId Universe::SpawnShipAs(EntityId _entity, const UniversePos& _posUniverse, float _headingRad, std::uint32_t _hullId,
                             FactionId _factionId)
{
  // An entity existing twice in one universe is the failure the whole mechanism exists to make
  // impossible, so a duplicate is refused rather than resolved to whichever copy is found first.
  if (_entity == INVALID_ENTITY_ID || ResolveEntity(_entity) != INVALID_SHIP_ID)
    return INVALID_SHIP_ID;

  ShipState ship;
  ship.posUniverse = _posUniverse;
  ship.prevPos = _posUniverse;
  ship.headingRad = _headingRad;
  ship.prevHeading = _headingRad;
  ship.hullId = _hullId;
  ship.factionId = _factionId;
  // Whole, or zero for a hull that cannot be hurt at all -- the same value either way, because
  // maxHullPoints is what "indestructible" is spelled as (HullSpec.h).
  ship.hullPoints = HullSpecOf(_hullId).maxHullPoints;

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
  m_slots[slot].entity = _entity;
  InsertEntityRow(_entity, slot);

  // Past any local id handed in, so a reloaded universe cannot go on to mint one its own file already
  // used. A foreign id says nothing about this shard's counter and moves it not at all.
  if (EntityShardOf(_entity) == m_shard && EntitySerialOf(_entity) >= m_nextEntitySerial)
    m_nextEntitySerial = EntitySerialOf(_entity) + 1;

  m_shipSlot.push_back(slot);
  m_routes.emplace_back();
  m_patrols.emplace_back();
  m_dockings.emplace_back();
  m_protectors.emplace_back();

  // Every mount starts pointed where it was bolted on, which is the only aim it has before it has
  // seen anything. A turret slews from there; a fixed mount never leaves it.
  ShipMounts& mounts = m_mounts.emplace_back();
  const HullSpec& spawnedHull = HullSpecOf(_hullId);
  for (std::uint32_t at = 0; at < spawnedHull.MountCount(); ++at)
    mounts.mount[at].aimBearingRad = spawnedHull.loadout.mount[at].bearingRad;

  // Only an immovable can change the static set. A spawn appends, so no existing id moves and
  // nothing already in the store is disturbed -- which is why this needs no id-shift caveat and the
  // despawn below does (Design/Archive/MmoScalabilityReview.md U4). The gate is deliberately looser than
  // the store's own filter, which is immovable *and* collidable: a Stargate costs one rebuild it did
  // not need, and a gate that is a superset of the filter cannot miss one that was needed, which is
  // the direction to be wrong in if the two ever drift apart.
  if (HullSpecOf(_hullId).immovable)
    m_staticIndexDirty = true;
  return id;
}

bool Universe::DespawnShip(ShipHandle _handle, DespawnCause _cause)
{
  const ShipId id = Resolve(_handle);
  if (id == INVALID_SHIP_ID)
    return false;

  // Logged before the slot is retired, so the publisher can tell this death from a departure. A
  // despawn no subscriber held is dropped where the publisher intersects it with what that
  // subscriber knew: you cannot be told of the death of something you never knew about
  // (Design/Archive/Hostiles.md 4.4).
  m_despawnLog.push_back(DespawnRecord{_handle, m_slots[_handle.slot].entity, _cause});

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
    m_protectors[id] = m_protectors[last];
    m_mounts[id] = m_mounts[last];
    m_shipSlot[id] = m_shipSlot[last];
    m_slots[m_shipSlot[id]].ship = id; // the moved ship keeps its slot, and its handles keep working
  }
  m_ships.pop_back();
  m_routes.pop_back();
  m_patrols.pop_back();
  m_dockings.pop_back();
  m_protectors.pop_back();
  m_mounts.pop_back();
  m_shipSlot.pop_back();

  Slot& freed = m_slots[_handle.slot];
  EraseEntityRow(freed.entity);
  freed.ship = INVALID_SHIP_ID;
  // Cleared, not kept: a slot that still named its last occupant would answer EntityIdOf for a ship
  // that no longer exists, and the generation bump below is what makes every handle to it null.
  freed.entity = INVALID_ENTITY_ID;
  ++freed.generation;
  if (freed.generation == 0)
    freed.generation = 1;
  m_freeSlots.push_back(_handle.slot);
  return true;
}

Standing Universe::StandingOf(FactionId _owner, FactionId _other) const noexcept
{
  // Out of range is Hostile, not Neutral. Nobody authored that faction, every caller here is a gate
  // or a warning colour, and the failure directions are not symmetric: a stranger refused a dock is
  // a bug report, a stranger admitted is a hole.
  if (_owner >= FACTION_LIMIT || _other >= FACTION_LIMIT)
    return Standing::Hostile;
  return m_standings.rows[_owner][_other];
}

std::uint8_t Universe::HostileMaskFor(FactionId _viewer) const noexcept
{
  std::uint8_t mask = 0;
  for (std::uint32_t faction = 0; faction < FACTION_LIMIT; ++faction)
  {
    // Their opinion of the viewer, not the viewer's of them. The client colours a faction that has
    // turned on *it*, and refuses to offer a dock it would be refused (Design/Archive/Stations.md 4.3, 9.3).
    if (StandingOf(static_cast<FactionId>(faction), _viewer) == Standing::Hostile)
      mask |= static_cast<std::uint8_t>(1u << faction);
  }
  return mask;
}

void Universe::RecordAggression(ShipHandle _attacker, StationId _station)
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
  // widens to per-player rows exactly as ADR 0014's authority gate does (Design/Archive/Stations.md 4.2, 12).
  m_standings.rows[owner][attackerFaction] = Standing::Hostile;

  // Standing is imperial and the response is local: the flip above is the whole government's, and
  // the list below is one station's. A second aggression against a second station scrambles that
  // one too (Design/Archive/Stations.md 8.1).
  Station& station = m_stations[_station];
  for (const ShipHandle known : station.targets)
  {
    if (known == _attacker)
      return; // already being hunted; attacking twice does not queue two protectors
  }

  // A full list drops the newest, deterministically. The standing flip already happened, which is
  // the part that matters -- a criminal the garrison is too busy to chase is still a criminal.
  if (station.targets.size() < station.targetCap)
    station.targets.push_back(_attacker);
}

void Universe::PursueTarget(ShipId _ship, ShipId _target)
{
  // Re-aimed when the ship has nothing to do, or when the target has walked far enough from the
  // point last aimed at to be worth a new plan -- never every tick, which would cost everything and
  // change nothing. The point last aimed at is the route's own destination, which is why a pursuer
  // is sent at the target itself rather than at a slot around it: eight ships aimed at one point all
  // answer this test the same way on the same tick.
  ShipState& ship = m_ships[_ship];
  const UniversePos& targetPos = m_ships[_target].posUniverse;
  // Measured against where the target was when this route was planned, and NOT against the route's
  // destination: with a stand-off those are different points, and the difference is a constant that
  // the old test read as drift on every tick of every chase (Universe.h, Route::pursuitAimedAt).
  const bool drifted = Distance(m_routes[_ship].pursuitAimedAt, targetPos) > PURSUIT_REPLAN_METRES;
  if (ship.order != OrderState::Idle && !drifted)
    return;

  ship.order = OrderState::Moving;
  ship.orderHasFacing = false;
  ship.orderSpeedCapMetresPerSec = 0.0f; // a chase runs at the hull's own speed
  m_patrols[_ship].active = false;
  // A fleet member may have been halfway into a station when the shooting started. On the protector
  // path this is already false by the time a duty pursues, so it costs that caller nothing.
  m_dockings[_ship].active = false;

  // Short of the target by what this hull's turrets need, along the bearing from the target back to
  // the pursuer, so a gunship holds where its guns bear. Zero for a hull with no traversing mount,
  // which is what sends a fighter at its quarry and makes the pass a pass (Design/Combat.md 8).
  const HullSpec& hull = HullSpecOf(ship.hullId);
  UniversePos aimPoint = targetPos;
  const float standoff = EngageStandoffMetres(hull);
  if (standoff > 0.0f)
  {
    const float toPursuerX = OffsetX(targetPos, ship.posUniverse);
    const float toPursuerZ = OffsetZ(targetPos, ship.posUniverse);
    const float span = std::sqrt(toPursuerX * toPursuerX + toPursuerZ * toPursuerZ);
    // Clamped to where the pursuer already is, so a stand-off only ever stops a chase short and
    // never turns one into a withdrawal. A hull already inside its own gunnery range is a hull whose
    // guns already bear, and backing it out to a nominal range would spend seconds to arrive at a
    // worse position than it was in -- besides oscillating against any target that closes.
    const float held = (span < standoff) ? span : standoff;
    // Sitting exactly on top of the target leaves no bearing to stand off along. Holding station
    // where it is beats dividing by zero, and the next tick it has moved and there is one again.
    if (span > 0.001f)
      Translate(aimPoint, toPursuerX / span * held, toPursuerZ / span * held);
  }

  m_routes[_ship].pursuitAimedAt = targetPos;
  PlanRoute(_ship, aimPoint, hull.BoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
}

void Universe::RecordHostileAct(ShipHandle _attacker, ShipHandle _victim)
{
  const ShipId victim = Resolve(_victim);
  if (victim == INVALID_SHIP_ID)
    return;

  const FleetId fleet = FleetAt(victim);
  if (fleet == INVALID_FLEET_ID)
    return; // nobody's to answer for: a loose ship has no response of its own

  // The latest act wins, anchor and all. A fleet attacked at both ends holds one threat because the
  // posture is one judgment and the wire is one bit (Design/Archive/Fleets.md 13).
  Fleet& row = m_fleets[fleet];
  row.threat = _attacker;
  row.threatAnchorPos = m_ships[victim].posUniverse;
  row.alertTicks = FLEET_ALERT_TICKS;
}

const Universe::ProtectorDuty& Universe::ProtectorOf(ShipId _id) const noexcept
{
  return m_protectors[_id];
}

const Universe::ShipMounts& Universe::MountsOf(ShipId _id) const noexcept
{
  return m_mounts[_id];
}

const UniversePos& Universe::PursuitAimedAt(ShipId _id) const noexcept
{
  return m_routes[_id].pursuitAimedAt;
}

std::uint32_t Universe::LaunchedProtectorCount(StationId _station) const noexcept
{
  std::uint32_t launched = 0;
  for (const ProtectorDuty& duty : m_protectors)
    launched += (duty.active && duty.home == _station) ? 1u : 0u;
  return launched;
}

Universe::StationId Universe::MakeStation(ShipId _structure, const StationDesc& _desc)
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

Universe::GateId Universe::MakeGate(ShipId _structure, const GateDesc& _desc)
{
  if (_structure >= m_ships.size())
    return INVALID_GATE_ID;

  Gate gate;
  gate.structure = HandleOf(_structure);
  gate.destination = _desc.destination;
  gate.ownerFaction = _desc.ownerFaction;

  const GateId id = static_cast<GateId>(m_gates.size());
  m_gates.push_back(gate);
  return id;
}

Universe::GateId Universe::GateAt(ShipId _id) const noexcept
{
  if (_id >= m_ships.size())
    return INVALID_GATE_ID;

  // StationAt's walk and its reason: through Resolve rather than by comparing stored ids, because
  // swap-and-pop moves ids and a row holding a raw one would name whichever ship arrived in that
  // slot (ADR 0005). A linear scan over single digits of rows, which becomes an index the day
  // there are hundreds -- the sentence the station table already carries.
  for (std::size_t at = 0; at < m_gates.size(); ++at)
  {
    if (Resolve(m_gates[at].structure) == _id)
      return static_cast<GateId>(at);
  }
  return INVALID_GATE_ID;
}

const Universe::Gate& Universe::GateOf(GateId _id) const noexcept
{
  static const Gate NONE;
  return (_id < m_gates.size()) ? m_gates[_id] : NONE;
}

Universe::StationId Universe::StationAt(ShipId _id) const noexcept
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

const Universe::Station& Universe::StationOf(StationId _id) const noexcept
{
  return m_stations[_id];
}

bool Universe::CanTakeSlot(FactionId _ownerFaction, std::uint8_t _slot) const noexcept
{
  return _slot < FLEET_SLOTS && _ownerFaction < FACTION_LIMIT && FleetInSlot(_ownerFaction, _slot) == INVALID_FLEET_ID;
}

Universe::FleetId Universe::FormFleet(FactionId _ownerFaction, std::uint8_t _slot, std::span<const ShipId> _ships)
{
  // In the order the header lists them. Which one refuses is not observable -- every refusal is the
  // same invalid id and the same untouched table -- so the order is for the reader.
  if (!CanTakeSlot(_ownerFaction, _slot))
    return INVALID_FLEET_ID;
  if (_ships.empty() || _ships.size() > MAX_FLEET_SHIPS)
    return INVALID_FLEET_ID;

  // The whole list is checked before anything is written, because a refusal has to change nothing:
  // a half-formed fleet is one nobody asked for, and its size is one of the rules.
  for (std::size_t at = 0; at < _ships.size(); ++at)
  {
    const ShipId id = _ships[at];
    if (id >= m_ships.size() || m_ships[id].factionId != _ownerFaction)
      return INVALID_FLEET_ID;
    if (FleetAt(id) != INVALID_FLEET_ID)
      return INVALID_FLEET_ID;
    // Named twice is the same defect as already in a fleet, arriving from inside one call rather
    // than across two: it would spend two of the eight places on one ship.
    for (std::size_t before = 0; before < at; ++before)
    {
      if (_ships[before] == id)
        return INVALID_FLEET_ID;
    }
  }

  Fleet fleet;
  fleet.ownerFaction = _ownerFaction;
  fleet.slot = _slot;
  fleet.memberCount = static_cast<std::uint32_t>(_ships.size());
  for (std::size_t at = 0; at < _ships.size(); ++at)
    fleet.members[at] = HandleOf(_ships[at]);

  const FleetId id = static_cast<FleetId>(m_fleets.size());
  m_fleets.push_back(fleet);
  return id;
}

Universe::FleetId Universe::FleetInSlot(FactionId _ownerFaction, std::uint8_t _slot) const noexcept
{
  for (std::size_t at = 0; at < m_fleets.size(); ++at)
  {
    if (m_fleets[at].ownerFaction == _ownerFaction && m_fleets[at].slot == _slot)
      return static_cast<FleetId>(at);
  }
  return INVALID_FLEET_ID;
}

Universe::FleetId Universe::FleetAt(ShipId _id) const noexcept
{
  if (_id >= m_ships.size())
    return INVALID_FLEET_ID;

  for (std::size_t at = 0; at < m_fleets.size(); ++at)
  {
    const Fleet& fleet = m_fleets[at];
    for (std::uint32_t member = 0; member < fleet.memberCount; ++member)
    {
      if (Resolve(fleet.members[member]) == _id)
        return static_cast<FleetId>(at);
    }
  }
  return INVALID_FLEET_ID;
}

const Universe::Fleet& Universe::FleetOf(FleetId _id) const noexcept
{
  static constexpr Fleet NONE;
  return (_id < m_fleets.size()) ? m_fleets[_id] : NONE;
}

void Universe::LedgerFor(StationId _station, FactionId _asker, std::span<std::uint32_t> _outCounts) const noexcept
{
  const std::size_t stated = (_outCounts.size() < HULL_COUNT) ? _outCounts.size() : std::size_t{HULL_COUNT};
  for (std::size_t hull = 0; hull < stated; ++hull)
    _outCounts[hull] = 0;

  if (_station >= m_stations.size())
    return;

  const Station& station = m_stations[_station];
  if (Resolve(station.structure) == INVALID_SHIP_ID)
    return;

  // The same standing gate ComposeFleet applies, and zeros rather than a refusal because this
  // function has no way to say no -- a caller that needs the distinction asks ComposeFleet, which
  // still runs its own gate and still returns RefusedStanding.
  if (StandingOf(station.ownerFaction, _asker) == Standing::Hostile)
    return;

  for (const DockedShip& docked : station.docked)
  {
    if (docked.factionId == _asker && docked.hullId < stated)
      ++_outCounts[docked.hullId];
  }
}

Universe::ComposeResult Universe::ComposeFleet(StationId _station, std::uint8_t _slot, std::span<const std::uint32_t> _hullCounts,
                                               FactionId _issuerFaction)
{
  if (_station >= m_stations.size())
    return ComposeResult::NotAStation;

  Station& station = m_stations[_station];
  if (Resolve(station.structure) == INVALID_SHIP_ID)
    return ComposeResult::NotAStation;

  if (StandingOf(station.ownerFaction, _issuerFaction) == Standing::Hostile)
    return ComposeResult::RefusedStanding;

  if (!CanTakeSlot(_issuerFaction, _slot))
    return ComposeResult::SlotTaken;

  // Each count is bounded before it is summed, so no arithmetic here can overflow whatever a caller
  // passes -- the total is at most HULL_COUNT * MAX_FLEET_SHIPS before the gate below sees it.
  std::uint32_t wanted[HULL_COUNT] = {};
  std::uint32_t total = 0;
  for (std::size_t hull = 0; hull < _hullCounts.size(); ++hull)
  {
    if (_hullCounts[hull] == 0)
      continue;
    // No ledger holds a hull the table does not have, so a count past the end of it is the honest
    // NotDocked rather than a silent truncation of what was asked for.
    if (hull >= HULL_COUNT)
      return ComposeResult::NotDocked;
    if (_hullCounts[hull] > MAX_FLEET_SHIPS)
      return ComposeResult::TooMany;
    wanted[hull] = _hullCounts[hull];
    total += _hullCounts[hull];
  }
  if (total == 0 || total > MAX_FLEET_SHIPS)
    return ComposeResult::TooMany;

  // The issuer's own rows, and only those -- through the function a ledger request over the wire
  // also answers with, so what a screen was shown and what this gate reads cannot drift apart
  // (Design/Archive/Fleets.md 8.3).
  std::uint32_t available[HULL_COUNT] = {};
  LedgerFor(_station, _issuerFaction, available);
  for (std::uint32_t hull = 0; hull < HULL_COUNT; ++hull)
  {
    if (wanted[hull] > available[hull])
      return ComposeResult::NotDocked;
  }

  // Past every gate: from here nothing can fail, and the ledger may be written.
  Fleet fleet;
  fleet.ownerFaction = _issuerFaction;
  fleet.slot = _slot;
  fleet.launchStructure = station.structure;

  // Ascending hull id, and stated rather than incidental: it is the launch order, the launch order
  // is the order members join the fleet in, and that is the order a formation solve reads.
  for (std::uint32_t hull = 0; hull < HULL_COUNT; ++hull)
  {
    for (std::uint32_t at = 0; at < wanted[hull]; ++at)
      fleet.manifest[fleet.manifestCount++] = hull;
  }

  // The rows leave the ledger now rather than one per launch, so that two things cannot happen: a
  // second compose claiming the same rows, and a ledger a screen has shown disagreeing with what the
  // launch finds (Design/Archive/Fleets.md 5.2). Compacted in place, in array order, so which rows are drawn
  // is a function of the ledger and not of anything else.
  std::uint32_t drawn[HULL_COUNT] = {};
  std::size_t live = 0;
  for (std::size_t at = 0; at < station.docked.size(); ++at)
  {
    const DockedShip& docked = station.docked[at];
    if (docked.factionId == _issuerFaction && docked.hullId < HULL_COUNT && drawn[docked.hullId] < wanted[docked.hullId])
    {
      ++drawn[docked.hullId];
      continue;
    }
    station.docked[live++] = station.docked[at];
  }
  station.docked.resize(live);

  m_fleets.push_back(fleet);
  return ComposeResult::Composed;
}

float Universe::FleetCruiseSpeedMetresPerSec(const Fleet& _fleet) const noexcept
{
  float slowest = 0.0f;
  for (std::uint32_t at = 0; at < _fleet.memberCount; ++at)
  {
    const ShipId member = Resolve(_fleet.members[at]);
    if (member == INVALID_SHIP_ID)
      continue;
    const float top = HullSpecOf(m_ships[member].hullId).maxSpeedMetresPerSec;
    if (slowest == 0.0f || top < slowest)
      slowest = top;
  }
  return slowest;
}

void Universe::LowerFleetOrder(Fleet& _fleet)
{
  m_fleetShipScratch.clear();
  for (std::uint32_t at = 0; at < _fleet.memberCount; ++at)
  {
    const ShipId member = Resolve(_fleet.members[at]);
    if (member != INVALID_SHIP_ID)
      m_fleetShipScratch.push_back(member);
  }
  if (m_fleetShipScratch.empty())
    return;

  // Every branch ends in a call a player's click has always gone through. What a fleet order adds is
  // the referent and the gate, never a second way to fly (Design/Archive/Fleets.md 6.2).
  if (_fleet.orderKind == FleetOrderKind::Move)
  {
    (void)IssueMoveOrder(m_fleetShipScratch, _fleet.orderPoint, _fleet.orderHasFacing, _fleet.orderFacingRad, _fleet.ownerFaction);
  }
  else if (_fleet.orderKind == FleetOrderKind::Dock)
  {
    const ShipId station = Resolve(_fleet.orderStation);
    if (station != INVALID_SHIP_ID)
      (void)IssueDockOrder(m_fleetShipScratch, station, _fleet.ownerFaction);
  }
  else if (_fleet.orderKind == FleetOrderKind::Jump)
  {
    // The approach, through the same call a player's click has always gone through. The pass below
    // does the crossing; this only gets the fleet to the door, in formation, so the members arrive
    // together rather than trickling into the radius one at a time (Design/Universe.md 6.1).
    const ShipId gate = Resolve(_fleet.orderGate);
    if (gate != INVALID_SHIP_ID)
    {
      // Aimed at a point beside the gate rather than at its centre, which is inside the structure
      // and is a point the wall forbids (ADR 0042). The dock pass solves the same problem with
      // DockApproachPoint, and this is that call for the same reason.
      const HullSpec& gateHull = HullSpecOf(m_ships[gate].hullId);
      const UniversePos here = m_ships[Resolve(_fleet.members[0])].posUniverse;
      const UniversePos approach =
        DockApproachPoint(m_ships[gate].posUniverse, here, gateHull.BoundingRadiusMetres() + GATE_APPROACH_METRES);
      (void)IssueMoveOrder(m_fleetShipScratch, approach, false, 0.0f, _fleet.ownerFaction);
    }
  }
  else if (_fleet.orderKind == FleetOrderKind::Attack)
  {
    // The combatants take the target and the rest hold where the order found them -- Stop's
    // treatment, for them alone. The pass keeps the chase aimed from here on; this is where it
    // starts (Design/Archive/Fleets.md 6.2).
    const ShipId target = Resolve(_fleet.orderTarget);
    for (const ShipId member : m_fleetShipScratch)
    {
      if (target != INVALID_SHIP_ID && HullSpecOf(m_ships[member].hullId).combatant)
      {
        PursueTarget(member, target);
        continue;
      }
      m_ships[member].order = OrderState::Idle;
      m_ships[member].orderSpeedCapMetresPerSec = 0.0f;
      m_dockings[member].active = false;
    }
  }
}

Universe::FleetOrderResult Universe::IssueFleetOrder(FactionId _issuerFaction, std::uint8_t _slot, const FleetCommand& _command)
{
  // The whole authority gate, and the whole of what naming a fleet buys here: one comparison where
  // a ship-list order needs a filter over every id it carries (ADR 0049, ADR 0014).
  const FleetId id = FleetInSlot(_issuerFaction, _slot);
  if (id == INVALID_FLEET_ID)
    return FleetOrderResult::NoSuchFleet;

  // Checked before anything is written, so that a refusal leaves the standing order exactly as it
  // was rather than half replaced.
  ShipHandle station;
  if (_command.kind == FleetOrderKind::Dock)
  {
    const StationId row = StationAt(_command.station);
    if (row == INVALID_STATION_ID)
      return FleetOrderResult::NotAStation;
    // IssueDockOrder's own gate, asked here so the refusal has a name rather than arriving as an
    // order that silently did nothing (Design/Archive/Stations.md 7.1).
    if (StandingOf(m_stations[row].ownerFaction, _issuerFaction) == Standing::Hostile)
      return FleetOrderResult::RefusedStanding;
    station = m_stations[row].structure;
  }
  ShipHandle target;
  if (_command.kind == FleetOrderKind::Attack)
  {
    if (_command.target >= m_ships.size())
      return FleetOrderResult::NoSuchTarget;
    // No mount may resolve to a friend and neither may an order, which is where the structural half
    // of "there is no friendly fire" is actually held (Design/Combat.md 11). In the simulation and
    // not on the sheet, for ADR 0014's reason.
    if (m_ships[_command.target].factionId == _issuerFaction)
      return FleetOrderResult::RefusedFriendly;
    target = HandleOf(_command.target);
  }
  ShipHandle gate;
  if (_command.kind == FleetOrderKind::Jump)
  {
    const GateId row = GateAt(_command.gate);
    if (row == INVALID_GATE_ID)
      return FleetOrderResult::NotAGate;
    // One gate and one refusal. There is deliberately no standing check beside it: a gate takes
    // anyone this phase, and half a gate-standings design invented here is the mistake the stations
    // design declined rather than a head start (Design/Universe.md 6.1).
    gate = m_gates[row].structure;
  }
  else if (_command.kind == FleetOrderKind::Mine)
  {
    // Known to the wire, and refused until there is a mining design and something in the universe to
    // mine. The byte is spent either way, which is the point of reserving it (ShipState.h).
    return FleetOrderResult::Unsupported;
  }

  Fleet& fleet = m_fleets[id];
  if (_command.kind == FleetOrderKind::Stop || _command.kind == FleetOrderKind::Idle)
  {
    // A brake. Every member is left where it stands with nothing to do, and the row holds no order
    // at all -- stopping is asking for Idle, not for a destination.
    fleet.orderKind = FleetOrderKind::Idle;
    fleet.orderStation = ShipHandle{};
    fleet.orderTarget = ShipHandle{};
    fleet.orderGate = ShipHandle{};
    fleet.threat = ShipHandle{};
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      const ShipId member = Resolve(fleet.members[at]);
      if (member == INVALID_SHIP_ID)
        continue;
      m_ships[member].order = OrderState::Idle;
      m_ships[member].orderSpeedCapMetresPerSec = 0.0f;
      m_dockings[member].active = false;
    }
    return FleetOrderResult::Ordered;
  }

  fleet.orderKind = _command.kind;
  fleet.orderPoint = _command.point;
  fleet.orderFacingRad = _command.facingRad;
  fleet.orderHasFacing = _command.hasFacing;
  fleet.orderStation = station;
  fleet.orderTarget = target;
  fleet.orderGate = gate;

  // An explicit order outranks the standing behavior, which is the line IssueDockOrder already
  // carries for patrols: the threat is dropped and everybody is re-tasked. The alert is left
  // burning -- the button should not stop glowing because the player gave an order -- and if the
  // attacker persists, the next stated act rouses the defense again with a fresh anchor
  // (Design/Archive/Fleets.md 7.4).
  fleet.threat = ShipHandle{};
  LowerFleetOrder(fleet);
  return FleetOrderResult::Ordered;
}

std::span<const DespawnRecord> Universe::DespawnsSince(std::uint64_t _cursor) const noexcept
{
  if (_cursor <= m_despawnBase)
    return m_despawnLog;
  const std::uint64_t offset = _cursor - m_despawnBase;
  if (offset >= m_despawnLog.size())
    return {};
  return std::span<const DespawnRecord>(m_despawnLog).subspan(static_cast<std::size_t>(offset));
}

void Universe::TrimDespawnsBefore(std::uint64_t _cursor) noexcept
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

std::span<const ShotRecord> Universe::ShotsSince(std::uint64_t _cursor) const noexcept
{
  // DespawnsSince' rule, and its reason: a cursor older than what the log still holds returns
  // everything held rather than reporting the gap. Over-reporting is the safe direction here too --
  // the publisher intersects these with one subscriber's interest set, so a shot it should not see
  // is dropped there.
  if (_cursor <= m_shotBase)
    return m_shotLog;
  const std::uint64_t offset = _cursor - m_shotBase;
  if (offset >= m_shotLog.size())
    return {};
  return std::span<const ShotRecord>(m_shotLog).subspan(static_cast<std::size_t>(offset));
}

void Universe::TrimShotsBefore(std::uint64_t _cursor) noexcept
{
  if (_cursor <= m_shotBase)
    return;
  const std::uint64_t drop = std::min<std::uint64_t>(_cursor - m_shotBase, m_shotLog.size());
  m_shotLog.erase(m_shotLog.begin(), m_shotLog.begin() + static_cast<std::ptrdiff_t>(drop));
  m_shotBase += drop;
}

ShipHandle Universe::HandleOf(ShipId _id) const noexcept
{
  if (_id >= m_ships.size())
    return {};
  const std::uint32_t slot = m_shipSlot[_id];
  return ShipHandle{slot, m_slots[slot].generation};
}

ShipId Universe::Resolve(ShipHandle _handle) const noexcept
{
  if (_handle.generation == 0 || _handle.slot >= m_slots.size())
    return INVALID_SHIP_ID;
  const Slot& slot = m_slots[_handle.slot];
  return (slot.generation == _handle.generation) ? slot.ship : INVALID_SHIP_ID;
}

void Universe::ConfigureShard(ShardId _shard) noexcept
{
  m_shard = _shard;
}

EntityId Universe::EntityIdOf(ShipId _id) const noexcept
{
  return (_id < m_ships.size()) ? m_slots[m_shipSlot[_id]].entity : INVALID_ENTITY_ID;
}

EntityId Universe::EntityIdOf(ShipHandle _handle) const noexcept
{
  if (_handle.generation == 0 || _handle.slot >= m_slots.size())
    return INVALID_ENTITY_ID;
  const Slot& slot = m_slots[_handle.slot];
  return (slot.generation == _handle.generation) ? slot.entity : INVALID_ENTITY_ID;
}

// The index is sorted by id, so this is a binary search returning the row that holds _entity, or
// m_entityRows.size() when nothing does. One function for all three operations below, because a
// lookup and an insertion point are the same question asked twice.
std::uint32_t Universe::FindEntityRow(EntityId _entity) const noexcept
{
  const auto at = std::lower_bound(m_entityRows.begin(), m_entityRows.end(), _entity,
                                   [](const EntityRow& _row, EntityId _key) { return _row.entity < _key; });
  return static_cast<std::uint32_t>(at - m_entityRows.begin());
}

void Universe::InsertEntityRow(EntityId _entity, std::uint32_t _slot)
{
  // Append when it belongs at the end, which is every spawn this universe mints for itself: serials
  // increase, so the new id is greater than every id already here. Only an id issued elsewhere --
  // a handoff, or a reload out of order -- pays for the insert.
  if (m_entityRows.empty() || m_entityRows.back().entity < _entity)
  {
    m_entityRows.push_back(EntityRow{_entity, _slot});
    return;
  }
  m_entityRows.insert(m_entityRows.begin() + FindEntityRow(_entity), EntityRow{_entity, _slot});
}

void Universe::EraseEntityRow(EntityId _entity) noexcept
{
  const std::uint32_t at = FindEntityRow(_entity);
  if (at < m_entityRows.size() && m_entityRows[at].entity == _entity)
    m_entityRows.erase(m_entityRows.begin() + at);
}

ShipId Universe::ResolveEntity(EntityId _entity) const noexcept
{
  if (_entity == INVALID_ENTITY_ID)
    return INVALID_SHIP_ID;
  const std::uint32_t at = FindEntityRow(_entity);
  if (at >= m_entityRows.size() || m_entityRows[at].entity != _entity)
    return INVALID_SHIP_ID;
  return m_slots[m_entityRows[at].slot].ship;
}

ShipHandle Universe::HandleOfEntity(EntityId _entity) const noexcept
{
  const ShipId id = ResolveEntity(_entity);
  return (id != INVALID_SHIP_ID) ? HandleOf(id) : ShipHandle{};
}

void Universe::ConfigureIndex(const SpatialIndex::Desc& _desc)
{
  m_index.Configure(_desc);
  m_staticIndexDirty = true;
}

std::span<const Neighbour> Universe::NeighboursOf(ShipId _id) const noexcept
{
  if (_id >= m_neighbourCount.size())
    return {};
  return std::span<const Neighbour>(m_neighbours).subspan(m_neighbourStart[_id], m_neighbourCount[_id]);
}

std::span<const UniversePos> Universe::RouteOf(ShipId _id) const noexcept
{
  if (_id >= m_routes.size())
    return {};
  const Route& route = m_routes[_id];
  return std::span<const UniversePos>(route.waypoint, route.count).subspan(route.cursor);
}

void Universe::AssignPatrol(ShipId _ship, ShipId _anchorStation, float _ringRadiusMetres, float _cruiseSpeedMetresPerSec)
{
  if (_ship >= m_ships.size() || _anchorStation >= m_ships.size() || _ship == _anchorStation)
    return;

  Patrol& patrol = m_patrols[_ship];
  patrol.anchor = HandleOf(_anchorStation);
  patrol.ringRadiusMetres = _ringRadiusMetres;
  patrol.cruiseSpeedMetresPerSec = _cruiseSpeedMetresPerSec;
  // The nearest point *minus one*, because the pass issues waypointIndex + 1: the first leg then
  // goes to the point the ship is already nearest, rather than back round the ring to zero.
  const std::uint32_t nearest = NearestPatrolRingIndex(m_ships[_anchorStation].posUniverse, m_ships[_ship].posUniverse);
  patrol.waypointIndex = (nearest + PATROL_RING_WAYPOINTS - 1) % PATROL_RING_WAYPOINTS;
  patrol.active = true;
}

const Universe::Patrol& Universe::PatrolOf(ShipId _id) const noexcept
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
// without ceremony, is what docking is until stations have an inside (Design/Archive/Stations.md 14). Its
// *own* range rather than a shared one, so a Carrier is not asked to fly to an Interceptor's
// doorstep and shove its way there against the separation pass.
[[nodiscard]] UniversePos DockApproachPoint(const UniversePos& _station, const UniversePos& _ship, float _dockRangeMetres) noexcept
{
  const float dx = OffsetX(_station, _ship);
  const float dz = OffsetZ(_station, _ship);
  const float distance = std::sqrt(dx * dx + dz * dz);

  UniversePos point = _station;
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

// The bearing from a ship to a point, in that ship's own hull frame: 0 is dead ahead, positive is to
// starboard, wrapped to (-pi, pi].
//
// atan2(x, z) rather than the usual atan2(y, x), because this game's heading convention is north-up
// -- 0 points along +Z and forward is (sin h, 0, cos h) -- and PatrolRingPoint reads a bearing the
// same way. XMScalarModAngle is the tree's wrap and Movement.cpp's headingError already uses it;
// a second one written here would be a second thing to get wrong.
[[nodiscard]] float BearingInHullFrame(const ShipState& _ship, const UniversePos& _targetPos) noexcept
{
  const float universeBearingRad = std::atan2(OffsetX(_ship.posUniverse, _targetPos), OffsetZ(_ship.posUniverse, _targetPos));
  return XMScalarModAngle(universeBearingRad - _ship.headingRad);
}
} // namespace

const Universe::Docking& Universe::DockingOf(ShipId _id) const noexcept
{
  return m_dockings[_id];
}

Universe::DockOrderResult Universe::IssueDockOrder(std::span<const ShipId> _ships, ShipId _station, FactionId _issuerFaction)
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
    const UniversePos approach = DockApproachPoint(m_ships[_station].posUniverse, ship.posUniverse, approachRange);
    PlanRoute(id, approach, HullSpecOf(ship.hullId).BoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
  }
  return DockOrderResult::Ordered;
}

void Universe::StepDockings()
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
    if (Distance(m_ships[structure].posUniverse, ship.posUniverse) <= DockRangeMetres(stationHull, shipHull))
    {
      // Checked again here, and not only at order time. This closes the window between an accepted
      // order and an aggression recorded during the flight: the door is guarded, not just the
      // doorbell (Design/Archive/Stations.md 7.3).
      if (StandingOf(m_stations[station].ownerFaction, ship.factionId) == Standing::Hostile)
      {
        docking.active = false;
        continue;
      }
      // Everything the capture needs, taken now: after the walk this ship's id may name another.
      // Hull and faction are the whole of what a ship is today, so they are the whole ledger row --
      // when undocking arrives it spawns a fresh ship from it, with a fresh handle
      // (Design/Archive/Stations.md 7.3).
      // A garrison ship coming home is not a guest: no ledger row, and the hull returns to the
      // complement by simply stopping being counted (Design/Archive/Stations.md 8.3).
      const bool garrison = m_protectors[id].active && m_protectors[id].home == station;
      m_captureScratch.push_back(Capture{HandleOf(id), station, ship.hullId, ship.factionId, garrison});
      continue;
    }

    if (ship.order != OrderState::Idle)
      continue; // still flying the last leg; arrival is the order machinery's, not this pass's

    // Re-issued whenever the ship goes Idle short of range -- shoved off by traffic, replanned,
    // blocked -- so docking is patient the way a patrol is, with no arrival logic of its own.
    ship.order = OrderState::Moving;
    ship.orderHasFacing = false;
    ship.orderSpeedCapMetresPerSec = 0.0f;
    const UniversePos approach =
      DockApproachPoint(m_ships[structure].posUniverse, ship.posUniverse, DockApproachRangeMetres(stationHull, shipHull));
    PlanRoute(id, approach, shipHull.BoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
  }

  // After the walk, in collection order -- which is array order, which is deterministic. During it
  // would make the visit order depend on who docked, because DespawnShip swap-and-pops four
  // parallel tables. A station id stays valid across this loop: stations do not despawn.
  for (const Capture& capture : m_captureScratch)
  {
    if (!capture.isGarrison)
      m_stations[capture.station].docked.push_back(DockedShip{capture.hullId, capture.factionId});
    (void)DespawnShip(capture.ship, DespawnCause::Docked);
  }
  m_captureScratch.clear();
}

void Universe::StepJumps()
{
  m_jumpScratch.clear();

  // Walked per fleet rather than per ship, because atomicity is a fleet's property: the question
  // this pass asks is "is EVERY live member inside the gate", and no ship can answer it alone
  // (ADR 0056).
  for (Fleet& fleet : m_fleets)
  {
    if (fleet.orderKind != FleetOrderKind::Jump)
      continue;

    const ShipId gate = Resolve(fleet.orderGate);
    const GateId row = (gate == INVALID_SHIP_ID) ? INVALID_GATE_ID : GateAt(gate);
    if (row == INVALID_GATE_ID)
    {
      fleet.orderKind = FleetOrderKind::Idle; // the gate is gone, or is no longer one: stand down
      fleet.orderGate = ShipHandle{};
      continue;
    }

    // Where the road leads, resolved before anybody is despawned. A destination that no longer names
    // a live gate strands nobody: the fleet holds at the near gate with its order standing, and the
    // moment the far side exists again it crosses. Losing a fleet into a gate that leads nowhere is
    // the one failure this pass must not have (Design/Universe-slice-2.md 4.6).
    const ShipId farGate = ResolveEntity(m_gates[row].destination);
    if (farGate == INVALID_SHIP_ID || farGate == gate || GateAt(farGate) == INVALID_GATE_ID)
      continue;

    // Whole or not at all, and the test is over every LIVE member: a member that died on the way is
    // not a member any more, and the fleet pass prunes it at the end of this same tick. A fleet with
    // nobody left in space crosses nothing.
    bool everyoneIsHere = false;
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      const ShipId member = Resolve(fleet.members[at]);
      if (member == INVALID_SHIP_ID)
        continue;
      everyoneIsHere = true;
      // Measured to the skins, per pair, because a Structure's centre is 251 m inside its own hull
      // and a flat centre-to-centre radius is a circle no ship can enter (HullSpec.h, GateRangeMetres).
      if (Distance(m_ships[member].posUniverse, m_ships[gate].posUniverse) >
          GateRangeMetres(HullSpecOf(m_ships[gate].hullId), HullSpecOf(m_ships[member].hullId)))
      {
        everyoneIsHere = false;
        break;
      }
    }
    if (!everyoneIsHere)
      continue; // still arriving; the approach order keeps them coming, and this pass is patient

    // The fleet arrives pointed the way the far gate faces. Which way that is away from a star is a
    // genesis concept this library does not have, and a heading is content the structure already
    // carries -- so using it keeps this pass free of any layout knowledge.
    const float exitRad = m_ships[farGate].headingRad;
    const UniversePos farPos = m_ships[farGate].posUniverse;

    // How many are crossing, counted before anything is captured, so the spread below can be centred
    // on the gate rather than starting at it.
    std::uint32_t crossing = 0;
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
      crossing += (Resolve(fleet.members[at]) != INVALID_SHIP_ID) ? 1u : 0u;

    // Everything the far side needs, taken while the ship is still here: after the walk this id may
    // name another ship. Ids would not do -- each despawn swap-and-pops -- which is the dock pass's
    // argument for its captures, unchanged.
    std::uint32_t placed = 0;
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      const ShipId member = Resolve(fleet.members[at]);
      if (member == INVALID_SHIP_ID)
        continue;

      // Abreast on the exit bearing, one spacing apart and centred on the gate, so a fleet arrives
      // spread rather than stacked on one point. The separation solver would push them apart anyway;
      // arriving already apart is what makes a crossing read as a fleet rather than as a pile.
      const float across = JUMP_ARRIVAL_SPACING_METRES * (static_cast<float>(placed) - 0.5f * static_cast<float>(crossing - 1u));
      UniversePos arrival = farPos;
      Translate(arrival, std::cos(exitRad) * across + std::sin(exitRad) * JUMP_ARRIVAL_STANDOFF_METRES,
                -std::sin(exitRad) * across + std::cos(exitRad) * JUMP_ARRIVAL_STANDOFF_METRES);

      const ShipState& ship = m_ships[member];
      m_jumpScratch.push_back(Jumper{fleet.members[at], EntityIdOf(fleet.members[at]), ship.hullId, ship.factionId, ship.hullPoints,
                                     arrival, exitRad, fleet.ownerFaction, fleet.slot, at});
      ++placed;
    }

    // The order is spent by being obeyed, and the alert goes with it. Fleeing through a gate is
    // escape: a leash anchored a system away would never release, so the threat is dropped here
    // rather than left to time out on the far side (Design/Universe.md 6.2).
    fleet.orderKind = FleetOrderKind::Idle;
    fleet.orderGate = ShipHandle{};
    fleet.threat = ShipHandle{};
    fleet.alertTicks = 0;
  }

  // After the walk, in collection order -- which is array order, which is deterministic. During it
  // would make the visit order depend on who jumped, for the reason the dock pass gives.
  for (const Jumper& jumper : m_jumpScratch)
  {
    if (!DespawnShip(jumper.ship, DespawnCause::JumpedOut))
      continue;

    // SpawnShipAs under the SAME identity is the whole of what a jump is: the ship that left is the
    // ship that arrives, and every client that holds it goes on holding it. It is also, exactly, a
    // shard handoff with no transport in the middle -- which is the property this pass exists to
    // prove (ADR 0056).
    const ShipId born = SpawnShipAs(jumper.entity, jumper.arrivalPos, jumper.headingRad, jumper.hullId, jumper.factionId);
    if (born == INVALID_SHIP_ID)
      continue;

    // Damage rides across; intent does not. A fresh row's route, patrol, docking, duty and mounts
    // are already at their rest state, which is exactly what the far side should re-derive
    // (Design/Universe.md 6.3).
    m_ships[born].hullPoints = jumper.hullPoints;

    // The fleet takes the new handle in the slot the old one left. Without this the row goes on
    // holding handles that no longer resolve, StepFleets prunes every one of them at the end of this
    // same tick, and the fleet retires on the tick it arrived -- the ships would be there and the
    // fleet would not (Design/Universe.md 6.2).
    const FleetId fleetId = FleetInSlot(jumper.ownerFaction, jumper.slot);
    if (fleetId != INVALID_FLEET_ID && jumper.memberIndex < m_fleets[fleetId].memberCount)
      m_fleets[fleetId].members[jumper.memberIndex] = HandleOf(born);
  }
  m_jumpScratch.clear();
}

void Universe::StepPatrols()
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
    const UniversePos waypoint = PatrolRingPoint(m_ships[anchor].posUniverse, patrol.waypointIndex, patrol.ringRadiusMetres);
    PlanRoute(id, waypoint, HullSpecOf(ship.hullId).BoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
  }
}

void Universe::StepProtectors()
{
  // A launch spawns a ship and a pursuit issues an order, and both plan routes, so the islands have
  // to be current here for the reason StepPatrols rebuilds them.
  RebuildStaticIfDirty();

  // 1. The duty pass. Re-target what has lost its quarry, pursue what has one.
  for (ShipId id = 0; id < ShipCount(); ++id)
  {
    ProtectorDuty& duty = m_protectors[id];
    if (!duty.active)
      continue;
    if (duty.home >= m_stations.size())
    {
      duty.active = false; // its station is not one any more; nothing to be a garrison of
      continue;
    }

    ShipId target = Resolve(duty.target);
    if (target == INVALID_SHIP_ID)
    {
      // Dead or docked -- a stale handle either way, and the difference is not this pass's business.
      // The home station's list is pruned as it is read, so it stays dense and in arrival order.
      Station& home = m_stations[duty.home];
      std::size_t live = 0;
      for (std::size_t at = 0; at < home.targets.size(); ++at)
      {
        if (Resolve(home.targets[at]) != INVALID_SHIP_ID)
          home.targets[live++] = home.targets[at];
      }
      home.targets.resize(live);

      if (home.targets.empty())
      {
        // Nothing left to hunt: go home and dock, through the same table a player's dock order
        // writes. Standing down is one intent write rather than a return-behavior of its own, which
        // is the reuse the docking table exists for (Design/Archive/Stations.md 8.3, 13).
        duty.target = ShipHandle{};
        const ShipId structure = Resolve(m_stations[duty.home].structure);
        if (structure != INVALID_SHIP_ID && !m_dockings[id].active)
        {
          m_dockings[id].station = m_stations[duty.home].structure;
          m_dockings[id].active = true;
          m_ships[id].order = OrderState::Idle; // let the dock pass issue the approach on its terms
        }
        continue;
      }

      duty.target = home.targets.front();
      target = Resolve(duty.target);

      // It may have been on its way home. It has somewhere to be again.
      m_dockings[id].active = false;
      m_ships[id].order = OrderState::Idle; // re-aim below rather than finish the leg home
    }

    // Pursue. The law does not cruise, and it does not give up: PursueTarget is the same chassis a
    // fleet's defense turns on whoever shot at it (Design/Archive/Fleets.md 3).
    PursueTarget(id, target);
  }

  // 2. The launch pass. Per station in index order, which is the only order there is.
  m_launchScratch.clear();
  for (StationId station = 0; station < m_stations.size(); ++station)
  {
    Station& home = m_stations[station];

    std::size_t live = 0;
    for (std::size_t at = 0; at < home.targets.size(); ++at)
    {
      if (Resolve(home.targets[at]) != INVALID_SHIP_ID)
        home.targets[live++] = home.targets[at];
    }
    home.targets.resize(live);

    if (home.targets.empty() || home.protectorComplement == 0)
    {
      // A station at peace does not run a metronome, and one that reset its cooldown launches its
      // first protector on the tick it is provoked rather than up to launchEveryTicks later.
      home.launchCooldownTicks = 0;
      continue;
    }

    if (home.launchCooldownTicks > 0)
    {
      --home.launchCooldownTicks;
      continue;
    }
    if (LaunchedProtectorCount(station) >= home.protectorComplement)
      continue;

    const ShipId structure = Resolve(home.structure);
    const ShipId target = Resolve(home.targets.front());
    if (structure == INVALID_SHIP_ID || target == INVALID_SHIP_ID)
      continue;

    // At the skin, on the bearing toward the first live target, heading outward. Clear of the
    // station by AVOID_MARGIN_METRES so a protector does not appear inside its own home's
    // separation band and spend its first seconds being shoved out of it.
    const float dx = OffsetX(m_ships[structure].posUniverse, m_ships[target].posUniverse);
    const float dz = OffsetZ(m_ships[structure].posUniverse, m_ships[target].posUniverse);
    const float distance = std::sqrt(dx * dx + dz * dz);
    const float bearingRad = (distance > 0.0001f) ? std::atan2(dx, dz) : 0.0f;

    const float standoff = HullSpecOf(m_ships[structure].hullId).BoundingRadiusMetres() +
                           HullSpecOf(home.protectorHullId).BoundingRadiusMetres() + AVOID_MARGIN_METRES;
    UniversePos posUniverse = m_ships[structure].posUniverse;
    Translate(posUniverse, std::sin(bearingRad) * standoff, std::cos(bearingRad) * standoff);

    m_launchScratch.push_back(Launch{station, posUniverse, bearingRad, home.protectorHullId, home.ownerFaction});
    home.launchCooldownTicks = home.launchEveryTicks;
  }

  // 3. Apply. After the passes, because a spawn appends to the very tables they walk -- and a ship
  // spawned here enters pass 0 with prevPos == posUniverse and participates from its first tick,
  // exactly as a boot spawn does (Design/Archive/Stations.md 10).
  for (const Launch& launch : m_launchScratch)
  {
    const ShipId id = SpawnShip(launch.posUniverse, launch.headingRad, launch.hullId, launch.factionId);
    m_protectors[id].home = launch.home;
    m_protectors[id].target = m_stations[launch.home].targets.empty() ? ShipHandle{} : m_stations[launch.home].targets.front();
    m_protectors[id].active = true;
  }
  m_launchScratch.clear();
}

void Universe::StepFleets()
{
  // Prune. In array order, compacting in place, which is the idiom StepProtectors uses on a
  // station's target list: the survivors keep their relative order, and that order is the one the
  // fleet was formed in -- which is what a formation solve will read as slot order.
  for (Fleet& fleet : m_fleets)
  {
    std::uint32_t live = 0;
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      if (Resolve(fleet.members[at]) != INVALID_SHIP_ID)
        fleet.members[live++] = fleet.members[at];
    }
    // Cleared rather than left as it was, so that a row written out live-members-only reads back
    // equal to the row that was saved. A Route deliberately leaves its dead waypoints alone; the
    // difference is that nothing ever compares two routes whole and this row is compared whole.
    for (std::uint32_t at = live; at < fleet.memberCount; ++at)
      fleet.members[at] = ShipHandle{};
    fleet.memberCount = live;
  }

  // Retire. Backwards, so the row swap-and-pop brings in from the end is one this walk has already
  // looked at and needs no second visit. Nothing stores a fleet index across a tick -- an owner and
  // a slot is the name that survives -- so there is nothing for the swap to break.
  for (std::size_t at = m_fleets.size(); at-- > 0;)
  {
    if (m_fleets[at].memberCount != 0 || m_fleets[at].manifestCount != 0)
      continue;
    if (at + 1 < m_fleets.size())
      m_fleets[at] = m_fleets.back();
    m_fleets.pop_back();
  }

  // Launch. One hull off the manifest per FLEET_LAUNCH_EVERY_TICKS, at the station's skin, fanned
  // across the station's outward bearing (Design/Archive/Fleets.md 5.3).
  //
  // This pass spawns during its own walk where StepProtectors collects and applies after one, and
  // the difference is deliberate rather than an oversight: that pass walks the very tables a spawn
  // appends to, and this one walks m_fleets, which a spawn does not touch.
  for (Fleet& fleet : m_fleets)
  {
    if (fleet.manifestCount == 0)
      continue;

    const ShipId structure = Resolve(fleet.launchStructure);
    if (structure == INVALID_SHIP_ID || StationAt(structure) == INVALID_STATION_ID)
    {
      // The door is gone, and launching is the only way out of a ledger, so the manifest can never
      // become ships. Dropping it is what stops a fleet nothing can ever fill from holding one of
      // five slots for ever; the retire above frees it on the next tick.
      fleet.manifestCount = 0;
      continue;
    }

    if (fleet.launchCooldownTicks > 0)
    {
      --fleet.launchCooldownTicks;
      continue;
    }

    // Every distance below is the widest hull of the WHOLE composed set -- what is still inside and
    // what is already out -- so the geometry does not move as the manifest empties.
    std::uint32_t largestHullId = fleet.manifest[0];
    float largestRadius = 0.0f;
    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      const ShipId member = Resolve(fleet.members[at]);
      if (member != INVALID_SHIP_ID && HullSpecOf(m_ships[member].hullId).BoundingRadiusMetres() > largestRadius)
      {
        largestHullId = m_ships[member].hullId;
        largestRadius = HullSpecOf(largestHullId).BoundingRadiusMetres();
      }
    }
    for (std::uint32_t at = 0; at < fleet.manifestCount; ++at)
    {
      if (HullSpecOf(fleet.manifest[at]).BoundingRadiusMetres() > largestRadius)
      {
        largestHullId = fleet.manifest[at];
        largestRadius = HullSpecOf(largestHullId).BoundingRadiusMetres();
      }
    }

    // Taken by value, and that is not a style choice: SpawnShip below appends to m_ships, which may
    // reallocate it, and a reference into that vector would be dangling by the time the rally point
    // is worked out. It reads correct for as long as the vector happens to have spare capacity,
    // which is why the test that caught it is the one that reloads a universe -- a universe out of a file
    // has exactly as much capacity as it has ships.
    const UniversePos stationPos = m_ships[structure].posUniverse;
    // The station's own heading is which way is out. Design/Archive/Fleets.md 5.3 says the outward bearing
    // from the system's star, and Universe has no star and must not learn about one -- the layout is
    // content the composition root reads (ADR 0037). A station's facing is already simulation state
    // and already authored by whoever spawned it, which makes it the honest place for a door.
    const float outwardRad = m_ships[structure].headingRad;
    // Safe to hold: a HullSpec is a row of a constexpr table, not of m_ships.
    const HullSpec& stationHull = HullSpecOf(m_ships[structure].hullId);

    const float spacing = SlotSpacingMetres(largestRadius);
    const float standoff = stationHull.BoundingRadiusMetres() + largestRadius + AVOID_MARGIN_METRES;

    // Which of the composed formation's slots this hull is launching into, and the whole of what
    // decides where it appears and where it goes. It counts down with the manifest, so it is unique
    // per launch whatever else happens to the fleet.
    const int composedCount = static_cast<int>(fleet.memberCount + fleet.manifestCount);
    const int slot = static_cast<int>(fleet.manifestCount) - 1;

    // Two launches of one fleet can never be born touching, and that is construction rather than
    // hope: consecutive spawn points sit one slot spacing apart on the circle they appear on, and
    // the slot they are indexed by strictly decreases -- so no two launches of one fleet ever share
    // a bearing, not even across a loss. The cadence is not what makes this safe; 0.75 s buys a
    // Corvette about 5.6 m from a standing start, well inside its own hull.
    //
    // The fan runs to one side of the door rather than either side of it, and that is what keeps it
    // independent of how many ships are left: a fan centred on the outward bearing would shift by
    // half a step every time the composed count changed, and two launches could then land on one
    // bearing after a loss.
    const float laneStepRad = spacing / standoff;
    const float bearingRad = outwardRad + static_cast<float>(slot) * laneStepRad;

    UniversePos spawnPos = stationPos;
    Translate(spawnPos, std::sin(bearingRad) * standoff, std::cos(bearingRad) * standoff);

    // Off the front of the manifest, so the launch order is the ascending hull id ComposeFleet
    // filled it in. The vacated entry is cleared for the reason a pruned member's is.
    const std::uint32_t hullId = fleet.manifest[0];
    for (std::uint32_t at = 1; at < fleet.manifestCount; ++at)
      fleet.manifest[at - 1] = fleet.manifest[at];
    --fleet.manifestCount;
    fleet.manifest[fleet.manifestCount] = 0;

    const ShipId launched = SpawnShip(spawnPos, bearingRad, hullId, fleet.ownerFaction);
    fleet.members[fleet.memberCount++] = HandleOf(launched);

    // Where the fleet forms up: outward, one slot spacing clear of the dock approach lane, so a
    // fleet assembling is not standing in the doorway. Through DockApproachRangeMetres rather than
    // its sum restated, so the launch and the dock cannot disagree about where a station's door is.
    UniversePos rallyPos = stationPos;
    const float rallyRange = DockApproachRangeMetres(stationHull, HullSpecOf(largestHullId)) + spacing;
    Translate(rallyPos, std::sin(outwardRad) * rallyRange, std::cos(outwardRad) * rallyRange);

    // A fleet that already has somewhere to be does not rally: the hull just born joins the order,
    // and the whole formation is re-solved for the ships that are now out. Re-solving is right here
    // and wrong at the rally below, which looks inconsistent and is not: at the rally the fleet is
    // packed against the station's door and a reshuffle makes ships cross at close quarters, while
    // under a standing order it is spread out and IssueMoveOrder's slot assignment -- by where the
    // ships already lie across the formation -- is exactly the property wanted (Design/Archive/Fleets.md 5.3).
    if (fleet.orderKind != FleetOrderKind::Idle)
    {
      LowerFleetOrder(fleet);
      fleet.launchCooldownTicks = FLEET_LAUNCH_EVERY_TICKS - 1;
      continue;
    }

    // Each ship gets its OWN slot of the formation the whole composed set will hold, decided once
    // when it launches and never reassigned, and it is the same slot index its spawn bearing was
    // taken from -- so the fan and the formation run the same way round and no two ships have to
    // cross to reach their places.
    //
    // The alternative is to re-issue one order over every member after each launch and let
    // IssueMoveOrder hand out slots by where the ships lie. That re-plans the whole fleet eight
    // times and reshuffles who is where, so ships already on station cross each other on every
    // launch -- which is how a formation that is merely forming up starts brushing. Measured, with
    // the reassignment: 1.0 cm of capsule overlap during a Corvette launch.
    const FormationShape shape = static_cast<FormationShape>(std::clamp(FORMATION_SHAPE, 0, 3));
    const XMFLOAT2 local = FormationOffset(slot, composedCount, shape, spacing);
    const float cosOut = std::cos(outwardRad);
    const float sinOut = std::sin(outwardRad);
    Translate(rallyPos, local.x * cosOut + local.y * sinOut, -local.x * sinOut + local.y * cosOut);

    // One ship, to one point, facing the way the fleet faces. A one-ship formation puts it exactly
    // there (FormationOffset's slot 0 of 1 is the origin), so the wedge is assembled a slot at a
    // time rather than solved again on every launch.
    m_fleetShipScratch.assign(1, launched);
    (void)IssueMoveOrder(m_fleetShipScratch, rallyPos, true, outwardRad, fleet.ownerFaction);

    // Minus one because the launch tick is one of the N: waiting the full count would put
    // consecutive launches N + 1 ticks apart, and the constant's name would be off by a tick.
    fleet.launchCooldownTicks = FLEET_LAUNCH_EVERY_TICKS - 1;
  }

  // The defense, then cruise and patience. All three are about a standing state rather than about
  // the tick it was set on, which is why they are a pass and not a line inside IssueFleetOrder.
  for (Fleet& fleet : m_fleets)
  {
    // The alert burns down whatever else happens, and takes the threat with it when it goes out: a
    // stale handle left in the row is one more thing for the codec to carry and for a later reader
    // to wonder about (Design/Archive/Fleets.md 7.3).
    if (fleet.alertTicks > 0)
      --fleet.alertTicks;

    // Engaged is three things at once, and losing any of them stands the fleet down. The alert being
    // one of them is what bounds a defense in time: without it, one shot from an attacker that then
    // parks inside the leash and does nothing would hold a fleet's combatants out of their orders
    // for ever. It is also what makes the wire's two bits differ -- a fleet can be under attack and
    // no longer engaged, which is the alert outliving the fight (Design/Archive/Fleets.md 7.2, 7.3, 8.2).
    bool engaged = false;
    if (fleet.threat.generation != 0)
    {
      const ShipId threat = Resolve(fleet.threat);
      engaged = fleet.alertTicks > 0 && threat != INVALID_SHIP_ID &&
                Distance(m_ships[threat].posUniverse, fleet.threatAnchorPos) <= FLEET_ENGAGE_RANGE_METRES;
      if (!engaged)
      {
        // Stood down, once: the threat is dead, docked, past the leash, or the alert has burned out.
        // The anchor goes with it -- what is stale is not left lying in the row for the codec to
        // carry and a later reader to wonder about.
        fleet.threat = ShipHandle{};
        fleet.threatAnchorPos = UniversePos{};

        // Back to the standing order, and NOT by leaving it to patience: pursuit overwrote each
        // combatant's route destination with the target's position, so patience would send it back
        // to where its quarry used to be. Re-lowering is what "return to the standing order" has to
        // mean once the order has been suspended (Design/Archive/Fleets.md 7.2).
        if (fleet.orderKind != FleetOrderKind::Idle)
        {
          LowerFleetOrder(fleet);
        }
        else
        {
          // Nothing else to do. Stopping is the honest answer to a fight ending with no orders.
          for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
          {
            const ShipId member = Resolve(fleet.members[at]);
            if (member != INVALID_SHIP_ID && HullSpecOf(m_ships[member].hullId).combatant)
            {
              m_ships[member].order = OrderState::Idle;
              m_ships[member].orderSpeedCapMetresPerSec = 0.0f;
            }
          }
        }
      }
    }

    // What the combatants are chasing, if anything. The defense outranks the standing order for as
    // long as it lasts; an ordered attack is what they chase when nothing is chasing them.
    ShipId chase = INVALID_SHIP_ID;
    if (engaged)
    {
      chase = Resolve(fleet.threat);
    }
    else if (fleet.orderKind == FleetOrderKind::Attack)
    {
      chase = Resolve(fleet.orderTarget);
      if (chase == INVALID_SHIP_ID)
      {
        // The target died or docked, which completes the order: the fleet reverts to Idle where it
        // stands rather than flying on to where its quarry was (Design/Archive/Fleets.md 6.5).
        fleet.orderKind = FleetOrderKind::Idle;
        fleet.orderTarget = ShipHandle{};
        for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
        {
          const ShipId member = Resolve(fleet.members[at]);
          if (member != INVALID_SHIP_ID)
          {
            m_ships[member].order = OrderState::Idle;
            m_ships[member].orderSpeedCapMetresPerSec = 0.0f;
          }
        }
      }
    }

    if (chase != INVALID_SHIP_ID)
    {
      // The combatants turn; everybody else carries on with whatever it was told, unmoved. They do
      // not flee: fleeing is a judgment about where safety is, which is a sense, and this design has
      // none (Design/Archive/Fleets.md 7.2, ADR 0041).
      for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
      {
        const ShipId member = Resolve(fleet.members[at]);
        if (member != INVALID_SHIP_ID && HullSpecOf(m_ships[member].hullId).combatant)
          PursueTarget(member, chase);
      }
    }

    if (fleet.orderKind != FleetOrderKind::Move && fleet.orderKind != FleetOrderKind::Dock)
      continue;

    // The fleet travels at its slowest member's speed, so it arrives -- and fights -- as one body
    // rather than strung out over kilometers (Design/Archive/Fleets.md 6.3, owner decision 3).
    //
    // Re-applied every tick rather than written once when the order was given, and that is a
    // correction the tree forces rather than a preference: StepDockings re-issues a docking ship's
    // approach whenever it goes Idle and zeroes orderSpeedCapMetresPerSec when it does, so a cap
    // written once would be dropped by the first re-issue. A rule the pass restates is a property; a
    // rule written once is a race with whatever else writes that field. It covers a member that
    // joined after the order for free.
    const float cruise = FleetCruiseSpeedMetresPerSec(fleet);

    for (std::uint32_t at = 0; at < fleet.memberCount; ++at)
    {
      const ShipId member = Resolve(fleet.members[at]);
      if (member == INVALID_SHIP_ID)
        continue;

      // A combatant that is chasing has had its standing order suspended: neither the fleet's pace
      // nor its patience applies to it until it stands down.
      if (chase != INVALID_SHIP_ID && HullSpecOf(m_ships[member].hullId).combatant)
        continue;

      ShipState& ship = m_ships[member];
      ship.orderSpeedCapMetresPerSec = cruise;

      // Patience. A member with nothing to do that is not where its order left it was shoved off,
      // blocked or re-planned, and its leg is re-issued to the point it already had -- never to a
      // re-solved formation, which would reshuffle the whole fleet every time one ship was jostled.
      // The dock pass does this for its own approach, which is why a docking fleet needs nothing
      // here beyond the cap above (Design/Archive/Fleets.md 4.4).
      if (fleet.orderKind != FleetOrderKind::Move || ship.order != OrderState::Idle)
        continue;

      // The member's own route destination, and never the fleet's order point. That is not a detail:
      // a route whose point the wall forbids ends as close as the geometry allows and AdvanceRoute
      // moves the destination to where the ship stands, so that it is never re-planned back at a
      // point it cannot reach (ADR 0042). Reading route.destination inherits that; reading
      // fleet.orderPoint would discard it and re-plan a fleet ordered into a wall on every tick for
      // ever -- an A* a tick, invisible from outside the tick, because the ship would be set Moving
      // at the top of Step and put back to Idle before the end of it.
      const Route& route = m_routes[member];
      const HullSpec& hull = HullSpecOf(ship.hullId);
      if (Distance(ship.posUniverse, route.destination) <= ArrivalRadiusMetres(hull))
        continue; // arrived, or standing as close to its slot as it is ever going to get

      ship.order = OrderState::Moving;
      PlanRoute(member, route.destination, hull.BoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
    }
  }
}

bool Universe::HoldsHostile(ShipId _owner, ShipId _other) const noexcept
{
  return StandingOf(m_ships[_owner].factionId, m_ships[_other].factionId) == Standing::Hostile;
}

bool Universe::MountTargetStands(ShipId _shooter, ShipId _target, const DeviceSpec& _device) const noexcept
{
  if (_target == INVALID_SHIP_ID || _target >= m_ships.size() || _target == _shooter)
    return false;

  const ShipState& shooter = m_ships[_shooter];
  const ShipState& target = m_ships[_target];

  // No priority may resolve to a friend. This one line is where "there is no friendly fire" stops
  // being a promise in a design document and becomes a property of the simulation: a shot lands on
  // its acquired target and nowhere else, and an own-faction ship is never acquired
  // (Design/Combat.md 11, ADR 0052).
  if (target.factionId == shooter.factionId)
    return false;

  const float toSkin = Distance(shooter.posUniverse, target.posUniverse) - HullSpecOf(target.hullId).BoundingRadiusMetres();
  return toSkin <= _device.rangeMetres;
}

ShipId Universe::ChooseMountTarget(ShipId _ship, const DeviceSpec& _device, const MountState& _mount) const noexcept
{
  // 1 and 2 are the fleet's: the threat it took, then the attack it was ordered to make. A stated
  // target is shot whatever the standing table says, because an ordered attack on a neutral is the
  // player spending their own standing -- which is what makes it a decision rather than an accident.
  const FleetId fleet = FleetAt(_ship);
  if (fleet != INVALID_FLEET_ID)
  {
    const Fleet& row = m_fleets[fleet];
    // A threat handle only survives in the row while the fleet is engaged: StepFleets clears it on
    // the stand-down, so this needs no second reading of the alert and the leash.
    const ShipId threat = Resolve(row.threat);
    if (MountTargetStands(_ship, threat, _device))
      return threat;

    if (row.orderKind == FleetOrderKind::Attack)
    {
      const ShipId ordered = Resolve(row.orderTarget);
      if (MountTargetStands(_ship, ordered, _device))
        return ordered;
    }
  }

  // 3 is a protector's, and it is a protector's whole life: it is in no fleet, and the station that
  // launched it named the aggressor it is out here for.
  if (m_protectors[_ship].active)
  {
    const ShipId quarry = Resolve(m_protectors[_ship].target);
    if (MountTargetStands(_ship, quarry, _device))
      return quarry;
  }

  // The target this mount already held, which is what stops it restarting its traverse every time
  // two candidates trade places by a metre -- avoidHeadingRad's argument at gunnery scale. Hostile
  // standing is required of it even though it may have been acquired as a stated target, so an order
  // that ends is an order the guns stop obeying rather than a grudge they keep.
  const ShipId held = Resolve(_mount.target);
  if (MountTargetStands(_ship, held, _device) && HoldsHostile(_ship, held))
    return held;

  // 4 is the only sense in this design: the nearest ship this one's faction already holds hostile,
  // read out of the list the sense pass built and sorted by (surface proximity, ShipId). No query of
  // its own, no cadence, no scan -- and the neighbour cap's honest consequence rides along with it.
  // A hull whose K nearest are all friends does not see the enemy K+1 away, holds its fire and is
  // shot; the stated act that follows rouses its fleet, which is the failure correcting itself
  // rather than a case needing a mechanism (Design/Combat.md 5.2).
  for (const Neighbour& candidate : NeighboursOf(_ship))
  {
    if (MountTargetStands(_ship, candidate.id, _device) && HoldsHostile(_ship, candidate.id))
      return candidate.id;
  }
  return INVALID_SHIP_ID;
}

void Universe::StepMounts()
{
  m_shotScratch.clear();
  m_deathScratch.clear();

  // 1. The walk. It reads every ship's settled position and writes only the mounts of the ship it is
  //    visiting, so no part of its answer depends on the order the array is walked in.
  for (ShipId id = 0; id < m_ships.size(); ++id)
  {
    const ShipState& shooter = m_ships[id];
    const HullSpec& hull = HullSpecOf(shooter.hullId);
    ShipMounts& mounts = m_mounts[id];

    for (std::uint32_t at = 0; at < hull.MountCount(); ++at)
    {
      MountState& mount = mounts.mount[at];
      if (mount.cooldownTicks > 0)
        --mount.cooldownTicks;

      const MountSpec& spec = hull.loadout.mount[at];
      const DeviceSpec& device = DeviceSpecOf(spec.device);
      // A mining tool cycles in a design that does not exist yet. The byte is reserved so it never
      // renumbers and the pass skips it, rather than guessing what extraction would mean.
      if (device.kind != DeviceKind::Gun)
        continue;

      const ShipId target = ChooseMountTarget(id, device, mount);
      mount.target = (target != INVALID_SHIP_ID) ? HandleOf(target) : ShipHandle{};
      if (target == INVALID_SHIP_ID)
        continue;

      // Recomputed from where the ships are, never from the neighbour record's cached offsets: that
      // list is a candidate set built before this tick's motion, and the ships have moved since.
      const float bearingRad = BearingInHullFrame(shooter, m_ships[target].posUniverse);
      const float offBoreRad = XMScalarModAngle(bearingRad - spec.bearingRad);
      const bool bears = std::fabs(offBoreRad) <= spec.arcHalfRad;

      // The slew happens whether or not the shot does, because tracking is what a turret is for and
      // arriving aimed is the thing FIRE_ALIGN_RAD measures. A fixed mount has no slew at all: its
      // arc is its whole gate, and the hull's own turn is its traverse.
      bool aimed = true;
      if (!device.Fixed())
      {
        const float wantedRad = XMScalarModAngle(spec.bearingRad + std::clamp(offBoreRad, -spec.arcHalfRad, spec.arcHalfRad));
        const float stepRad = device.traverseRadPerSec * TICK_DT;
        const float deltaRad = std::clamp(XMScalarModAngle(wantedRad - mount.aimBearingRad), -stepRad, stepRad);
        mount.aimBearingRad = XMScalarModAngle(mount.aimBearingRad + deltaRad);
        aimed = std::fabs(XMScalarModAngle(bearingRad - mount.aimBearingRad)) <= FIRE_ALIGN_RAD;
      }

      // Range was spent in the selection above, which is where it belongs (Universe.h,
      // MountTargetStands). What is left is arc, aim and the cooldown.
      if (!bears || !aimed || mount.cooldownTicks > 0)
        continue;

      mount.cooldownTicks = device.cooldownTicks;
      m_shotScratch.push_back(Shot{HandleOf(id), HandleOf(target), device.damage, at});
    }
  }

  // 2. Damage, applied after the walk rather than during it: the walk is a read of every ship's
  //    position and this is a write to some of them.
  for (const Shot& shot : m_shotScratch)
  {
    const ShipId victim = Resolve(shot.victim);
    if (victim == INVALID_SHIP_ID)
      continue;

    // Logged before the damage lands and above the discard below, because a shot at a station is
    // still a shot: it draws a tracer and an impact whatever the hull does with the damage. Here
    // rather than in the walk so that a shot at something already gone is not reported -- the walk
    // resolves nothing, and this is the first place that does.
    m_shotLog.push_back(ShotRecord{EntityIdOf(shot.shooter), EntityIdOf(shot.victim), shot.mount});

    ShipState& hit = m_ships[victim];
    // An indestructible hull discards its damage, which is how Design/Archive/Stations.md 8.5's rule
    // -- "however it models damage, a Vanguard station's is discarded" -- is implemented. It is the
    // hull that decides, not the faction, so there is no station special case here at all.
    if (HullSpecOf(hit.hullId).maxHullPoints == 0)
      continue;

    const bool wasStanding = hit.hullPoints > 0;
    hit.hullPoints = (hit.hullPoints > shot.damage) ? hit.hullPoints - shot.damage : 0;
    // On the transition only, so eight shots landing on one hull in one tick collect one death.
    if (wasStanding && hit.hullPoints == 0)
      m_deathScratch.push_back(shot.victim);
  }

  // 3. The acts, and BEFORE the deaths below. That order is the whole reason these are three loops
  //    and not one: RecordHostileAct resolves its victim and returns early on a stale handle, so a
  //    fleet member killed outright by a single shot would rouse nobody at all if its death had
  //    already been applied. Being killed is the loudest hostile act there is.
  for (const Shot& shot : m_shotScratch)
  {
    RecordHostileAct(shot.shooter, shot.victim);

    const ShipId victim = Resolve(shot.victim);
    if (victim == INVALID_SHIP_ID)
      continue;

    // An act against a station, or against a garrison ship out on its duty, is the law's business
    // rather than a fleet's: Design/Archive/Stations.md 8.1's "the first hostile act against a
    // station or its garrison", which is the sentence ADR 0041 left this design to call.
    StationId provoked = StationAt(victim);
    if (provoked == INVALID_STATION_ID && m_protectors[victim].active)
      provoked = m_protectors[victim].home;
    if (provoked != INVALID_STATION_ID)
      RecordAggression(shot.shooter, provoked);
  }

  // 4. The deaths, through the door ADR 0040 already opened. Everything a death means to the rest of
  //    the game -- the departure run on the wire, the shatter and the shock ring, the fleet's prune,
  //    a protector's slot returning to its complement -- is already on the other side of it, which
  //    is why this pass is so much smaller than what it does.
  for (const ShipHandle& dead : m_deathScratch)
    (void)DespawnShip(dead, DespawnCause::Destroyed);
}

void Universe::PlanRoute(ShipId _id, const UniversePos& _destination, float _requiredClearanceMetres)
{
  ShipState& ship = m_ships[_id];
  Route& route = m_routes[_id];

  ++m_routePlans;
  const bool complete = m_pathIslands.FindPath(ship.posUniverse, _destination, _requiredClearanceMetres, m_routeScratch);
  route.destination = _destination;
  route.requiredClearanceMetres = _requiredClearanceMetres;
  route.count = std::min<std::uint32_t>(MAX_PATH_WAYPOINTS, static_cast<std::uint32_t>(m_routeScratch.size()));
  for (std::uint32_t at = 0; at < route.count; ++at)
    route.waypoint[at] = m_routeScratch[at];
  route.cursor = 0;
  route.legStart = ship.posUniverse;
  route.gridVersion = m_pathIslands.Version();
  route.reachesDestination = complete;
  route.blockedTicks = 0;

  // A grid that declined to build, or a start with nowhere to go, leaves the destination itself as
  // the only waypoint -- which is exactly the behaviour before this phase existed.
  ship.steerTargetPos = (route.count > 0) ? route.waypoint[0] : _destination;
}

void Universe::AdvanceRoute(ShipId _id)
{
  ShipState& ship = m_ships[_id];
  Route& route = m_routes[_id];
  if (ship.order != OrderState::Moving || route.count == 0)
    return;

  const HullSpec& hull = HullSpecOf(ship.hullId);

  // A waypoint the wall will not let the ship reach is taken as reached. Nothing else ends a leg
  // whose point is behind a structure: the order solver keeps asking for the point, the avoidance
  // fan scores every reachable heading as equally dangerous and keeps the one it has, and the
  // blocking pass undoes the advance every tick -- a ship at full thrust going nowhere, which is
  // what the owner saw. Taking the point as reached lets the next waypoint, or the arrival, decide
  // instead. The destination itself is moved to where the ship stands, so a route that ran out is
  // not re-planned back at the same unreachable point.
  if (route.blockedTicks >= BLOCKED_WAYPOINT_TICKS)
  {
    route.blockedTicks = 0;
    if (route.cursor + 1 < route.count)
    {
      ++route.cursor;
      route.legStart = ship.posUniverse;
      ship.steerTargetPos = route.waypoint[route.cursor];
    }
    else
    {
      route.destination = ship.posUniverse;
      route.reachesDestination = true;
      ship.steerTargetPos = ship.posUniverse;
    }
    return;
  }

  const bool arrived = Distance(ship.posUniverse, ship.steerTargetPos) <= ArrivalRadiusMetres(hull);
  if (arrived && route.cursor + 1 < route.count)
  {
    ++route.cursor;
    route.legStart = ship.posUniverse;
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
      const float alongX = OffsetX(route.legStart, ship.posUniverse);
      const float alongZ = OffsetZ(route.legStart, ship.posUniverse);
      const float along = std::clamp((alongX * legX + alongZ * legZ) / legLengthSquared, 0.0f, 1.0f);
      const float offX = alongX - legX * along;
      const float offZ = alongZ - legZ * along;
      if (offX * offX + offZ * offZ > PATH_REPLAN_DEVIATION_METRES * PATH_REPLAN_DEVIATION_METRES)
        PlanRoute(_id, route.destination, route.requiredClearanceMetres);
    }
  }
}

float Universe::AuthorityOf(ShipId _id) const noexcept
{
  return AvoidanceAuthorityOf(HullSpecOf(m_ships[_id].hullId), m_ships[_id].order);
}

void Universe::SnapshotPreviousTick() noexcept
{
  // Two lines, hoisted out of StepShip into a pass of their own, and that hoist is the whole of
  // the order-independence property. prevPos was already written here for the view to interpolate
  // between ticks; making it a whole pass turns it into the authoritative start-of-tick state that
  // every neighbour read below sees, so ship 0 and ship 500 see the same universe.
  for (ShipState& ship : m_ships)
  {
    ship.prevPos = ship.posUniverse;
    ship.prevHeading = ship.headingRad;
  }
}

void Universe::SettleDerivedState()
{
  RebuildStaticIfDirty();
}

void Universe::RebuildStaticIfDirty()
{
  if (!m_staticIndexDirty)
    return;
  {
    m_staticEntries.clear();
    for (ShipId id = 0; id < m_ships.size(); ++id)
    {
      const HullSpec& hull = HullSpecOf(m_ships[id].hullId);
      if (hull.immovable && hull.collidable)
        m_staticEntries.push_back({id, m_ships[id].posUniverse, hull.BoundingRadiusMetres()});
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

void Universe::RebuildIndex()
{
  RebuildStaticIfDirty();

  // The neighbourhood's extent rides along with the rebuild, which already walks every ship. It is
  // what stops a skirmish between fighters paying the Carrier's query radius because a Carrier
  // exists in the hull table (Design/Archive/MmoScalabilityReview.md U2).
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

void Universe::GatherNeighbours()
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

void Universe::ApplySeparation()
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

        const Capsule against =
          CapsuleAt(other, otherHull, OffsetX(ship.posUniverse, other.posUniverse), OffsetZ(ship.posUniverse, other.posUniverse));
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
      Translate(m_ships[id].posUniverse, m_correctionX[id], m_correctionZ[id]);
      m_appliedX[id] += m_correctionX[id];
      m_appliedZ[id] += m_correctionZ[id];
    }

    // A maximum rather than a sum, and that is not incidental: max is exact and order-independent
    // over floats where a sum is neither, so where the loop stops cannot depend on array order.
    if (largestCorrection < SEPARATION_SETTLE_METRES)
      break;
  }
}

void Universe::ApplyBlocking()
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

      const Capsule against =
        CapsuleAt(other, otherHull, OffsetX(ship.posUniverse, other.posUniverse), OffsetZ(ship.posUniverse, other.posUniverse));
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
  {
    Translate(m_ships[id].posUniverse, m_correctionX[id], m_correctionZ[id]);

    // Pushed away from the point it is steering at, or not blocked at all. A correction that does
    // not oppose the ship's own leg -- a hull shouldered sideways while it rounds a station -- does
    // not count, because that ship is still gaining on its point and the route is still right.
    Route& route = m_routes[id];
    const ShipState& ship = m_ships[id];
    const float toTargetX = OffsetX(ship.posUniverse, ship.steerTargetPos);
    const float toTargetZ = OffsetZ(ship.posUniverse, ship.steerTargetPos);
    const bool opposed = ship.order == OrderState::Moving && (m_correctionX[id] * toTargetX + m_correctionZ[id] * toTargetZ) < 0.0f;
    route.blockedTicks = opposed ? route.blockedTicks + 1 : 0;
  }
}

void Universe::Step()
{
  StepDockings();
  StepJumps();
  StepPatrols();
  StepProtectors();
  StepFleets();
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

  // The guns go last, once every ship has finished moving and while the neighbour list the sense
  // pass built is still addressed by the ids it was built with (Universe.h, StepMounts).
  StepMounts();
  ++m_tick;
}

float Universe::IssueMoveOrder(std::span<const ShipId> _ships, const UniversePos& _point, bool _hasFacing, float _facingRad,
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
      m_headingScratch.push_back(m_ships[id].posUniverse);
    heading = FormationHeading(m_headingScratch, _point, m_ships[chosen[0]].headingRad);
  }

  // Hand out slots in the order the ships already lie across the formation, so they do not have to
  // cross each other on the way in.
  const float rightX = std::cos(heading);
  const float rightZ = -std::sin(heading);
  const auto acrossFormation = [&](ShipId _id)
  { return OffsetX(_point, m_ships[_id].posUniverse) * rightX + OffsetZ(_point, m_ships[_id].posUniverse) * rightZ; };
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
    // (Design/Archive/Stations.md 7.1).
    m_dockings[id].active = false;

    UniversePos destination = _point;
    Translate(destination, local.x * cosH + local.y * sinH, -local.x * sinH + local.y * cosH);
    PlanRoute(id, destination, groupClearance);
  }
  return heading;
}
} // namespace Game
