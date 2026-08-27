#pragma once

#include "Gfx.h"

#include <DirectXMath.h>

#include <string>
#include <vector>

// Walks up from the executable looking for the folder that holds GameData and tuning.ini, so the
// toy runs from a build tree or from the repository root without a configured path.
std::wstring FindDataRoot();

constexpr float SIM_HZ = 60.0f;
constexpr float SIM_DT = 1.0f / SIM_HZ;

// Everything the toy knows about a pointer, decoded from WM_POINTER* in Main.cpp and queued rather
// than acted on immediately: the frame loop drains it once the camera matrices are current, and
// stage 5 records the same queue.
struct PointerEvent
{
  enum class Kind : uint8_t
  {
    Down,
    Update,
    Up,
    Wheel
  };

  Kind kind = Kind::Update;
  uint32_t pointerId = 0;
  float xPx = 0.0f;
  float yPx = 0.0f;
  uint32_t buttons = 0; // bit 0 first (left / touch contact), bit 1 second, bit 2 third
  bool isTouch = false;
  bool shift = false;
  int32_t wheelNotches = 0;
  int64_t timestampQpc = 0; // when the message reached us
};

enum class OrderState : uint8_t
{
  Idle,
  Moving,   // steering towards orderPos
  Aligning  // arrived; turning onto the ordered facing
};

struct Ship
{
  // Simulation state, advanced only in Step().
  DirectX::XMFLOAT3 posWorld{0.0f, 0.0f, 0.0f};
  float headingRad = 0.0f; // 0 points north (+Z); forward is (sin h, 0, cos h)
  float speed = 0.0f;
  float turnRateRadPerSec = 0.0f;

  // The tick before, so rendering can interpolate between them.
  DirectX::XMFLOAT3 prevPos{0.0f, 0.0f, 0.0f};
  float prevHeading = 0.0f;

  OrderState order = OrderState::Idle;
  DirectX::XMFLOAT3 orderPos{0.0f, 0.0f, 0.0f};
  float orderFacingRad = 0.0f;
  bool orderHasFacing = false;

  UINT mesh = 0;
  float restY = 0.0f; // lifts the hull so its lowest vertex rests on the ground plane
  DirectX::XMFLOAT3 pickCentre{0.0f, 0.0f, 0.0f};  // mesh bounds centre, in local space
  DirectX::XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f}; // mesh half-size about that centre
  bool selected = false;
};

struct Scene
{
  void Init(Gfx& _gfx);
  void QueuePointerEvent(const PointerEvent& _event);
  void ClearSelection();
  int SelectedCount() const;

  // Once per frame: refresh the camera matrices, then drain the pointer queue against them.
  void Update(uint32_t _viewWidthPx, uint32_t _viewHeightPx);

  // One fixed 60 Hz tick.
  void Step();

  // _alpha is the fraction of a tick already accumulated, for interpolation.
  void Render(Gfx& _gfx, float _alpha);

  void UpdateCamera();
  void ScreenRay(float _xPx, float _yPx, DirectX::XMFLOAT3& _origin, DirectX::XMFLOAT3& _direction) const;
  bool RayToGround(float _xPx, float _yPx, DirectX::XMFLOAT3& _point) const;
  bool WorldToScreen(const DirectX::XMFLOAT3& _world, float& _xPx, float& _yPx) const;
  int PickShip(float _xPx, float _yPx) const;
  void IssueMoveOrder(const DirectX::XMFLOAT3& _point, bool _hasFacing, float _facingRad);
  void ApplyPointerEvent(const PointerEvent& _event);
  void HandleTap(float _xPx, float _yPx, bool _shift, int64_t _qpc);
  void FinishBoxSelect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, bool _additive);
  void FinishOrderDrag(float _x0Px, float _y0Px, float _x1Px, float _y1Px);

  std::vector<Ship> m_ships;
  UINT m_groundMesh = 0;

  DirectX::XMFLOAT3 m_cameraTarget{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 m_cameraEye{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT4X4 m_viewProj;
  DirectX::XMFLOAT4X4 m_invViewProj;
  uint32_t m_viewWidthPx = 1;
  uint32_t m_viewHeightPx = 1;

  int m_hoverShip = -1; // stage 4 draws the highlight; stage 3 just tracks it
  bool m_boxActive = false;
  float m_boxX0Px = 0.0f, m_boxY0Px = 0.0f, m_boxX1Px = 0.0f, m_boxY1Px = 0.0f;
  bool m_orderDragActive = false;
  float m_orderX0Px = 0.0f, m_orderY0Px = 0.0f, m_orderX1Px = 0.0f, m_orderY1Px = 0.0f;

  std::vector<PointerEvent> m_pendingEvents;
};
