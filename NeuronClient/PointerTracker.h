#pragma once

#include "Camera.h"
#include "PointerEvent.h"

#include "FrameClock.h"

#include <cstdint>

namespace Neuron
{
// What the tracker asks of the game, and tells it. The tracker owns everything that is true of any
// pointer -- which contacts are down, how far one has travelled, whether two of them are pinching,
// how long ago the last tap was -- and none of what a contact means, because that is the game's.
//
// The one question that has to go the other way is WantsBoxSelect: whether a drag with the primary
// button bands a selection box or lays down an order. Only the game knows, because the answer
// depends on whether anything is selected.
class PointerListener
{
public:
  virtual ~PointerListener() = default;

  PointerListener(const PointerListener&) = delete;
  PointerListener& operator=(const PointerListener&) = delete;

  [[nodiscard]] virtual bool WantsBoxSelect(bool _shiftHeld) = 0;

  virtual void OnHover(float _xPx, float _yPx) = 0;
  virtual void OnDragUpdate(bool _boxSelect, float _x0Px, float _y0Px, float _x1Px, float _y1Px) = 0;
  virtual void OnDragCancelled() = 0;
  virtual void OnBoxSelect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, bool _additive) = 0;
  virtual void OnOrderDrag(float _x0Px, float _y0Px, float _x1Px, float _y1Px) = 0;
  virtual void OnTap(float _xPx, float _yPx, bool _shiftHeld, bool _doubleTap) = 0;

protected:
  PointerListener() = default;
};

// One path for mouse and touch: WM_POINTER gives both, and the second and third buttons drive the
// camera so a single contact is always free for selection and orders. On a tablet the same job is
// done by two fingers -- the centroid pans, the spread zooms, the twist orbits -- which is worth
// the bookkeeping because without it a tablet has no camera control at all.
class PointerTracker
{
public:
  struct Desc
  {
    float dragThresholdPx = 6.0f;
    float tapMaxDurationMs = 320.0f;
    float doubleTapWindowMs = 300.0f;
  };

  static constexpr int MAX_POINTERS = 4;

  void Init(const Desc& _desc) noexcept;

  // Applies one event, driving the camera itself and reporting anything the game has to decide.
  void Apply(const PointerEvent& _event, Camera& _camera, PointerListener& _listener);

  // Forgets the last tap, so the next one cannot pair with it into a double tap. The game calls
  // this when a tap landed on something -- a tap on a hull is not the start of a double tap on
  // empty ground.
  void ResetTapHistory() noexcept;

private:
  struct PointerTrack
  {
    std::uint32_t id = 0;
    bool active = false;
    bool isTouch = false;
    std::uint32_t buttons = 0;
    float startXPx = 0.0f, startYPx = 0.0f;
    float xPx = 0.0f, yPx = 0.0f;
    float prevXPx = 0.0f, prevYPx = 0.0f;
    std::int64_t downQpc = 0;
    bool dragging = false;
    bool boxSelecting = false;
    bool cameraDrag = false; // held with the second or third button
    bool inGesture = false;  // part of a two-finger gesture, so its release means nothing
  };

  [[nodiscard]] PointerTrack* FindTrack(std::uint32_t _id) noexcept;
  [[nodiscard]] PointerTrack* ClaimTrack(std::uint32_t _id) noexcept;
  [[nodiscard]] int ActiveTouchCount() const noexcept;
  [[nodiscard]] bool TwoTouches(PointerTrack*& _outFirst, PointerTrack*& _outSecond) noexcept;
  [[nodiscard]] float ElapsedMs(std::int64_t _fromQpc, std::int64_t _toQpc) const noexcept;

  void ApplyCameraDrag(const PointerTrack& _track, Camera& _camera) const;
  void ApplyTwoFingerGesture(const PointerTrack& _first, const PointerTrack& _second, Camera& _camera);

  Desc m_desc;
  FrameClock m_clock;
  PointerTrack m_pointers[MAX_POINTERS];

  std::int64_t m_lastTapQpc = 0;
  float m_lastTapXPx = 0.0f;
  float m_lastTapYPx = 0.0f;

  // Two-finger gesture, remembered between updates so pan, pinch and twist all read clean deltas.
  bool m_gestureActive = false;
  float m_gestureCentroidXPx = 0.0f;
  float m_gestureCentroidYPx = 0.0f;
  float m_gestureSpreadPx = 0.0f;
  float m_gestureAngleRad = 0.0f;
};
} // namespace Neuron
