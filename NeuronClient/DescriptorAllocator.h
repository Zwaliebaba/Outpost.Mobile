#pragma once

#include <cstdint>
#include <vector>

namespace Neuron
{
// The bookkeeping behind a shader-visible descriptor slot: which of a fixed capacity are live and
// which are free.
//
// It holds no D3D12, exactly as HandleStore holds none: the device-facing half -- the one shared
// CBV_SRV_UAV heap, its stride, the CPU and GPU handles a slot resolves to -- lives on GpuDevice,
// which owns the heap the way it owns the RTV and DSV heaps. That split is what lets this be
// tested without a device, and it is the whole reason the per-pass constant-sized heaps could go:
// a pass asks for slots instead of owning a heap sized to today's content exactly
// (Design/MmoScalabilityReview.md G4, Design/CompressedTextures-work-order.md 2.1).
//
// No generations, deliberately, where HandleStore carries them. A mesh handle is given out and
// outlives the resource it names, so a stale one must resolve to nothing; a descriptor slot never
// leaves the pass that allocated it, and the descriptor itself is rewritten in place on reuse.
// A lifetime nobody shares needs no stale-detection.
class DescriptorAllocator
{
public:
  static constexpr std::uint32_t INVALID_SLOT = 0xFFFFFFFFu;

  // Fixed at Init and never grown: the heap this indexes is created once, and a capacity that could
  // move would move every GPU handle already handed out.
  void Init(std::uint32_t _capacity);

  // The most recently freed slot, or a new one past the high-water mark -- last-in-first-out for
  // HandleStore's reason: reuse is reproducible. Returns INVALID_SLOT when every slot is live,
  // which is a diagnostic and not a crash: the caller is a content load, and content reports and
  // carries on (AGENTS.md 5).
  [[nodiscard]] std::uint32_t Allocate();

  // Returns a slot to the free list. Freeing a slot that is not live, or is out of range, is a
  // no-op that traces: a double free must not hand the same slot to two textures.
  void Free(std::uint32_t _slot) noexcept;

  [[nodiscard]] std::uint32_t Capacity() const noexcept
  {
    return m_capacity;
  }

  [[nodiscard]] std::uint32_t LiveCount() const noexcept
  {
    return m_liveCount;
  }

private:
  std::vector<std::uint32_t> m_free; // slots returned by Free, taken back last-in-first-out
  std::vector<bool> m_live;          // per slot, so a double free is caught rather than obeyed
  std::uint32_t m_capacity = 0;
  std::uint32_t m_next = 0; // the high-water mark: slots at and past it have never been allocated
  std::uint32_t m_liveCount = 0;
};
} // namespace Neuron
