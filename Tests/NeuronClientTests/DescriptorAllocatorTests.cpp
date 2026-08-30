#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronClientTests
{
TEST_CLASS(DescriptorAllocatorTests)
{
public:
  TEST_METHOD(SlotsComeOutInOrderUntilTheHighWaterMark)
  {
    Neuron::DescriptorAllocator slots;
    slots.Init(4);
    for (std::uint32_t expected = 0; expected < 4; ++expected)
      Assert::AreEqual(expected, slots.Allocate(), L"fresh slots are not sequential");
    Assert::AreEqual(4u, slots.LiveCount());
  }

  TEST_METHOD(ExhaustionRefusesAndSaysSo)
  {
    Neuron::DescriptorAllocator slots;
    slots.Init(2);
    (void)slots.Allocate();
    (void)slots.Allocate();
    Assert::AreEqual(Neuron::DescriptorAllocator::INVALID_SLOT, slots.Allocate(), L"a full allocator handed out a slot");
    Assert::AreEqual(2u, slots.LiveCount(), L"a refused allocation changed the live count");
  }

  TEST_METHOD(ReuseIsLastInFirstOut)
  {
    // HandleStore's rule for the same reason: reuse is then reproducible, which matters the day a
    // scene is rebuilt from a recording rather than from a keypress.
    Neuron::DescriptorAllocator slots;
    slots.Init(8);
    for (int at = 0; at < 5; ++at)
      (void)slots.Allocate();
    slots.Free(1);
    slots.Free(3);
    Assert::AreEqual(3u, slots.Allocate(), L"the most recently freed slot did not come back first");
    Assert::AreEqual(1u, slots.Allocate(), L"the older freed slot did not come back second");
    Assert::AreEqual(5u, slots.Allocate(), L"the high-water mark did not resume after the free list drained");
  }

  TEST_METHOD(ADoubleFreeIsIgnored)
  {
    Neuron::DescriptorAllocator slots;
    slots.Init(4);
    (void)slots.Allocate();
    (void)slots.Allocate();
    slots.Free(0);
    slots.Free(0); // must not put the slot on the free list twice
    Assert::AreEqual(1u, slots.LiveCount());
    Assert::AreEqual(0u, slots.Allocate(), L"the freed slot did not come back");
    Assert::AreEqual(2u, slots.Allocate(), L"a double free duplicated a slot on the free list");
  }

  TEST_METHOD(AFreeOutOfRangeIsIgnored)
  {
    Neuron::DescriptorAllocator slots;
    slots.Init(2);
    (void)slots.Allocate();
    slots.Free(7);
    slots.Free(Neuron::DescriptorAllocator::INVALID_SLOT);
    Assert::AreEqual(1u, slots.LiveCount(), L"an out-of-range free changed the live count");
  }
};
} // namespace NeuronClientTests
