#include "pch.h"
#include "UniverseView.h"

#include "ViewTuning.h"

#include "NmoFile.h"

using namespace DirectX;
using namespace Neuron;

namespace Outpost
{
void UniverseView::Init(Neuron::Transport& _transport, Camera& _camera, const MeshLibrary& _meshes, MeshHandle _quadMesh)
{
  m_transport = &_transport;
  m_camera = &_camera;
  m_meshes = &_meshes;
  m_quadMesh = _quadMesh;

  Neuron::SpriteParticles::Desc particleDesc;
  particleDesc.capacity = EXPLOSION_PARTICLE_CAPACITY;
  m_particles.Init(particleDesc);
  // Reserved once: a frame with five explosions on screen copies vertices, it does not allocate.
  m_fxFragmentVerts.reserve(Neuron::FxRenderer::MAX_FX_VERTS);
  m_fxSpriteVerts.reserve(Neuron::FxRenderer::MAX_FX_VERTS);
}

void UniverseView::AddBody(const BodyView& _body)
{
  m_bodies.push_back(_body);
  // A rate is what a caller has; an orientation is what this accumulates. Forcing identity here is
  // what makes that split true rather than a convention nobody checks.
  XMStoreFloat3x3(&m_bodies.back().tumble, XMMatrixIdentity());
  m_bodyWorlds.reserve(m_bodies.size());
}

void UniverseView::ClearBodies() noexcept
{
  m_bodies.clear();
  m_bodyWorlds.clear();
  m_bodyLod.clear();
  m_bodyTriangles = 0;
}

void UniverseView::RegisterHullMesh(Game::HullId _hull, MeshHandle _mesh)
{
  const std::size_t row = static_cast<std::size_t>(_hull);
  if (row < m_hullMeshes.size())
    m_hullMeshes[row] = _mesh;
}

std::span<const Game::ShipSnapshot> UniverseView::Ships() const noexcept
{
  return m_receiver.HasSnapshot() ? std::span<const Game::ShipSnapshot>(m_receiver.Latest().ships) : std::span<const Game::ShipSnapshot>();
}

void UniverseView::PumpNetwork()
{
  if (m_transport == nullptr)
    return;

  m_transport->Poll();

  // Drain everything waiting rather than one datagram per tick: a snapshot is several fragments and
  // stopping part-way would strand the rest until the next tick, which is how a pipeline that works
  // for three ships stops working for thirty.
  std::array<std::uint8_t, Neuron::MAX_DATAGRAM_BYTES> datagram{};
  bool arrived = false;
  for (;;)
  {
    const std::uint32_t size = m_transport->Receive(datagram.data(), static_cast<std::uint32_t>(datagram.size()));
    if (size == 0)
      break;
    arrived = m_receiver.Accept(std::span<const std::uint8_t>(datagram.data(), size)) || arrived;
  }

  // Then the reliable lane, which carries departures. After the datagrams rather than before, so a
  // departure and the upserts of the same update apply in the order the server wrote them; the
  // receiver is safe either way, and this is the order that needs no argument.
  m_reliableScratch.resize(Neuron::MAX_RELIABLE_BYTES);
  for (;;)
  {
    const std::uint32_t size = m_transport->ReceiveReliable(m_reliableScratch.data(), Neuron::MAX_RELIABLE_BYTES);
    if (size == 0)
      break;
    arrived = m_receiver.Accept(std::span<const std::uint8_t>(m_reliableScratch.data(), size)) || arrived;
  }

  if (arrived)
    ApplySnapshot();
}

// Presentation state follows the ship, not the array slot it happened to occupy last tick.
void UniverseView::ApplySnapshot()
{
  const std::vector<Game::ShipSnapshot>& ships = m_receiver.Latest().ships;
  const std::uint64_t tick = m_receiver.Latest().tick;

  m_carryScratch.clear();
  m_carryEntities.clear();
  m_carryScratch.swap(m_ships);
  m_carryEntities.swap(m_entities);

  m_ships.reserve(ships.size());
  m_entities.reserve(ships.size());
  for (const Game::ShipSnapshot& ship : ships)
  {
    // Linear: the carried set is the previous snapshot's ships, and at these counts a map would
    // cost more than it saved. It is also the one lookup in this class, so if it ever matters it
    // is one place to change.
    std::size_t found = m_carryEntities.size();
    for (std::size_t at = 0; at < m_carryEntities.size(); ++at)
    {
      if (m_carryEntities[at] == ship.entity)
      {
        found = at;
        break;
      }
    }

    if (found < m_carryScratch.size())
    {
      ShipView& view = m_carryScratch[found];

      // An update upserts, so a record the priority accumulator skipped is still in the snapshot
      // and reads exactly as it did last time. Only a record that actually changed is a new sample;
      // the tick it describes is the update's, since every record in one update was written on the
      // same tick. A ship that is genuinely still gets no new sample and holds, which is right.
      const bool changed = view.to.pos.sectorX != ship.posUniverse.sectorX || view.to.pos.sectorZ != ship.posUniverse.sectorZ ||
                           view.to.pos.localX != ship.posUniverse.localX || view.to.pos.localZ != ship.posUniverse.localZ ||
                           view.to.headingRad != ship.headingRad;
      if (changed)
      {
        view.from = view.to;
        view.to = SampleOf(ship, tick);
      }
      view.faction = ship.factionId;
      view.hullFraction = ship.hullFraction;
      m_ships.push_back(std::move(view));
      // Struck off so the leftovers can be walked below. No shard ever mints the null id, so it can
      // never match a live ship on a later pass either.
      m_carryEntities[found] = Game::INVALID_ENTITY_ID;
    }
    else
    {
      // A ship this half has not seen before. Its mesh comes from the hull table rather than from
      // whoever spawned it, because a snapshot carries a hullId and knows nothing about meshes.
      const std::size_t row = static_cast<std::size_t>(ship.hullId);
      const MeshHandle mesh = (row < m_hullMeshes.size()) ? m_hullMeshes[row] : INVALID_MESH;
      ShipView view;
      view.mesh = mesh;
      view.faction = ship.factionId;
      view.hullFraction = ship.hullFraction;
      view.to = SampleOf(ship, tick);
      view.from = view.to; // one sample: it is drawn there until the next one gives it somewhere to go
      if (mesh != INVALID_MESH)
      {
        const MeshData& data = m_meshes->Data(mesh);
        view.restY = data.RestY();
        view.pickCentre = data.BoundsCentre();
        view.halfExtents = data.HalfExtents();
        // One walk of the authored markers. Gun, Point and Unknown are carried by the file and
        // consumed by nobody here, exactly as Design/Archive/NmoFormat.md 9 says.
        for (const MeshMarker& marker : data.markers)
        {
          if (marker.kind == MarkerKind::Exhaust)
          {
            view.exhausts.push_back(ExhaustView{.local = marker.position,
                                                .colour = Rgba{marker.colour.x, marker.colour.y, marker.colour.z, marker.colour.w},
                                                .radiusMetres = marker.scale,
                                                .raceTinted = marker.raceTinted});
          }
          else if (marker.kind == MarkerKind::NavLight)
          {
            // A period past the clamp is a content mistake, not a bad file: the light still draws,
            // and the trace says which hull to look at.
            float periodSec = std::max(0.0f, marker.param0);
            if (periodSec > NAV_LIGHT_MAX_PERIOD_SEC)
            {
              DebugTrace(L"nav light blink period {} exceeds the clamp; using {}\n", periodSec, NAV_LIGHT_MAX_PERIOD_SEC);
              periodSec = NAV_LIGHT_MAX_PERIOD_SEC;
            }
            view.navLights.push_back(NavLightView{.local = marker.position,
                                                  .colour = Rgba{marker.colour.x, marker.colour.y, marker.colour.z, marker.colour.w},
                                                  .radiusMetres = marker.scale,
                                                  .periodSec = periodSec,
                                                  .phase = marker.param1});
          }
        }
        view.trail.assign(view.exhausts.size() * TRAIL_SAMPLES, XMFLOAT3(0.0f, 0.0f, 0.0f));
      }

      // The wink-in. A record this client has never held, whose identity it watched jump out, IS
      // the ship that left -- a jump preserves the entity (ADR 0056) -- so the far side of the
      // crossing flashes the way the near side did, keyed on nothing but identity.
      for (std::size_t watch = 0; watch < m_jumpWatches.size(); ++watch)
      {
        if (m_jumpWatches[watch].entity != ship.entity)
          continue;
        const float y = SHIP_HOVER_HEIGHT + (view.restY + view.pickCentre.y) * SHIP_SCALE;
        m_jumpFlashes.push_back(
          JumpFlash{.posWorld = XMFLOAT3(ViewX(ship.posUniverse), y, ViewZ(ship.posUniverse)), .headingRad = ship.headingRad});
        m_jumpWatches.erase(m_jumpWatches.begin() + static_cast<std::ptrdiff_t>(watch));
        break;
      }
      m_ships.push_back(std::move(view));
    }
    m_entities.push_back(ship.entity);
  }

  // Before ExplodeTheLost, and that ordering is the whole of why this is its own pass: a shot's two
  // ends are resolved against the records this client holds, and ExplodeTheLost is what discards the
  // record of anything that just died. Resolved after it, every killing shot would draw from nowhere.
  TakeTheGunfire();

  ExplodeTheLost(tick);

  // Last, and after the carry rather than inside it: what is selected is a set of slots, and which
  // records that lights up is a function of the rosters this update may just have changed. A hull
  // launched into a selected fleet is ringed the moment its roster arrives, which is what deriving
  // the flag buys over carrying it.
  RefreshSelection();
  RefreshKnownHulls();

  // After ExplodeTheLost, because it is what set the docked-out flags this reads, and last because
  // it is the pass that takes the copy of the rosters the next update will attribute against.
  ReportFleetEvents();
}

std::size_t UniverseView::IndexOfEntity(Game::EntityId _entity) const noexcept
{
  // m_entities is parallel to m_ships and rebuilt with it on every update, so this is the one place
  // that has to know that. Linear because an interest set is tens of records and a shot resolves two
  // of them per update; a sorted index would be a second thing to keep in step for no measured gain.
  for (std::size_t at = 0; at < m_entities.size() && at < m_ships.size(); ++at)
  {
    if (m_entities[at] == _entity)
      return at;
  }
  return m_ships.size(); // the one-past-the-end this file already uses to mean "no such ship"
}

void UniverseView::TakeTheGunfire()
{
  const std::span<const Game::FireEvent> fired = m_receiver.Fire();
  for (const Game::FireEvent& event : fired)
  {
    // Either end is enough, which is what slice 2's filter spent itself on: being shot at from
    // outside your own view is exactly the event a player must not be denied. What is missing is
    // drawn from the end that is known.
    const std::size_t shooter = IndexOfEntity(event.shooter);
    const std::size_t target = IndexOfEntity(event.target);
    if (shooter == m_ships.size() && target == m_ships.size())
      continue; // fire between two ships neither of which is on screen

    GunShot shot;
    if (shooter != m_ships.size())
    {
      shot.fromWorld = HullPointToWorld(m_ships[shooter], DisplayedPose(shooter), XMFLOAT3(0.0f, 0.0f, 0.0f));
      shot.colour = LiveryOf(m_ships[shooter].faction, IsOwn(shooter), m_receiver.IsHostileToMe(m_ships[shooter].faction));
    }
    if (target != m_ships.size())
      shot.toWorld = HullPointToWorld(m_ships[target], DisplayedPose(target), XMFLOAT3(0.0f, 0.0f, 0.0f));
    // A shot from off screen: the impact is all there is to draw, and it keeps GunShot's default
    // white. Not a guess at red -- this half genuinely does not know whose fire that was, and a
    // colour is a claim about it (Design/Combat-slice-4.md 2.2).
    if (shooter == m_ships.size())
      shot.fromWorld = shot.toWorld;
    if (target == m_ships.size())
      shot.toWorld = shot.fromWorld;

    // Oldest out, so a pathological update cannot turn into a frame spike. The newest gunfire is
    // the gunfire a player is looking at, which is the cap the writer applies at the other end.
    if (m_shots.size() >= MAX_DRAWN_SHOTS)
      m_shots.erase(m_shots.begin());
    m_shots.push_back(shot);

    // Remembered only when it was this client's own ship that fired, because the only thing this
    // list is for is crediting a kill to the player.
    if (shooter != m_ships.size() && IsOwn(shooter) && event.target != Game::INVALID_ENTITY_ID)
      m_shotAt.push_back(ShotAt{event.target, 0.0f});
  }

  // Drawn, so the receiver may forget them. Fire accumulates across every message in a drain for
  // the reason deaths do, and nothing else clears it.
  m_receiver.ClearFire();
}

// A ShipView that was not carried is a ship that has left the snapshot, and this is the only place
// that knows it: the new snapshot has no record for it and the view is about to be discarded.
//
// It consumes what the server stated, not what the client inferred. A ship leaves the snapshot both
// when it dies and when it simply falls out of the interest radius, and until hostiles existed those
// were the same event -- every ship was the player's, so the only leave was F4's despawn. A patrol
// living 1.2 km out makes the inference wrong in the most visible way there is: phantom explosions,
// a camera shake and a SHIP LOST alert for enemies that are alive and patrolling. So a departure
// removes the view silently and only a death detonates (Design/Archive/Hostiles.md 4.4).
//
// Destroyed() describes the last update the receiver applied, and PumpNetwork applies at most one
// per tick because the composition root pumps on every tick -- which is what makes reading it here,
// once per ApplySnapshot, the whole of what the server said since the last one.
void UniverseView::ExplodeTheLost(std::uint64_t _tick)
{
  // Docked first, and through a loop of its own rather than a branch in the one below: a docked
  // hull is removed with no ceremony -- no explosion, no shake, no SHIP LOST -- and the two lists
  // arrive in the same message, so a consumer that treated the spans alike would look like a bug in
  // the explosion rather than in the drain (Design/Archive/Stations.md 7.4, Stations-slice-plan.md 9). Only
  // the player's own are counted for the line: a protector coming home is the station's business.
  const std::span<const Game::EntityId> docked = m_receiver.Docked();
  int ownDocked = 0;
  for (std::size_t at = 0; at < m_carryEntities.size() && at < m_carryScratch.size(); ++at)
  {
    const Game::EntityId entity = m_carryEntities[at];
    if (entity == Game::INVALID_ENTITY_ID)
      continue;
    bool inside = false;
    for (const Game::EntityId gone : docked)
      inside = inside || gone == entity;
    if (!inside)
      continue;
    if (m_carryScratch[at].faction == m_ownFaction)
      ++ownDocked;

    NoteFleetDeparture(entity, true);
  }
  if (ownDocked > 0 && m_log != nullptr)
    m_log->PushFormat(EventLog::Severity::Friendly, SimTimeSec(), "DOCKED | %d SHIPS", ownDocked);
  m_receiver.ClearDocked();

  // Jumped, on the docked run's terms and for its reason: the hull leaves with no explosion, no
  // shake and no SHIP LOST, because the ship is not dead -- it is in another system under the same
  // identity (ADR 0056). Without its own run this would arrive as a DESTROY and a fleet crossing a
  // gate would detonate on screen, which is the whole reason the wire states a cause at all.
  //
  // The wink Design/Archive/Universe.md 9 left as a placeholder: the hull goes out in a flash where
  // it stood, and its identity is watched for, so the far side can blink the same way the moment
  // the same entity re-enters this client's view (ADR 0056).
  const std::span<const Game::EntityId> jumped = m_receiver.Jumped();
  int ownJumped = 0;
  int crossedSlot = -1;
  for (std::size_t at = 0; at < m_carryEntities.size() && at < m_carryScratch.size(); ++at)
  {
    const Game::EntityId entity = m_carryEntities[at];
    if (entity == Game::INVALID_ENTITY_ID)
      continue;
    bool inside = false;
    for (const Game::EntityId gone : jumped)
      inside = inside || gone == entity;
    if (!inside)
      continue;
    if (m_carryScratch[at].faction == m_ownFaction)
      ++ownJumped;

    // The wink-out, at the spot the hull was last drawn. A jumper that was never drawn -- off
    // screen, or its mesh missing -- leaves without one, exactly as it would leave without an
    // explosion; the watch is set either way, so its arrival still blinks if the camera crosses.
    const ShipView& gone = m_carryScratch[at];
    if (gone.drawn)
      m_jumpFlashes.push_back(
        JumpFlash{.posWorld = XMFLOAT3(gone.lastWorld._41, gone.lastWorld._42, gone.lastWorld._43), .headingRad = gone.to.headingRad});
    m_jumpWatches.push_back(JumpWatch{.entity = entity});

    // Which of this client's own fleets went through, so the camera can go with it. Noted before
    // NoteFleetDeparture, which reads the same rosters -- the roster that still names this entity is
    // the fleet it left in.
    for (int slot = 0; slot < FLEET_SLOTS; ++slot)
    {
      for (const Game::EntityId member : m_lastRoster[slot])
      {
        if (member == entity && m_carryScratch[at].faction == m_ownFaction)
          crossedSlot = slot;
      }
    }

    NoteFleetDeparture(entity, true);
  }
  if (ownJumped > 0 && m_log != nullptr)
    m_log->PushFormat(EventLog::Severity::Friendly, SimTimeSec(), "JUMPED | %d SHIPS", ownJumped);

  // The camera crosses with the fleet, through the machinery the fleet button already uses: the
  // focus re-reads the fleet's position every frame, so it lands wherever the far side turned out to
  // be rather than where this client guessed. It snaps rather than flies, because the gap is a
  // galaxy's (UpdateFocus, CAMERA_SNAP_METRES).
  //
  // Only a fleet this client had SELECTED. A fleet crossing somewhere else is not a reason to take
  // the camera off what the player is looking at.
  if (crossedSlot >= 0 && IsFleetSelected(crossedSlot))
    FocusFleet(crossedSlot);
  m_receiver.ClearJumped();

  const std::span<const Game::EntityId> destroyed = m_receiver.Destroyed();
  for (std::size_t at = 0; at < m_carryEntities.size() && at < m_carryScratch.size(); ++at)
  {
    const Game::EntityId entity = m_carryEntities[at];
    if (entity == Game::INVALID_ENTITY_ID)
      continue; // carried onto the new snapshot

    bool died = false;
    for (const Game::EntityId dead : destroyed)
      died = died || dead == entity;
    if (!died)
      continue; // it left this client's view, which is not a death and never was

    NoteFleetDeparture(entity, false);

    const ShipView& lost = m_carryScratch[at];
    // A ship that despawned before it was ever drawn, or whose mesh never loaded, has nowhere to
    // explode and no triangles to do it with.
    if (!lost.drawn || lost.mesh == INVALID_MESH || m_meshes == nullptr)
      continue;

    ShipExplosion::Spawn spawn;
    spawn.mesh = &m_meshes->Data(lost.mesh);
    spawn.world = lost.lastWorld;
    spawn.velMetresPerSec = lost.lastVelMetresPerSec;
    spawn.halfExtents = lost.halfExtents;
    spawn.livery = lost.lastLivery;
    // The same ship dying on the same tick shatters the same way, which is what a replay of a
    // recorded match will want and costs nothing to give it now. The odd constant is the golden
    // ratio in 64 bits, which is what stops two nearby ticks producing two nearby streams.
    //
    // The id is the seed directly now that identity is 64 bits of its own. It used to be a slot
    // shifted over a generation, which was the same 64 bits assembled by hand -- and which would
    // have made the same ship shatter differently after a shard handed it on, because its slot
    // would have changed (ADR 0047).
    spawn.seed = entity ^ (_tick * 0x9E3779B97F4A7C15ull);
    // Whatever gives a station a lifecycle sets this from the station itself. Until something can
    // kill one, every death carries a ring, because otherwise nothing would ever draw one
    // (ViewTuning.h).
    spawn.shockRing = SHOCK_RING_ON_EVERY_DEATH;

    m_explosions.emplace_back().Start(spawn, m_particles);
    TriggerCameraShake();
    // Only the player's own dead earn the alert. An enemy going down is not a loss -- its line is
    // the kill credit below -- and a push gated on nothing said SHIP LOST for every faction, so
    // shooting down a Vandal read as losing a ship. The explosion and the shake stay for every
    // death: they are about what happened on screen, not whose ship it was.
    if (m_log != nullptr && lost.faction == m_ownFaction)
      m_log->Push(EventLog::Severity::Alert, SimTimeSec(), "SHIP LOST");

    // Attributed from two facts this client already holds and never from the wire: the fire block
    // said who shot at whom, the departure run says who died, and the join of the two is a kill the
    // player made. The wire deliberately states no killer (ADR 0053), and this is what that costs --
    // a line that is right when the shot arrived and silent when it did not, which is a flourish on
    // the log rather than a fact the player is owed (Design/Combat-slice-4.md 2.5).
    for (const ShotAt& aimed : m_shotAt)
    {
      if (aimed.entity != entity || m_log == nullptr)
        continue;
      m_log->Push(EventLog::Severity::Friendly, SimTimeSec(), "TARGET DESTROYED");
      break;
    }
  }

  // Drawn, so the receiver may forget them. Deaths accumulate across every message in a drain now
  // that departures have their own lane, and nothing else clears them (ADR 0029).
  m_receiver.ClearDestroyed();
}

UniverseView::MotionSample UniverseView::SampleOf(const Game::ShipSnapshot& _ship, std::uint64_t _tick) noexcept
{
  MotionSample sample;
  sample.tick = static_cast<float>(_tick);
  sample.pos = _ship.posUniverse;
  sample.headingRad = _ship.headingRad;
  // prevPos is the tick before, so the offset between the two is exactly one tick of travel.
  sample.velX = Game::OffsetX(_ship.prevPos, _ship.posUniverse);
  sample.velZ = Game::OffsetZ(_ship.prevPos, _ship.posUniverse);
  sample.turnRadPerTick = XMScalarModAngle(_ship.headingRad - _ship.prevHeading);
  return sample;
}

void UniverseView::SetDisplayTime(float _tickTime) noexcept
{
  m_displayTick = _tickTime - INTERP_DELAY_TICKS;
}

UniverseView::DisplayPose UniverseView::DisplayedPose(std::size_t _index) const noexcept
{
  DisplayPose pose;
  if (_index >= m_ships.size())
    return pose;

  const MotionSample& from = m_ships[_index].from;
  const MotionSample& to = m_ships[_index].to;

  if (m_displayTick <= from.tick || to.tick <= from.tick)
  {
    // Before the older sample -- the first frames after a ship appears -- or with only one sample
    // to go on: hold at the older one rather than invent motion that was never reported.
    pose.pos = from.pos;
    pose.headingRad = from.headingRad;
    return pose;
  }

  if (m_displayTick <= to.tick)
  {
    const float t = (m_displayTick - from.tick) / (to.tick - from.tick);
    pose.pos = Game::Lerp(from.pos, to.pos, t);
    pose.headingRad = from.headingRad + XMScalarModAngle(to.headingRad - from.headingRad) * t;
    return pose;
  }

  // Past the newest sample: the wire is behind, or this ship refreshes at a lower priority. Carry
  // it on at its last velocity for a bounded while, then hold.
  const float ahead = std::min(m_displayTick - to.tick, INTERP_MAX_EXTRAPOLATE_TICKS);
  pose.pos = to.pos;
  Game::Translate(pose.pos, to.velX * ahead, to.velZ * ahead);
  pose.headingRad = to.headingRad + to.turnRadPerTick * ahead;
  return pose;
}

void UniverseView::ClearSelection() noexcept
{
  for (int slot = 0; slot < FLEET_SLOTS; ++slot)
    m_fleetSelected[slot] = false;
  RefreshSelection();
}

float UniverseView::SimTimeSec() const noexcept
{
  return static_cast<float>(m_receiver.Latest().tick) / Game::TICK_HZ;
}

void UniverseView::SelectFleet(int _slot, bool _additive)
{
  if (_slot < 0 || _slot >= FLEET_SLOTS || !IsFleetHeld(_slot))
    return; // an empty slot selects nothing; the button says so instead (Design/Archive/Fleets.md 9.1)

  if (_additive)
  {
    m_fleetSelected[_slot] = !m_fleetSelected[_slot];
  }
  else
  {
    for (int at = 0; at < FLEET_SLOTS; ++at)
      m_fleetSelected[at] = (at == _slot);
  }
  RefreshSelection();
}

int UniverseView::SelectedFleetCount() const noexcept
{
  int count = 0;
  for (int slot = 0; slot < FLEET_SLOTS; ++slot)
    count += m_fleetSelected[slot] ? 1 : 0;
  return count;
}

int UniverseView::FirstSelectedFleet() const noexcept
{
  for (int slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    if (m_fleetSelected[slot])
      return slot;
  }
  return -1;
}

int UniverseView::FleetSlotOf(Game::EntityId _entity) const noexcept
{
  if (_entity == Game::INVALID_ENTITY_ID)
    return -1;
  for (int slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    for (const Game::EntityId member : m_receiver.RosterOf(static_cast<std::uint8_t>(slot)))
    {
      if (member == _entity)
        return slot;
    }
  }
  return -1;
}

void UniverseView::RefreshSelection() noexcept
{
  for (std::size_t at = 0; at < m_ships.size(); ++at)
  {
    const Game::EntityId entity = (at < m_entities.size()) ? m_entities[at] : Game::INVALID_ENTITY_ID;
    const int slot = FleetSlotOf(entity);
    m_ships[at].selected = (slot >= 0) && m_fleetSelected[slot];
  }
}

const char* UniverseView::FleetActivity(int _slot) const noexcept
{
  if (!IsFleetHeld(_slot))
    return "EMPTY";

  // Engaged outranks the standing order in what is SAID, because it is the thing that changes what
  // the fleet is doing right now; the order is still there underneath and resumes (Design/Archive/Fleets.md 7.4).
  if ((FleetStatusFlags(_slot) & Game::FLEET_FLAG_ENGAGED) != 0)
    return "DEFENDING";

  // Launching outranks it in turn, for the reason the block states both: a fleet still pouring out
  // of a dock is doing that and its standing order, and the launch is the one a player has no other
  // way to see.
  if ((FleetStatusFlags(_slot) & Game::FLEET_FLAG_LAUNCHING) != 0)
    return "LAUNCHING";

  switch (FleetStatusKind(_slot))
  {
  case static_cast<std::uint8_t>(Game::FleetOrderKind::Move):
    return "MOVING";
  case static_cast<std::uint8_t>(Game::FleetOrderKind::Dock):
    return "DOCKING";
  case static_cast<std::uint8_t>(Game::FleetOrderKind::Attack):
    return "ATTACKING";
  case static_cast<std::uint8_t>(Game::FleetOrderKind::Mine):
    return "MINING";
  // Never drawn until this slice: Jump shared its value with LAUNCHING, so a jumping fleet said it
  // was launching (Design/Archive/FleetStatus-work-order.md).
  case static_cast<std::uint8_t>(Game::FleetOrderKind::Jump):
    return "JUMPING";
  default:
    return "IDLE";
  }
}

UniverseView::ButtonPress UniverseView::PressFleetButton(int _slot, bool _longPress)
{
  if (_slot < 0 || _slot >= FLEET_SLOTS)
    return ButtonPress::Nothing;

  if (!IsFleetHeld(_slot))
  {
    // Inert to a tap, and a hold says the one thing there is to say about an empty slot. Saying it
    // on the hold rather than on the tap is deliberate: a mis-tap on a dead button should be
    // silent, and a deliberate hold is a question.
    if (_longPress && m_log)
      m_log->PushFormat(EventLog::Severity::Info, SimTimeSec(), "FLEET %d | COMPOSE AT A STATION", _slot + 1);
    return ButtonPress::Nothing;
  }

  // A hold opens the sheet, and selects the fleet on the way: reading a fleet and holding it are
  // the same act under decision 1, and a sheet whose commands went to some other fleet would be
  // the one way this panel could lie.
  if (_longPress)
  {
    SelectFleet(_slot, false);
    return ButtonPress::OpenSheet;
  }

  // Selecting and attending are one gesture. A shift-held tap on a button is not a thing the bar
  // offers -- the modifier belongs to the universe, where a hull is tapped -- so this selects whole.
  SelectFleet(_slot, false);
  FocusFleet(_slot);
  return ButtonPress::Selected;
}

void UniverseView::ArmFleetOrder(ArmedOrder _kind)
{
  m_armed = _kind;
  if (m_armed == ArmedOrder::None || m_log == nullptr)
    return;

  const char* verb = "DOCK";
  if (m_armed == ArmedOrder::Move)
    verb = "MOVE";
  else if (m_armed == ArmedOrder::Attack)
    verb = "ATTACK";
  else if (m_armed == ArmedOrder::Jump)
    verb = "JUMP";
  m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "%s | TAP A TARGET", verb);
}

void UniverseView::IssueStopOrder()
{
  Game::FleetOrder order;
  order.kind = Game::FleetOrderKind::Stop;
  const std::uint32_t sent = SendToSelectedFleets(order);
  if (sent > 0 && m_log)
    m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "STOP | %d %s", static_cast<int>(sent), (sent == 1) ? "FLEET" : "FLEETS");
}

float UniverseView::ConditionOfMember(Game::EntityId _entity) const noexcept
{
  const std::size_t at = IndexOfEntity(_entity);
  if (at == m_ships.size())
    return -1.0f; // no record: a fleet somewhere this camera has never been
  return static_cast<float>(m_ships[at].hullFraction) / 255.0f;
}

std::uint32_t UniverseView::HullOfMember(Game::EntityId _entity) const noexcept
{
  for (const KnownHull& known : m_knownHulls)
  {
    if (known.entity == _entity)
      return known.hullId;
  }
  return Game::HULL_COUNT;
}

void UniverseView::RefreshKnownHulls()
{
  // Anything in a roster this update, remembered from the record if one is in view and carried over
  // from what was already known if not. Rebuilt rather than patched, so the list is exactly the
  // current rosters and cannot outlive them.
  std::vector<KnownHull> rebuilt;
  rebuilt.reserve(static_cast<std::size_t>(FLEET_SLOTS) * Game::MAX_FLEET_SHIPS);

  const std::span<const Game::ShipSnapshot> state = Ships();
  for (int slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    for (const Game::EntityId member : m_receiver.RosterOf(static_cast<std::uint8_t>(slot)))
    {
      std::uint32_t hullId = Game::HULL_COUNT;
      for (const Game::ShipSnapshot& ship : state)
      {
        if (ship.entity == member)
        {
          hullId = ship.hullId;
          break;
        }
      }
      if (hullId == Game::HULL_COUNT)
        hullId = HullOfMember(member); // out of view now; what it was when it last was not
      rebuilt.push_back(KnownHull{member, hullId});
    }
  }
  m_knownHulls.swap(rebuilt);
}

// Which slot a departing member left, and how, recorded against the moment its slot empties.
//
// Against the roster of the update BEFORE this one: rosters ride the reliable lane and are applied
// ahead of the records, so by the time a departure is read the entity is in none of them.
//
// The LAST departure wins rather than any docking ever seen, which is what Design/Archive/Fleets.md 9.6's
// "on the last capture" means: a fleet that lands one hull and then loses the rest was lost, and a
// sticky flag would have called it docked (ADR 0040).
void UniverseView::NoteFleetDeparture(Game::EntityId _entity, bool _docked) noexcept
{
  for (int slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    for (const Game::EntityId member : m_lastRoster[slot])
    {
      if (member == _entity)
        m_slotDockedOut[slot] = _docked;
    }
  }
}

// The three lines only this half can draw. The alert's edge could have stayed in the HUD, where it
// began; the other two could not, because the departure lists say whether a slot emptied by docking
// or by dying and ExplodeTheLost clears them -- so all of Design/Archive/Fleets.md 9.6's fleet lines are
// here rather than split across two files by which one happened to need what.
void UniverseView::ReportFleetEvents()
{
  const std::uint8_t mask = m_receiver.FleetMask();
  for (int slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    const bool held = (mask & (1u << slot)) != 0;
    const bool wasHeld = (m_lastFleetMask & (1u << slot)) != 0;

    if (held)
    {
      const std::uint8_t bits = FleetStatusFlags(slot);
      const bool alert = (bits & Game::FLEET_FLAG_UNDER_ATTACK) != 0;
      // The rising edge only. The bit holds for ten seconds and a fight keeps refilling it, so a
      // line per update would bury everything else the log has to say (Design/Archive/Fleets.md 7.3).
      if (alert && !m_wasUnderAttack[slot] && m_log)
        m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "FLEET %d UNDER ATTACK", slot + 1);
      m_wasUnderAttack[slot] = alert;

      const bool launching = (bits & Game::FLEET_FLAG_LAUNCHING) != 0;
      // The falling edge of launching is the manifest emptying, which is the moment the fleet is
      // whole -- the one thing about a launch that is worth a line rather than a number ticking up.
      if (!launching && m_wasLaunching[slot] && m_log)
        m_log->PushFormat(EventLog::Severity::Friendly, SimTimeSec(), "FLEET %d | %d SHIPS OUT", slot + 1, FleetCount(slot));
      m_wasLaunching[slot] = launching;
    }
    else if (wasHeld && m_log)
    {
      // The slot cleared. Which line it earns is the departure's stated cause, which is exactly
      // what ADR 0040 put on the wire and what nothing had read at fleet grain until now.
      if (m_slotDockedOut[slot])
        m_log->PushFormat(EventLog::Severity::Friendly, SimTimeSec(), "FLEET %d | DOCKED", slot + 1);
      else
        m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "FLEET %d LOST", slot + 1);
    }

    if (!held)
    {
      m_wasUnderAttack[slot] = false;
      m_wasLaunching[slot] = false;
      m_slotDockedOut[slot] = false;
    }
    m_lastRoster[slot].assign(m_receiver.RosterOf(static_cast<std::uint8_t>(slot)).begin(),
                              m_receiver.RosterOf(static_cast<std::uint8_t>(slot)).end());
  }
  m_lastFleetMask = mask;
}

void UniverseView::FocusFleet(int _slot)
{
  // Held only. Focusing an empty slot would fly the camera to wherever a stale block last put it,
  // which is a worse answer than doing nothing.
  m_focusSlot = IsFleetHeld(_slot) ? _slot : -1;
}

void UniverseView::UpdateFocus(float _dtSec)
{
  if (m_focusSlot < 0 || m_camera == nullptr)
    return;

  // A fleet that stopped being held mid-flight -- docked, or lost its last ship -- releases the
  // camera where it stands rather than carrying it to a position nothing states any more.
  if (!IsFleetHeld(m_focusSlot))
  {
    m_focusSlot = -1;
    return;
  }

  // Re-read every frame: the fleet is flying, and a goal captured at the tap lands where it used to
  // be. The status block is stamped on every update, so this costs nothing but a read.
  const Game::UniversePos target = FleetPosition(m_focusSlot);
  const float x = ViewX(target);
  const float z = ViewZ(target);

  // A crossing rather than a flight: past CAMERA_SNAP_METRES the camera arrives instead of easing,
  // because the ease would spend seconds in interstellar space showing nothing and the half-life
  // never lands. Measured from where the camera IS, so a fleet that jumps while the camera is
  // already following it snaps once and then eases normally on the far side.
  const float gapX = x - m_camera->Target().x;
  const float gapZ = z - m_camera->Target().z;
  if (gapX * gapX + gapZ * gapZ > CAMERA_SNAP_METRES * CAMERA_SNAP_METRES)
    m_camera->SnapGoal(x, z);
  else
    m_camera->SetGoal(x, z);
  m_camera->Follow(0.0f, 0.0f, _dtSec);

  // Whoever moves the camera recomputes it, which is the convention PointerTracker's own gestures
  // keep. Without this the matrices Render is about to read would be a frame behind the target, and
  // a flight would judder against everything else on screen.
  m_camera->Update();

  const float dx = m_camera->Target().x - x;
  const float dz = m_camera->Target().z - z;
  if (dx * dx + dz * dz <= FLEET_FOCUS_ARRIVE_METRES * FLEET_FOCUS_ARRIVE_METRES)
    m_focusSlot = -1; // arrived; the camera is the player's again
}

void UniverseView::TriggerCameraShake() noexcept
{
  m_camera->Shake();
}

int UniverseView::SelectedCount() const noexcept
{
  int count = 0;
  for (const ShipView& ship : m_ships)
    count += ship.selected ? 1 : 0;
  return count;
}

XMMATRIX UniverseView::HullMatrix(const ShipView& _view, const DisplayPose& _pose) const noexcept
{
  // Roll about the hull's own mid-height axis, not its base, or a banked ship pivots on one
  // wingtip. SHIP_HOVER_HEIGHT is what keeps the low wing out of the ground while it does.
  const float rollAxisY = _view.pickCentre.y;
  return XMMatrixTranslation(0.0f, -rollAxisY, 0.0f) * XMMatrixRotationZ(_view.bankRad) *
         XMMatrixTranslation(0.0f, rollAxisY + _view.restY, 0.0f) * XMMatrixScaling(SHIP_SCALE, SHIP_SCALE, SHIP_SCALE) *
         XMMatrixRotationY(_pose.headingRad) * XMMatrixTranslation(ViewX(_pose.pos), SHIP_HOVER_HEIGHT, ViewZ(_pose.pos));
}

XMFLOAT3 UniverseView::HullPointToWorld(const ShipView& _view, const DisplayPose& _pose, const XMFLOAT3& _local) const noexcept
{
  XMFLOAT3 world;
  XMStoreFloat3(&world, XMVector3Transform(XMLoadFloat3(&_local), HullMatrix(_view, _pose)));
  return world;
}

// One sample per nozzle per tick, so trail length means the same thing whatever the frame rate.
void UniverseView::SampleTrails()
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  const size_t count = std::min(m_ships.size(), state.size());

  for (size_t i = 0; i < count; ++i)
  {
    ShipView& view = m_ships[i];
    if (view.exhausts.empty())
      continue;
    // Sampled where the ship is drawn, not where the latest record puts it, or the trail would step
    // once an update while the hull glides.
    const DisplayPose pose = DisplayedPose(i);

    view.trailHead = (view.trailHead + 1) % TRAIL_SAMPLES;
    for (size_t nozzle = 0; nozzle < view.exhausts.size(); ++nozzle)
    {
      view.trail[nozzle * TRAIL_SAMPLES + static_cast<size_t>(view.trailHead)] = HullPointToWorld(view, pose, view.exhausts[nozzle].local);
    }
    view.trailCount = std::min(view.trailCount + 1, TRAIL_SAMPLES);
  }
}

void UniverseView::UpdateFeedback(float _dtSec)
{
  const float dt = std::clamp(_dtSec, 0.0f, 0.1f);

  // The sky's clock, wrapped rather than left to run: the twinkle is a sine of time times a rate, and
  // a float second counter left running for a few hours loses enough precision that consecutive
  // frames land on the same argument and every star freezes.
  //
  // The wrap is seamless, and it is the vertex packing that makes it so. A star's rate is a fraction
  // of the frame's maximum quantized to eight bits (SkyVertex.h), so every rate in the sky is
  // n/255 * max for a whole n -- and after 255 * 2pi / max seconds *every* star has completed exactly
  // n whole cycles, whatever its n. Nothing jumps, and there is nothing to tune.
  m_skyTimeSec += dt;
  const float skyWrapSec = 255.0f * XM_2PI / std::max(m_skyTuning.twinkleMaxRateRadPerSec, 1e-3f);
  if (m_skyTimeSec > skyWrapSec)
    m_skyTimeSec -= skyWrapSec;

  // The nav lights' clock, wrapped for the reason above: precision. The sky's wrap is seamless
  // because every rate up there divides it; this one is not, because a marker may carry any period
  // at all -- a light whose period does not divide the wrap loses or gains at most one beat every
  // NAV_LIGHT_MAX_PERIOD_SEC. That is invisible on a free-running beacon and is the whole price of a
  // clock that never goes imprecise, which a frozen blink after two hours is not.
  //
  // Real time, so a light drifts against the simulation and against a recording. That is what a
  // running light does.
  m_navTimeSec += dt;

  // Gunfire ages on the same real-time clock, and for the same reason: a tracer is a drawing of
  // something that already happened, so it is not the simulation's to time (ADR 0053).
  for (GunShot& shot : m_shots)
    shot.ageSec += dt;
  std::erase_if(m_shots, [](const GunShot& _shot) { return _shot.ageSec >= GUN_TRACER_SEC; });

  for (ShotAt& aimed : m_shotAt)
    aimed.ageSec += dt;
  std::erase_if(m_shotAt, [](const ShotAt& _aimed) { return _aimed.ageSec >= GUN_KILL_CREDIT_SEC; });

  // The jump winks age on the tracers' clock, and a watched identity that never re-entered -- a
  // spectator's jumper, a crossing the camera did not follow -- is an expectation let go rather
  // than kept for ever.
  for (JumpFlash& flash : m_jumpFlashes)
    flash.ageSec += dt;
  std::erase_if(m_jumpFlashes, [](const JumpFlash& _flash) { return _flash.ageSec >= JUMP_FLASH_SEC; });

  for (JumpWatch& watch : m_jumpWatches)
    watch.ageSec += dt;
  std::erase_if(m_jumpWatches, [](const JumpWatch& _watch) { return _watch.ageSec >= JUMP_ARRIVAL_WATCH_SEC; });
  if (m_navTimeSec > NAV_LIGHT_MAX_PERIOD_SEC)
    m_navTimeSec -= NAV_LIGHT_MAX_PERIOD_SEC;

  // Bodies turn on real time, like every other feedback here, so the debug keys that slow the
  // simulation do not slow a planet: what 1/2/3 change is how fast the universe is simulated, and a
  // planet is not in the universe.
  for (BodyView& body : m_bodies)
  {
    body.spinRad += body.spinRadPerSec * dt;
    if (body.spinRad > XM_2PI)
      body.spinRad -= XM_2PI;

    // Composed step by step, the explosion's Tumbler rule: rot = rot * delta, so the increment is
    // about the body's own axes and not the world's. Skipped when the rates are zero rather than
    // multiplied by an identity every frame, which would let rounding drift a planet off its axis.
    const XMFLOAT3& rate = body.tumbleRadPerSec;
    if (rate.x != 0.0f || rate.y != 0.0f || rate.z != 0.0f)
    {
      const XMMATRIX delta = XMMatrixRotationRollPitchYaw(rate.x * dt, rate.y * dt, rate.z * dt);
      XMStoreFloat3x3(&body.tumble, XMMatrixMultiply(XMLoadFloat3x3(&body.tumble), delta));
    }
  }

  const float maxBank = XMConvertToRadians(BANK_MAX_ANGLE_DEG);
  const std::span<const Game::ShipSnapshot> state = Ships();

  for (int i = 0; i < static_cast<int>(m_ships.size()) && i < static_cast<int>(state.size()); ++i)
  {
    ShipView& view = m_ships[static_cast<size_t>(i)];
    const Game::ShipSnapshot& ship = state[static_cast<size_t>(i)];
    // Bank and thruster flare are both fractions of what this hull can do, so both come from its
    // own row. Against one global value a Carrier would barely bank and would never light up.
    const Game::HullSpec& hull = Game::HullSpecOf(ship.hullId);

    // Selection ring: a straight ramp on the tuned duration for the fade, and a spring for the
    // scale so it sails past and settles back.
    const float rampSec = std::max(0.001f, (view.selected ? SEL_RING_FADE_IN_MS : SEL_RING_FADE_OUT_MS) * 0.001f);
    view.ringFade = MoveTowards(view.ringFade, view.selected ? 1.0f : 0.0f, dt / rampSec);
    // The spring chases the selected state directly rather than the fade ramp, so the peak really
    // is the tuned overshoot and the two knobs stay independent: one shapes alpha, one shapes size.
    SpringTowards(view.ringScale, view.ringScaleVel, view.selected ? 1.0f : 0.0f, SEL_RING_SCALE_OVERSHOOT, SEL_OVERSHOOT_SETTLE_HALF_LIFE,
                  dt);
    view.ringScale = std::max(0.0f, view.ringScale);

    // Hover.
    view.hoverAmount += ((i == m_hoverShip ? 1.0f : 0.0f) - view.hoverAmount) * HalfLifeBlend(dt, SEL_HOVER_RESPONSE_HALF_LIFE);

    // Bank into the turn, proportional to angular velocity. Rolling starboard down in a starboard
    // turn means a negative roll, since a positive rotation about +Z lifts the starboard side.
    const float bankTarget = -std::clamp(ship.turnRateRadPerSec / hull.maxTurnRateRadPerSec, -1.0f, 1.0f) * maxBank;
    const bool goingIn = std::fabs(bankTarget) > std::fabs(view.bankRad);
    const float bankHalfLife = goingIn ? BANK_RESPONSE_HALF_LIFE : BANK_RETURN_HALF_LIFE;
    view.bankRad += (bankTarget - view.bankRad) * HalfLifeBlend(dt, bankHalfLife);

    // Thrusters follow acceleration, not speed: they flare on the way up to cruise and go quiet
    // once the ship is coasting.
    const float drive = std::clamp(ship.accelSample / hull.accelerationMetresPerSec2, 0.0f, 1.0f);
    const float thrusterTarget = THRUSTER_IDLE_INTENSITY + (THRUSTER_MAX_INTENSITY - THRUSTER_IDLE_INTENSITY) * drive;
    view.thrusterIntensity += (thrusterTarget - view.thrusterIntensity) * HalfLifeBlend(dt, THRUSTER_RESPONSE_HALF_LIFE);
  }

  // The explosion ages on real time, like every other feedback here: it is presentation, none of it
  // feeds back into a tick, and keys 1/2/3 scale the simulation rather than the frame.
  m_particles.Advance(dt, m_fxRng);
  for (ShipExplosion& explosion : m_explosions)
    explosion.Advance(dt);
  // Dropped once the shatters have faded out. The particles the same death emitted outlive it, in
  // the shared pool, and are aged above.
  std::erase_if(m_explosions, [](const ShipExplosion& _explosion) { return _explosion.Finished(); });

  // Order markers age out on their own.
  const float markerLifeSec = std::max(0.05f, MARKER_LIFETIME_MS * 0.001f);
  for (OrderMarker& marker : m_markers)
    marker.ageSec += dt;
  std::erase_if(m_markers, [markerLifeSec](const OrderMarker& _marker) { return _marker.ageSec >= markerLifeSec; });

  // Camera. While a selection is under way the goal rides with it; otherwise it stays where panning
  // left it. The lead pushes ahead of where the group is going, and the target eases in behind,
  // which is the lag.
  float leadX = 0.0f;
  float leadZ = 0.0f;
  int movingCount = 0;
  float centreX = 0.0f;
  float centreZ = 0.0f;
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    if (!m_ships[i].selected || state[i].order != Game::OrderState::Moving)
      continue;
    ++movingCount;
    const DisplayPose pose = DisplayedPose(i);
    centreX += ViewX(pose.pos);
    centreZ += ViewZ(pose.pos);
    leadX += std::sin(pose.headingRad) * state[i].speed;
    leadZ += std::cos(pose.headingRad) * state[i].speed;
  }
  if (movingCount > 0)
  {
    m_camera->SetGoal(centreX / static_cast<float>(movingCount), centreZ / static_cast<float>(movingCount));
    leadX = leadX / static_cast<float>(movingCount) * CAMERA_LEAD_FACTOR;
    leadZ = leadZ / static_cast<float>(movingCount) * CAMERA_LEAD_FACTOR;
  }
  else
  {
    leadX = 0.0f;
    leadZ = 0.0f;
  }

  m_camera->Follow(leadX, leadZ, dt);
  m_camera->UpdateShake(dt);
  m_camera->Update(); // everything above moved it
}

Rgba UniverseView::LiveryOf(Game::FactionId _faction, bool _own, bool _hostileToMe) noexcept
{
  // The hostile row outranks the faction rows (Design/Archive/Stations.md 9.3): a Vanguard ship whose
  // faction holds this client hostile paints the Vandals' red, because the law turning on you is the
  // thing the player must see.
  if (_own)
    return SELECTABLE_LIVERIES[PLAYER_LIVERY_INDEX];
  if (_hostileToMe)
    return LIVERY_VANDAL;
  if (_faction == Game::FACTION_VANGUARD)
    return LIVERY_VANGUARD;
  // FACTION_VANDAL reaches here only if it ever stops being hostile to this client, and a faction
  // a later slice adds reaches it until someone gives it a row. Red is the safe answer for both:
  // a stranger drawn as a friend is the one mistake this table must not make.
  return LIVERY_VANDAL;
}

bool UniverseView::IsOwn(std::size_t _index) const noexcept
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  return _index < state.size() && state[_index].factionId == m_ownFaction;
}

// Ray against a hull's oriented bounding box. A sphere would be far too loose on a hull three
// times longer than it is wide.
float UniverseView::RayHitDistance(std::size_t _index, const XMFLOAT3& _origin, const XMFLOAT3& _direction, float _padding) const noexcept
{
  const ShipView& view = m_ships[_index];
  // Against the hull as drawn: the latest record can be a hull-length ahead of it.
  const DisplayPose pose = DisplayedPose(_index);
  const float cosH = std::cos(pose.headingRad);
  const float sinH = std::sin(pose.headingRad);

  const XMFLOAT3 centre(ViewX(pose.pos) + (view.pickCentre.x * cosH + view.pickCentre.z * sinH) * SHIP_SCALE,
                        (view.restY + view.pickCentre.y) * SHIP_SCALE,
                        ViewZ(pose.pos) + (-view.pickCentre.x * sinH + view.pickCentre.z * cosH) * SHIP_SCALE);

  // Into hull space: to the centre, undo the heading, undo the scale.
  const float rx = _origin.x - centre.x;
  const float rz = _origin.z - centre.z;
  const float localOrigin[3] = {(rx * cosH - rz * sinH) / SHIP_SCALE, (_origin.y - centre.y) / SHIP_SCALE,
                                (rx * sinH + rz * cosH) / SHIP_SCALE};
  const float localDir[3] = {(_direction.x * cosH - _direction.z * sinH) / SHIP_SCALE, _direction.y / SHIP_SCALE,
                             (_direction.x * sinH + _direction.z * cosH) / SHIP_SCALE};
  const float extent[3] = {view.halfExtents.x * _padding, view.halfExtents.y * _padding, view.halfExtents.z * _padding};

  float tMin = 0.0f;
  float tMax = 1e30f;
  bool hit = true;
  for (int axis = 0; axis < 3 && hit; ++axis)
  {
    if (std::fabs(localDir[axis]) < 1e-8f)
    {
      hit = std::fabs(localOrigin[axis]) <= extent[axis];
      continue;
    }
    float t1 = (-extent[axis] - localOrigin[axis]) / localDir[axis];
    float t2 = (extent[axis] - localOrigin[axis]) / localDir[axis];
    if (t1 > t2)
      std::swap(t1, t2);
    tMin = std::max(tMin, t1);
    tMax = std::min(tMax, t2);
    hit = tMax >= tMin;
  }
  return hit ? tMin : -1.0f;
}

// Somebody else's ship is not pickable at all, so no hover highlight, selection ring, tap, shift-tap
// or double-tap can ever land on one. What a client cannot command it should not appear able to
// (Design/Archive/Hostiles.md 7).
int UniverseView::PickShip(float _xPx, float _yPx) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  m_camera->ScreenRay(_xPx, _yPx, origin, direction);
  const std::span<const Game::ShipSnapshot> state = Ships();

  int best = -1;
  float bestT = 1e30f;
  for (std::size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    if (!IsOwn(i))
      continue;
    const float t = RayHitDistance(i, origin, direction, INPUT_PICK_PADDING);
    if (t >= 0.0f && t < bestT)
    {
      bestT = t;
      best = static_cast<int>(i);
    }
  }
  return best;
}

int UniverseView::PickStation(float _xPx, float _yPx) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  m_camera->ScreenRay(_xPx, _yPx, origin, direction);
  const std::span<const Game::ShipSnapshot> state = Ships();

  int best = -1;
  float bestT = 1e30f;
  for (std::size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    // By the record's own flag, never by the hull table: the wire states what admits ships, and a
    // client working it out from "immovable" would be the inference the flag exists to end.
    if ((state[i].flags & Game::SHIP_FLAG_STATION) == 0)
      continue;
    const float t = RayHitDistance(i, origin, direction, INPUT_STRUCTURE_PICK_PADDING);
    if (t >= 0.0f && t < bestT)
    {
      bestT = t;
      best = static_cast<int>(i);
    }
  }
  return best;
}

int UniverseView::PickGate(float _xPx, float _yPx) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  m_camera->ScreenRay(_xPx, _yPx, origin, direction);
  const std::span<const Game::ShipSnapshot> state = Ships();

  int best = -1;
  float bestT = 1e30f;
  for (std::size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    if ((state[i].flags & Game::SHIP_FLAG_GATE) == 0)
      continue;
    const float t = RayHitDistance(i, origin, direction, INPUT_STRUCTURE_PICK_PADDING);
    if (t >= 0.0f && t < bestT)
    {
      bestT = t;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void UniverseView::IssueJumpOrder(std::size_t _gate)
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  if (m_transport == nullptr || _gate >= state.size() || SelectedFleetCount() == 0)
    return;

  // No standing check before this one, and that is not an omission: a gate takes anyone this phase,
  // so an affordance that refused would be telling a truth the simulation does not hold
  // (Design/Archive/Universe.md 6.1). The day gates can be owned, this is where the refusal goes -- beside
  // IssueDockOrder's, and with the simulation's gate still standing behind it.
  Game::FleetOrder order;
  order.kind = Game::FleetOrderKind::Jump;
  order.gate = state[_gate].entity;
  const std::uint32_t sent = SendToSelectedFleets(order);
  if (sent == 0)
    return;

  // The marker the tap earns, on the gate, so the tap visibly landed on the thing rather than the
  // ground beside it. Amber, because a gate is a road rather than an allegiance -- the one marker in
  // this view whose colour is not somebody's livery.
  const DisplayPose pose = DisplayedPose(_gate);
  OrderMarker marker;
  marker.posUniverse = XMFLOAT3(ViewX(pose.pos), 0.0f, ViewZ(pose.pos));
  marker.colour = HUD_ACCENT_AMBER;
  m_markers.push_back(marker);

  if (m_log)
    m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "JUMPING | %d %s", static_cast<int>(sent), (sent == 1) ? "FLEET" : "FLEETS");
}

int UniverseView::PickHostile(float _xPx, float _yPx) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  m_camera->ScreenRay(_xPx, _yPx, origin, direction);
  const std::span<const Game::ShipSnapshot> state = Ships();

  int best = -1;
  float bestT = 1e30f;
  for (std::size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    // By the update header's mask, never by the faction id: a client that decided who its enemies
    // were from an identity would be inferring server state, which is the thing the mask exists to
    // end (Design/Archive/Stations.md 4.3).
    if (!IsHostileToMe(state[i].factionId))
      continue;
    const float t = RayHitDistance(i, origin, direction, INPUT_PICK_PADDING);
    if (t >= 0.0f && t < bestT)
    {
      bestT = t;
      best = static_cast<int>(i);
    }
  }
  return best;
}

// One message per selected slot, and nothing in any of them names a ship. A selection of five
// fleets of eight is five fixed-size messages against an order budget of eight, where the ship-list
// order this replaced would have been forty handles and a cap to check against (ADR 0049).
std::uint32_t UniverseView::SendToSelectedFleets(const Game::FleetOrder& _order)
{
  if (m_transport == nullptr)
    return 0;

  std::uint32_t sent = 0;
  Game::FleetOrder order = _order;
  for (int slot = 0; slot < FLEET_SLOTS; ++slot)
  {
    if (!m_fleetSelected[slot] || !IsFleetHeld(slot))
      continue;
    order.slot = static_cast<std::uint8_t>(slot);
    // A refused send is the queue being full, which Transport.h calls normal: that fleet's order is
    // dropped and the rest still go. Nothing is retried, exactly as a ship-list order was not -- but
    // it is no longer silent. The player pressed a button and that fleet will not move, and until
    // an order is answered on the wire (Design/GameDesignPlan.md 4, decision 1) this line is the
    // only thing that says so.
    if (Game::WriteFleetOrder(order, *m_transport))
    {
      ++sent;
    }
    else if (m_log != nullptr)
    {
      m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "ORDER DROPPED | FLEET %d", slot + 1);
    }
  }
  return sent;
}

void UniverseView::IssueMoveOrder(const XMFLOAT3& _point, bool _hasFacing, float _facingRad)
{
  if (m_transport == nullptr || SelectedFleetCount() == 0)
    return;

  // Gathered for the marker's heading only. The order itself carries no ships at all -- which is
  // why there is no MaxShipsPerOrder check here any more, and no ORDER TOO LARGE line: a fleet
  // order cannot be too large, and that is the property the message was shaped for (ADR 0049).
  const std::span<const Game::ShipSnapshot> state = Ships();
  m_orderPositions.clear();
  float firstHeading = 0.0f;
  for (std::size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    if (!m_ships[i].selected)
      continue;
    if (m_orderPositions.empty())
      firstHeading = state[i].headingRad;
    m_orderPositions.push_back(state[i].posUniverse);
  }

  Game::FleetOrder order;
  order.kind = Game::FleetOrderKind::Move;
  order.point = UniversePosAt(_point.x, _point.z);
  order.facingRad = _facingRad;
  order.hasFacing = _hasFacing;
  const std::uint32_t sent = SendToSelectedFleets(order);
  if (sent == 0)
    return;

  // Nothing comes back down the wire to say which way a formation settled, so the marker is
  // oriented here -- by the same function the other half uses, on the same positions, the same
  // point and the same fallback. Not a prediction: the same arithmetic on the same inputs
  // (Design/Archive/Collision-slice-2b.md 2.5).
  //
  // Over every selected fleet's members together, where the server solves each fleet about the
  // point on its own. With one fleet selected -- which is the ordinary case -- the two are the same
  // arithmetic; with several the marker shows one heading for orders that will settle into several,
  // which is a thing one marker cannot say and the design accepted when it made five buttons
  // orderable at once (Design/Archive/Fleets.md 9.2).
  const float heading = _hasFacing ? _facingRad : Game::FormationHeading(m_orderPositions, order.point, firstHeading);

  OrderMarker marker;
  marker.posUniverse = XMFLOAT3(_point.x, 0.0f, _point.z);
  marker.facingRad = heading;
  marker.hasFacing = _hasFacing;
  marker.colour = MARKER_COLOUR;
  m_markers.push_back(marker);

  if (m_log)
    m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "MOVE ORDER | %d %s", static_cast<int>(sent),
                      (sent == 1) ? "FLEET" : "FLEETS");
}

void UniverseView::OrderMoveAt(float _viewX, float _viewZ)
{
  // Said out loud when it cannot be obeyed: a map tap that silently does nothing reads as a broken
  // map, and the log is where every other refused order already speaks.
  if (SelectedFleetCount() == 0)
  {
    if (m_log)
      m_log->Push(EventLog::Severity::Info, SimTimeSec(), "NO FLEET SELECTED");
    return;
  }
  IssueMoveOrder(XMFLOAT3(_viewX, 0.0f, _viewZ), false, 0.0f);
}

void UniverseView::IssueDockOrder(std::size_t _station)
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  if (m_transport == nullptr || _station >= state.size() || SelectedFleetCount() == 0)
    return;
  const Game::FactionId owner = state[_station].factionId;

  // The affordance tells the truth first: an owner that holds this client hostile refuses here,
  // and nothing is sent. The simulation's gate still stands behind it, per the twice-on-purpose
  // rule -- affordances tell the truth, and clients are not trusted (Design/Archive/Stations.md 9.2).
  if (IsHostileToMe(owner))
  {
    if (m_log)
    {
      const char* name = (owner < m_factionNames.size()) ? m_factionNames[owner] : "UNKNOWN";
      m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "DOCKING REFUSED | %s HOSTILE", name);
    }
    return;
  }

  Game::FleetOrder order;
  order.kind = Game::FleetOrderKind::Dock;
  order.station = state[_station].entity;
  const std::uint32_t sent = SendToSelectedFleets(order);
  if (sent == 0)
    return;

  // The marker the tap earns, on the station and in its colour, so the tap visibly landed on the
  // thing and not the ground beside it. No facing: a dock order has none.
  const DisplayPose pose = DisplayedPose(_station);
  OrderMarker marker;
  marker.posUniverse = XMFLOAT3(ViewX(pose.pos), 0.0f, ViewZ(pose.pos));
  marker.colour = LiveryOf(owner, owner == m_ownFaction, false);
  m_markers.push_back(marker);

  if (m_log)
    m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "DOCKING | %d %s", static_cast<int>(sent), (sent == 1) ? "FLEET" : "FLEETS");
}

// The third tap meaning, and the one this slice adds. There is no affordance to check first, unlike
// the dock: the picker only ever offers a record the mask already says is hostile, so a tap that
// reaches here has passed the only question there is to ask (Design/Archive/Fleets.md 9.3).
void UniverseView::IssueAttackOrder(std::size_t _target)
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  if (m_transport == nullptr || _target >= state.size() || SelectedFleetCount() == 0)
    return;

  Game::FleetOrder order;
  order.kind = Game::FleetOrderKind::Attack;
  order.target = state[_target].entity;
  const std::uint32_t sent = SendToSelectedFleets(order);
  if (sent == 0)
    return;

  // On the target and in ITS colour, exactly as the dock marker wears the station's. The scene is
  // an identity language and the HUD is a relation one (ViewTuning.h says so at HUD_ALERT_RED), so
  // a marker in the universe says which ship was tapped rather than how the HUD feels about it.
  const Game::FactionId owner = state[_target].factionId;
  const DisplayPose pose = DisplayedPose(_target);
  OrderMarker marker;
  marker.posUniverse = XMFLOAT3(ViewX(pose.pos), 0.0f, ViewZ(pose.pos));
  marker.colour = LiveryOf(owner, owner == m_ownFaction, IsHostileToMe(owner));
  m_markers.push_back(marker);

  if (m_log)
    m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "ATTACK | %d %s", static_cast<int>(sent), (sent == 1) ? "FLEET" : "FLEETS");
}

// --- pointer intent -----------------------------------------------------------------------------
// With nothing selected a drag bands a box; with a selection it lays down a move order and its
// final facing. Shift forces the box either way.
bool UniverseView::WantsBoxSelect(bool _shiftHeld)
{
  return _shiftHeld || SelectedFleetCount() == 0;
}

void UniverseView::OnHover(float _xPx, float _yPx)
{
  m_hoverShip = PickShip(_xPx, _yPx);
}

void UniverseView::OnDragUpdate(bool _boxSelect, float _x0Px, float _y0Px, float _x1Px, float _y1Px)
{
  CancelFocus(); // the player has taken the camera back
  // An order drag draws nothing while held -- the marker on release is its feedback. The line it
  // used to draw read as a path the fleet would fly, which a move-with-facing order is not.
  m_boxActive = _boxSelect;
  m_boxX0Px = _x0Px;
  m_boxY0Px = _y0Px;
  m_boxX1Px = _x1Px;
  m_boxY1Px = _y1Px;
}

void UniverseView::OnDragCancelled()
{
  m_boxActive = false;
}

void UniverseView::OnBoxSelect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, bool _additive)
{
  const float left = std::min(_x0Px, _x1Px);
  const float right = std::max(_x0Px, _x1Px);
  const float top = std::min(_y0Px, _y1Px);
  const float bottom = std::max(_y0Px, _y1Px);
  if (!_additive)
    ClearSelection();

  // A band takes every fleet it touches, WHOLE. Sub-fleet selection does not exist, so a box over
  // half a wedge selects the wedge -- which is the decision the design took once and this is the
  // only place a player could otherwise have contradicted it (Design/Archive/Fleets.md 15, decision 1).
  const std::span<const Game::ShipSnapshot> state = Ships();
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    if (!IsOwn(i))
      continue; // a box drawn over the enemy's base selects nothing, the same as a tap on it

    const DisplayPose pose = DisplayedPose(i);
    const XMFLOAT3 centre(ViewX(pose.pos), m_ships[i].halfExtents.y * SHIP_SCALE, ViewZ(pose.pos));
    float xPx = 0.0f;
    float yPx = 0.0f;
    if (!m_camera->WorldToScreen(centre, xPx, yPx) || xPx < left || xPx > right || yPx < top || yPx > bottom)
      continue;
    const int slot = FleetSlotOf((i < m_entities.size()) ? m_entities[i] : Game::INVALID_ENTITY_ID);
    if (slot >= 0)
      m_fleetSelected[slot] = true;
  }
  RefreshSelection();
}

void UniverseView::OnOrderDrag(float _x0Px, float _y0Px, float _x1Px, float _y1Px)
{
  XMFLOAT3 from;
  XMFLOAT3 to;
  if (!m_camera->RayToGround(_x0Px, _y0Px, from))
    return;
  if (!m_camera->RayToGround(_x1Px, _y1Px, to))
  {
    IssueMoveOrder(from, false, 0.0f);
    return;
  }
  const float dx = to.x - from.x;
  const float dz = to.z - from.z;
  const bool hasFacing = (dx * dx + dz * dz) > 1.0f;
  IssueMoveOrder(from, hasFacing, hasFacing ? std::atan2(dx, dz) : 0.0f);
}

void UniverseView::OnTap(float _xPx, float _yPx, bool _shiftHeld, bool _doubleTap)
{
  CancelFocus(); // the player is working the universe again, so the camera stops flying

  // An armed command takes the tap before anything else means anything, and clears whether or not
  // the tap landed on something it can use: one prompt, one tap (Design/Archive/Fleets.md 9.3).
  if (m_armed != ArmedOrder::None)
  {
    const ArmedOrder armed = m_armed;
    m_armed = ArmedOrder::None;
    if (m_tracker)
      m_tracker->ResetTapHistory();

    if (armed == ArmedOrder::Attack)
    {
      const int hostile = PickHostile(_xPx, _yPx);
      if (hostile >= 0)
      {
        IssueAttackOrder(static_cast<std::size_t>(hostile));
        return;
      }
    }
    else if (armed == ArmedOrder::Dock)
    {
      const int station = PickStation(_xPx, _yPx);
      if (station >= 0)
      {
        IssueDockOrder(static_cast<std::size_t>(station));
        return;
      }
    }
    else if (armed == ArmedOrder::Jump)
    {
      const int gate = PickGate(_xPx, _yPx);
      if (gate >= 0)
      {
        IssueJumpOrder(static_cast<std::size_t>(gate));
        return;
      }
    }
    else
    {
      XMFLOAT3 point;
      if (m_camera->RayToGround(_xPx, _yPx, point))
      {
        IssueMoveOrder(point, false, 0.0f);
        return;
      }
    }

    // The tap did not supply what the command needed. Cancelled, and said so -- a prompt that
    // vanished with nothing happening reads as a broken game.
    if (m_log)
      m_log->PushFormat(EventLog::Severity::Info, SimTimeSec(), "ORDER CANCELLED");
    return;
  }

  // A tapped hull selects its whole FLEET, not the hull. A hull whose roster has not arrived yet
  // selects nothing and says nothing: that is a launch one update old, not an error.
  const int hit = PickShip(_xPx, _yPx);
  if (hit >= 0)
  {
    const std::size_t at = static_cast<std::size_t>(hit);
    const int slot = FleetSlotOf((at < m_entities.size()) ? m_entities[at] : Game::INVALID_ENTITY_ID);
    if (slot >= 0)
      SelectFleet(slot, _shiftHeld);
    if (m_tracker)
      m_tracker->ResetTapHistory(); // tapping a hull does not begin a double tap
    return;
  }

  // With a selection, a tap on something is an order to every selected fleet, in this order: a
  // station docks, a hostile record attacks, the ground moves. With nothing selected it is nothing
  // at all -- selection-for-inspection is the station screen's, which is slice 7
  // (Design/Archive/Stations.md 9.1, Design/Archive/Fleets.md 9.3).
  if (SelectedFleetCount() > 0)
  {
    const int station = PickStation(_xPx, _yPx);
    if (station >= 0)
    {
      IssueDockOrder(static_cast<std::size_t>(station));
      if (m_tracker)
        m_tracker->ResetTapHistory();
      return;
    }

    // After the station and before the ground: a hostile structure is a place to attack rather than
    // a place to dock, and the dock gate would have refused it anyway.
    const int hostile = PickHostile(_xPx, _yPx);
    if (hostile >= 0)
    {
      IssueAttackOrder(static_cast<std::size_t>(hostile));
      if (m_tracker)
        m_tracker->ResetTapHistory();
      return;
    }
  }

  // Double tapping empty ground is how a selection is dropped, since a single tap with a selection
  // is already a move order.
  if (_doubleTap)
  {
    ClearSelection();
    return;
  }
  if (SelectedFleetCount() == 0)
    return;
  XMFLOAT3 point;
  if (m_camera->RayToGround(_xPx, _yPx, point))
    IssueMoveOrder(point, false, 0.0f);
}

// --- the station ledger ---------------------------------------------------------------------------
// A long press over a station asks what this client has docked there, and the answer opens the
// assembly screen. It is the one request/reply on this seam (ADR 0051), and the one gesture
// Design/Archive/Stations.md 14 held PointerTracker back from until there was a menu to open.
void UniverseView::OnLongPress(float _xPx, float _yPx)
{
  CancelFocus();

  const int station = PickStation(_xPx, _yPx);
  if (station < 0 || m_transport == nullptr)
    return;

  const std::span<const Game::ShipSnapshot> state = Ships();
  if (static_cast<std::size_t>(station) >= state.size())
    return;

  // The mask first, before the wire is touched. A port that holds this client hostile answers a
  // ledger request with zeros by the same gate that refuses a compose there (Universe::LedgerFor), so
  // asking would spend a message to be told nothing -- and the player would read an empty station
  // rather than a closed one. The affordance tells the truth first and the gate stands behind it,
  // which is IssueDockOrder's rule applied to a read (Design/Archive/Stations.md 9.2).
  const Game::FactionId owner = state[static_cast<std::size_t>(station)].factionId;
  if (IsHostileToMe(owner))
  {
    if (m_log)
    {
      const char* name = (owner < m_factionNames.size()) ? m_factionNames[owner] : "UNKNOWN";
      m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "LEDGER REFUSED | %s HOSTILE", name);
    }
    return;
  }

  Game::LedgerRequest request;
  request.station = state[static_cast<std::size_t>(station)].entity;
  if (!Game::WriteLedgerRequest(request, *m_transport))
    return; // the lane is full or not up: the press is dropped, and a second one is the retry

  m_ledgerAsked = request.station;
  m_ledgerAskedAtCount = m_receiver.LedgerReplyCount();
}

Game::FactionId UniverseView::FactionOfEntity(Game::EntityId _entity) const noexcept
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  for (const Game::ShipSnapshot& ship : state)
  {
    if (ship.entity == _entity)
      return ship.factionId;
  }
  return static_cast<Game::FactionId>(Game::FACTION_LIMIT);
}

bool UniverseView::TakeLedgerReply(Game::LedgerReply& _outReply)
{
  if (m_ledgerAsked == Game::INVALID_ENTITY_ID || m_receiver.LedgerReplyCount() == m_ledgerAskedAtCount)
    return false;

  // The counter moved, so something was answered. Whether it answers THIS question is the station,
  // and a reply for another one is dropped rather than shown: it is this client's own answer to an
  // ask it has since replaced.
  //
  // The ask is forgotten either way. One long press is one question, and a question that got the
  // wrong answer is not re-asked here -- a second press is the retry, and the player has it.
  const Game::LedgerReply& reply = m_receiver.Ledger();
  const Game::EntityId asked = m_ledgerAsked;
  m_ledgerAsked = Game::INVALID_ENTITY_ID;
  if (reply.station != asked)
    return false;

  _outReply = reply;
  return true;
}

void UniverseView::SendComposeOrder(Game::EntityId _station, std::uint8_t _slot, std::span<const std::uint32_t> _hullCounts)
{
  if (m_transport == nullptr)
    return;

  Game::ComposeOrder order;
  order.station = _station;
  order.slot = _slot;
  for (std::size_t hull = 0; hull < _hullCounts.size() && hull < Game::HULL_COUNT; ++hull)
    order.hullCounts[hull] = _hullCounts[hull];

  // Fire and forget, like every order on this lane. A refusal -- a raced slot, a ledger that moved
  // -- simply leaves the button empty, and the screen's next opening asks again (Design/Archive/Fleets.md 9.4).
  if (!Game::WriteComposeOrder(order, *m_transport))
    return;

  if (m_log)
  {
    std::uint32_t total = 0;
    for (std::size_t hull = 0; hull < _hullCounts.size() && hull < Game::HULL_COUNT; ++hull)
      total += _hullCounts[hull];
    m_log->PushFormat(EventLog::Severity::Friendly, SimTimeSec(), "FLEET %d | LAUNCHING %d SHIPS", static_cast<int>(_slot) + 1,
                      static_cast<int>(total));
  }
}

// ------------------------------------------------------------------------------------------------
// Rendering. Every position and heading is read at the display time set by SetDisplayTime, between
// the two samples that bracket it, so motion is smooth however far the swapchain runs ahead of the
// simulation rate and however far the update rate sits below it.

void UniverseView::Render(SceneRenderer& _renderer, GpuDevice& _gpu, TextRenderer& _text)
{
  // The sky, before everything. It neither tests nor writes depth, so the ground, the hulls and the
  // planets all draw over it whatever their distance -- which is the point: a body two kilometers out
  // has to sit in front of a sphere nominally at five (SkyRenderer.h).
  if (m_sky != nullptr && m_sky->Ready())
  {
    SkyRenderer::Frame sky = m_skyTuning;
    sky.viewProj = m_camera->ViewProj();
    sky.cameraRight = m_camera->Right();
    sky.cameraUp = m_camera->Up();
    sky.cameraPos = m_camera->Eye();
    sky.timeSec = m_skyTimeSec;
    m_sky->Draw(_gpu, sky);
  }

  SceneFrame frame = {};
  frame.viewProj = m_camera->ViewProj();
  frame.lightDir = XMFLOAT3(LIGHT_DIR_X, LIGHT_DIR_Y, LIGHT_DIR_Z);
  frame.ambient = AMBIENT_LEVEL;
  frame.cameraPos = m_camera->Eye();
  _renderer.BeginScene(_gpu, frame);

  // One frustum a frame, tested against everything below. Built from the camera's two matrices
  // rather than their product, because that is what BoundingFrustum is defined on
  // (Design/Archive/MmoScalabilityReview.md G2).
  m_frustum = Neuron::WorldFrustum(m_camera->View(), m_camera->Proj());
  const BoundingFrustum& frustum = m_frustum;
  m_submittedCount = 0;
  m_culledCount = 0;
  for (MeshBucket& bucket : m_meshBuckets)
    bucket.instances.clear();

  XMFLOAT4X4 world;

  // The bodies' world matrices. One transform per body per frame, not two: the terrain pass and
  // the outline pass over the same body want exactly the same matrix.
  m_bodyWorlds.clear();
  for (const BodyView& body : m_bodies)
  {
    const XMMATRIX orientation =
      XMMatrixMultiply(XMLoadFloat3x3(&body.tumble), XMMatrixRotationAxis(XMLoadFloat3(&body.spinAxis), body.spinRad));
    XMFLOAT4X4 bodyWorld;
    XMStoreFloat4x4(&bodyWorld, XMMatrixMultiply(orientation, XMMatrixTranslation(ViewX(body.centre), body.centreY, ViewZ(body.centre))));
    m_bodyWorlds.push_back(bodyWorld);
  }

  // Visibility and the level of detail are decided once per body, here, and the terrain and
  // outline passes below reuse both: two passes over the same spheres would be two chances for
  // them to disagree, and a body whose outline drew but whose land did not is a wireframe hanging
  // in empty space. The projected radius that chooses the level also finishes the culling: an
  // asteroid smaller on screen than BODY_CULL_BELOW_PX is not submitted at all, which is the
  // distance cull slice 9's frustum test always lacked (Design/Archive/BodyLod-work-order.md 2.3).
  m_bodyVisible.clear();
  m_bodyLod.clear();
  m_bodyTriangles = 0;
  const float projScalePx = static_cast<float>(_gpu.HeightPx()) * 0.5f / std::tan(XMConvertToRadians(CAMERA_FOV_DEG) * 0.5f);
  const XMFLOAT3 eye = m_camera->Eye();
  for (std::size_t i = 0; i < m_bodies.size(); ++i)
  {
    const BodyView& body = m_bodies[i];
    const XMFLOAT3 centre(ViewX(body.centre), body.centreY, ViewZ(body.centre));
    const float radius = body.boundingRadiusMetres * CULL_BODY_RADIUS_SCALE;
    bool visible = Neuron::IsSphereVisible(frustum, centre, radius);

    const float dx = centre.x - eye.x;
    const float dy = centre.y - eye.y;
    const float dz = centre.z - eye.z;
    const float distance = std::max(1.0f, std::sqrt(dx * dx + dy * dy + dz * dz));
    const float projectedPx = body.boundingRadiusMetres / distance * projScalePx;
    if (visible && !body.textured && projectedPx < BODY_CULL_BELOW_PX)
      visible = false; // a rock too small to see; a world is never distance-culled

    std::uint8_t lod = 0;
    if (projectedPx < BODY_LOD2_BELOW_PX)
      lod = 2;
    else if (projectedPx < BODY_LOD1_BELOW_PX)
      lod = 1;
    // A level that failed to bake falls back to the finest that exists, so a partial content
    // failure degrades to what the scene drew before levels existed.
    while (lod > 0 && body.terrainLod[lod] == Neuron::INVALID_BODY)
      --lod;

    m_bodyVisible.push_back(visible);
    m_bodyLod.push_back(lod);
    if (visible)
    {
      ++m_submittedCount;
      m_bodyTriangles += body.triangleCountLod[lod];
    }
    else
    {
      ++m_culledCount;
    }
  }

  const std::span<const Game::ShipSnapshot> state = Ships();
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    ShipView& view = m_ships[i];

    const DisplayPose pose = DisplayedPose(i);
    const float x = ViewX(pose.pos);
    const float z = ViewZ(pose.pos);
    const float heading = pose.headingRad;

    const XMMATRIX hull = HullMatrix(view, pose);
    XMStoreFloat4x4(&world, hull);

    // Whose paint this hull wears -- in-scene IFF the moment a hull is on screen, rather than an
    // overview the player has to look away to read. Only the surfaces the model declared RaceTinted
    // take it; the plating and the glass are the model's own whoever is flying
    // (Design/Archive/NmoFormat.md 5.5).
    //
    // "Hostile to me" is the server's word, from the update header's mask, and never inferred from
    // the faction: a Vanguard hull is azure until the law turns on the player, and then it is red
    // with the Vandals' (Design/Archive/Stations.md 4.3, 9.3).
    const bool own = IsOwn(i);
    const Rgba livery = LiveryOf(state[i].factionId, own, IsHostileToMe(state[i].factionId));
    view.lastLivery = livery;

    // Selection is a brightness now, not a hue: a mint-green selected hull would read as a different
    // faction, and the player's own livery might be mint (ViewTuning.h). The hover lift folds into
    // the same channel.
    const float highlight =
      std::clamp((view.selected ? SELECTED_HIGHLIGHT_LIFT : 0.0f) + view.hoverAmount * SEL_HOVER_HIGHLIGHT_STRENGTH, 0.0f, 1.0f);

    // The bounding sphere the hull was drawn through: the mesh's own bounds, carried to where the
    // hull is and scaled the way the hull is. Padded, because a tight sphere is exactly what pops at
    // the edge of the screen and the plume trails outside the bounds entirely.
    const XMFLOAT3 boundsCentre(x + (view.pickCentre.x * std::cos(heading) + view.pickCentre.z * std::sin(heading)) * SHIP_SCALE,
                                SHIP_HOVER_HEIGHT + (view.restY + view.pickCentre.y) * SHIP_SCALE,
                                z + (-view.pickCentre.x * std::sin(heading) + view.pickCentre.z * std::cos(heading)) * SHIP_SCALE);
    const float boundsRadius = std::sqrt(view.halfExtents.x * view.halfExtents.x + view.halfExtents.y * view.halfExtents.y +
                                         view.halfExtents.z * view.halfExtents.z) *
                                 SHIP_SCALE +
                               CULL_RADIUS_PAD_METRES;
    view.visible = Neuron::IsSphereVisible(frustum, boundsCentre, boundsRadius);
    if (view.visible)
    {
      ++m_submittedCount;
      // Bucketed by mesh rather than drawn, so a fleet of one hull is one draw. Bucketing by the
      // mesh handle and not the hull id is what makes two hull ids sharing a mesh share a draw, and
      // the handle is what the draw needs anyway.
      Bucket(view.mesh).push_back(Neuron::MeshInstance{.world = world, .tint = {livery.r, livery.g, livery.b, highlight}});
    }
    else
    {
      ++m_culledCount;
    }

    // Remembered for the explosion, which needs where the hull was and how it was moving at a point
    // where the snapshot no longer has a record for it. Taken from the drawn pose rather than the
    // latest one, so the shards start exactly where the hull was and drift the way it was pointing.
    //
    // Outside the visibility test on purpose. A ship that dies off screen still explodes, and the
    // player may well be looking at where it was a moment later -- if this only ran for hulls that
    // were submitted, those shards would start from wherever the hull last happened to be on screen.
    // Culling decides what is drawn; it must not decide what is remembered.
    view.lastWorld = world;
    view.lastVelMetresPerSec = XMFLOAT3(std::sin(heading) * state[i].speed, 0.0f, std::cos(heading) * state[i].speed);
    view.drawn = true;
  }

  // One draw per hull family present. Order within the opaque pass does not matter -- it writes and
  // tests depth -- so grouping by mesh costs nothing and buys everything. A mesh with translucent
  // parts (NmoRenderFlags::AlphaBlend) is drawn here as its opaque runs and further down as its
  // glass, so the part table decides the draw count and a hull without one still costs one draw.
  const auto blendedPart = [](const Neuron::MeshSubMesh& _part)
  { return (_part.renderFlags & static_cast<std::uint32_t>(Neuron::NmoRenderFlags::AlphaBlend)) != 0; };
  const auto hasBlendedPart = [&](const Neuron::MeshData& _data)
  {
    for (const Neuron::MeshSubMesh& part : _data.subMeshes)
    {
      if (blendedPart(part))
        return true;
    }
    return false;
  };
  bool anyBlended = false;
  for (const MeshBucket& bucket : m_meshBuckets)
  {
    if (bucket.instances.empty())
      continue;
    const Neuron::MeshData& data = m_meshes->Data(bucket.mesh);
    if (!hasBlendedPart(data))
    {
      _renderer.DrawMeshInstanced(_gpu, bucket.mesh, bucket.instances);
      continue;
    }
    anyBlended = true;

    // The parts tile the soup in file order (MeshData.h), so the opaque remainder is the runs
    // between the translucent parts, merged where they touch.
    std::uint32_t runStart = 0;
    for (const Neuron::MeshSubMesh& part : data.subMeshes)
    {
      if (!blendedPart(part))
        continue;
      if (part.firstVertex > runStart)
        _renderer.DrawMeshInstancedRange(_gpu, bucket.mesh, bucket.instances, runStart, part.firstVertex - runStart);
      runStart = part.firstVertex + part.vertexCount;
    }
    const std::uint32_t total = static_cast<std::uint32_t>(data.verts.size());
    if (total > runStart)
      _renderer.DrawMeshInstancedRange(_gpu, bucket.mesh, bucket.instances, runStart, total - runStart);
  }

  // The bodies: every terrain, then every outline. Two passes rather than two draws per body, so
  // there is one pipeline switch per pass and body A's outline tests against body B's depth
  // (Design/Archive/PlanetRenderer.md 7.3).
  if (m_bodyRenderer != nullptr && !m_bodies.empty())
  {
    m_bodyRenderer->Begin(_gpu, frame.viewProj, frame.lightDir, frame.ambient, frame.cameraPos, BODY_OVERLAY);
    for (std::size_t i = 0; i < m_bodies.size(); ++i)
    {
      if (!m_bodyVisible[i])
        continue;
      if (m_bodies[i].textured)
        m_bodyRenderer->DrawPlanet(_gpu, m_bodies[i].terrainLod[m_bodyLod[i]], m_bodyWorlds[i]);
      else
        m_bodyRenderer->DrawMain(_gpu, m_bodies[i].terrainLod[m_bodyLod[i]], m_bodyWorlds[i]);
    }
    // The outline belongs to a generated body. Over a textured one it reads as a cage drawn on a
    // photograph, so a textured world is skipped here rather than being given a fainter one.
    for (std::size_t i = 0; i < m_bodies.size(); ++i)
    {
      if (m_bodyVisible[i] && !m_bodies[i].textured)
        m_bodyRenderer->DrawOverlay(_gpu, m_bodies[i].terrainLod[m_bodyLod[i]], m_bodyWorlds[i]);
    }
  }

  // The hulls' translucent parts, after the opaque hulls AND the bodies: they test depth against
  // the settled world and write none, so a gate's aperture film blends over whatever stands or
  // flies behind it -- drawn any earlier, a body would overwrite the film wherever the film had
  // declined to write depth. This is the blended overlay pass the format has always named
  // (Design/Archive/NmoFormat.md 5.5); the explosion fragments and sprites below blend over it in
  // turn. BeginScene again first, because the body pass swapped in its own root signature.
  if (anyBlended)
  {
    _renderer.BeginScene(_gpu, frame);
    for (const MeshBucket& bucket : m_meshBuckets)
    {
      if (bucket.instances.empty())
        continue;
      const Neuron::MeshData& data = m_meshes->Data(bucket.mesh);
      for (const Neuron::MeshSubMesh& part : data.subMeshes)
      {
        if (blendedPart(part) && part.vertexCount > 0)
          _renderer.DrawMeshInstancedBlended(_gpu, bucket.mesh, bucket.instances, part.firstVertex, part.vertexCount, part.alpha);
      }
    }
  }

  // Hull fragments before the decals: they are blended but write depth, so a shard occludes what is
  // behind it, and the rings and thruster glow only test (Design/Archive/SpaceshipExplosion.md 8.3).
  if (m_fx != nullptr && m_fx->Ready() && !m_explosions.empty())
  {
    m_fxFragmentVerts.clear();
    for (const ShipExplosion& explosion : m_explosions)
      explosion.BuildFragments(m_fxFragmentVerts);
    if (!m_fxFragmentVerts.empty())
    {
      m_fx->Begin(_gpu, m_camera->ViewProj(), frame.lightDir, frame.ambient, m_camera->Eye());
      m_fx->DrawFragments(_gpu, m_fxFragmentVerts);
    }
  }

  DrawFeedback(_renderer, _gpu, frame);

  // Sprites after: they do not write depth, so they have to see the rings rather than punch a hole
  // through them. Dark before additive, as the source draws them -- the fireball goes on top of the
  // smoke, not behind it.
  if (m_fx != nullptr && m_fx->Ready() && m_particles.Count() > 0)
  {
    const XMFLOAT3& right = m_camera->Right();
    const XMFLOAT3& up = m_camera->Up();
    m_fx->Begin(_gpu, m_camera->ViewProj(), frame.lightDir, frame.ambient, m_camera->Eye());

    m_fxSpriteVerts.clear();
    m_particles.Build(SpriteBlend::Dark, right, up, m_fxSpriteVerts);
    m_fx->DrawSpritesDark(_gpu, m_fxSpriteVerts);

    m_fxSpriteVerts.clear();
    m_particles.Build(SpriteBlend::Additive, right, up, m_fxSpriteVerts);
    m_fx->DrawSpritesAdd(_gpu, m_fxSpriteVerts);
  }

  // Screen-space feedback for whatever the pointer is in the middle of. These go through the text
  // pipeline, so they land on top of everything when it flushes.
  if (m_boxActive)
  {
    const Rgba edge = SEL_RING_COLOUR;
    const Rgba fill{edge.r, edge.g, edge.b, edge.a * 0.12f};
    const float left = std::min(m_boxX0Px, m_boxX1Px);
    const float right = std::max(m_boxX0Px, m_boxX1Px);
    const float top = std::min(m_boxY0Px, m_boxY1Px);
    const float bottom = std::max(m_boxY0Px, m_boxY1Px);
    _text.DrawScreenRect(left, top, right, bottom, fill);
    _text.DrawScreenRect(left, top, right, top + 1.0f, edge);
    _text.DrawScreenRect(left, bottom - 1.0f, right, bottom, edge);
    _text.DrawScreenRect(left, top, left + 1.0f, bottom, edge);
    _text.DrawScreenRect(right - 1.0f, top, right, bottom, edge);
  }
}

// ------------------------------------------------------------------------------------------------
// The overlay pass: selection and hover rings on the ground, order markers, thruster glow and
// trail in the air. All of it is the same unit quad shaped by the decal shader.

std::vector<Neuron::MeshInstance>& UniverseView::Bucket(Neuron::MeshHandle _mesh)
{
  for (MeshBucket& bucket : m_meshBuckets)
  {
    if (bucket.mesh == _mesh)
      return bucket.instances;
  }
  m_meshBuckets.push_back(MeshBucket{.mesh = _mesh, .instances = {}});
  return m_meshBuckets.back().instances;
}

void UniverseView::DrawFeedback(SceneRenderer& _renderer, GpuDevice& _gpu, const SceneFrame& _frame)
{
  _renderer.BeginDecals(_gpu, m_camera->ViewProj(), m_camera->Eye());

  const std::span<const Game::ShipSnapshot> state = Ships();
  XMFLOAT4X4 world;

  // --- selection and hover rings ----------------------------------------------------------------
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    const ShipView& view = m_ships[i];
    // A ring is drawn around a hull and is smaller than the sphere that hull was tested with, so
    // the hull's answer is the ring's answer and there is nothing to test again.
    if (!view.visible)
      continue;

    const float hullRadius = std::max(view.halfExtents.x, view.halfExtents.z) * SHIP_SCALE;
    const DisplayPose pose = DisplayedPose(i);
    const float x = ViewX(pose.pos);
    const float z = ViewZ(pose.pos);

    if (view.ringFade > 0.002f && view.ringScale > 0.002f)
    {
      const float radius = hullRadius * SEL_RING_RADIUS_SCALE * view.ringScale;
      XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) * XMMatrixTranslation(x, DECAL_LIFT_Y, z));
      _renderer.DrawDecal(_gpu, m_quadMesh, world,
                          Rgba{SEL_RING_COLOUR.r, SEL_RING_COLOUR.g, SEL_RING_COLOUR.b, SEL_RING_COLOUR.a * view.ringFade},
                          SEL_RING_THICKNESS, 0.0f);
    }

    // The hover ring sits just outside the selection ring so both are readable at once.
    if (view.hoverAmount > 0.002f)
    {
      const float radius = hullRadius * SEL_RING_RADIUS_SCALE * 1.12f;
      XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) * XMMatrixTranslation(x, DECAL_LIFT_Y, z));
      _renderer.DrawDecal(_gpu, m_quadMesh, world,
                          Rgba{SEL_RING_COLOUR.r, SEL_RING_COLOUR.g, SEL_RING_COLOUR.b, SEL_HOVER_RING_ALPHA * view.hoverAmount},
                          SEL_RING_THICKNESS * 0.7f, 0.0f);
    }
  }

  // --- order markers ----------------------------------------------------------------------------
  const float expandSec = std::max(0.001f, MARKER_EXPAND_MS * 0.001f);
  const float pulseSec = std::max(0.001f, MARKER_PULSE_PERIOD_MS * 0.001f);
  const float lifeSec = std::max(0.05f, MARKER_LIFETIME_MS * 0.001f);
  const float fadeSec = std::max(0.001f, MARKER_FADE_OUT_MS * 0.001f);

  for (const OrderMarker& marker : m_markers)
  {
    // Expands in, holds while it pulses, fades out at the end of its life.
    const float grow = std::clamp(marker.ageSec / expandSec, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - grow) * (1.0f - grow); // ease out
    const float fadeStart = std::max(0.0f, lifeSec - fadeSec);
    const float fade = (marker.ageSec <= fadeStart) ? 1.0f : std::clamp(1.0f - (marker.ageSec - fadeStart) / fadeSec, 0.0f, 1.0f);

    const float sincePulseStart = marker.ageSec - expandSec;
    const float pulseIndex = sincePulseStart / pulseSec;
    float beat = 0.0f;
    if (sincePulseStart > 0.0f && pulseIndex < static_cast<float>(MARKER_PULSE_COUNT))
    {
      const float withinPulse = pulseIndex - std::floor(pulseIndex);
      beat = std::sin(withinPulse * XM_PI); // 0 at each pulse boundary, 1 in the middle
    }

    const float radius = MARKER_RADIUS * eased * (1.0f + beat * MARKER_PULSE_SCALE);
    const float alpha = marker.colour.a * fade * (0.72f + beat * 0.28f);
    XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) *
                              XMMatrixTranslation(marker.posUniverse.x, DECAL_LIFT_Y, marker.posUniverse.z));
    _renderer.DrawDecal(_gpu, m_quadMesh, world, Rgba{marker.colour.r, marker.colour.g, marker.colour.b, alpha}, MARKER_THICKNESS, 0.10f);

    // Each pulse also throws a ripple outwards, which is what makes the count readable.
    if (beat > 0.0f)
    {
      const float withinPulse = pulseIndex - std::floor(pulseIndex);
      const float rippleRadius = radius * (1.0f + withinPulse * 1.1f);
      XMStoreFloat4x4(&world, XMMatrixScaling(rippleRadius * 2.0f, 1.0f, rippleRadius * 2.0f) *
                                XMMatrixTranslation(marker.posUniverse.x, DECAL_LIFT_Y, marker.posUniverse.z));
      _renderer.DrawDecal(_gpu, m_quadMesh, world,
                          Rgba{marker.colour.r, marker.colour.g, marker.colour.b, alpha * (1.0f - withinPulse) * 0.7f},
                          MARKER_THICKNESS * 0.6f, 0.0f);
    }

    // A pip out along the ordered facing, so a drag order shows which way the ships will end up.
    if (marker.hasFacing)
    {
      const float pip = radius * 0.28f;
      const float outX = marker.posUniverse.x + std::sin(marker.facingRad) * radius * 1.5f;
      const float outZ = marker.posUniverse.z + std::cos(marker.facingRad) * radius * 1.5f;
      XMStoreFloat4x4(&world, XMMatrixScaling(pip * 2.0f, 1.0f, pip * 2.0f) * XMMatrixTranslation(outX, DECAL_LIFT_Y, outZ));
      _renderer.DrawDecal(_gpu, m_quadMesh, world, Rgba{marker.colour.r, marker.colour.g, marker.colour.b, alpha}, 1.0f, 1.0f);
    }
  }

  // --- shock rings ------------------------------------------------------------------------------
  // One expanding front per death that asked for one, through the same decal the selection rings
  // and order markers use. Drawn here rather than by the fx pass because it is a ground marker in
  // the game's existing language, not a billboard.
  for (const ShipExplosion& explosion : m_explosions)
  {
    if (!explosion.HasShockRing())
      continue;

    const float radius = explosion.ShockRingRadiusMetres();
    if (radius <= 0.5f)
      continue; // the frame it is born in, before the first Advance has widened it

    // A front is a flat disc on the ground and its centre and radius are exactly known, so the
    // sphere here needs no padding beyond the ring's own width.
    if (!Neuron::IsSphereVisible(m_frustum, explosion.ShockRingCentre(), radius + SHOCK_RING_WIDTH_METRES))
      continue;

    // The decal's thickness is a fraction of its own half-extent, so holding the front to a width
    // in metres means shrinking that fraction as the ring grows. A constant fraction would draw a
    // band that thickens as it expands, which reads as a spreading stain rather than a wave.
    const float thickness = std::clamp(SHOCK_RING_WIDTH_METRES / radius, 0.0f, 1.0f);
    const XMFLOAT3& centre = explosion.ShockRingCentre();
    XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) * XMMatrixTranslation(centre.x, DECAL_LIFT_Y, centre.z));
    _renderer.DrawDecal(_gpu, m_quadMesh, world,
                        Rgba{SHOCK_RING_COLOUR.r, SHOCK_RING_COLOUR.g, SHOCK_RING_COLOUR.b, explosion.ShockRingAlpha()}, thickness, 0.0f);
  }

  // --- thruster glow and trail ------------------------------------------------------------------
  // Billboards, built into the effect's vertex ring and drawn in one call. This used to be one draw
  // per sample: a three-nozzle hull running a full trail is 96 of them, and a hundred such ships is
  // over nine thousand draws a frame for the plume alone (Design/Archive/MmoScalabilityReview.md G1). What
  // each glow looks like has not changed -- the same disc, the same falloff, the same additive blend
  // -- only how many times the frame is asked to draw one.
  if (m_fx != nullptr && m_fx->RingReady() && !m_ships.empty())
  {
    const float trailLength = std::max(0.0f, THRUSTER_TRAIL_LENGTH);
    const float trailFade = std::max(0.01f, THRUSTER_TRAIL_FADE);

    m_glowSamples.clear();
    for (size_t i = 0; i < m_ships.size(); ++i)
    {
      const ShipView& view = m_ships[i];
      // A hull the frustum rejected has no plume and no running lights worth building. The trail
      // streams behind the ship rather than around it, which is why CULL_RADIUS_PAD_METRES is a
      // trail length: the sphere that decided this has to have covered the ribbon, not just the hull.
      if (!view.visible)
        continue;

      // Navigation lights burn whether or not the ship is under way, so they come before the thrust
      // test. Blink is free-running real time, so two ships of one hull blink together and only a
      // different authored phase separates them -- which is what an author controls with param1.
      if (!view.navLights.empty())
      {
        const DisplayPose pose = DisplayedPose(i);
        for (const NavLightView& light : view.navLights)
        {
          float blink = 1.0f; // a zero period is a steady light, and most of them are
          if (light.periodSec > 0.0f)
          {
            const float cycles = m_navTimeSec / light.periodSec + light.phase;
            const float phase01 = cycles - std::floor(cycles);
            blink = (phase01 < NAV_LIGHT_DUTY) ? 1.0f : NAV_LIGHT_OFF_LEVEL;
          }
          // The marker's alpha is an intensity (Design/Archive/NmoFormat.md 5.10): every shipped light has
          // 1, so nothing visible changes, and an author who dims one gets what they asked for.
          const float alpha = NAV_LIGHT_INTENSITY * blink * light.colour.a;
          if (alpha <= 0.002f)
            continue;
          const float radius = std::max(0.1f, light.radiusMetres * NAV_LIGHT_GLOW_SCALE) * SHIP_SCALE;
          m_glowSamples.push_back(Neuron::GlowSample{.posWorld = HullPointToWorld(view, pose, light.local),
                                                     .radiusMetres = radius,
                                                     .colour = Rgba{light.colour.r, light.colour.g, light.colour.b, alpha}});
        }
      }

      if (view.exhausts.empty() || view.thrusterIntensity <= 0.002f)
        continue;

      // Every exhaust gets its own glow and its own trail: a bomber flying with three nozzles lays
      // down three ribbons, and they fan apart through a turn because the outboard ones sweep wider.
      for (size_t nozzle = 0; nozzle < view.exhausts.size(); ++nozzle)
      {
        // Hoisted out of the step loop, which is what the hoist this replaced was protecting: one
        // lookup per ribbon rather than one per billboard. The colour is the marker's now, so a
        // friend and a foe flying the same hull burn the same plume -- faction stays readable
        // through the selection ring, the minimap and the contact count (Design/Archive/NmoFormat.md 9).
        const ExhaustView& exhaust = view.exhausts[nozzle];
        // A liveried plume is a shade under the same multiply the hull's flagged surfaces take, so
        // one authored plume burns azure, red or the player's own. An unflagged one draws as
        // authored -- and a nav light never multiplies at all (Design/Archive/NmoFormat.md 5.10).
        const Rgba plume = exhaust.raceTinted ? Rgba{exhaust.colour.r * view.lastLivery.r, exhaust.colour.g * view.lastLivery.g,
                                                     exhaust.colour.b * view.lastLivery.b, exhaust.colour.a}
                                              : exhaust.colour;
        const float glowRadius = std::max(0.1f, exhaust.radiusMetres * THRUSTER_GLOW_SCALE) * SHIP_SCALE;
        const XMFLOAT3* const samples = view.trail.data() + nozzle * TRAIL_SAMPLES;

        // Newest sample first, walking back along the path until trailLength runs out. The trail
        // follows the path the nozzle actually took, so it curves through a turn.
        float travelled = 0.0f;
        XMFLOAT3 previous = samples[view.trailHead];
        for (int step = 0; step < view.trailCount; ++step)
        {
          const int index = ((view.trailHead - step) % TRAIL_SAMPLES + TRAIL_SAMPLES) % TRAIL_SAMPLES;
          const XMFLOAT3 point = samples[index];
          if (step > 0)
          {
            travelled += Distance2D(previous.x, previous.z, point.x, point.z);
            if (travelled >= trailLength)
              break;
          }
          previous = point;

          const float along = (trailLength > 0.0f) ? travelled / trailLength : 1.0f;
          const float taper = std::pow(std::max(0.0f, 1.0f - along), trailFade);
          const float radius = glowRadius * (step == 0 ? 1.0f : taper * 0.8f);
          const float alpha = view.thrusterIntensity * (step == 0 ? 1.0f : taper * 0.55f) * plume.a;
          if (alpha <= 0.002f || radius <= 0.001f)
            continue;

          m_glowSamples.push_back(
            Neuron::GlowSample{.posWorld = point, .radiusMetres = radius, .colour = Rgba{plume.r, plume.g, plume.b, alpha}});

          if (trailLength <= 0.0f)
            break;
        }
      }
    }

    // --- gunfire ------------------------------------------------------------------------------
    // A shot is three glows on the billboards the running lights already ride: a muzzle at the
    // shooter, beads down the line so it reads as a direction rather than a dot, and an impact at
    // the far end. Nothing new enters the renderer's contract, which is what let this land without
    // the slice that turns the turrets (Design/Combat-slice-4.md 1, 2.2).
    for (const GunShot& shot : m_shots)
    {
      // Squared, so a tracer is bright for the first instant and gone rather than dimming evenly.
      // A shot that lingers at half brightness reads as a beam, which is a different weapon.
      const float life = std::clamp(1.0f - shot.ageSec / GUN_TRACER_SEC, 0.0f, 1.0f);
      const float fade = life * life;
      if (fade <= 0.002f)
        continue;

      const Rgba colour{shot.colour.r, shot.colour.g, shot.colour.b, fade};
      m_glowSamples.push_back(
        Neuron::GlowSample{.posWorld = shot.fromWorld, .radiusMetres = GUN_MUZZLE_RADIUS_METRES * fade * SHIP_SCALE, .colour = colour});
      m_glowSamples.push_back(
        Neuron::GlowSample{.posWorld = shot.toWorld, .radiusMetres = GUN_IMPACT_RADIUS_METRES * fade * SHIP_SCALE, .colour = colour});

      for (int bead = 1; bead < GUN_TRACER_BEADS; ++bead)
      {
        const float along = static_cast<float>(bead) / static_cast<float>(GUN_TRACER_BEADS);
        const XMFLOAT3 at(shot.fromWorld.x + (shot.toWorld.x - shot.fromWorld.x) * along,
                          shot.fromWorld.y + (shot.toWorld.y - shot.fromWorld.y) * along,
                          shot.fromWorld.z + (shot.toWorld.z - shot.fromWorld.z) * along);
        m_glowSamples.push_back(
          Neuron::GlowSample{.posWorld = at, .radiusMetres = GUN_TRACER_RADIUS_METRES * fade * SHIP_SCALE, .colour = colour});
      }
    }

    // --- the jump wink ----------------------------------------------------------------------
    // A crossing blooms where the hull was: a white core inside an azure halo that swells to
    // JUMP_FLASH_EXPAND times its birth radius as it dies, and a streak along the hull's last
    // heading that collapses into the bloom, so the wink reads as something LEAVING rather than a
    // flash bulb. Same billboards, same clock, same pass as the gunfire above.
    for (const JumpFlash& flash : m_jumpFlashes)
    {
      const float k = std::clamp(flash.ageSec / JUMP_FLASH_SEC, 0.0f, 1.0f);
      const float fade = (1.0f - k) * (1.0f - k);
      if (fade <= 0.002f)
        continue;
      const float radius = JUMP_FLASH_RADIUS_METRES * (1.0f + (JUMP_FLASH_EXPAND - 1.0f) * k) * SHIP_SCALE;
      const Rgba halo{JUMP_FLASH_COLOUR.r, JUMP_FLASH_COLOUR.g, JUMP_FLASH_COLOUR.b, fade};
      m_glowSamples.push_back(Neuron::GlowSample{.posWorld = flash.posWorld, .radiusMetres = radius, .colour = halo});
      m_glowSamples.push_back(
        Neuron::GlowSample{.posWorld = flash.posWorld, .radiusMetres = radius * 0.45f, .colour = Rgba{1.0f, 1.0f, 1.0f, fade}});

      const float alongX = std::sin(flash.headingRad) * JUMP_FLASH_STREAK_METRES * SHIP_SCALE * (1.0f - k);
      const float alongZ = std::cos(flash.headingRad) * JUMP_FLASH_STREAK_METRES * SHIP_SCALE * (1.0f - k);
      m_glowSamples.push_back(
        Neuron::GlowSample{.posWorld = XMFLOAT3(flash.posWorld.x + alongX, flash.posWorld.y, flash.posWorld.z + alongZ),
                           .radiusMetres = radius * 0.4f,
                           .colour = Rgba{halo.r, halo.g, halo.b, fade * 0.7f}});
      m_glowSamples.push_back(
        Neuron::GlowSample{.posWorld = XMFLOAT3(flash.posWorld.x - alongX, flash.posWorld.y, flash.posWorld.z - alongZ),
                           .radiusMetres = radius * 0.4f,
                           .colour = Rgba{halo.r, halo.g, halo.b, fade * 0.7f}});
    }

    if (!m_glowSamples.empty())
    {
      m_fxGlowVerts.clear();
      Neuron::BuildGlowBillboards(m_glowSamples, m_camera->Right(), m_camera->Up(), m_fxGlowVerts);
      m_fx->Begin(_gpu, m_camera->ViewProj(), _frame.lightDir, _frame.ambient, m_camera->Eye());
      m_fx->DrawGlows(_gpu, m_fxGlowVerts, THRUSTER_GLOW_FALLOFF);
    }
  }
}
} // namespace Outpost
