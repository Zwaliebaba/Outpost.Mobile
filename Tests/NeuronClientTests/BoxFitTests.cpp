#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronClientTests
{
namespace
{
// A source box wider than it is tall, so the two axes cannot agree by accident. Every expectation
// below is arithmetic against these four numbers rather than a remembered pixel.
constexpr float SRC_X0 = -400.0f;
constexpr float SRC_Y0 = -100.0f;
constexpr float SRC_X1 = 400.0f;
constexpr float SRC_Y1 = 100.0f;

[[nodiscard]] Neuron::BoxFit FitInto(float _wPx, float _hPx) noexcept
{
  return Neuron::FitBoxIsotropic(SRC_X0, SRC_Y0, SRC_X1, SRC_Y1, 0.0f, 0.0f, _wPx, _hPx);
}
} // namespace

TEST_CLASS(BoxFitTests)
{
public:
  TEST_METHOD(OneScaleServesBothAxes)
  {
    // The whole claim. A per-axis fit would give 1.0 across and 4.0 down here, and the picture would
    // fill the box while every distance read off it was wrong by four.
    const Neuron::BoxFit fit = FitInto(800.0f, 800.0f);
    Assert::AreEqual(1.0f, fit.scale, 1e-5f, L"the fit did not take the smaller of the two axis scales");

    // Two source segments of equal length come back equal in pixels, whichever way they run. This is
    // the property a player reads off the map, stated as the test rather than as the scale.
    const float across = fit.XPx(100.0f) - fit.XPx(0.0f);
    const float down = fit.YPx(100.0f) - fit.YPx(0.0f);
    Assert::AreEqual(across, down, 1e-5f, L"a length measured across the box is not the same length measured down it");
  }

  TEST_METHOD(TheSourceFitsInsideTheDestinationAtEveryAspect)
  {
    // Three shapes: taller than the source, wider than it, and square. In every one the whole source
    // must land inside the box, which is what "fits" means and is the half a wrong scale breaks.
    const float sizes[][2] = {{800.0f, 800.0f}, {1920.0f, 200.0f}, {300.0f, 1200.0f}};
    for (const float(&size)[2] : sizes)
    {
      const Neuron::BoxFit fit = FitInto(size[0], size[1]);
      Assert::IsTrue(fit.XPx(SRC_X0) >= -1e-3f, L"the source's left edge fell outside the box");
      Assert::IsTrue(fit.XPx(SRC_X1) <= size[0] + 1e-3f, L"the source's right edge fell outside the box");
      Assert::IsTrue(fit.YPx(SRC_Y0) >= -1e-3f, L"the source's top edge fell outside the box");
      Assert::IsTrue(fit.YPx(SRC_Y1) <= size[1] + 1e-3f, L"the source's bottom edge fell outside the box");

      // And it touches: an isotropic fit is the LARGEST one, so at least one axis has no slack.
      const float slackX = size[0] - (fit.XPx(SRC_X1) - fit.XPx(SRC_X0));
      const float slackY = size[1] - (fit.YPx(SRC_Y1) - fit.YPx(SRC_Y0));
      Assert::IsTrue(std::min(slackX, slackY) < 1e-3f, L"the fit left slack in both axes, so it was not the largest one");
    }
  }

  TEST_METHOD(TheSlackIsCentred)
  {
    // 1600 x 200 for an 800 x 200 source at scale 1: 800 px of slack across, none down.
    const Neuron::BoxFit fit = FitInto(1600.0f, 200.0f);
    Assert::AreEqual(1.0f, fit.scale, 1e-5f, L"the height should have decided the scale");
    Assert::AreEqual(400.0f, fit.XPx(SRC_X0), 1e-3f, L"the left margin is not half the slack");
    Assert::AreEqual(1200.0f, fit.XPx(SRC_X1), 1e-3f, L"the right margin is not half the slack");
    Assert::AreEqual(0.0f, fit.YPx(SRC_Y0), 1e-3f, L"the axis with no slack should sit against the edge");
  }

  TEST_METHOD(TheDestinationOffsetIsCarried)
  {
    // The same fit inside a panel that does not start at the origin: every pixel moves by exactly
    // the panel's corner and nothing else does.
    const Neuron::BoxFit at0 = FitInto(800.0f, 800.0f);
    const Neuron::BoxFit at64 = Neuron::FitBoxIsotropic(SRC_X0, SRC_Y0, SRC_X1, SRC_Y1, 64.0f, 32.0f, 864.0f, 832.0f);
    Assert::AreEqual(at0.scale, at64.scale, 1e-5f, L"moving the panel changed the scale");
    Assert::AreEqual(at0.XPx(0.0f) + 64.0f, at64.XPx(0.0f), 1e-3f, L"the panel's x offset was not carried");
    Assert::AreEqual(at0.YPx(0.0f) + 32.0f, at64.YPx(0.0f), 1e-3f, L"the panel's y offset was not carried");
  }

  TEST_METHOD(ADegenerateSourceDoesNotDivideByZero)
  {
    // Every point on one horizontal line: the height decides nothing, so the width must, and the
    // line must land centred rather than at infinity.
    const Neuron::BoxFit flat = Neuron::FitBoxIsotropic(-50.0f, 10.0f, 50.0f, 10.0f, 0.0f, 0.0f, 400.0f, 200.0f);
    Assert::AreEqual(4.0f, flat.scale, 1e-5f, L"the axis with extent should have decided the scale");
    Assert::AreEqual(0.0f, flat.XPx(-50.0f), 1e-3f, L"the flat source did not span the box");
    Assert::AreEqual(400.0f, flat.XPx(50.0f), 1e-3f, L"the flat source did not span the box");
    Assert::AreEqual(100.0f, flat.YPx(10.0f), 1e-3f, L"the axis with no extent did not centre");

    // One point. There is no scale that means anything, so the fit must be finite and centred.
    const Neuron::BoxFit point = Neuron::FitBoxIsotropic(7.0f, 7.0f, 7.0f, 7.0f, 0.0f, 0.0f, 400.0f, 200.0f);
    Assert::AreEqual(1.0f, point.scale, 1e-5f, L"a source with no extent should fit at scale 1");
    Assert::AreEqual(200.0f, point.XPx(7.0f), 1e-3f, L"the single point did not centre across");
    Assert::AreEqual(100.0f, point.YPx(7.0f), 1e-3f, L"the single point did not centre down");
  }

  TEST_METHOD(ADestinationWithNoRoomDrawsNothingRatherThanInsideOut)
  {
    // A window mid-resize. The scale must not come back positive, because a negative one drawn as if
    // it were positive is a picture mirrored through its own centre.
    const Neuron::BoxFit fit = FitInto(0.0f, 0.0f);
    Assert::IsTrue(fit.scale <= 0.0f, L"an empty destination produced a drawable scale");
  }
};
} // namespace NeuronClientTests
