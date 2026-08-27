#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronServerTests
{
namespace
{
// A simulation that does nothing but count, which is all the host is entitled to know about one.
class CountingSimulation final : public Neuron::Simulation
{
public:
  void Step() override { ++m_steps; }
  [[nodiscard]] std::uint64_t Tick() const override { return m_steps; }

private:
  std::uint64_t m_steps = 0;
};
} // namespace

TEST_CLASS(ServerHostTests)
{
public:
  TEST_METHOD(WholeTicksOnly)
  {
    CountingSimulation simulation;
    Neuron::ServerHost host;
    Neuron::ServerHost::Desc desc;
    desc.tickHz = 60.0f;
    host.Init(desc, simulation);

    // Half a tick is not a tick, however often it is delivered.
    Assert::AreEqual(0, host.Advance(1.0f / 120.0f), L"half a frame ran a tick");
    Assert::AreEqual(1, host.Advance(1.0f / 120.0f), L"two halves did not make one tick");
    Assert::AreEqual(std::uint64_t{1}, host.Tick(), L"the tick count disagrees with the steps run");
  }

  TEST_METHOD(ASlowFrameCatchesUp)
  {
    CountingSimulation simulation;
    Neuron::ServerHost host;
    Neuron::ServerHost::Desc desc;
    desc.tickHz = 60.0f;
    host.Init(desc, simulation);

    Assert::AreEqual(6, host.Advance(0.1f), L"a 100 ms frame did not run six 60 Hz ticks");
  }

  TEST_METHOD(AStallDoesNotSpiral)
  {
    // The reason the accumulator is capped: a dragged window or a breakpoint must cost a moment of
    // slow motion, not a burst of ticks that stalls the frame after it as well.
    CountingSimulation simulation;
    Neuron::ServerHost host;
    Neuron::ServerHost::Desc desc;
    desc.tickHz = 60.0f;
    desc.maxCatchUpSec = 0.25f;
    host.Init(desc, simulation);

    const int steps = host.Advance(30.0f);
    Assert::IsTrue(steps <= 15, L"a stalled frame ran an unbounded number of ticks");
  }

  TEST_METHOD(AlphaRunsBetweenTicks)
  {
    CountingSimulation simulation;
    Neuron::ServerHost host;
    Neuron::ServerHost::Desc desc;
    desc.tickHz = 60.0f;
    host.Init(desc, simulation);

    host.Advance(1.0f / 120.0f);
    Assert::AreEqual(0.5f, host.InterpolationAlpha(), 1e-3f, L"half a tick did not read as half a tick");

    host.Advance(1.0f / 120.0f);
    Assert::AreEqual(0.0f, host.InterpolationAlpha(), 1e-3f, L"a completed tick left the accumulator behind");
  }

  TEST_METHOD(AHostWithNoSimulationIsInert)
  {
    Neuron::ServerHost host;
    Assert::AreEqual(0, host.Advance(1.0f), L"an uninitialised host ran ticks");
    Assert::AreEqual(std::uint64_t{0}, host.Tick(), L"an uninitialised host reported a tick");
  }
};
} // namespace NeuronServerTests
