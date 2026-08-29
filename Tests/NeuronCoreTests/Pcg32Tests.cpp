#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronCoreTests
{
// The published output of the PCG reference's own pcg32-demo for pcg32_srandom_r(42, 54), round 1.
// An implementation that does not reproduce these is not PCG32, whatever else it is, and a stream
// that does not match the reference is a stream nothing else can replay.
constexpr std::uint32_t REFERENCE_42_54[] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu};

// The generator has to be usable at compile time, so that a value derived from a seed can be a
// constant. Pinning the first output here proves it, and the test below proves the rest.
static_assert(
  []
    {
      Neuron::Pcg32 rng(42u, 54u);
      return rng.Next();
    }() == 0xa15c02b7u,
  "Pcg32 is not constexpr, or does not produce the reference stream");

TEST_CLASS(Pcg32Tests)
{
public:
  TEST_METHOD(TheStreamIsTheReferenceStream)
  {
    Neuron::Pcg32 rng(42u, 54u);
    for (int i = 0; i < 5; ++i)
      Assert::AreEqual(REFERENCE_42_54[i], rng.Next(), L"the stream diverged from the PCG reference vectors");
  }

  TEST_METHOD(TheDefaultIsTheReferenceDefault)
  {
    Neuron::Pcg32 defaulted;
    Neuron::Pcg32 spelled(Neuron::Pcg32::DEFAULT_SEED, Neuron::Pcg32::DEFAULT_SEQUENCE);
    for (int i = 0; i < 100; ++i)
      Assert::AreEqual(spelled.Next(), defaulted.Next(), L"the default constructor is not the default seed and sequence");
  }

  TEST_METHOD(TheSameSeedReplaysTheSameStream)
  {
    // The property a seed exists for. Everything that wants a reproducible explosion, or a replayed
    // tick, rests on exactly this.
    Neuron::Pcg32 first(12345u);
    Neuron::Pcg32 second(12345u);
    for (int i = 0; i < 1000; ++i)
      Assert::AreEqual(first.Next(), second.Next(), L"two generators seeded alike produced different values");
  }

  TEST_METHOD(TheSequenceSelectsADifferentStream)
  {
    // Same seed, different sequence: two independent streams, not two offsets into one. They part
    // company on the first value or the sequence is not doing anything.
    Neuron::Pcg32 first(42u, 54u);
    Neuron::Pcg32 second(42u, 55u);
    Assert::AreNotEqual(first.Next(), second.Next(), L"the sequence did not select a different stream");
  }

  TEST_METHOD(BelowStaysInRangeAndCoversIt)
  {
    Neuron::Pcg32 rng(1234u);
    int counts[5] = {};
    for (int i = 0; i < 100000; ++i)
    {
      const std::uint32_t draw = rng.Below(5u);
      Assert::IsTrue(draw < 5u, L"Below returned a value at or above its bound");
      ++counts[draw];
    }

    // A modulo of the raw output would still pass the range check above; only the coverage of every
    // bucket says the rejection is doing its job.
    for (int i = 0; i < 5; ++i)
      Assert::IsTrue(counts[i] >= 15000, L"one of five buckets was starved, so the draw is biased");
  }

  TEST_METHOD(BelowHandlesTheDegenerateBounds)
  {
    Neuron::Pcg32 rng(1234u);
    for (int i = 0; i < 1000; ++i)
      Assert::AreEqual(0u, rng.Below(1u), L"Below(1) returned something other than 0");

    // Zero is the caller's mistake, not a divide by zero. An empty range has one sensible answer.
    Assert::AreEqual(0u, rng.Below(0u), L"Below(0) did not return 0");
  }

  TEST_METHOD(Float01StaysInRangeAndIsCentred)
  {
    Neuron::Pcg32 rng(99u);
    double total = 0.0;
    for (int i = 0; i < 100000; ++i)
    {
      const float draw = rng.Float01();
      Assert::IsTrue(draw >= 0.0f && draw < 1.0f, L"Float01 left [0, 1)");
      total += draw;
    }
    Assert::AreEqual(0.5, total / 100000.0, 0.01, L"Float01 is not uniform over [0, 1)");
  }

  TEST_METHOD(SignedStraddlesZero)
  {
    Neuron::Pcg32 rng(7u);
    double total = 0.0;
    for (int i = 0; i < 100000; ++i)
    {
      const float draw = rng.Signed(100.0f);
      Assert::IsTrue(draw >= -100.0f && draw < 100.0f, L"Signed left [-magnitude, +magnitude)");
      total += draw;
    }
    Assert::AreEqual(0.0, total / 100000.0, 1.0, L"Signed is not centred on zero");
  }

  TEST_METHOD(ReseedingRestartsTheStream)
  {
    Neuron::Pcg32 rng(42u, 54u);
    for (int i = 0; i < 17; ++i)
      (void)rng.Next();

    rng.Seed(42u, 54u);
    for (int i = 0; i < 5; ++i)
      Assert::AreEqual(REFERENCE_42_54[i], rng.Next(), L"Seed did not put the generator back at the start of the stream");
  }
};
} // namespace NeuronCoreTests
