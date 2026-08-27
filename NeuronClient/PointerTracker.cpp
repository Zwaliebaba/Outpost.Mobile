#include "pch.h"
#include "PointerTracker.h"

using namespace DirectX;

namespace Neuron
{
void PointerTracker::Init(const Desc& _desc) noexcept
{
  m_desc = _desc;
}

void PointerTracker::ResetTapHistory() noexcept
{
  m_lastTapQpc = 0;
}

float PointerTracker::ElapsedMs(std::int64_t _fromQpc, std::int64_t _toQpc) const noexcept
{
  return m_clock.ElapsedMs(_fromQpc, _toQpc);
}

PointerTracker::PointerTrack* PointerTracker::FindTrack(std::uint32_t _id) noexcept
{
  for (PointerTrack& track : m_pointers)
  {
    if (track.active && track.id == _id)
      return &track;
  }
  return nullptr;
}

PointerTracker::PointerTrack* PointerTracker::ClaimTrack(std::uint32_t _id) noexcept
{
  if (PointerTrack* existing = FindTrack(_id))
    return existing;
  for (PointerTrack& candidate : m_pointers)
  {
    if (!candidate.active)
      return &candidate;
  }
  return nullptr; // more contacts than slots: the extra ones are ignored, not queued
}

int PointerTracker::ActiveTouchCount() const noexcept
{
  int count = 0;
  for (const PointerTrack& track : m_pointers)
    count += (track.active && track.isTouch) ? 1 : 0;
  return count;
}

// The two oldest live touches, in slot order.
bool PointerTracker::TwoTouches(PointerTrack*& _outFirst, PointerTrack*& _outSecond) noexcept
{
  _outFirst = nullptr;
  _outSecond = nullptr;
  for (PointerTrack& track : m_pointers)
  {
    if (!track.active || !track.isTouch)
      continue;
    if (!_outFirst)
      _outFirst = &track;
    else if (!_outSecond)
    {
      _outSecond = &track;
      return true;
    }
  }
  return false;
}

// Second button orbits, third button pans.
void PointerTracker::ApplyCameraDrag(const PointerTrack& _track, Camera& _camera) const
{
  const float dx = _track.xPx - _track.prevXPx;
  const float dy = _track.yPx - _track.prevYPx;
  if (dx == 0.0f && dy == 0.0f)
    return;

  if ((_track.buttons & PointerEvent::BUTTON_SECOND) != 0)
  {
    _camera.Orbit(dx, dy);
    _camera.Update();
    return;
  }

  XMFLOAT3 before;
  XMFLOAT3 after;
  if (_camera.RayToGround(_track.prevXPx, _track.prevYPx, before) && _camera.RayToGround(_track.xPx, _track.yPx, after))
  {
    _camera.PanByGround(before, after);
    _camera.Update();
  }
}

// Two fingers: the centroid pans, the spread zooms, the twist orbits.
void PointerTracker::ApplyTwoFingerGesture(const PointerTrack& _first, const PointerTrack& _second, Camera& _camera)
{
  const float centroidX = (_first.xPx + _second.xPx) * 0.5f;
  const float centroidY = (_first.yPx + _second.yPx) * 0.5f;
  const float spread = std::max(1.0f, Distance2D(_first.xPx, _first.yPx, _second.xPx, _second.yPx));
  const float angle = std::atan2(_second.yPx - _first.yPx, _second.xPx - _first.xPx);

  if (!m_gestureActive)
  {
    m_gestureActive = true;
    m_gestureCentroidXPx = centroidX;
    m_gestureCentroidYPx = centroidY;
    m_gestureSpreadPx = spread;
    m_gestureAngleRad = angle;
    return;
  }

  _camera.ZoomFactor(m_gestureSpreadPx / spread);
  _camera.OrbitTwist(XMConvertToDegrees(std::remainder(angle - m_gestureAngleRad, XM_2PI)));
  _camera.Update();

  XMFLOAT3 before;
  XMFLOAT3 after;
  if (_camera.RayToGround(m_gestureCentroidXPx, m_gestureCentroidYPx, before) && _camera.RayToGround(centroidX, centroidY, after))
  {
    _camera.PanByGround(before, after);
    _camera.Update();
  }

  m_gestureCentroidXPx = centroidX;
  m_gestureCentroidYPx = centroidY;
  m_gestureSpreadPx = spread;
  m_gestureAngleRad = angle;
}

void PointerTracker::Apply(const PointerEvent& _event, Camera& _camera, PointerListener& _listener)
{
  if (_event.kind == PointerEvent::Kind::Wheel)
  {
    _camera.ZoomSteps(static_cast<float>(_event.wheelNotches));
    return;
  }

  if (_event.kind == PointerEvent::Kind::Down)
  {
    PointerTrack* slot = ClaimTrack(_event.pointerId);
    if (!slot)
      return;
    *slot = PointerTrack{};
    slot->id = _event.pointerId;
    slot->active = true;
    slot->isTouch = _event.isTouch;
    slot->buttons = _event.buttons;
    slot->startXPx = slot->xPx = slot->prevXPx = _event.xPx;
    slot->startYPx = slot->yPx = slot->prevYPx = _event.yPx;
    slot->downQpc = _event.timestampQpc;
    slot->cameraDrag = (_event.buttons & PointerEvent::BUTTON_CAMERA) != 0;
    m_gestureActive = false; // a new contact restarts any gesture
    return;
  }

  PointerTrack* track = FindTrack(_event.pointerId);
  if (!track)
    return;
  track->prevXPx = track->xPx;
  track->prevYPx = track->yPx;
  track->xPx = _event.xPx;
  track->yPx = _event.yPx;
  track->buttons = _event.buttons;

  if (_event.kind == PointerEvent::Kind::Update)
  {
    _listener.OnHover(_event.xPx, _event.yPx);

    PointerTrack* first = nullptr;
    PointerTrack* second = nullptr;
    if (TwoTouches(first, second))
    {
      first->inGesture = true;
      second->inGesture = true;
      ApplyTwoFingerGesture(*first, *second, _camera);
      return;
    }
    m_gestureActive = false;

    if (track->cameraDrag)
    {
      ApplyCameraDrag(*track, _camera);
      return;
    }
    if ((_event.buttons & PointerEvent::BUTTON_FIRST) == 0) // hovering, nothing held
      return;

    if (!track->dragging && Distance2D(track->startXPx, track->startYPx, track->xPx, track->yPx) >= m_desc.dragThresholdPx)
    {
      track->dragging = true;
      track->boxSelecting = _listener.WantsBoxSelect(_event.shift);
    }
    if (track->dragging)
      _listener.OnDragUpdate(track->boxSelecting, track->startXPx, track->startYPx, track->xPx, track->yPx);
    return;
  }

  // Release.
  const PointerTrack finished = *track;
  track->active = false;
  _listener.OnDragCancelled();
  if (ActiveTouchCount() < 2)
    m_gestureActive = false;
  if (finished.inGesture || finished.cameraDrag)
    return;

  if (finished.dragging)
  {
    if (finished.boxSelecting)
      _listener.OnBoxSelect(finished.startXPx, finished.startYPx, _event.xPx, _event.yPx, _event.shift);
    else
      _listener.OnOrderDrag(finished.startXPx, finished.startYPx, _event.xPx, _event.yPx);
    return;
  }

  if (ElapsedMs(finished.downQpc, _event.timestampQpc) <= m_desc.tapMaxDurationMs)
  {
    const bool doubleTap = m_lastTapQpc != 0 && ElapsedMs(m_lastTapQpc, _event.timestampQpc) <= m_desc.doubleTapWindowMs &&
                           Distance2D(m_lastTapXPx, m_lastTapYPx, _event.xPx, _event.yPx) <= m_desc.dragThresholdPx * 3.0f;
    m_lastTapQpc = _event.timestampQpc;
    m_lastTapXPx = _event.xPx;
    m_lastTapYPx = _event.yPx;
    _listener.OnTap(_event.xPx, _event.yPx, _event.shift, doubleTap);
  }
}
} // namespace Neuron
