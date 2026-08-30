#pragma once

#include "DescriptorAllocator.h"
#include "RenderTypes.h"

#include <d3d12.h>
#include <dxgi1_6.h>

namespace Neuron
{
// The device, the swapchain and the frame pacing: everything that exists once per window and
// nothing that knows what is being drawn. Passes (SceneRenderer, TextRenderer) borrow its command
// list; they never own a device of their own, so there is one place that decides when a frame
// begins, when it ends, and how deep the pipeline is allowed to run.
class GpuDevice
{
public:
  static constexpr std::uint32_t FRAME_COUNT = 3;

  // The one shader-visible CBV_SRV_UAV heap every pass allocates from. A capacity, not a budget:
  // today's content uses eighteen slots, and the constant is what to raise the day an allocation
  // traces a refusal. Fixed for the run, because a heap that could grow would move every GPU
  // handle already handed out (Design/Archive/CompressedTextures-work-order.md 2.1).
  static constexpr std::uint32_t SRV_HEAP_CAPACITY = 256;

  void Init(HWND _hwnd);
  void Shutdown();
  void Resize(std::uint32_t _widthPx, std::uint32_t _heightPx);

  // Waits for this frame's slot to come free, resets its allocator, and clears colour and depth.
  void BeginFrame(Rgba _clear);

  // Transitions to present, submits, presents, and signals the frame's fence value. Anything that
  // wants to draw on top of the scene -- the HUD -- must be flushed before this is called.
  void EndFrame();

  // Blocks until the GPU has drained. Used at resize and at shutdown, and by the one-shot uploads
  // that record into the command list during initialisation.
  void WaitForGpu();

  // Opens the command list for a batch of uploads. Every batch is bracketed by this and
  // ExecuteAndWait, and outside those brackets the list is closed -- which is what lets a second
  // thing upload at boot. It used to be left open by Init instead, and that quietly made exactly
  // one uploader possible: the next one recorded into a closed list and D3D12 rejected every call.
  //
  // It drains the GPU before it resets, so a batch opened between two frames -- F5 regenerating
  // every body -- is as safe as one opened at boot.
  void BeginUploads();

  // Closes, submits and waits for whatever BeginUploads opened. Initialisation-only: it is how a
  // resource upload that needs the copy engine finishes before the first frame.
  void ExecuteAndWait();

  // --- the copy queue --------------------------------------------------------------------------
  //
  // A batch of pure copies, on a queue of its own. Buffer and texture uploads belong here; a bake
  // does not, because a compute dispatch is not a copy and a COPY queue cannot run one -- which is
  // why the direct-queue bracket above stays rather than being replaced (ADR 0044).
  //
  // What it buys is that the CPU no longer blocks on a load. BeginUploads used to drain the whole
  // GPU because it reset frame 0's allocator; this bracket has its own allocator on its own queue,
  // so it waits only for the previous batch of copies and never for a frame.
  void BeginCopies();

  // Closes, submits, signals the copy fence, and makes the GRAPHICS queue wait on it. The CPU is
  // not blocked: that is the whole point of the pair.
  //
  // No resource barrier is written on either side of a copy recorded here, and that is documented
  // behaviour rather than an omission. Everything a copy queue touches decays to COMMON when its
  // ExecuteCommandLists completes, and a buffer or texture in COMMON is promoted implicitly on its
  // first graphics use -- to VERTEX_AND_CONSTANT_BUFFER, to PIXEL_SHADER_RESOURCE -- for free.
  // Writing the barriers by hand would be both unnecessary and illegal: a COPY queue cannot express
  // a transition to a shader-resource state at all
  // ("Using Resource Barriers to Synchronize Resource States in Direct3D 12", implicit transitions).
  void SubmitCopies();

  // The list BeginCopies opened. Only copies may be recorded into it.
  [[nodiscard]] ID3D12GraphicsCommandList* CopyList() const noexcept
  {
    return m_copyList.get();
  }

  // The value the last SubmitCopies signalled, and how far the copy queue has actually got. A
  // caller holding a staging buffer releases it once the second has reached the first; nothing may
  // release one before that, because the copy has only been recorded.
  [[nodiscard]] std::uint64_t LastCopyFence() const noexcept
  {
    return m_copyFenceValue;
  }

  [[nodiscard]] std::uint64_t CompletedCopyFence() const noexcept;

  // Blocks until every submitted copy has run. Shutdown and resize need it; a load does not, and a
  // load calling it would give back exactly what this queue was added to save.
  void WaitForCopies();

  [[nodiscard]] ID3D12Device* Device() const noexcept
  {
    return m_device.get();
  }
  [[nodiscard]] ID3D12GraphicsCommandList* CommandList() const noexcept
  {
    return m_cmd.get();
  }
  [[nodiscard]] std::uint32_t FrameIndex() const noexcept
  {
    return m_frameIndex;
  }
  [[nodiscard]] std::uint32_t WidthPx() const noexcept
  {
    return m_widthPx;
  }
  [[nodiscard]] std::uint32_t HeightPx() const noexcept
  {
    return m_heightPx;
  }
  [[nodiscard]] bool Ready() const noexcept
  {
    return m_device && m_widthPx > 0 && m_heightPx > 0;
  }

  // The shared heap and the slots inside it. Passes allocate slots at Init and keep them for the
  // run -- nothing frees per frame, and a pass that dies with the device frees nothing at all. The
  // bookkeeping lives in DescriptorAllocator, device-free and tested; the handles live here with
  // the heap, the split HandleStore set the pattern for.
  [[nodiscard]] ID3D12DescriptorHeap* SrvHeap() const noexcept
  {
    return m_srvHeap.get();
  }
  [[nodiscard]] DescriptorAllocator& SrvAllocator() noexcept
  {
    return m_srvAllocator;
  }
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(std::uint32_t _slot) const noexcept;
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(std::uint32_t _slot) const noexcept;

  // The current back buffer's view, for a pass that needs to rebind the target without depth.
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE BackBufferView() const noexcept;

  // The depth view BeginFrame bound, for a pass that rebinds the target and has to keep depth.
  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthView() const noexcept;

private:
  void CreateSizedResources();
  void ReleaseSizedResources();

  std::uint32_t m_widthPx = 0;
  std::uint32_t m_heightPx = 0;

  GpuPtr<IDXGIFactory6> m_factory;
  GpuPtr<ID3D12Device> m_device;
  GpuPtr<ID3D12CommandQueue> m_queue;
  GpuPtr<IDXGISwapChain3> m_swapChain;
  GpuPtr<ID3D12DescriptorHeap> m_rtvHeap;
  GpuPtr<ID3D12DescriptorHeap> m_dsvHeap;
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap; // the shared shader-visible heap; slots via m_srvAllocator
  DescriptorAllocator m_srvAllocator;
  std::uint32_t m_srvStride = 0;
  GpuPtr<ID3D12Resource> m_backBuffers[FRAME_COUNT];
  GpuPtr<ID3D12Resource> m_depth;
  GpuPtr<ID3D12CommandAllocator> m_allocators[FRAME_COUNT];

  // The direct-queue upload bracket's own allocator, so BeginUploads stops resetting frame 0's.
  // That reset is what forced it to drain the whole GPU first: an allocator may not be reset while
  // the GPU is still reading what it holds, and allocator 0 is a frame's. With one of its own the
  // bracket waits for the previous upload batch instead of for every frame in flight (ADR 0044).
  GpuPtr<ID3D12CommandAllocator> m_uploadAllocator;
  std::uint64_t m_uploadFenceValue = 0;

  GpuPtr<ID3D12GraphicsCommandList> m_cmd;
  GpuPtr<ID3D12Fence> m_fence;
  HANDLE m_fenceEvent = nullptr;
  std::uint64_t m_fenceValues[FRAME_COUNT] = {};
  std::uint64_t m_fenceNext = 0;

  // The copy queue and everything that belongs to it. Its own fence, because the graphics fence
  // counts frames and a copy that waited on that would be waiting for a frame.
  GpuPtr<ID3D12CommandQueue> m_copyQueue;
  GpuPtr<ID3D12CommandAllocator> m_copyAllocator;
  GpuPtr<ID3D12GraphicsCommandList> m_copyList;
  GpuPtr<ID3D12Fence> m_copyFence;
  HANDLE m_copyEvent = nullptr;
  std::uint64_t m_copyFenceValue = 0;
  std::uint32_t m_frameIndex = 0;
  std::uint32_t m_rtvStride = 0;
};
} // namespace Neuron
