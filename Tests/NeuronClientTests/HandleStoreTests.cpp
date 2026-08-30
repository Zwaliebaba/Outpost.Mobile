#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronClientTests
{
namespace
{
// Widened for the assertions: a slot and a generation are both parts of a u32 and print as numbers,
// but the framework needs a type it knows how to show.
[[nodiscard]] std::uint32_t Count(std::uint32_t _value)
{
  return _value;
}
} // namespace

TEST_CLASS(HandleStoreTests)
{
public:
  TEST_METHOD(AFreshStoreHandsOutAscendingSlots)
  {
    Neuron::HandleStore store;
    Assert::AreEqual(Count(0), Count(store.LiveCount()), L"a fresh store is not empty");
    Assert::AreEqual(Count(0), Count(store.SlotCount()), L"a fresh store already has slots");

    for (std::uint32_t at = 0; at < 8; ++at)
    {
      const Neuron::HandleStore::Allocation allocation = store.Alloc();
      Assert::AreEqual(at, allocation.slot, L"a fresh store did not hand out the next slot");
      Assert::AreEqual(at, store.SlotOf(allocation.handle), L"a handle did not resolve to its own slot");
      Assert::AreNotEqual(Neuron::HandleStore::INVALID_HANDLE, allocation.handle, L"a fresh store handed out the null handle");
    }
    Assert::AreEqual(Count(8), Count(store.LiveCount()), L"the live count did not follow the allocations");
    Assert::AreEqual(Count(8), Count(store.SlotCount()), L"the slot count did not follow the allocations");
  }

  TEST_METHOD(AFreedHandleStopsResolvingAndItsSlotComesBack)
  {
    // The whole point: the slot is reused and the handle to what used to be in it is not fooled.
    Neuron::HandleStore store;
    const Neuron::HandleStore::Allocation first = store.Alloc();
    const Neuron::HandleStore::Allocation second = store.Alloc();

    Assert::AreEqual(first.slot, store.Free(first.handle), L"Free did not report the slot to release");
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(first.handle), L"a freed handle still resolves");
    Assert::AreEqual(second.slot, store.SlotOf(second.handle), L"freeing one handle disturbed another");
    Assert::AreEqual(Count(1), Count(store.LiveCount()), L"the live count did not follow the free");
    Assert::AreEqual(Count(2), Count(store.SlotCount()), L"the slot count shrank, so the payload array would have to move");

    const Neuron::HandleStore::Allocation reused = store.Alloc();
    Assert::AreEqual(first.slot, reused.slot, L"the freed slot was not reused");
    Assert::AreNotEqual(first.handle, reused.handle, L"the reused slot reissued the dead handle");
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(first.handle), L"the dead handle resolves again after the reuse");
    Assert::AreEqual(reused.slot, store.SlotOf(reused.handle), L"the reused handle does not resolve");
    Assert::AreEqual(Count(2), Count(store.SlotCount()), L"reusing a slot grew the store");
  }

  TEST_METHOD(ADoubleFreeIsANoOpThatSaysSo)
  {
    // It has to say so rather than silently succeed: the caller releases a GPU resource at the slot
    // Free reports, and a second release would drop a reference somebody else now owns.
    Neuron::HandleStore store;
    const Neuron::HandleStore::Allocation only = store.Alloc();
    Assert::AreEqual(only.slot, store.Free(only.handle), L"the first free did not report a slot");
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.Free(only.handle), L"a double free reported a slot to release again");
    Assert::AreEqual(Count(0), Count(store.LiveCount()), L"a double free moved the live count twice");

    // And the slot is on the free list exactly once, or the next two allocations would collide.
    const Neuron::HandleStore::Allocation a = store.Alloc();
    const Neuron::HandleStore::Allocation b = store.Alloc();
    Assert::AreNotEqual(a.slot, b.slot, L"a double free put one slot on the list twice");
  }

  TEST_METHOD(TheNullHandleAndAnInventedOneResolveToNothing)
  {
    Neuron::HandleStore store;
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(Neuron::HandleStore::INVALID_HANDLE),
                     L"the null handle resolved in an empty store");
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(0), L"a zero handle resolved in an empty store");

    (void)store.Alloc();
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(Neuron::HandleStore::INVALID_HANDLE),
                     L"the null handle resolved once a slot existed");
    // Slot 0 with a generation nobody issued. This is the case a bare index would have got wrong.
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(0x00070000u), L"an invented generation resolved");
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(9999u), L"a slot past the end resolved");
  }

  TEST_METHOD(AFabricatedHandleOnAFreedSlotResolvesToNothing)
  {
    // The case the `live` flag exists for, and the one the generation alone cannot catch. Free bumps
    // the generation, so the value the freed slot is *now* on has never been issued to anybody -- and
    // a handle carrying it would otherwise resolve to a slot whose payload has just been released.
    Neuron::HandleStore store;
    const Neuron::HandleStore::Allocation only = store.Alloc();
    Assert::AreEqual(only.slot, store.Free(only.handle), L"the free did not report a slot");

    const std::uint32_t issued = Neuron::HandleStore::GenerationOf(only.handle);
    const std::uint32_t fabricated = ((issued + 1u) << 16) | only.slot;
    Assert::AreNotEqual(only.handle, fabricated, L"the fabricated handle is the issued one, so this proves nothing");
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(fabricated),
                     L"a handle nobody was issued resolved to a slot that had been freed");
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.Free(fabricated), L"a fabricated handle freed a slot a second time");
  }

  TEST_METHOD(TheStoreFillsToItsCapAndThenRefuses)
  {
    // The reserved slot, driven to rather than reasoned about. 0xFFFF is never allocated so that a
    // live handle can never be all ones; a store that handed it out would issue a handle equal to
    // INVALID_MESH, and every `mesh != INVALID_MESH` test in the renderer would quietly skip it.
    Neuron::HandleStore store;
    for (std::uint32_t at = 0; at < Neuron::HandleStore::MAX_SLOTS; ++at)
    {
      const Neuron::HandleStore::Allocation allocation = store.Alloc();
      Assert::AreEqual(at, allocation.slot, L"the store did not hand out slots in order up to its cap");
      Assert::AreNotEqual(Neuron::HandleStore::INVALID_HANDLE, allocation.handle, L"an allocation collided with the null handle");
    }
    Assert::AreEqual(Count(Neuron::HandleStore::MAX_SLOTS), Count(store.SlotCount()), L"the store did not fill to its cap");

    // Full: a content load reports and carries on rather than crashing (AGENTS.md 5).
    const Neuron::HandleStore::Allocation refused = store.Alloc();
    Assert::AreEqual(Neuron::HandleStore::INVALID_HANDLE, refused.handle, L"a full store handed out a handle");
    Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, refused.slot, L"a full store handed out a slot");
    Assert::AreEqual(Count(Neuron::HandleStore::MAX_SLOTS), Count(store.SlotCount()), L"a refused allocation grew the store");
  }

  TEST_METHOD(ALiveHandleIsNeverTheNullHandle)
  {
    // Slot 0xFFFF is never allocated, so a live handle cannot be all ones. That is what lets
    // INVALID_MESH stay 0xFFFFFFFF with no special case at any call site.
    Neuron::HandleStore store;
    for (std::uint32_t at = 0; at < 64; ++at)
    {
      const Neuron::HandleStore::Allocation allocation = store.Alloc();
      Assert::AreNotEqual(Neuron::HandleStore::INVALID_HANDLE, allocation.handle, L"an allocation collided with the null handle");
      Assert::IsTrue(Neuron::HandleStore::SlotBitsOf(allocation.handle) < Neuron::HandleStore::MAX_SLOTS,
                     L"a slot was allocated in the reserved range");
    }
  }

  TEST_METHOD(FreeAllEmptiesTheStoreAndRefillsIntoTheSameSlots)
  {
    // What F5 does: clear the scene, build another. The second scene has to land in the first one's
    // slots, or the payload array grows by a scene on every press -- which is the leak this store
    // exists to close.
    Neuron::HandleStore store;
    std::vector<std::uint32_t> firstSlots;
    for (std::uint32_t at = 0; at < 8; ++at)
      firstSlots.push_back(store.Alloc().slot);

    std::vector<std::uint32_t> released;
    store.FreeAll([&](std::uint32_t _slot) { released.push_back(_slot); });
    Assert::AreEqual(static_cast<std::size_t>(8), released.size(), L"FreeAll did not release every live slot");
    Assert::AreEqual(Count(0), Count(store.LiveCount()), L"FreeAll left something live");
    Assert::AreEqual(Count(8), Count(store.SlotCount()), L"FreeAll shrank the payload array");

    std::vector<std::uint32_t> secondSlots;
    for (std::uint32_t at = 0; at < 8; ++at)
      secondSlots.push_back(store.Alloc().slot);
    Assert::IsTrue(firstSlots == secondSlots, L"a cleared store did not refill into the same slots in the same order");
    Assert::AreEqual(Count(8), Count(store.SlotCount()), L"refilling grew the store");
  }

  TEST_METHOD(TenClearAndRefillCyclesHoldTheStoreFlat)
  {
    // The acceptance, as a number: ten F5 reseeds must not grow the store. Nine bodies a scene,
    // because the tree spawns eight plus a planet and an off-by-one in the free list would show up
    // as a slot count that creeps by one a press rather than by a scene.
    Neuron::HandleStore store;
    std::vector<std::uint32_t> handles;
    for (int cycle = 0; cycle < 10; ++cycle)
    {
      handles.clear();
      for (int at = 0; at < 9; ++at)
        handles.push_back(store.Alloc().handle);
      Assert::AreEqual(Count(9), Count(store.LiveCount()), L"a scene did not come out the size it went in");
      Assert::AreEqual(Count(9), Count(store.SlotCount()), L"the store grew across a reseed");

      for (const std::uint32_t handle : handles)
        Assert::AreNotEqual(Neuron::HandleStore::INVALID_SLOT, store.Free(handle), L"a live handle would not free");
    }
    Assert::AreEqual(Count(0), Count(store.LiveCount()), L"the last scene did not clear");
    Assert::AreEqual(Count(9), Count(store.SlotCount()), L"ten reseeds grew the store");

    // And every handle from the last cycle is stale, which is what stops a draw reaching a resource
    // that has been released.
    for (const std::uint32_t handle : handles)
      Assert::AreEqual(Neuron::HandleStore::INVALID_SLOT, store.SlotOf(handle), L"a handle survived its own scene");
  }

  TEST_METHOD(AGenerationWrapNeverReissuesALiveHandle)
  {
    // 65,535 reuses of one slot is not reachable by hand -- it is 65,535 F5 presses -- but the wrap
    // has to be right anyway, because getting it wrong means one handle in 65,535 silently names a
    // resource that was freed. Driven to the wrap directly rather than assumed.
    Neuron::HandleStore store;
    std::uint32_t handle = store.Alloc().handle;
    const std::uint32_t firstGeneration = Neuron::HandleStore::GenerationOf(handle);
    Assert::AreEqual(Count(1), Count(firstGeneration), L"a slot's first generation is not 1");

    bool wrapped = false;
    for (std::uint32_t cycle = 0; cycle < 70000; ++cycle)
    {
      Assert::AreNotEqual(Neuron::HandleStore::INVALID_SLOT, store.Free(handle), L"a live handle would not free");
      const Neuron::HandleStore::Allocation next = store.Alloc();
      Assert::AreEqual(Count(0), Count(next.slot), L"the store stopped reusing its one slot");
      Assert::AreNotEqual(Count(0), Count(Neuron::HandleStore::GenerationOf(next.handle)), L"a generation of 0 was issued");
      wrapped = wrapped || (cycle > 0 && Neuron::HandleStore::GenerationOf(next.handle) == firstGeneration);
      handle = next.handle;
    }
    Assert::IsTrue(wrapped, L"70,000 cycles did not reach the wrap, so this test proved nothing");
    Assert::AreEqual(Count(1), Count(store.SlotCount()), L"70,000 cycles grew the store");
  }

  TEST_METHOD(ManySlotsResolveToThemselves)
  {
    // A sweep rather than a sample, because a packing error shows up at one bit position and a test
    // that allocates three handles never reaches it.
    Neuron::HandleStore store;
    std::vector<std::uint32_t> handles;
    for (std::uint32_t at = 0; at < 5000; ++at)
      handles.push_back(store.Alloc().handle);

    for (std::uint32_t at = 0; at < handles.size(); ++at)
      Assert::AreEqual(at, store.SlotOf(handles[at]), L"a handle in a large store resolved to the wrong slot");

    // Free every third one, then refill and check nothing crossed over.
    std::vector<std::uint32_t> freed;
    for (std::uint32_t at = 0; at < handles.size(); at += 3)
    {
      Assert::AreEqual(at, store.Free(handles[at]), L"a live handle would not free");
      freed.push_back(at);
    }
    for (std::uint32_t at = 0; at < handles.size(); ++at)
    {
      const bool wasFreed = (at % 3) == 0;
      const std::uint32_t resolved = store.SlotOf(handles[at]);
      Assert::AreEqual(wasFreed ? Neuron::HandleStore::INVALID_SLOT : at, resolved, L"a handle resolved wrongly after a partial free");
    }

    // The free list is last-in-first-out, so the refill walks the freed slots backwards.
    for (std::size_t at = freed.size(); at > 0; --at)
      Assert::AreEqual(freed[at - 1], store.Alloc().slot, L"the free list is not last-in-first-out");
    Assert::AreEqual(Count(5000), Count(store.SlotCount()), L"the refill grew the store");
  }
};
} // namespace NeuronClientTests
