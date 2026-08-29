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
      view.to = SampleOf(ship, tick);
      view.from = view.to; // one sample: it is drawn there until the next one gives it somewhere to go
      if (mesh != INVALID_MESH)
      {
        const MeshData& data = m_meshes->Data(mesh);
        view.restY = data.RestY();
        view.pickCentre = data.BoundsCentre();
        view.halfExtents = data.HalfExtents();
        view.thrusterLocals = data.attachPoints;
        view.trail.assign(view.thrusterLocals.size() * TRAIL_SAMPLES, XMFLOAT3(0.0f, 0.0f, 0.0f));
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
// removes the view silently and only a death detonates (Design/Hostiles.md 4.4).
//
// Destroyed() describes the last update the receiver applied, and PumpNetwork applies at most one
// per tick because the composition root pumps on every tick -- which is what makes reading it here,
// once per ApplySnapshot, the whole of what the server said since the last one.
void WorldView::ExplodeTheLost(std::uint64_t _tick)
{
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

  std::vector<Game::ShipId>& group = m_groups[_group];
  group.clear();
  for (size_t i = 0; i < m_ships.size(); ++i)
  {
    if (m_ships[i].selected)
      group.push_back(static_cast<Game::ShipId>(i));
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
  for (const Game::ShipId id : m_groups[_group])
  {
    // A group holds indices into the snapshot it was assigned from, and a later snapshot can put a
    // different ship at that index -- so the faction is checked here too rather than trusted from
    // assignment time. Without it a recalled group is the one way a hostile could end up selected.
    if (id < m_ships.size() && IsOwn(id))
      m_ships[id].selected = true;
  }
  m_activeGroup = m_groups[_group].empty() ? -1 : _group;
}

int WorldView::GroupSize(int _group) const noexcept
{
  if (_group < 0 || _group >= CONTROL_GROUPS)
    return 0;
  return static_cast<int>(m_groups[_group].size());
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

// One sample per nozzle per tick, so trail length means the same thing whatever the frame rate.
void WorldView::SampleTrails()
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  const size_t count = std::min(m_ships.size(), state.size());

  for (size_t i = 0; i < count; ++i)
  {
    ShipView& view = m_ships[i];
    if (view.thrusterLocals.empty())
      continue;
    // Sampled where the ship is drawn, not where the latest record puts it, or the trail would step
    // once an update while the hull glides.
    const DisplayPose pose = DisplayedPose(i);

    const float cosH = std::cos(pose.headingRad);
    const float sinH = std::sin(pose.headingRad);
    view.trailHead = (view.trailHead + 1) % TRAIL_SAMPLES;
    for (size_t nozzle = 0; nozzle < view.thrusterLocals.size(); ++nozzle)
    {
      const XMFLOAT3& local = view.thrusterLocals[nozzle];
      view.trail[nozzle * TRAIL_SAMPLES + static_cast<size_t>(view.trailHead)] =
        XMFLOAT3(ViewX(pose.pos) + (local.x * cosH + local.z * sinH) * SHIP_SCALE, SHIP_HOVER_HEIGHT + (view.restY + local.y) * SHIP_SCALE,
                 ViewZ(pose.pos) + (-local.x * sinH + local.z * cosH) * SHIP_SCALE);
    }
    view.trailCount = std::min(view.trailCount + 1, TRAIL_SAMPLES);
  }
}

void WorldView::UpdateFeedback(float _dtSec)
{
  const float dt = std::clamp(_dtSec, 0.0f, 0.1f);

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

bool WorldView::IsOwn(std::size_t _index) const noexcept
{
  const std::span<const Game::ShipSnapshot> state = Ships();
  return _index < state.size() && state[_index].factionId == m_ownFaction;
}

// Ray against each hull's oriented bounding box. A sphere would be far too loose on a hull three
// times longer than it is wide.
//
// Somebody else's ship is not pickable at all, so no hover highlight, selection ring, tap, shift-tap
// or double-tap can ever land on one. What a client cannot command it should not appear able to
// (Design/Hostiles.md 7).
int WorldView::PickShip(float _xPx, float _yPx) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  m_camera->ScreenRay(_xPx, _yPx, origin, direction);
  const std::span<const Game::ShipSnapshot> state = Ships();

  int best = -1;
  float bestT = 1e30f;
  for (int i = 0; i < static_cast<int>(m_ships.size()) && i < static_cast<int>(state.size()); ++i)
  {
    if (!IsOwn(static_cast<size_t>(i)))
      continue;

    const ShipView& view = m_ships[static_cast<size_t>(i)];
    // Against the hull as drawn: the latest record can be a hull-length ahead of it.
    const DisplayPose pose = DisplayedPose(static_cast<size_t>(i));
    const float cosH = std::cos(pose.headingRad);
    const float sinH = std::sin(pose.headingRad);

    const XMFLOAT3 centre(ViewX(pose.pos) + (view.pickCentre.x * cosH + view.pickCentre.z * sinH) * SHIP_SCALE,
                          (view.restY + view.pickCentre.y) * SHIP_SCALE,
                          ViewZ(pose.pos) + (-view.pickCentre.x * sinH + view.pickCentre.z * cosH) * SHIP_SCALE);

    // Into hull space: to the centre, undo the heading, undo the scale.
    const float rx = origin.x - centre.x;
    const float rz = origin.z - centre.z;
    const float localOrigin[3] = {(rx * cosH - rz * sinH) / SHIP_SCALE, (origin.y - centre.y) / SHIP_SCALE,
                                  (rx * sinH + rz * cosH) / SHIP_SCALE};
    const float localDir[3] = {(direction.x * cosH - direction.z * sinH) / SHIP_SCALE, direction.y / SHIP_SCALE,
                               (direction.x * sinH + direction.z * cosH) / SHIP_SCALE};
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
    if (hit && tMin < bestT)
    {
      bestT = tMin;
      best = i;
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
  m_markers.push_back(marker);

  if (m_log)
    m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "MOVE ORDER | %d SHIPS", static_cast<int>(order.ships.size()));
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
  SceneFrame frame = {};
  frame.viewProj = m_camera->ViewProj();
  frame.lightDir = XMFLOAT3(LIGHT_DIR_X, LIGHT_DIR_Y, LIGHT_DIR_Z);
  frame.ambient = AMBIENT_LEVEL;
  frame.gridColour = GRID_COLOUR;
  frame.gridSpacing = GRID_SPACING;
  frame.gridLineWidthPx = GRID_LINE_WIDTH_PX;
  frame.gridFadeDistance = GRID_FADE_DISTANCE;
  frame.cameraPos = m_camera->Eye();
  _renderer.BeginScene(_gpu, frame);

  // The ground follows the camera, so a plane a few kilometres across is enough however far the
  // view travels. The grid on it is procedural, so nothing has to be rebuilt when it moves.
  XMFLOAT4X4 world;
  const XMFLOAT3& target = m_camera->Target();
  XMStoreFloat4x4(&world, XMMatrixScaling(GROUND_SIZE, 1.0f, GROUND_SIZE) * XMMatrixTranslation(target.x, 0.0f, target.z));
  _renderer.DrawMesh(_gpu, m_quadMesh, world, GROUND_COLOUR, 0.0f, true);

  const std::span<const Game::ShipSnapshot> state = Ships();
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    ShipView& view = m_ships[i];

    const DisplayPose pose = DisplayedPose(i);
    const float x = ViewX(pose.pos);
    const float z = ViewZ(pose.pos);
    const float heading = pose.headingRad;

    // Roll about the hull's own mid-height axis, not its base, or a banked ship pivots on one
    // wingtip. SHIP_HOVER_HEIGHT is what keeps the low wing out of the ground while it does.
    const float rollAxisY = view.pickCentre.y;
    const XMMATRIX hull = XMMatrixTranslation(0.0f, -rollAxisY, 0.0f) * XMMatrixRotationZ(view.bankRad) *
                          XMMatrixTranslation(0.0f, rollAxisY + view.restY, 0.0f) * XMMatrixScaling(SHIP_SCALE, SHIP_SCALE, SHIP_SCALE) *
                          XMMatrixRotationY(heading) * XMMatrixTranslation(x, SHIP_HOVER_HEIGHT, z);
    XMStoreFloat4x4(&world, hull);

    Rgba tint = view.selected ? SELECTED_COLOUR : SHIP_COLOUR;
    const float lift = view.hoverAmount * SEL_HOVER_HIGHLIGHT_STRENGTH;
    tint = Rgba{tint.r + (1.0f - tint.r) * lift, tint.g + (1.0f - tint.g) * lift, tint.b + (1.0f - tint.b) * lift, 1.0f};
    _renderer.DrawMesh(_gpu, view.mesh, world, tint, SHIP_MATERIAL_MIX, false);

    // Remembered for the explosion, which needs where the hull was and how it was moving at a point
    // where the snapshot no longer has a record for it. Taken from the drawn pose rather than the
    // latest one, so the shards start exactly where the hull was and drift the way it was pointing.
    view.lastWorld = world;
    view.lastVelMetresPerSec = XMFLOAT3(std::sin(heading) * state[i].speed, 0.0f, std::cos(heading) * state[i].speed);
    view.drawn = true;
  }

  // The bodies: every terrain, then every outline. Two passes rather than two draws per body, so
  // there is one pipeline switch per pass and body A's outline tests against body B's depth
  // (Design/PlanetRenderer.md 7.3). They go after the hulls because the ocean, when slice 5 lands
  // it, goes through the scene pass and has to be in the depth buffer before the coast dips into it.
  if (m_bodyRenderer != nullptr && !m_bodies.empty())
  {
    // Built once and read twice: the world matrix of a body is four transforms and the second pass
    // wants exactly the matrix the first one used, not one recomputed from a spin that has not moved.
    m_bodyWorlds.clear();
    for (const BodyView& body : m_bodies)
    {
      const XMMATRIX orientation =
        XMMatrixMultiply(XMLoadFloat3x3(&body.tumble), XMMatrixRotationAxis(XMLoadFloat3(&body.spinAxis), body.spinRad));
      XMFLOAT4X4 bodyWorld;
      XMStoreFloat4x4(&bodyWorld, XMMatrixMultiply(orientation, XMMatrixTranslation(ViewX(body.centre), body.centreY, ViewZ(body.centre))));
      m_bodyWorlds.push_back(bodyWorld);
    }

    m_bodyRenderer->Begin(_gpu, frame.viewProj, frame.lightDir, frame.ambient, frame.cameraPos, BODY_OVERLAY);
    for (std::size_t i = 0; i < m_bodies.size(); ++i)
      m_bodyRenderer->DrawMain(_gpu, m_bodies[i].terrain, m_bodyWorlds[i]);
    for (std::size_t i = 0; i < m_bodies.size(); ++i)
      m_bodyRenderer->DrawOverlay(_gpu, m_bodies[i].terrain, m_bodyWorlds[i]);
  }

  // Hull fragments before the decals: they are blended but write depth, so a shard occludes what is
  // behind it, and the rings and thruster glow only test (Design/SpaceshipExplosion.md 8.3).
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

  DrawFeedback(_renderer, _gpu);

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

void WorldView::DrawFeedback(SceneRenderer& _renderer, GpuDevice& _gpu)
{
  _renderer.BeginDecals(_gpu, m_camera->ViewProj(), m_camera->Eye());

  const std::span<const Game::ShipSnapshot> state = Ships();
  XMFLOAT4X4 world;

  // --- selection and hover rings ----------------------------------------------------------------
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    const ShipView& view = m_ships[i];

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
    const float alpha = MARKER_COLOUR.a * fade * (0.72f + beat * 0.28f);
    XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) *
                              XMMatrixTranslation(marker.posWorld.x, DECAL_LIFT_Y, marker.posWorld.z));
    _renderer.DrawDecal(_gpu, m_quadMesh, world, Rgba{MARKER_COLOUR.r, MARKER_COLOUR.g, MARKER_COLOUR.b, alpha}, MARKER_THICKNESS, 0.10f);

    // Each pulse also throws a ripple outwards, which is what makes the count readable.
    if (beat > 0.0f)
    {
      const float withinPulse = pulseIndex - std::floor(pulseIndex);
      const float rippleRadius = radius * (1.0f + withinPulse * 1.1f);
      XMStoreFloat4x4(&world, XMMatrixScaling(rippleRadius * 2.0f, 1.0f, rippleRadius * 2.0f) *
                                XMMatrixTranslation(marker.posWorld.x, DECAL_LIFT_Y, marker.posWorld.z));
      _renderer.DrawDecal(_gpu, m_quadMesh, world,
                          Rgba{MARKER_COLOUR.r, MARKER_COLOUR.g, MARKER_COLOUR.b, alpha * (1.0f - withinPulse) * 0.7f},
                          MARKER_THICKNESS * 0.6f, 0.0f);
    }

    // A pip out along the ordered facing, so a drag order shows which way the ships will end up.
    if (marker.hasFacing)
    {
      const float pip = radius * 0.28f;
      const float outX = marker.posWorld.x + std::sin(marker.facingRad) * radius * 1.5f;
      const float outZ = marker.posWorld.z + std::cos(marker.facingRad) * radius * 1.5f;
      XMStoreFloat4x4(&world, XMMatrixScaling(pip * 2.0f, 1.0f, pip * 2.0f) * XMMatrixTranslation(outX, DECAL_LIFT_Y, outZ));
      _renderer.DrawDecal(_gpu, m_quadMesh, world, Rgba{MARKER_COLOUR.r, MARKER_COLOUR.g, MARKER_COLOUR.b, alpha}, 1.0f, 1.0f);
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
  // Billboards: the unit quad lies in XZ, so putting the camera right vector in row 0 and the
  // camera up vector in row 2 turns it to face the eye wherever it is.
  const float glowRadius = std::max(0.1f, THRUSTER_GLOW_RADIUS) * SHIP_SCALE;
  const float trailLength = std::max(0.0f, THRUSTER_TRAIL_LENGTH);
  const float trailFade = std::max(0.01f, THRUSTER_TRAIL_FADE);
  const XMFLOAT3& cameraRight = m_camera->Right();
  const XMFLOAT3& cameraUp = m_camera->Up();

  for (const ShipView& view : m_ships)
  {
    if (view.thrusterLocals.empty() || view.thrusterIntensity <= 0.002f)
      continue;

    // Every exhaust gets its own glow and its own trail: a bomber flying with three nozzles lays
    // down three ribbons, and they fan apart through a turn because the outboard ones sweep wider.
    for (size_t nozzle = 0; nozzle < view.thrusterLocals.size(); ++nozzle)
    {
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
        const float alpha = view.thrusterIntensity * (step == 0 ? 1.0f : taper * 0.55f);
        if (alpha <= 0.002f || radius <= 0.001f)
          continue;

        XMFLOAT4X4 billboard;
        billboard._11 = cameraRight.x * radius * 2.0f;
        billboard._12 = cameraRight.y * radius * 2.0f;
        billboard._13 = cameraRight.z * radius * 2.0f;
        billboard._14 = 0.0f;
        billboard._21 = 0.0f;
        billboard._22 = 1.0f;
        billboard._23 = 0.0f;
        billboard._24 = 0.0f;
        billboard._31 = cameraUp.x * radius * 2.0f;
        billboard._32 = cameraUp.y * radius * 2.0f;
        billboard._33 = cameraUp.z * radius * 2.0f;
        billboard._34 = 0.0f;
        billboard._41 = point.x;
        billboard._42 = point.y;
        billboard._43 = point.z;
        billboard._44 = 1.0f;
        _renderer.DrawGlow(_gpu, m_quadMesh, billboard, Rgba{SELECTED_COLOUR.r, SELECTED_COLOUR.g, SELECTED_COLOUR.b, alpha},
                           THRUSTER_GLOW_FALLOFF);

        if (trailLength <= 0.0f)
          break;
      }
    }
  }
}
} // namespace Outpost
