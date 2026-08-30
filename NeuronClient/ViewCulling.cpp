#include "pch.h"
#include "ViewCulling.h"

using namespace DirectX;

namespace Neuron
{
BoundingFrustum WorldFrustum(const XMFLOAT4X4& _view, const XMFLOAT4X4& _proj) noexcept
{
  BoundingFrustum viewSpace;
  BoundingFrustum::CreateFromMatrix(viewSpace, XMLoadFloat4x4(&_proj), false); // false: left-handed

  // The view maps world to view, so its inverse maps the frustum back out. A view matrix is a
  // rotation and a translation with no scale, which is exactly what Transform is defined for.
  BoundingFrustum worldSpace;
  viewSpace.Transform(worldSpace, XMMatrixInverse(nullptr, XMLoadFloat4x4(&_view)));
  return worldSpace;
}

bool IsSphereVisible(const BoundingFrustum& _frustum, const XMFLOAT3& _centre, float _radiusMetres) noexcept
{
  const BoundingSphere sphere(_centre, (_radiusMetres > 0.0f) ? _radiusMetres : 0.0f);
  return _frustum.Contains(sphere) != DISJOINT;
}
} // namespace Neuron
