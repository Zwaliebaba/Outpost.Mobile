#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace Neuron
{
// An orbit camera over a ground plane, plus the projection maths that every screen-to-world
// question in the client goes through: picking, order placement, drag panning, and where a world
// point lands on the HUD.
//
// It has no idea what it is looking at. Following, leading and shake are driven from outside by
// whoever knows which entities matter, which keeps the game's framing rules in the game and the
// matrix algebra in one tested place.
//
// This tree is left-handed. Where an API offers LH and RH, take LH: render space is
// (east, up, north), and Direct3D is left-handed too. An RH call does not fail, it mirrors --
// east ends up on the left of the screen -- so nothing catches it but the rule.
class Camera
{
public:
  struct Desc
  {
    float minZoom = 40.0f;
    float maxZoom = 900.0f;
    float targetHeight = 3.0f;
    float fovDeg = 45.0f;
    float nearPlane = 0.5f;
    float farPlane = 8000.0f;
    float minPitchDeg = 5.0f;
    float maxPitchDeg = 89.0f;
    float rotateSpeedDegPerPx = 0.35f;
    float zoomStepFactor = 1.12f;
    float panSpeed = 1.0f;
    float followHalfLife = 0.18f;
    float shakeAmplitude = 2.5f;
    float shakeDecayHalfLife = 0.15f;
    float shakeFrequencyHz = 22.0f;
  };

  void Init(const Desc& _desc) noexcept;
  void SetViewport(std::uint32_t _widthPx, std::uint32_t _heightPx) noexcept;

  // Recomputes eye, basis and matrices from the current orbit and target. Call after anything that
  // moves the camera and before anything that unprojects.
  void Update() noexcept;

  // --- gestures ---------------------------------------------------------------------------------
  void Orbit(float _dxPx, float _dyPx) noexcept;
  void OrbitTwist(float _twistDeg) noexcept;
  void ZoomSteps(float _notches) noexcept;
  void ZoomFactor(float _factor) noexcept;
  // Panning unprojects two pointer positions onto the ground so the world stays stuck to the
  // finger, which makes panSpeed a plain multiplier on that rather than a screen-space guess.
  void PanByGround(const DirectX::XMFLOAT3& _before, const DirectX::XMFLOAT3& _after) noexcept;

  // --- framing ----------------------------------------------------------------------------------
  void SetGoal(float _x, float _z) noexcept;
  // Eases the target towards the goal plus a lead, on real time.
  void Follow(float _leadX, float _leadZ, float _dtSec) noexcept;
  void Shake() noexcept;
  void UpdateShake(float _dtSec) noexcept;

  // --- projection -------------------------------------------------------------------------------
  void ScreenRay(float _xPx, float _yPx, DirectX::XMFLOAT3& _outOrigin, DirectX::XMFLOAT3& _outDirection) const noexcept;
  [[nodiscard]] bool RayToGround(float _xPx, float _yPx, DirectX::XMFLOAT3& _outPoint) const noexcept;
  [[nodiscard]] bool WorldToScreen(const DirectX::XMFLOAT3& _world, float& _outXPx, float& _outYPx) const noexcept;

  [[nodiscard]] const DirectX::XMFLOAT4X4& ViewProj() const noexcept
  {
    return m_viewProj;
  }
  // The two halves, for the one caller that cannot use the product: DirectX::BoundingFrustum is
  // built from a projection and then transformed by a view's inverse (Neuron::WorldFrustum).
  [[nodiscard]] const DirectX::XMFLOAT4X4& View() const noexcept
  {
    return m_view;
  }
  [[nodiscard]] const DirectX::XMFLOAT4X4& Proj() const noexcept
  {
    return m_proj;
  }
  [[nodiscard]] const DirectX::XMFLOAT3& Eye() const noexcept
  {
    return m_eye;
  }
  [[nodiscard]] const DirectX::XMFLOAT3& Right() const noexcept
  {
    return m_right;
  }
  [[nodiscard]] const DirectX::XMFLOAT3& Up() const noexcept
  {
    return m_up;
  }
  [[nodiscard]] const DirectX::XMFLOAT3& Target() const noexcept
  {
    return m_target;
  }
  [[nodiscard]] float Distance() const noexcept
  {
    return m_distance;
  }

private:
  Desc m_desc;

  // Orbit, driven by the drag and pinch gestures.
  float m_yawDeg = 0.0f;
  float m_pitchDeg = 52.0f;
  float m_distance = 190.0f;

  DirectX::XMFLOAT3 m_goal{0.0f, 0.0f, 0.0f};   // where panning and following put it
  DirectX::XMFLOAT3 m_target{0.0f, 0.0f, 0.0f}; // where the camera has actually eased to
  DirectX::XMFLOAT3 m_eye{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 m_right{1.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 m_up{0.0f, 1.0f, 0.0f};
  DirectX::XMFLOAT3 m_shakeOffset{0.0f, 0.0f, 0.0f};
  float m_shakeAmount = 0.0f;
  float m_shakeTimeSec = 0.0f;

  DirectX::XMFLOAT4X4 m_viewProj;
  DirectX::XMFLOAT4X4 m_invViewProj;
  DirectX::XMFLOAT4X4 m_view;
  DirectX::XMFLOAT4X4 m_proj;
  std::uint32_t m_viewWidthPx = 1;
  std::uint32_t m_viewHeightPx = 1;
};
} // namespace Neuron
