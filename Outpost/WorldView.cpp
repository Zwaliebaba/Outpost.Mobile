#include "pch.h"
#include "WorldView.h"

#include "ViewTuning.h"

using namespace DirectX;
using namespace Neuron;

namespace Outpost
{
void WorldView::Init(Game::World& _world, Camera& _camera, const MeshLibrary& _meshes, MeshHandle _quadMesh)
{
  m_world = &_world;
  m_camera = &_camera;
  m_meshes = &_meshes;
  m_quadMesh = _quadMesh;
}

void WorldView::AddShip(MeshHandle _mesh)
{
  const MeshData& data = m_meshes->Data(_mesh);

  ShipView view;
  view.mesh = _mesh;
  view.restY = data.RestY();
  view.pickCentre = data.BoundsCentre();
  view.halfExtents = data.HalfExtents();
  view.thrusterLocals = data.attachPoints;
  view.trail.assign(view.thrusterLocals.size() * TRAIL_SAMPLES, XMFLOAT3(0.0f, 0.0f, 0.0f));
  m_ships.push_back(std::move(view));
}

void WorldView::ClearSelection() noexcept
{
  for (ShipView& ship : m_ships)
    ship.selected = false;
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
  const std::span<const Game::ShipState> state = m_world->Ships();
  const size_t count = std::min(m_ships.size(), state.size());

  for (size_t i = 0; i < count; ++i)
  {
    ShipView& view = m_ships[i];
    if (view.thrusterLocals.empty())
      continue;
    const Game::ShipState& ship = state[i];

    const float cosH = std::cos(ship.headingRad);
    const float sinH = std::sin(ship.headingRad);
    view.trailHead = (view.trailHead + 1) % TRAIL_SAMPLES;
    for (size_t nozzle = 0; nozzle < view.thrusterLocals.size(); ++nozzle)
    {
      const XMFLOAT3& local = view.thrusterLocals[nozzle];
      view.trail[nozzle * TRAIL_SAMPLES + static_cast<size_t>(view.trailHead)] =
        XMFLOAT3(ship.posWorld.x + (local.x * cosH + local.z * sinH) * SHIP_SCALE,
                 ship.posWorld.y + SHIP_HOVER_HEIGHT + (view.restY + local.y) * SHIP_SCALE,
                 ship.posWorld.z + (-local.x * sinH + local.z * cosH) * SHIP_SCALE);
    }
    view.trailCount = std::min(view.trailCount + 1, TRAIL_SAMPLES);
  }
}

void WorldView::UpdateFeedback(float _dtSec)
{
  const float dt = std::clamp(_dtSec, 0.0f, 0.1f);
  const float maxTurnRate = XMConvertToRadians(Game::TURN_RATE_DEG_PER_SEC);
  const float maxBank = XMConvertToRadians(BANK_MAX_ANGLE_DEG);
  const std::span<const Game::ShipState> state = m_world->Ships();

  for (int i = 0; i < static_cast<int>(m_ships.size()) && i < static_cast<int>(state.size()); ++i)
  {
    ShipView& view = m_ships[static_cast<size_t>(i)];
    const Game::ShipState& ship = state[static_cast<size_t>(i)];

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
    const float bankTarget = -std::clamp(ship.turnRateRadPerSec / maxTurnRate, -1.0f, 1.0f) * maxBank;
    const bool goingIn = std::fabs(bankTarget) > std::fabs(view.bankRad);
    const float bankHalfLife = goingIn ? BANK_RESPONSE_HALF_LIFE : BANK_RETURN_HALF_LIFE;
    view.bankRad += (bankTarget - view.bankRad) * HalfLifeBlend(dt, bankHalfLife);

    // Thrusters follow acceleration, not speed: they flare on the way up to cruise and go quiet
    // once the ship is coasting.
    const float drive = std::clamp(ship.accelSample / Game::ACCELERATION, 0.0f, 1.0f);
    const float thrusterTarget = THRUSTER_IDLE_INTENSITY + (THRUSTER_MAX_INTENSITY - THRUSTER_IDLE_INTENSITY) * drive;
    view.thrusterIntensity += (thrusterTarget - view.thrusterIntensity) * HalfLifeBlend(dt, THRUSTER_RESPONSE_HALF_LIFE);
  }

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
    centreX += state[i].posWorld.x;
    centreZ += state[i].posWorld.z;
    leadX += std::sin(state[i].headingRad) * state[i].speed;
    leadZ += std::cos(state[i].headingRad) * state[i].speed;
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

// Ray against each hull's oriented bounding box. A sphere would be far too loose on a hull three
// times longer than it is wide.
int WorldView::PickShip(float _xPx, float _yPx) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  m_camera->ScreenRay(_xPx, _yPx, origin, direction);
  const std::span<const Game::ShipState> state = m_world->Ships();

  int best = -1;
  float bestT = 1e30f;
  for (int i = 0; i < static_cast<int>(m_ships.size()) && i < static_cast<int>(state.size()); ++i)
  {
    const ShipView& view = m_ships[static_cast<size_t>(i)];
    const Game::ShipState& ship = state[static_cast<size_t>(i)];
    const float cosH = std::cos(ship.headingRad);
    const float sinH = std::sin(ship.headingRad);

    const XMFLOAT3 centre(ship.posWorld.x + (view.pickCentre.x * cosH + view.pickCentre.z * sinH) * SHIP_SCALE,
                          ship.posWorld.y + (view.restY + view.pickCentre.y) * SHIP_SCALE,
                          ship.posWorld.z + (-view.pickCentre.x * sinH + view.pickCentre.z * cosH) * SHIP_SCALE);

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
  std::vector<Game::ShipId> chosen;
  chosen.reserve(m_ships.size());
  for (size_t i = 0; i < m_ships.size(); ++i)
  {
    if (m_ships[i].selected)
      chosen.push_back(static_cast<Game::ShipId>(i));
  }
  if (chosen.empty())
    return;

  // The world solves the formation and reports the heading it settled on, so the marker and the
  // ships cannot disagree about which way the order points.
  const float heading = m_world->IssueMoveOrder(chosen, _point, _hasFacing, _facingRad);

  OrderMarker marker;
  marker.posWorld = XMFLOAT3(_point.x, 0.0f, _point.z);
  marker.facingRad = heading;
  marker.hasFacing = _hasFacing;
  m_markers.push_back(marker);
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

  const std::span<const Game::ShipState> state = m_world->Ships();
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    const XMFLOAT3 centre(state[i].posWorld.x, state[i].posWorld.y + m_ships[i].halfExtents.y * SHIP_SCALE, state[i].posWorld.z);
    float xPx = 0.0f;
    float yPx = 0.0f;
    if (m_camera->WorldToScreen(centre, xPx, yPx) && xPx >= left && xPx <= right && yPx >= top && yPx <= bottom)
      m_ships[i].selected = true;
  }
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
// Rendering. _alpha is the fraction of a tick already accumulated: every position and heading is
// read between the last two ticks, so motion is smooth however far the swapchain runs ahead of the
// simulation rate.

void WorldView::Render(SceneRenderer& _renderer, GpuDevice& _gpu, TextRenderer& _text, float _alpha)
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

  const std::span<const Game::ShipState> state = m_world->Ships();
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    const ShipView& view = m_ships[i];
    const Game::ShipState& ship = state[i];

    const float x = ship.prevPos.x + (ship.posWorld.x - ship.prevPos.x) * _alpha;
    const float y = ship.prevPos.y + (ship.posWorld.y - ship.prevPos.y) * _alpha;
    const float z = ship.prevPos.z + (ship.posWorld.z - ship.prevPos.z) * _alpha;
    const float heading = ship.prevHeading + XMScalarModAngle(ship.headingRad - ship.prevHeading) * _alpha;

    // Roll about the hull's own mid-height axis, not its base, or a banked ship pivots on one
    // wingtip. SHIP_HOVER_HEIGHT is what keeps the low wing out of the ground while it does.
    const float rollAxisY = view.pickCentre.y;
    const XMMATRIX hull = XMMatrixTranslation(0.0f, -rollAxisY, 0.0f) * XMMatrixRotationZ(view.bankRad) *
                          XMMatrixTranslation(0.0f, rollAxisY + view.restY, 0.0f) * XMMatrixScaling(SHIP_SCALE, SHIP_SCALE, SHIP_SCALE) *
                          XMMatrixRotationY(heading) * XMMatrixTranslation(x, y + SHIP_HOVER_HEIGHT, z);
    XMStoreFloat4x4(&world, hull);

    Rgba tint = view.selected ? SELECTED_COLOUR : SHIP_COLOUR;
    const float lift = view.hoverAmount * SEL_HOVER_HIGHLIGHT_STRENGTH;
    tint = Rgba{tint.r + (1.0f - tint.r) * lift, tint.g + (1.0f - tint.g) * lift, tint.b + (1.0f - tint.b) * lift, 1.0f};
    _renderer.DrawMesh(_gpu, view.mesh, world, tint, SHIP_MATERIAL_MIX, false);
  }

  DrawFeedback(_renderer, _gpu, _alpha);

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

void WorldView::DrawFeedback(SceneRenderer& _renderer, GpuDevice& _gpu, float _alpha)
{
  _renderer.BeginDecals(_gpu, m_camera->ViewProj(), m_camera->Eye());

  const std::span<const Game::ShipState> state = m_world->Ships();
  XMFLOAT4X4 world;

  // --- selection and hover rings ----------------------------------------------------------------
  for (size_t i = 0; i < m_ships.size() && i < state.size(); ++i)
  {
    const ShipView& view = m_ships[i];
    const Game::ShipState& ship = state[i];

    const float hullRadius = std::max(view.halfExtents.x, view.halfExtents.z) * SHIP_SCALE;
    const float x = ship.prevPos.x + (ship.posWorld.x - ship.prevPos.x) * _alpha;
    const float z = ship.prevPos.z + (ship.posWorld.z - ship.prevPos.z) * _alpha;

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
