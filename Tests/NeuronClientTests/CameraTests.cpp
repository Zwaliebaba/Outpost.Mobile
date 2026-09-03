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
    // A ray at or above the horizon has no ground intersection, and saying it had one would put an
    // order marker at infinity.
    //
    // The default framing cannot show this, which is what the first version of this test got
    // wrong: at 52 degrees of pitch with a 45 degree vertical field of view, the top of the screen
    // is still 29.5 degrees below horizontal, so every pixel is ground and reporting a hit there is
    // correct. Drop the camera to its shallowest pitch, where the top of the screen clears the
    // horizon by 17.5 degrees, and the case actually exists.
    Neuron::Camera camera = MakeCamera();
    camera.Orbit(0.0f, 10000.0f); // drag down hard; the pitch clamps at its minimum
    camera.Update();

    XMFLOAT3 ground;
    Assert::IsFalse(camera.RayToGround(800.0f, 0.0f, ground), L"a ray into the sky reported a ground hit");

    // The same camera, below the horizon, still finds the ground -- or the test above would pass
    // just as well against a RayToGround that never succeeded.
    Assert::IsTrue(camera.RayToGround(800.0f, 890.0f, ground), L"a ray into the ground missed it");
    Assert::AreEqual(0.0f, ground.y, 1e-3f, L"the ground hit is not on the ground plane");
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

  TEST_METHOD(TheNearPlaneRidesTheZoomOnlyOnceTheFloorIsPassed)
  {
    // The game can pull the camera back far enough to frame a whole sector, which needs a far plane
    // three times what it was; against a fixed near plane that is depth precision spent everywhere,
    // including up close where nothing needed it. So the near plane is max(floor, distance * f) --
    // and the half that matters is that the floor still wins at every distance the camera reached
    // before, because a projection that changed there would change picking and framing in a scene
    // nobody asked to have touched.
    Neuron::Camera::Desc desc;
    desc.minZoom = 40.0f;
    desc.maxZoom = 9900.0f;
    desc.nearPlane = 0.5f;
    desc.nearFractionOfDistance = 5.5e-4f; // the floor holds out to 909 m
    Neuron::Camera camera;
    camera.SetViewport(1600, 900);
    camera.Init(desc);

    for (int i = 0; i < 100; ++i)
      camera.ZoomSteps(1.0f); // in, to the near limit
    camera.Update();
    Assert::AreEqual(desc.nearPlane, camera.NearPlane(), 1e-4f, L"zoomed in, the fraction beat the floor");

    for (int i = 0; i < 200; ++i)
      camera.ZoomSteps(-1.0f); // out, to the far limit
    camera.Update();
    Assert::AreEqual(desc.maxZoom * desc.nearFractionOfDistance, camera.NearPlane(), 1e-3f,
                     L"zoomed out, the near plane did not follow the distance");

    // And the whole point of it: the depth range the projection spans is no worse at the far limit
    // than the fixed near plane gave at the old one.
    Assert::IsTrue(desc.farPlane / camera.NearPlane() < desc.farPlane / desc.nearPlane, L"the near-to-far ratio did not improve");
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
