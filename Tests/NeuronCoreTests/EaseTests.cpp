#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronCoreTests
{
TEST_CLASS(EaseTests)
{
public:
  TEST_METHOD(AHalfLifeHalvesTheRemainingDistance)
  {
    // The property the whole easing scheme rests on: after one half-life, half the error is gone.
    Assert::AreEqual(0.5f, Neuron::HalfLifeBlend(1.0f, 1.0f), 1e-5f, L"one half-life did not halve the error");
    Assert::AreEqual(0.75f, Neuron::HalfLifeBlend(2.0f, 1.0f), 1e-5f, L"two half-lives did not quarter the error");
    Assert::AreEqual(0.0f, Neuron::HalfLifeBlend(0.0f, 1.0f), 1e-5f, L"a zero step moved something");
  }

  TEST_METHOD(TheSameHalfLifeSettlesTheSameAtAnyFrameRate)
  {
    // What makes the easing framerate-independent, and the reason nothing in this tree eases with a
    // per-frame constant. One second of easing has to land in the same place whether it was
    // delivered in 60 steps or 240.
    const auto settle = [](int _steps)
    {
      const float dt = 1.0f / static_cast<float>(_steps);
      float value = 0.0f;
      for (int i = 0; i < _steps; ++i)
        value = Neuron::SmoothTowards(value, 1.0f, dt, 0.25f);
      return value;
    };

    Assert::AreEqual(settle(60), settle(240), 1e-4f, L"the frame rate changed where the easing landed");
  }

  TEST_METHOD(MoveTowardsIsRateLimitedAndLands)
  {
    Assert::AreEqual(0.5f, Neuron::MoveTowards(0.0f, 10.0f, 0.5f), 1e-5f, L"the step was not rate limited");
    Assert::AreEqual(-0.5f, Neuron::MoveTowards(0.0f, -10.0f, 0.5f), 1e-5f, L"the step was not rate limited downwards");
    Assert::AreEqual(10.0f, Neuron::MoveTowards(9.9f, 10.0f, 0.5f), 1e-5f, L"the last step overshot instead of landing");
  }

  TEST_METHOD(ASpringOvershootsAndSettles)
  {
    float value = 0.0f;
    float velocity = 0.0f;
    float peak = 0.0f;
    for (int i = 0; i < 600; ++i)
    {
      Neuron::SpringTowards(value, velocity, 1.0f, 1.35f, 0.09f, 1.0f / 240.0f);
      peak = std::max(peak, value);
    }
    Assert::IsTrue(peak > 1.0f, L"the spring never overshot its target");
    Assert::AreEqual(1.0f, value, 1e-2f, L"the spring did not settle on its target");
  }

  TEST_METHOD(ASpringWithATinyStepStaysStable)
  {
    // The omega cap: tuning the settle half-life to nothing must not turn the spring into an
    // explosion, because a tuning value is something a person drags to the end of its range.
    float value = 0.0f;
    float velocity = 0.0f;
    for (int i = 0; i < 400; ++i)
      Neuron::SpringTowards(value, velocity, 1.0f, 1.35f, 0.0f, 1.0f / 30.0f);
    Assert::IsTrue(std::isfinite(value), L"the spring diverged");
    Assert::IsTrue(std::fabs(value) < 10.0f, L"the spring blew up");
  }
};
} // namespace NeuronCoreTests
