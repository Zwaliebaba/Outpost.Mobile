#pragma once

#include <cstdint>
#include <vector>

namespace Neuron
{
// The bookkeeping behind a MeshHandle: which slots are live, which are free, and which generation
// each slot is on.
//
// It holds no D3D12 and no payload. The store that owns the resources keeps those in an array this
// indexes, which is what lets a renderer hand out a handle that survives the resource being freed
// and the slot being reused -- and what lets this be tested without a device (ADR 0044).
//
// Both renderers grew forever before it: MeshHandle and BodyHandle were array indices, UploadMesh
// and BakeBody only ever push_back'd, and F5's reseed left the scene it replaced on the GPU because
// there was nothing that could say a body was finished with
// (Design/MmoScalabilityReview.md G3, OutpostApp.cpp's own note beside the key).
//
// This is ShipHandle's argument on the client's side of the wire (ADR 0005), and the shape differs
// for one reason: a MeshHandle is a std::uint32_t in a dozen places -- ShipView::mesh, the hull
// table, INVALID_MESH -- so the generation is packed into the 32 bits it already had rather than
// widening the type. Sixteen bits each: 65,535 live meshes and 65,535 reuses of one slot before a
// generation wraps. The tree has eleven hulls and eight bodies, and a wrap needs 65,535 F5 presses.
class HandleStore
{
public:
  // Slot 0xFFFF is never allocated, so a live handle can never be all ones and INVALID_HANDLE never
  // collides with one. That costs the last slot of 65,536 and buys a null value that needs no
  // special case anywhere.
  static constexpr std::uint32_t MAX_SLOTS = 0xFFFFu;
  static constexpr std::uint32_t INVALID_HANDLE = 0xFFFFFFFFu;
  static constexpr std::uint32_t INVALID_SLOT = 0xFFFFFFFFu;

  // What Alloc hands back: the handle for the caller to give out, and the slot to write the payload
  // at. Both, because the caller needs the slot now and the handle later, and returning one and
  // making the caller ask for the other is two places that can disagree.
  struct Allocation
  {
    std::uint32_t handle = INVALID_HANDLE;
    std::uint32_t slot = INVALID_SLOT;
  };

  // Takes the most recently freed slot, or a new one past the high-water mark. Last-in-first-out for
  // the reason World's free list is: reuse is then reproducible, which matters the day a scene is
  // rebuilt from a recording rather than from a keypress.
  //
  // Returns INVALID_HANDLE when every slot is live. That is a diagnostic and not a crash: the caller
  // is a content load, and AGENTS.md 5's rule for content is that it reports and carries on.
  [[nodiscard]] Allocation Alloc();

  // Where a live handle's payload is, or INVALID_SLOT if the handle is stale, null, or was never
  // issued. This is the one lookup: a caller that has a handle asks here and indexes its own array.
  [[nodiscard]] std::uint32_t SlotOf(std::uint32_t _handle) const noexcept;

  // Retires a handle and returns its slot for the caller to release the payload at, or INVALID_SLOT
  // if the handle was already stale -- so a double free is a no-op that says so rather than a second
  // release of a resource somebody else now owns.
  [[nodiscard]] std::uint32_t Free(std::uint32_t _handle) noexcept;

  // Retires every live handle, newest first, and calls _release(slot) for each. Newest first so the
  // free list comes out in the order a run of Allocs would have produced, which keeps a clear and
  // refill -- F5 -- landing in the same slots every time.
  template <typename Fn> void FreeAll(Fn&& _release)
  {
    for (std::uint32_t slot = static_cast<std::uint32_t>(m_slots.size()); slot > 0; --slot)
    {
      const std::uint32_t at = slot - 1;
      if (!m_slots[at].live)
        continue;
      Retire(at);
      _release(at);
    }
  }

  // How many handles are live. This is what a "how many bodies are there" readout means, and it is
  // not the same as SlotCount once anything has been freed.
  [[nodiscard]] std::uint32_t LiveCount() const noexcept
  {
    return m_liveCount;
  }

  // The high-water mark: how long the caller's payload array has to be. It never shrinks, because a
  // slot that has been used once is the cheapest slot to use again.
  [[nodiscard]] std::uint32_t SlotCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_slots.size());
  }

  [[nodiscard]] static constexpr std::uint32_t SlotBitsOf(std::uint32_t _handle) noexcept
  {
    return _handle & 0xFFFFu;
  }

  [[nodiscard]] static constexpr std::uint32_t GenerationOf(std::uint32_t _handle) noexcept
  {
    return _handle >> 16;
  }

private:
  void Retire(std::uint32_t _slot) noexcept;

  struct Entry
  {
    // Starts at 1, so a slot's first handle is not generation 0 and a zero-initialised handle names
    // nothing. Wraps back to 1 rather than to 0 for the same reason.
    std::uint16_t generation = 1;
    bool live = false;
  };

  std::vector<Entry> m_slots;
  std::vector<std::uint32_t> m_freeSlots; // last-in-first-out
  std::uint32_t m_liveCount = 0;
};
} // namespace Neuron
