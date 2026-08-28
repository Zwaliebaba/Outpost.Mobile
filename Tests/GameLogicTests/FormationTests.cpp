#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace GameLogicTests
{
TEST_CLASS(FormationTests)
{
public:
  TEST_METHOD(SingleShipSitsOnThePoint)
  {
    for (int shape = 0; shape <= 3; ++shape)
    {
      const XMFLOAT2 offset = Game::FormationOffset(0, 1, static_cast<Game::FormationShape>(shape), 34.0f);
      Assert::AreEqual(0.0f, offset.x, 1e-4f, L"a lone ship is offset across the formation");
      Assert::AreEqual(0.0f, offset.y, 1e-4f, L"a lone ship is offset along the formation");
    }
  }

  TEST_METHOD(LineAbreastIsCentredAndEvenlySpaced)
  {
    constexpr int count = 4;
    constexpr float spacing = 30.0f;

    float sum = 0.0f;
    float previousX = 0.0f;
    for (int slot = 0; slot < count; ++slot)
    {
      const XMFLOAT2 offset = Game::FormationOffset(slot, count, Game::FormationShape::LineAbreast, spacing);
      Assert::AreEqual(0.0f, offset.y, 1e-4f, L"line abreast puts nothing ahead or behind");
      if (slot > 0)
        Assert::AreEqual(spacing, offset.x - previousX, 1e-3f, L"slots are evenly spaced");
      previousX = offset.x;
      sum += offset.x;
    }
    Assert::AreEqual(0.0f, sum, 1e-3f, L"the formation is centred on the ordered point");
  }

  TEST_METHOD(WedgeTrailsBehindTheLeadSlots)
  {
    constexpr int count = 5;
    const XMFLOAT2 centre = Game::FormationOffset(2, count, Game::FormationShape::Wedge, 30.0f);
    const XMFLOAT2 wing = Game::FormationOffset(0, count, Game::FormationShape::Wedge, 30.0f);
    Assert::IsTrue(wing.y < centre.y, L"the outboard slot sits behind the centre one");
  }

  TEST_METHOD(OffsetsAreStableAcrossCalls)
  {
    // The order pipeline is deterministic, so the same slot must resolve to the same place every
    // time it is asked -- including on a machine that has never run it before.
    for (int slot = 0; slot < 7; ++slot)
    {
      const XMFLOAT2 first = Game::FormationOffset(slot, 7, Game::FormationShape::Circle, 25.0f);
      const XMFLOAT2 second = Game::FormationOffset(slot, 7, Game::FormationShape::Circle, 25.0f);
      Assert::AreEqual(first.x, second.x, 0.0f, L"circle slot x is not reproducible");
      Assert::AreEqual(first.y, second.y, 0.0f, L"circle slot y is not reproducible");
    }
  }
};
} // namespace GameLogicTests
