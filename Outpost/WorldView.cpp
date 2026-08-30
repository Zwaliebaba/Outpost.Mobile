#include "pch.h"
#include "WorldView.h"

#include "ViewTuning.h"

using namespace DirectX;
using namespace Neuron;

namespace Outpost
{
void WorldView::Init(Neuron::Transport& _transport, Camera& _camera, const MeshLibrary& _meshes, MeshHandle _quadMesh)
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

void WorldView::AddBody(const BodyView& _body)
{
  m_bodies.push_back(_body);
  // A rate is what a caller has; an orientation is what this accumulates. Forcing identity here is
  // what makes that split true rather than a convention nobody checks.
  XMStoreFloat3x3(&m_bodies.back().tumble, XMMatrixIdentity());
  m_bodyWorlds.reserve(m_bodies.size());
  m_bodyTriangles += _body.triangleCount;
}

void WorldView::ClearBodies() noexcept
{
  m_bodies.clear();
  m_bodyWorlds.clear();
  m_bodyTriangles = 0;
}

void WorldView::RegisterHullMesh(Game::HullId _hull, MeshHandle _mesh)
{
  const std::size_t row = static_cast<std::size_t>(_hull);
  if (row < m_hullMeshes.size())
    m_hullMeshes[row] = _mesh;
}

std::span<const Game::ShipSnapshot> WorldView::Ships() const noexcept
{
  return m_receiver.HasSnapshot() ? std::span<const Game::ShipSnapshot>(m_receiver.Latest().ships) : std::span<const Game::ShipSnapshot>();
}

void WorldView::PumpNetwork()
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
void WorldView::ApplySnapshot()
{
  const std::vector<Game::ShipSnapshot>& ships = m_receiver.Latest().ships;
  const std::uint64_t tick = m_receiver.Latest().tick;

  m_carryScratch.clear();
  m_carryHandles.clear();
  m_carryScratch.swap(m_ships);
  m_carryHandles.swap(m_handles);

  m_ships.reserve(ships.size());
  m_handles.reserve(ships.size());
  for (const Game::ShipSnapshot& ship : ships)
  {
    // Linear: the carried set is the previous snapshot's ships, and at these counts a map would
    // cost more than it saved. It is also the one lookup in this class, so if it ever matters it
    // is one place to change.
    std::size_t found = m_carryHandles.size();
    for (std::size_t at = 0; at < m_carryHandles.size(); ++at)
    {
      if (m_carryHandles[at] == ship.handle)
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
      const bool changed = view.to.pos.sectorX != ship.posWorld.sectorX || view.to.pos.sectorZ != ship.posWorld.sectorZ ||
                           view.to.pos.localX != ship.posWorld.localX || view.to.pos.localZ != ship.posWorld.localZ ||
                           view.to.headingRad != ship.headingRad;
      if (changed)
      {
        view.from = view.to;
        view.to = SampleOf(ship, tick);
      }
      view.faction = ship.factionId;
      m_ships.push_back(std::move(view));
      // Struck off so the leftovers can be walked below. Generation 0 is never issued, so a null
      // handle can never match a live ship on a later pass either.
      m_carryHandles[found] = Game::ShipHandle{};
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
      m_ships.push_back(std::move(view));
    }
    m_handles.push_back(ship.handle);
  }

  ExplodeTheLost(tick);
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
void WorldView::ExplodeTheLost(std::uint64_t _tick)
{
  // Docked first, and through a loop of its own rather than a branch in the one below: a docked
  // hull is removed with no ceremony -- no explosion, no shake, no SHIP LOST -- and the two lists
  // arrive in the same message, so a consumer that treated the spans alike would look like a bug in
  // the explosion rather than in the drain (Design/Stations.md 7.4, Stations-slice-plan.md 9). Only
  // the player's own are counted for the line: a protector coming home is the station's business.
  const std::span<const Game::ShipHandle> docked = m_receiver.Docked();
  int ownDocked = 0;
  for (std::size_t at = 0; at < m_carryHandles.size() && at < m_carryScratch.size(); ++at)
  {
    const Game::ShipHandle handle = m_carryHandles[at];
    if (handle.generation == 0)
      continue;
    bool inside = false;
    for (const Game::ShipHandle gone : docked)
      inside = inside || gone == handle;
    if (inside && m_carryScratch[at].faction == m_ownFaction)
      ++ownDocked;
  }
  if (ownDocked > 0 && m_log != nullptr)
    m_log->PushFormat(EventLog::Severity::Friendly, SimTimeSec(), "DOCKED | %d SHIPS", ownDocked);
  m_receiver.ClearDocked();

  const std::span<const Game::ShipHandle> destroyed = m_receiver.Destroyed();
  for (std::size_t at = 0; at < m_carryHandles.size() && at < m_carryScratch.size(); ++at)
  {
    const Game::ShipHandle handle = m_carryHandles[at];
    if (handle.generation == 0)
      continue; // carried onto the new snapshot

    bool died = false;
    for (const Game::ShipHandle dead : destroyed)
      died = died || dead == handle;
    if (!died)
      continue; // it left this client's view, which is not a death and never was

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
    spawn.seed = ((static_cast<std::uint64_t>(handle.slot) << 32) | handle.generation) ^ (_tick * 0x9E3779B97F4A7C15ull);
    // Whatever gives a station a lifecycle sets this from the station itself. Until something can
    // kill one, every death carries a ring, because otherwise nothing would ever draw one
    // (ViewTuning.h).
    spawn.shockRing = SHOCK_RING_ON_EVERY_DEATH;

    m_explosions.emplace_back().Start(spawn, m_particles);
    TriggerCameraShake();
    if (m_log != nullptr)
      m_log->Push(EventLog::Severity::Alert, SimTimeSec(), "SHIP LOST");
  }

  // Drawn, so the receiver may forget them. Deaths accumulate across every message in a drain now
  // that departures have their own lane, and nothing else clears them (ADR 0029).
  m_receiver.ClearDestroyed();
}

WorldView::MotionSample WorldView::SampleOf(const Game::ShipSnapshot& _ship, std::uint64_t _tick) noexcept
{
  MotionSample sample;
  sample.tick = static_cast<float>(_tick);
  sample.pos = _ship.posWorld;
  sample.headingRad = _ship.headingRad;
  // prevPos is the tick before, so the offset between the two is exactly one tick of travel.
  sample.velX = Game::OffsetX(_ship.prevPos, _ship.posWorld);
  sample.velZ = Game::OffsetZ(_ship.prevPos, _ship.posWorld);
  sample.turnRadPerTick = XMScalarModAngle(_ship.headingRad - _ship.prevHeading);
  return sample;
}

void WorldView::SetDisplayTime(float _tickTime) noexcept
{
  m_displayTick = _tickTime - INTERP_DELAY_TICKS;
}

WorldView::DisplayPose WorldView::DisplayedPose(std::size_t _index) const noexcept
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

void WorldView::ClearSelection() noexcept
{
  for (ShipView& ship : m_ships)
    ship.selected = false;
  m_activeGroup = -1;
}

float WorldView::SimTimeSec() const noexcept
{
  return static_cast<float>(m_receiver.Latest().tick) / Game::TICK_HZ;
}

void WorldView::AssignGroup(int _group)
{
  if (_group < 0 || _group >= CONTROL_GROUPS)
    return;

  std::vector<Game::ShipHandle>& group = m_groups[_group];
  group.clear();
  for (size_t i = 0; i < m_ships.size() && i < m_handles.size(); ++i)
  {
    if (m_ships[i].selected)
      group.push_back(m_handles[i]);
  }

  if (m_log)
  {
    if (group.empty())
      m_log->PushFormat(EventLog::Severity::Info, SimTimeSec(), "GROUP %d CLEARED", _group + 1);
    else
      m_log->PushFormat(EventLog::Severity::Friendly, SimTimeSec(), "GROUP %d | %d SHIPS ASSIGNED", _group + 1,
                        static_cast<int>(group.size()));
  }
  m_activeGroup = group.empty() ? -1 : _group;
}

void WorldView::SelectGroup(int _group)
{
  if (_group < 0 || _group >= CONTROL_GROUPS)
    return;

  ClearSelection();
  int selected = 0;
  for (const Game::ShipHandle handle : m_groups[_group])
  {
    const int at = RecallableIndex(handle);
    if (at < 0)
      continue; // out of view or no longer this client's; kept in the group either way
    m_ships[static_cast<std::size_t>(at)].selected = true;
    ++selected;
  }
  // A group whose members are all gone leaves nothing active to be shown as the source of a
  // selection that did not happen.
  m_activeGroup = (selected > 0) ? _group : -1;
}

int WorldView::GroupSize(int _group) const noexcept
{
  if (_group < 0 || _group >= CONTROL_GROUPS)
    return 0;

  int live = 0;
  for (const Game::ShipHandle handle : m_groups[_group])
    live += (RecallableIndex(handle) >= 0) ? 1 : 0;
  return live;
}

void WorldView::TriggerCameraShake() noexcept
{
  m_camera->Shake();
}

int WorldView::SelectedCount() const noexcept
{
  int count = 0;
  for (const ShipView& ship : m_ships)
    count += ship.selected ? 1 : 0;
  return count;
}

XMMATRIX WorldView::HullMatrix(const ShipView& _view, const DisplayPose& _pose) const noexcept
{
  // Roll about the hull's own mid-height axis, not its base, or a banked ship pivots on one
  // wingtip. SHIP_HOVER_HEIGHT is what keeps the low wing out of the ground while it does.
  const float rollAxisY = _view.pickCentre.y;
  return XMMatrixTranslation(0.0f, -rollAxisY, 0.0f) * XMMatrixRotationZ(_view.bankRad) *
         XMMatrixTranslation(0.0f, rollAxisY + _view.restY, 0.0f) * XMMatrixScaling(SHIP_SCALE, SHIP_SCALE, SHIP_SCALE) *
         XMMatrixRotationY(_pose.headingRad) * XMMatrixTranslation(ViewX(_pose.pos), SHIP_HOVER_HEIGHT, ViewZ(_pose.pos));
}

XMFLOAT3 WorldView::HullPointToWorld(const ShipView& _view, const DisplayPose& _pose, const XMFLOAT3& _local) const noexcept
{
  XMFLOAT3 world;
  XMStoreFloat3(&world, XMVector3Transform(XMLoadFloat3(&_local), HullMatrix(_view, _pose)));
  return world;
}

// One sample per nozzle per tick, so trail length means the same thing whatever the frame rate.
void WorldView::SampleTrails()
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

void WorldView::UpdateFeedback(float _dtSec)
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
  if (m_navTimeSec > NAV_LIGHT_MAX_PERIOD_SEC)
    m_navTimeSec -= NAV_LIGHT_MAX_PERIOD_SEC;

  // Bodies turn on real time, like every other feedback here, so the debug keys that slow the
  // simulation do not slow a planet: what 1/2/3 change is how fast the world is simulated, and a
  // planet is not in the world.
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

Rgba WorldView::LiveryOf(Game::FactionId _faction, bool _own, bool _hostileToMe) noexcept
{
  // The hostile row outranks the faction rows (Design/Stations.md 9.3): a Vanguard ship whose
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

bool WorldView::IsOwn(std::size_t _index) const noexcept
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  return _index < state.size() && state[_index].factionId == m_ownFaction;
}

// The faction check here is not redundant, even though a handle names one ship for the whole of its
// life and only the client's own ships are ever assigned to a group. Ownership finer than faction
// arrives with the second subscriber (ADR 0014), and then a ship that was the player's when the
// group was assigned need not still be. Every path into the selection asks the same question at the
// moment it selects, rather than trusting what was true when something was remembered.
int WorldView::RecallableIndex(Game::ShipHandle _handle) const noexcept
{
  for (std::size_t at = 0; at < m_handles.size(); ++at)
  {
    if (m_handles[at] == _handle)
      return IsOwn(at) ? static_cast<int>(at) : -1;
  }
  return -1;
}

// Ray against a hull's oriented bounding box. A sphere would be far too loose on a hull three
// times longer than it is wide.
float WorldView::RayHitDistance(std::size_t _index, const XMFLOAT3& _origin, const XMFLOAT3& _direction) const noexcept
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
  const float extent[3] = {view.halfExtents.x * INPUT_PICK_PADDING, view.halfExtents.y * INPUT_PICK_PADDING,
                           view.halfExtents.z * INPUT_PICK_PADDING};

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
int WorldView::PickShip(float _xPx, float _yPx) const
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
    const float t = RayHitDistance(i, origin, direction);
    if (t >= 0.0f && t < bestT)
    {
      bestT = t;
      best = static_cast<int>(i);
    }
  }
  return best;
}

int WorldView::PickStation(float _xPx, float _yPx) const
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
    const float t = RayHitDistance(i, origin, direction);
    if (t >= 0.0f && t < bestT)
    {
      bestT = t;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void WorldView::IssueMoveOrder(const XMFLOAT3& _point, bool _hasFacing, float _facingRad)
{
  if (m_transport == nullptr)
    return;

  // Handles, not indices. Between this click and the order reaching the other half a ship can die,
  // and swap-and-pop would move a stranger into the index it left behind (ADR 0005).
  const std::span<const Game::ShipSnapshot> state = Ships();
  Game::MoveOrder order;
  order.ships.reserve(m_ships.size());
  m_orderPositions.clear();
  float firstHeading = 0.0f;
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    if (!m_ships[i].selected)
      continue;
    if (order.ships.empty())
      firstHeading = state[i].headingRad;
    order.ships.push_back(state[i].handle);
    m_orderPositions.push_back(state[i].posWorld);
  }
  if (order.ships.empty())
    return;

  // One order is one datagram, so a selection larger than a datagram holds cannot be ordered as a
  // unit. Say so rather than doing nothing: a click that vanishes reads as a broken game.
  if (order.ships.size() > Game::MaxShipsPerOrder())
  {
    if (m_log)
      m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "ORDER TOO LARGE | %d OF %d MAX", static_cast<int>(order.ships.size()),
                        static_cast<int>(Game::MaxShipsPerOrder()));
    return;
  }

  order.destination = WorldPosAt(_point.x, _point.z);
  order.facingRad = _facingRad;
  order.hasFacing = _hasFacing;
  if (!Game::WriteMoveOrder(order, *m_transport))
    return; // the send queue is full, which Transport.h calls normal: the click is dropped

  // Nothing comes back down the wire to say which way the formation settled, so the marker is
  // oriented here -- by the same function the other half uses, on the same positions, the same
  // point and the same fallback. Not a prediction: the same arithmetic on the same inputs, so the
  // marker and the ships cannot disagree about which way an order points (slice-2b 2.5).
  const float heading = _hasFacing ? _facingRad : Game::FormationHeading(m_orderPositions, order.destination, firstHeading);

  OrderMarker marker;
  marker.posWorld = XMFLOAT3(_point.x, 0.0f, _point.z);
  marker.facingRad = heading;
  marker.hasFacing = _hasFacing;
  marker.colour = MARKER_COLOUR;
  m_markers.push_back(marker);

  if (m_log)
    m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "MOVE ORDER | %d SHIPS", static_cast<int>(order.ships.size()));
}

void WorldView::IssueDockOrder(std::size_t _station)
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  if (m_transport == nullptr || _station >= state.size())
    return;
  const Game::FactionId owner = state[_station].factionId;

  // The affordance tells the truth first: an owner that holds this client hostile refuses here,
  // and nothing is sent. The simulation's gate still stands behind it, per the twice-on-purpose
  // rule -- affordances tell the truth, and clients are not trusted (Design/Stations.md 9.2).
  if (IsHostileToMe(owner))
  {
    if (m_log)
    {
      const char* name = (owner < m_factionNames.size()) ? m_factionNames[owner] : "UNKNOWN";
      m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "DOCKING REFUSED | %s HOSTILE", name);
    }
    return;
  }

  Game::DockOrder order;
  order.ships.reserve(m_ships.size());
  for (std::size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    if (m_ships[i].selected)
      order.ships.push_back(state[i].handle);
  }
  if (order.ships.empty())
    return;
  if (order.ships.size() > Game::MaxShipsPerOrder())
  {
    if (m_log)
      m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "ORDER TOO LARGE | %d OF %d MAX", static_cast<int>(order.ships.size()),
                        static_cast<int>(Game::MaxShipsPerOrder()));
    return;
  }
  order.station = state[_station].handle;
  if (!Game::WriteDockOrder(order, *m_transport))
    return; // the send queue is full, which Transport.h calls normal: the tap is dropped

  // The marker the tap earns, on the station and in its colour, so the tap visibly landed on the
  // thing and not the ground beside it. No facing: a dock order has none.
  const DisplayPose pose = DisplayedPose(_station);
  OrderMarker marker;
  marker.posWorld = XMFLOAT3(ViewX(pose.pos), 0.0f, ViewZ(pose.pos));
  marker.colour = LiveryOf(owner, owner == m_ownFaction, false);
  m_markers.push_back(marker);

  if (m_log)
    m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "DOCKING | %d SHIPS", static_cast<int>(order.ships.size()));
}

// --- pointer intent -----------------------------------------------------------------------------
// With nothing selected a drag bands a box; with a selection it lays down a move order and its
// final facing. Shift forces the box either way.
bool WorldView::WantsBoxSelect(bool _shiftHeld)
{
  return _shiftHeld || SelectedCount() == 0;
}

void WorldView::OnHover(float _xPx, float _yPx)
{
  m_hoverShip = PickShip(_xPx, _yPx);
}

void WorldView::OnDragUpdate(bool _boxSelect, float _x0Px, float _y0Px, float _x1Px, float _y1Px)
{
  m_boxActive = _boxSelect;
  m_orderDragActive = !_boxSelect;
  m_boxX0Px = m_orderX0Px = _x0Px;
  m_boxY0Px = m_orderY0Px = _y0Px;
  m_boxX1Px = m_orderX1Px = _x1Px;
  m_boxY1Px = m_orderY1Px = _y1Px;
}

void WorldView::OnDragCancelled()
{
  m_boxActive = false;
  m_orderDragActive = false;
}

void WorldView::OnBoxSelect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, bool _additive)
{
  const float left = std::min(_x0Px, _x1Px);
  const float right = std::max(_x0Px, _x1Px);
  const float top = std::min(_y0Px, _y1Px);
  const float bottom = std::max(_y0Px, _y1Px);
  if (!_additive)
    ClearSelection();

  const std::span<const Game::ShipSnapshot> state = Ships();
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    if (!IsOwn(i))
      continue; // a box drawn over the enemy's base selects nothing, the same as a tap on it

    const DisplayPose pose = DisplayedPose(i);
    const XMFLOAT3 centre(ViewX(pose.pos), m_ships[i].halfExtents.y * SHIP_SCALE, ViewZ(pose.pos));
    float xPx = 0.0f;
    float yPx = 0.0f;
    if (m_camera->WorldToScreen(centre, xPx, yPx) && xPx >= left && xPx <= right && yPx >= top && yPx <= bottom)
      m_ships[i].selected = true;
  }
  m_activeGroup = -1;
}

void WorldView::OnOrderDrag(float _x0Px, float _y0Px, float _x1Px, float _y1Px)
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

void WorldView::OnTap(float _xPx, float _yPx, bool _shiftHeld, bool _doubleTap)
{
  const int hit = PickShip(_xPx, _yPx);
  if (hit >= 0)
  {
    if (_shiftHeld)
      m_ships[static_cast<size_t>(hit)].selected = !m_ships[static_cast<size_t>(hit)].selected;
    else
    {
      ClearSelection();
      m_ships[static_cast<size_t>(hit)].selected = true;
    }
    m_activeGroup = -1;
    if (m_tracker)
      m_tracker->ResetTapHistory(); // tapping a hull does not begin a double tap
    return;
  }

  // A station under the tap, with something selected, is a dock order. With nothing selected it is
  // nothing at all: selection-for-inspection is the management menu's, which is the next phase
  // (Design/Stations.md 9.1).
  if (SelectedCount() > 0)
  {
    const int station = PickStation(_xPx, _yPx);
    if (station >= 0)
    {
      IssueDockOrder(static_cast<std::size_t>(station));
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
  if (SelectedCount() == 0)
    return;
  XMFLOAT3 point;
  if (m_camera->RayToGround(_xPx, _yPx, point))
    IssueMoveOrder(point, false, 0.0f);
}

// ------------------------------------------------------------------------------------------------
// Rendering. Every position and heading is read at the display time set by SetDisplayTime, between
// the two samples that bracket it, so motion is smooth however far the swapchain runs ahead of the
// simulation rate and however far the update rate sits below it.

void WorldView::Render(SceneRenderer& _renderer, GpuDevice& _gpu, TextRenderer& _text)
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
  // (Design/MmoScalabilityReview.md G2).
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

  // Visibility is decided once per body, here, and the terrain and outline passes below reuse it:
  // two passes over the same spheres would be two chances for them to disagree, and a body whose
  // outline drew but whose land did not is a wireframe hanging in empty space.
  m_bodyVisible.clear();
  for (std::size_t i = 0; i < m_bodies.size(); ++i)
  {
    const XMFLOAT3 centre(ViewX(m_bodies[i].centre), m_bodies[i].centreY, ViewZ(m_bodies[i].centre));
    const float radius = m_bodies[i].boundingRadiusMetres * CULL_BODY_RADIUS_SCALE;
    const bool visible = Neuron::IsSphereVisible(frustum, centre, radius);
    m_bodyVisible.push_back(visible);
    if (visible)
      ++m_submittedCount;
    else
      ++m_culledCount;
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
    // with the Vandals' (Design/Stations.md 4.3, 9.3).
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
  // tests depth -- so grouping by mesh costs nothing and buys everything.
  for (const MeshBucket& bucket : m_meshBuckets)
    _renderer.DrawMeshInstanced(_gpu, bucket.mesh, bucket.instances);

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
        m_bodyRenderer->DrawPlanet(_gpu, m_bodies[i].terrain, m_bodyWorlds[i]);
      else
        m_bodyRenderer->DrawMain(_gpu, m_bodies[i].terrain, m_bodyWorlds[i]);
    }
    // The outline belongs to a generated body. Over a textured one it reads as a cage drawn on a
    // photograph, so a textured world is skipped here rather than being given a fainter one.
    for (std::size_t i = 0; i < m_bodies.size(); ++i)
    {
      if (m_bodyVisible[i] && !m_bodies[i].textured)
        m_bodyRenderer->DrawOverlay(_gpu, m_bodies[i].terrain, m_bodyWorlds[i]);
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
  if (m_orderDragActive)
    _text.DrawScreenLine(m_orderX0Px, m_orderY0Px, m_orderX1Px, m_orderY1Px, 2.0f, MARKER_COLOUR);
}

// ------------------------------------------------------------------------------------------------
// The overlay pass: selection and hover rings on the ground, order markers, thruster glow and
// trail in the air. All of it is the same unit quad shaped by the decal shader.

std::vector<Neuron::MeshInstance>& WorldView::Bucket(Neuron::MeshHandle _mesh)
{
  for (MeshBucket& bucket : m_meshBuckets)
  {
    if (bucket.mesh == _mesh)
      return bucket.instances;
  }
  m_meshBuckets.push_back(MeshBucket{.mesh = _mesh, .instances = {}});
  return m_meshBuckets.back().instances;
}

void WorldView::DrawFeedback(SceneRenderer& _renderer, GpuDevice& _gpu, const SceneFrame& _frame)
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
                              XMMatrixTranslation(marker.posWorld.x, DECAL_LIFT_Y, marker.posWorld.z));
    _renderer.DrawDecal(_gpu, m_quadMesh, world, Rgba{marker.colour.r, marker.colour.g, marker.colour.b, alpha}, MARKER_THICKNESS, 0.10f);

    // Each pulse also throws a ripple outwards, which is what makes the count readable.
    if (beat > 0.0f)
    {
      const float withinPulse = pulseIndex - std::floor(pulseIndex);
      const float rippleRadius = radius * (1.0f + withinPulse * 1.1f);
      XMStoreFloat4x4(&world, XMMatrixScaling(rippleRadius * 2.0f, 1.0f, rippleRadius * 2.0f) *
                                XMMatrixTranslation(marker.posWorld.x, DECAL_LIFT_Y, marker.posWorld.z));
      _renderer.DrawDecal(_gpu, m_quadMesh, world,
                          Rgba{marker.colour.r, marker.colour.g, marker.colour.b, alpha * (1.0f - withinPulse) * 0.7f},
                          MARKER_THICKNESS * 0.6f, 0.0f);
    }

    // A pip out along the ordered facing, so a drag order shows which way the ships will end up.
    if (marker.hasFacing)
    {
      const float pip = radius * 0.28f;
      const float outX = marker.posWorld.x + std::sin(marker.facingRad) * radius * 1.5f;
      const float outZ = marker.posWorld.z + std::cos(marker.facingRad) * radius * 1.5f;
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
  // over nine thousand draws a frame for the plume alone (Design/MmoScalabilityReview.md G1). What
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
