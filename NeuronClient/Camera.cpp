#include "pch.h"
#include "Camera.h"

using namespace DirectX;

namespace Neuron
{
void Camera::Init(const Desc& _desc) noexcept
{
  m_desc = _desc;
  Update();
}

void Camera::SetViewport(std::uint32_t _widthPx, std::uint32_t _heightPx) noexcept
{
  m_viewWidthPx = std::max(1u, _widthPx);
  m_viewHeightPx = std::max(1u, _heightPx);
}

void Camera::Update() noexcept
{
  const float yaw = XMConvertToRadians(m_yawDeg);
  const float pitch = XMConvertToRadians(std::clamp(m_pitchDeg, m_desc.minPitchDeg, m_desc.maxPitchDeg));
  const float distance = std::clamp(m_distance, m_desc.minZoom, m_desc.maxZoom);
  m_target.y = m_desc.targetHeight;

  const float cosPitch = std::cos(pitch);
  const XMFLOAT3 shaken(m_target.x + m_shakeOffset.x, m_target.y + m_shakeOffset.y, m_target.z + m_shakeOffset.z);
  const XMVECTOR target = XMLoadFloat3(&shaken);
  const XMVECTOR offset = XMVectorSet(std::sin(yaw) * cosPitch, std::sin(pitch), -std::cos(yaw) * cosPitch, 0.0f);
  const XMVECTOR eye = XMVectorMultiplyAdd(offset, XMVectorReplicate(distance), target);
  XMStoreFloat3(&m_eye, eye);

  const XMVECTOR forward = XMVector3Normalize(XMVectorNegate(offset));
  const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
  XMStoreFloat3(&m_right, right);
  XMStoreFloat3(&m_up, XMVector3Cross(forward, right));

  const float aspect = static_cast<float>(m_viewWidthPx) / static_cast<float>(std::max(1u, m_viewHeightPx));
  const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
  const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(std::clamp(m_desc.fovDeg, 5.0f, 170.0f)), aspect,
                                                 std::max(0.01f, m_desc.nearPlane), std::max(1.0f, m_desc.farPlane));
  const XMMATRIX viewProj = view * proj;
  XMStoreFloat4x4(&m_viewProj, viewProj);
  XMStoreFloat4x4(&m_invViewProj, XMMatrixInverse(nullptr, viewProj));
}

void Camera::Orbit(float _dxPx, float _dyPx) noexcept
{
  m_yawDeg = std::remainder(m_yawDeg + _dxPx * m_desc.rotateSpeedDegPerPx, 360.0f);
  m_pitchDeg = std::clamp(m_pitchDeg - _dyPx * m_desc.rotateSpeedDegPerPx, m_desc.minPitchDeg, m_desc.maxPitchDeg);
}

void Camera::OrbitTwist(float _twistDeg) noexcept
{
  m_yawDeg = std::remainder(m_yawDeg + _twistDeg, 360.0f);
}

void Camera::ZoomSteps(float _notches) noexcept
{
  ZoomFactor(std::pow(std::max(1.001f, m_desc.zoomStepFactor), -_notches));
}

void Camera::ZoomFactor(float _factor) noexcept
{
  m_distance = std::clamp(m_distance * _factor, m_desc.minZoom, m_desc.maxZoom);
}

void Camera::PanByGround(const XMFLOAT3& _before, const XMFLOAT3& _after) noexcept
{
  // The goal only. The target eases in behind it through Follow, which is the camera lag; moving
  // both here would make a drag rigid and lose it.
  m_goal.x -= (_after.x - _before.x) * m_desc.panSpeed;
  m_goal.z -= (_after.z - _before.z) * m_desc.panSpeed;
}

void Camera::SetGoal(float _x, float _z) noexcept
{
  m_goal.x = _x;
  m_goal.z = _z;
}

void Camera::Follow(float _leadX, float _leadZ, float _dtSec) noexcept
{
  const float follow = HalfLifeBlend(_dtSec, m_desc.followHalfLife);
  m_target.x += (m_goal.x + _leadX - m_target.x) * follow;
  m_target.z += (m_goal.z + _leadZ - m_target.z) * follow;
}

void Camera::Shake() noexcept
{
  m_shakeAmount = 1.0f;
}

// Two detuned sines per axis so it never reads as a single wobble, decaying on its own half-life,
// thrown across the view rather than along the world axes.
void Camera::UpdateShake(float _dtSec) noexcept
{
  m_shakeTimeSec += _dtSec;
  m_shakeAmount -= m_shakeAmount * HalfLifeBlend(_dtSec, m_desc.shakeDecayHalfLife);
  if (m_shakeAmount < 0.001f)
  {
    m_shakeAmount = 0.0f;
    m_shakeOffset = XMFLOAT3(0.0f, 0.0f, 0.0f);
    return;
  }

  const float w = XM_2PI * std::max(0.1f, m_desc.shakeFrequencyHz);
  const float t = m_shakeTimeSec;
  const float swing = m_shakeAmount * m_desc.shakeAmplitude;
  const float acrossView = (std::sin(t * w) * 0.62f + std::sin(t * w * 1.71f) * 0.38f) * swing;
  const float upView = (std::sin(t * w * 1.31f) * 0.62f + std::sin(t * w * 2.13f) * 0.38f) * swing;
  m_shakeOffset =
    XMFLOAT3(m_right.x * acrossView + m_up.x * upView, m_right.y * acrossView + m_up.y * upView, m_right.z * acrossView + m_up.z * upView);
}

void Camera::ScreenRay(float _xPx, float _yPx, XMFLOAT3& _outOrigin, XMFLOAT3& _outDirection) const noexcept
{
  const float ndcX = (_xPx / static_cast<float>(m_viewWidthPx)) * 2.0f - 1.0f;
  const float ndcY = 1.0f - (_yPx / static_cast<float>(m_viewHeightPx)) * 2.0f;
  const XMMATRIX inverse = XMLoadFloat4x4(&m_invViewProj);
  const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverse);
  const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverse);
  XMStoreFloat3(&_outOrigin, nearPoint);
  XMStoreFloat3(&_outDirection, XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint)));
}

bool Camera::RayToGround(float _xPx, float _yPx, XMFLOAT3& _outPoint) const noexcept
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  ScreenRay(_xPx, _yPx, origin, direction);
  if (direction.y > -1e-5f) // at or above the horizon
    return false;
  const float t = -origin.y / direction.y;
  _outPoint = XMFLOAT3(origin.x + direction.x * t, 0.0f, origin.z + direction.z * t);
  return true;
}

bool Camera::WorldToScreen(const XMFLOAT3& _world, float& _outXPx, float& _outYPx) const noexcept
{
  const XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&_world), XMLoadFloat4x4(&m_viewProj));
  const float w = XMVectorGetW(clip);
  if (w <= 1e-4f) // behind the eye
    return false;
  _outXPx = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * static_cast<float>(m_viewWidthPx);
  _outYPx = (0.5f - XMVectorGetY(clip) / w * 0.5f) * static_cast<float>(m_viewHeightPx);
  return true;
}
} // namespace Neuron
