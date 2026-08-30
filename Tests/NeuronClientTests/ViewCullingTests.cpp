#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
// A camera at the origin looking down +Z, which is forward in a left-handed world. Every expected
// answer below reads off that: positive Z is in front, negative Z is behind.
constexpr float NEAR_PLANE = 1.0f;
constexpr float FAR_PLANE = 1000.0f;

[[nodiscard]] BoundingFrustum FrustumLookingDownPositiveZ()
{
  XMFLOAT4X4 view;
  XMFLOAT4X4 proj;
  XMStoreFloat4x4(
    &view, XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f), XMVectorSet(0.0f, 0.0f, 1.0f, 1.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
  XMStoreFloat4x4(&proj, XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 16.0f / 9.0f, NEAR_PLANE, FAR_PLANE));
  return Neuron::WorldFrustum(view, proj);
}
} // namespace

TEST_CLASS(ViewCullingTests)
{
public:
  TEST_METHOD(WhatIsInFrontIsVisibleAndWhatIsBehindIsNot)
  {
    // The whole slice rests on this being the right way round. Building the frustum right-handed
    // does not fail, it inverts: everything in front is culled and the screen empties. That is the
    // defect this row exists to catch, and it is why the flag is spelled out in WorldFrustum.
    const BoundingFrustum frustum = FrustumLookingDownPositiveZ();

    Assert::IsTrue(Neuron::IsSphereVisible(frustum, XMFLOAT3(0.0f, 0.0f, 100.0f), 1.0f), L"a sphere in front of the camera was culled");
    Assert::IsFalse(Neuron::IsSphereVisible(frustum, XMFLOAT3(0.0f, 0.0f, -100.0f), 1.0f), L"a sphere behind the camera was submitted");
  }

  TEST_METHOD(TheNearAndFarPlanesBothCut)
  {
    const BoundingFrustum frustum = FrustumLookingDownPositiveZ();

    Assert::IsFalse(Neuron::IsSphereVisible(frustum, XMFLOAT3(0.0f, 0.0f, NEAR_PLANE * 0.1f), 0.01f),
                    L"a sphere short of the near plane was submitted");
    Assert::IsFalse(Neuron::IsSphereVisible(frustum, XMFLOAT3(0.0f, 0.0f, FAR_PLANE * 2.0f), 1.0f),
                    L"a sphere past the far plane was submitted");
    Assert::IsTrue(Neuron::IsSphereVisible(frustum, XMFLOAT3(0.0f, 0.0f, FAR_PLANE * 0.5f), 1.0f),
                   L"a sphere between the planes was culled");
  }

  TEST_METHOD(SomethingOffToTheSideIsCulled)
  {
    // A 60-degree vertical field at 16:9 is a little under 100 degrees across, so half a right angle
    // out to the side at close range is comfortably outside it.
    const BoundingFrustum frustum = FrustumLookingDownPositiveZ();
    Assert::IsFalse(Neuron::IsSphereVisible(frustum, XMFLOAT3(500.0f, 0.0f, 10.0f), 1.0f), L"a sphere well off to the side was submitted");
  }

  TEST_METHOD(TheTestIsConservativeAtTheEdge)
  {
    // A sphere whose centre is outside but which straddles the plane has to be submitted: the
    // contract is that there are no false negatives, because a wrongly culled hull is a ship that
    // vanishes and a wrongly kept one is a fraction of a millisecond.
    const BoundingFrustum frustum = FrustumLookingDownPositiveZ();
    const XMFLOAT3 behind(0.0f, 0.0f, -5.0f);
    Assert::IsFalse(Neuron::IsSphereVisible(frustum, behind, 1.0f), L"the centre was not outside to begin with");
    Assert::IsTrue(Neuron::IsSphereVisible(frustum, behind, 50.0f), L"a sphere straddling the frustum was culled");
  }

  TEST_METHOD(APaddedRadiusKeepsMoreThanATightOne)
  {
    // What CULL_RADIUS_PAD_METRES buys, as a property rather than as a number: growing the radius
    // can only ever turn a culled thing into a submitted one, never the other way.
    const BoundingFrustum frustum = FrustumLookingDownPositiveZ();
    const XMFLOAT3 centres[] = {XMFLOAT3(0.0f, 0.0f, 100.0f), XMFLOAT3(400.0f, 0.0f, 10.0f), XMFLOAT3(0.0f, 0.0f, -20.0f),
                                XMFLOAT3(-60.0f, 30.0f, 90.0f)};
    for (const XMFLOAT3& centre : centres)
    {
      if (Neuron::IsSphereVisible(frustum, centre, 1.0f))
        Assert::IsTrue(Neuron::IsSphereVisible(frustum, centre, 25.0f), L"padding the radius culled something a tight radius kept");
    }
  }

  TEST_METHOD(TheFrustumFollowsTheCamera)
  {
    // Built in view space and carried out by the inverse of the view. Get that transform wrong and
    // the frustum sits at the origin whatever the camera does, which looks correct for exactly as
    // long as the camera stays there.
    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;
    const XMVECTOR eye = XMVectorSet(0.0f, 0.0f, 900.0f, 1.0f);
    XMStoreFloat4x4(&view, XMMatrixLookAtLH(eye, XMVectorSet(0.0f, 0.0f, 1000.0f, 1.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
    XMStoreFloat4x4(&proj, XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), 16.0f / 9.0f, NEAR_PLANE, FAR_PLANE));
    const BoundingFrustum moved = Neuron::WorldFrustum(view, proj);

    Assert::IsTrue(Neuron::IsSphereVisible(moved, XMFLOAT3(0.0f, 0.0f, 1000.0f), 1.0f),
                   L"the moved camera cannot see what is in front of it");
    Assert::IsFalse(Neuron::IsSphereVisible(moved, XMFLOAT3(0.0f, 0.0f, 100.0f), 1.0f),
                    L"the frustum stayed at the origin instead of following the camera");
  }

  TEST_METHOD(ANegativeRadiusIsTreatedAsAPoint)
  {
    // BoundingSphere does not defend itself against one and a caller subtracting its way to a
    // negative is a plausible mistake, so the answer is pinned rather than left to the library.
    const BoundingFrustum frustum = FrustumLookingDownPositiveZ();
    Assert::IsTrue(Neuron::IsSphereVisible(frustum, XMFLOAT3(0.0f, 0.0f, 100.0f), -5.0f),
                   L"a negative radius lost a point that is in view");
    Assert::IsFalse(Neuron::IsSphereVisible(frustum, XMFLOAT3(0.0f, 0.0f, -100.0f), -5.0f),
                    L"a negative radius kept a point that is behind");
  }
};
} // namespace NeuronClientTests
