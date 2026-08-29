#pragma once

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

  // Closes, submits and waits for whatever has been recorded so far. Initialisation-only: it is
  // how a resource upload that needs the copy engine finishes before the first frame.
  void ExecuteAndWait();

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
  GpuPtr<ID3D12Resource> m_backBuffers[FRAME_COUNT];
  GpuPtr<ID3D12Resource> m_depth;
  GpuPtr<ID3D12CommandAllocator> m_allocators[FRAME_COUNT];
  GpuPtr<ID3D12GraphicsCommandList> m_cmd;
  GpuPtr<ID3D12Fence> m_fence;
  HANDLE m_fenceEvent = nullptr;
  std::uint64_t m_fenceValues[FRAME_COUNT] = {};
  std::uint64_t m_fenceNext = 0;
  std::uint32_t m_frameIndex = 0;
  std::uint32_t m_rtvStride = 0;
};
} // namespace Neuron
