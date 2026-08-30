#pragma once

#include <DirectXCollision.h>
#include <DirectXMath.h>

namespace Neuron
{
// What is worth submitting: the camera's frustum, and a conservative sphere test against it.
//
// Everything the world draws used to be submitted whether or not it was on screen
// (Design/MmoScalabilityReview.md G2). At a fleet's scale that is thousands of draws for hulls
// behind the eye, and the cost is paid in full before the GPU discards them.
//
// Two functions rather than one, because the frustum is built once a frame and tested thousands of
// times, and because the build is the half that is easy to get silently wrong.

// The camera's frustum, in world space.
//
// DirectX::BoundingFrustum::CreateFromMatrix takes the projection alone and produces a frustum in
// view space; carrying it into world space is the inverse of the view. The handedness flag is
// **false**, meaning left-handed, because this tree is left-handed everywhere (AGENTS.md 5) and the
// camera builds its matrices with XMMatrixLookAtLH and XMMatrixPerspectiveFovLH. Passing true is
// the mistake this comment exists to prevent: it does not fail, it builds a frustum pointing the
// other way, and everything in front of the camera is culled.
[[nodiscard]] DirectX::BoundingFrustum WorldFrustum(const DirectX::XMFLOAT4X4& _view, const DirectX::XMFLOAT4X4& _proj) noexcept;

// True if a sphere is inside the frustum or straddles it -- conservative, so a false positive is a
// wasted draw and there are no false negatives.
//
// The radius is the caller's to pad. A bounding sphere that is a little tight pops at the edge of
// the screen, and the padding that fixes it is a property of what is being drawn rather than of the
// frustum, so it belongs at the call site where the number can be named.
[[nodiscard]] bool IsSphereVisible(const DirectX::BoundingFrustum& _frustum, const DirectX::XMFLOAT3& _centre,
                                   float _radiusMetres) noexcept;
} // namespace Neuron
