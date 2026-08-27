#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
Neuron::Camera MakeCamera()
{
  Neuron::Camera camera;
  camera.SetViewport(1600, 900);
  camera.Init(Neuron::Camera::Desc{});
  return camera;
}
} // namespace

TEST_CLASS(CameraTests)
{
public:
  TEST_METHOD(TheCentreOfTheScreenUnprojectsToTheTarget)
  {
    // Picking, order placement and drag panning all go through RayToGround, so this is the single
    // most load-bearing piece of maths in the client. No device needed to test it, which is the
    // point of the camera being its own object.
    Neuron::Camera camera = MakeCamera();

    XMFLOAT3 ground;
    Assert::IsTrue(camera.RayToGround(800.0f, 450.0f, ground), L"the centre of the screen missed the ground plane");
    Assert::AreEqual(0.0f, ground.y, 1e-3f, L"the ground hit is not on the ground plane");
    Assert::IsTrue(Neuron::Distance2D(ground.x, ground.z, camera.Target().x, camera.Target().z) < 6.0f,
                   L"the centre of the screen did not land near the camera target");
  }

  TEST_METHOD(AWorldPointRoundTripsThroughTheScreen)
  {
    Neuron::Camera camera = MakeCamera();

    XMFLOAT3 ground;
    Assert::IsTrue(camera.RayToGround(1100.0f, 600.0f, ground), L"the sample point missed the ground");

    float xPx = 0.0f;
    float yPx = 0.0f;
    Assert::IsTrue(camera.WorldToScreen(ground, xPx, yPx), L"a point in front of the eye projected as behind it");
    Assert::AreEqual(1100.0f, xPx, 0.5f, L"the round trip moved the point across the screen");
    Assert::AreEqual(600.0f, yPx, 0.5f, L"the round trip moved the point down the screen");
  }

  TEST_METHOD(TheSkyDoesNotHitTheGround)
  {
    // A ray at or above the horizon has no ground intersection, and saying it does would put an
    // order marker at infinity.
    Neuron::Camera camera = MakeCamera();
    XMFLOAT3 ground;
    Assert::IsFalse(camera.RayToGround(800.0f, 0.0f, ground), L"a ray into the sky reported a ground hit");
  }

  TEST_METHOD(ZoomIsClampedAtBothEnds)
  {
    Neuron::Camera::Desc desc;
    desc.minZoom = 40.0f;
    desc.maxZoom = 900.0f;
    Neuron::Camera camera;
    camera.SetViewport(1600, 900);
    camera.Init(desc);

    for (int i = 0; i < 100; ++i)
      camera.ZoomSteps(1.0f);
    Assert::AreEqual(desc.minZoom, camera.Distance(), 1e-3f, L"zooming in ran past the near limit");

    for (int i = 0; i < 200; ++i)
      camera.ZoomSteps(-1.0f);
    Assert::AreEqual(desc.maxZoom, camera.Distance(), 1e-3f, L"zooming out ran past the far limit");
  }

  TEST_METHOD(PitchIsClampedSoTheCameraNeverFlipsOver)
  {
    Neuron::Camera camera = MakeCamera();
    for (int i = 0; i < 500; ++i)
      camera.Orbit(0.0f, -50.0f); // drag up, hard
    camera.Update();

    // Above the target and still looking down at it: an unclamped pitch would put the eye under the
    // ground plane and mirror the whole scene.
    Assert::IsTrue(camera.Eye().y > camera.Target().y, L"the camera went under the ground plane");
  }
};
} // namespace NeuronClientTests
