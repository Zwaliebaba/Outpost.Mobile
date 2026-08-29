#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronClientTests
{
TEST_CLASS(Noise3Tests)
{
public:
  TEST_METHOD(EverySampleIsInsideTheHalfUnitRange)
  {
    // The amplitude law in Design/PlanetRenderer.md 5.2 multiplies this output by numbers in the
    // hundreds, so a sample that escaped its range would not look like a bug in the noise -- it would
    // look like a spike on a planet, three files away.
    Neuron::Pcg32 rng(7u);
    const Neuron::Noise3 noise(rng);

    float lowest = 1.0f;
    float highest = -1.0f;
    for (int x = 0; x < 32; ++x)
    {
      for (int y = 0; y < 32; ++y)
      {
        for (int z = 0; z < 32; ++z)
        {
          const float sample = noise.Sample(x * 0.37f + 0.11f, y * 0.53f - 0.29f, z * 0.71f + 0.07f);
          lowest = std::min(lowest, sample);
          highest = std::max(highest, sample);
        }
      }
    }

    Assert::IsTrue(lowest >= -0.5f, L"a sample went below -0.5");
    Assert::IsTrue(highest <= 0.5f, L"a sample went above 0.5");

    // And it does use the range it claims: a noise that never left [-0.05, 0.05] would pass the two
    // assertions above and produce a flat world.
    Assert::IsTrue(lowest < -0.4f && highest > 0.4f, L"the noise does not reach the ends of its own range");
  }

  TEST_METHOD(TheLatticePointsAreExactlyZero)
  {
    // Every gradient at an integer point is dotted with a zero displacement, so the answer is zero
    // rather than nearly zero. It is worth pinning because it is the cheapest possible check that the
    // fade weights and the corner lookups line up: get either wrong and this stops being exact.
    Neuron::Pcg32 rng(1u);
    const Neuron::Noise3 noise(rng);

    Assert::AreEqual(0.0f, noise.Sample(3.0f, 7.0f, -2.0f), L"an integer lattice point was not exactly zero");
    Assert::AreEqual(0.0f, noise.Sample(0.0f, 0.0f, 0.0f), L"the origin was not exactly zero");
    Assert::AreEqual(0.0f, noise.Sample(-11.0f, 4.0f, 9.0f), L"a negative lattice point was not exactly zero");
  }

  TEST_METHOD(TheSameSeedIsTheSameFunctionAndADifferentSeedIsNot)
  {
    // This is the replay guarantee at its smallest: a body is described by a seed, and everything
    // that follows from the seed has to follow from it alone (Design/PlanetRenderer.md 10).
    Neuron::Pcg32 first(11u);
    Neuron::Pcg32 second(11u);
    Neuron::Pcg32 other(12u);
    const Neuron::Noise3 a(first);
    const Neuron::Noise3 b(second);
    const Neuron::Noise3 c(other);

    for (int i = 0; i < 1000; ++i)
    {
      const float t = i * 0.017f;
      Assert::AreEqual(a.Sample(t, t * 1.7f, t * 0.3f), b.Sample(t, t * 1.7f, t * 0.3f),
                       L"two generators seeded alike produced different noise");
    }

    Assert::AreNotEqual(a.Sample(0.3f, 0.4f, 0.5f), c.Sample(0.3f, 0.4f, 0.5f), L"two different seeds produced the same noise");
  }

  TEST_METHOD(TheNoiseIsContinuous)
  {
    // A discontinuity here is a crack across a planet. A thousandth of a lattice cell may move the
    // sample by a hundredth at most, which is well inside the gradient the quintic fade allows and
    // nowhere near the half a unit a wrong corner lookup would jump by.
    Neuron::Pcg32 rng(3u);
    const Neuron::Noise3 noise(rng);
    Neuron::Pcg32 pick(99u);

    float worst = 0.0f;
    for (int i = 0; i < 1000; ++i)
    {
      const float x = pick.Signed(50.0f);
      const float y = pick.Signed(50.0f);
      const float z = pick.Signed(50.0f);
      worst = std::max(worst, std::fabs(noise.Sample(x + 0.001f, y, z) - noise.Sample(x, y, z)));
    }

    Assert::IsTrue(worst < 0.01f, L"a thousandth of a step moved the noise by more than a hundredth");
  }

  TEST_METHOD(AnAdoptedPermutationIsTheSameFunction)
  {
    // BodyParams carries the permutation so that a compute kernel can read it, and BodyField builds
    // its noise back out of the block rather than keeping a second copy (Design/PlanetRenderer.md
    // 17.3). The two constructors have to agree for that to be safe.
    Neuron::Pcg32 rng(23u);
    const Neuron::Noise3 shuffled(rng);
    const Neuron::Noise3 adopted(shuffled.Permutation());

    for (int i = 0; i < 500; ++i)
    {
      const float t = i * 0.023f;
      Assert::AreEqual(shuffled.Sample(t, t * 0.7f, -t), adopted.Sample(t, t * 0.7f, -t),
                       L"the adopted permutation is a different function");
    }
  }

  TEST_METHOD(ThePermutationIsAPermutation)
  {
    // A Fisher-Yates that indexes one past its range, or draws the wrong bound, loses values and
    // repeats others. The noise still looks like noise; it is simply a worse one, with a bias nobody
    // would ever find by eye.
    Neuron::Pcg32 rng(5u);
    const Neuron::Noise3 noise(rng);

    bool seen[Neuron::Noise3::PERMUTATION_SIZE] = {};
    for (const std::uint32_t value : noise.Permutation())
    {
      Assert::IsTrue(value < Neuron::Noise3::PERMUTATION_SIZE, L"the permutation holds a value outside its own range");
      Assert::IsFalse(seen[value], L"the permutation holds the same value twice");
      seen[value] = true;
    }
  }
};
} // namespace NeuronClientTests
