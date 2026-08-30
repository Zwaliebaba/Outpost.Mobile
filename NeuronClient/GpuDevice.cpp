#include "pch.h"
#include "GpuDevice.h"

#include "GpuHelpers.h"

namespace Neuron
{
void GpuDevice::Init(HWND _hwnd)
{
  UINT factoryFlags = 0;
#ifndef NDEBUG
  {
    // A probe, not an error check: the debug layer is optional and its absence is normal on a
    // machine without the graphics tools installed.
    GpuPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put()))))
    {
      debug->EnableDebugLayer();
      factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }
#endif

  check_hresult(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(m_factory.put())));

  for (UINT i = 0;; ++i)
  {
    GpuPtr<IDXGIAdapter1> adapter;
    if (m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put())) == DXGI_ERROR_NOT_FOUND)
      break;
    DXGI_ADAPTER_DESC1 ad = {};
    adapter->GetDesc1(&ad);
    if ((ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
      continue;
    if (SUCCEEDED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_device.put()))))
    {
      DebugTrace(L"adapter: {}\n", ad.Description);
      break;
    }
  }
  if (!m_device)
  {
    DebugTrace("no D3D12 feature level 11_0 adapter\n");
    check_hresult(DXGI_ERROR_NOT_FOUND);
  }

  // Every shader in the tree is DXIL for shader model 6.7 (AGENTS.md section 3, Decisions/0017), so
  // a device that stops short of it would fail later, in CreateGraphicsPipelineState, with an
  // E_INVALIDARG that names nothing. Asked here, the answer names the model the adapter reached.
  {
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_7};
    const HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    if (FAILED(hr) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_7)
    {
      DebugTrace("adapter reaches shader model 0x{:x}; the shaders need 6.7\n", static_cast<unsigned>(sm.HighestShaderModel));
      check_hresult(DXGI_ERROR_UNSUPPORTED);
    }
  }

  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  qd.NodeMask = 0;
  check_hresult(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(m_queue.put())));

  DXGI_SWAP_CHAIN_DESC1 scd = {};
  scd.Width = 0; // take the client area
  scd.Height = 0;
  scd.Format = BACK_BUFFER_FORMAT;
  scd.Stereo = FALSE;
  scd.SampleDesc.Count = 1;
  scd.SampleDesc.Quality = 0;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = FRAME_COUNT;
  scd.Scaling = DXGI_SCALING_STRETCH;
  scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
  scd.Flags = 0;

  GpuPtr<IDXGISwapChain1> swapChain1;
  check_hresult(m_factory->CreateSwapChainForHwnd(m_queue.get(), _hwnd, &scd, nullptr, nullptr, swapChain1.put()));
  check_hresult(m_factory->MakeWindowAssociation(_hwnd, DXGI_MWA_NO_ALT_ENTER)); // windowed only, no exclusive mode
  swapChain1.as(m_swapChain);

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = FRAME_COUNT;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  hd.NodeMask = 0;
  check_hresult(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(m_rtvHeap.put())));
  m_rtvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = 1;
  check_hresult(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(m_dsvHeap.put())));

  for (std::uint32_t i = 0; i < FRAME_COUNT; ++i)
    check_hresult(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_allocators[i].put())));
  check_hresult(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].get(), nullptr, IID_PPV_ARGS(m_cmd.put())));
  // A new list is already recording. Close it so that "the list is closed unless a caller opened
  // it" holds from the start, and every uploader brackets itself with BeginUploads.
  check_hresult(m_cmd->Close());

  // The upload bracket's own allocator. Without it BeginUploads reset allocator 0, which is also
  // frame 0's, and had to drain the whole GPU first to make that legal (ADR 0044).
  check_hresult(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_uploadAllocator.put())));

  check_hresult(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put())));
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent)
    throw_last_error();

  // The copy queue: its own queue, allocator, list and fence. A COPY-type command list can record
  // copies and nothing else -- no dispatch, no draw, and no barrier into a shader-resource state --
  // which is exactly the set of work that was making a load block a frame.
  D3D12_COMMAND_QUEUE_DESC cqd = {};
  cqd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
  cqd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  check_hresult(m_device->CreateCommandQueue(&cqd, IID_PPV_ARGS(m_copyQueue.put())));
  check_hresult(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(m_copyAllocator.put())));
  check_hresult(
    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_copyAllocator.get(), nullptr, IID_PPV_ARGS(m_copyList.put())));
  check_hresult(m_copyList->Close()); // closed unless a caller opened it, exactly as m_cmd is
  check_hresult(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_copyFence.put())));
  m_copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_copyEvent)
    throw_last_error();

  CreateSizedResources();
}

void GpuDevice::Shutdown()
{
  // A shutdown path must not throw, so nothing here is checked: there is nothing left to do about
  // a failure and an exception on the way out loses the real one.
  if (m_device)
    WaitForGpu();
  WaitForCopies(); // a copy still in flight is holding a staging buffer this is about to release
  if (m_fenceEvent)
  {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }
  if (m_copyEvent)
  {
    CloseHandle(m_copyEvent);
    m_copyEvent = nullptr;
  }
}

void GpuDevice::CreateSizedResources()
{
  DXGI_SWAP_CHAIN_DESC1 scd = {};
  check_hresult(m_swapChain->GetDesc1(&scd));
  m_widthPx = scd.Width;
  m_heightPx = scd.Height;

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (std::uint32_t i = 0; i < FRAME_COUNT; ++i)
  {
    check_hresult(m_swapChain->GetBuffer(i, IID_PPV_ARGS(m_backBuffers[i].put())));
    m_device->CreateRenderTargetView(m_backBuffers[i].get(), nullptr, rtv);
    rtv.ptr += m_rtvStride;
  }

  D3D12_RESOURCE_DESC dd = {};
  dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  dd.Alignment = 0;
  dd.Width = m_widthPx;
  dd.Height = m_heightPx;
  dd.DepthOrArraySize = 1;
  dd.MipLevels = 1;
  dd.Format = DEPTH_FORMAT;
  dd.SampleDesc.Count = 1;
  dd.SampleDesc.Quality = 0;
  dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE clear = {};
  clear.Format = DEPTH_FORMAT;
  clear.DepthStencil.Depth = 1.0f;
  clear.DepthStencil.Stencil = 0;

  const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  check_hresult(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                  IID_PPV_ARGS(m_depth.put())));

  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
  dsvDesc.Format = DEPTH_FORMAT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
  m_device->CreateDepthStencilView(m_depth.get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void GpuDevice::ReleaseSizedResources()
{
  for (std::uint32_t i = 0; i < FRAME_COUNT; ++i)
    m_backBuffers[i] = nullptr;
  m_depth = nullptr;
}

void GpuDevice::Resize(std::uint32_t _widthPx, std::uint32_t _heightPx)
{
  if (!m_device || _widthPx == 0 || _heightPx == 0)
    return;
  if (_widthPx == m_widthPx && _heightPx == m_heightPx)
    return;
  WaitForGpu();
  ReleaseSizedResources();
  check_hresult(m_swapChain->ResizeBuffers(FRAME_COUNT, _widthPx, _heightPx, BACK_BUFFER_FORMAT, 0));
  CreateSizedResources();
}

void GpuDevice::WaitForGpu()
{
  if (!m_queue || !m_fence)
    return;
  const std::uint64_t target = ++m_fenceNext;
  check_hresult(m_queue->Signal(m_fence.get(), target));
  if (m_fence->GetCompletedValue() < target)
  {
    check_hresult(m_fence->SetEventOnCompletion(target, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
  for (std::uint32_t i = 0; i < FRAME_COUNT; ++i)
    m_fenceValues[i] = target;
}

void GpuDevice::BeginUploads()
{
  // This bracket has an allocator of its own now, so what it waits for is its own previous batch
  // and not every frame in flight.
  //
  // It used to reset allocator 0, which is also frame 0's, and therefore had to drain the whole GPU
  // first: resetting an allocator the GPU is still reading is undefined behaviour. That drain was
  // free at boot, where nothing is in flight, and was the cost of every mid-session bake -- F5,
  // which regenerates every body (ADR 0044).
  //
  // The list may be reset the moment it has been submitted; it is the allocator that has to wait.
  if (m_fence->GetCompletedValue() < m_uploadFenceValue)
  {
    check_hresult(m_fence->SetEventOnCompletion(m_uploadFenceValue, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
  check_hresult(m_uploadAllocator->Reset());
  check_hresult(m_cmd->Reset(m_uploadAllocator.get(), nullptr));
}

void GpuDevice::ExecuteAndWait()
{
  check_hresult(m_cmd->Close());
  ID3D12CommandList* lists[] = {m_cmd.get()};
  m_queue->ExecuteCommandLists(1, lists);

  // Still a wait, and deliberately: what this bracket carries is a compute bake whose output the
  // very next thing reads, and a readback the CPU is about to look at. The drain this slice removed
  // is the one at the *other* end -- BeginUploads no longer waits for frames before it can start
  // (ADR 0044). Recorded against this bracket's own fence value so the next BeginUploads knows
  // exactly what it is waiting for.
  m_uploadFenceValue = ++m_fenceNext;
  check_hresult(m_queue->Signal(m_fence.get(), m_uploadFenceValue));
  if (m_fence->GetCompletedValue() < m_uploadFenceValue)
  {
    check_hresult(m_fence->SetEventOnCompletion(m_uploadFenceValue, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
  for (std::uint32_t i = 0; i < FRAME_COUNT; ++i)
    m_fenceValues[i] = m_uploadFenceValue;
}

void GpuDevice::BeginCopies()
{
  // The previous batch's allocator, and nothing else. A frame on the graphics queue is not waited
  // for and does not have to be: this is a different queue.
  WaitForCopies();
  check_hresult(m_copyAllocator->Reset());
  check_hresult(m_copyList->Reset(m_copyAllocator.get(), nullptr));
}

void GpuDevice::SubmitCopies()
{
  check_hresult(m_copyList->Close());
  ID3D12CommandList* lists[] = {m_copyList.get()};
  m_copyQueue->ExecuteCommandLists(1, lists);
  m_copyFenceValue = m_copyFenceValue + 1;
  check_hresult(m_copyQueue->Signal(m_copyFence.get(), m_copyFenceValue));

  // The GPU waits, not the CPU. Everything the copy queue touched decays to COMMON when this
  // ExecuteCommandLists completes, and the first graphics use promotes it back out of COMMON for
  // free -- which is why there is no barrier on either side of a copy recorded into this list.
  check_hresult(m_queue->Wait(m_copyFence.get(), m_copyFenceValue));
}

std::uint64_t GpuDevice::CompletedCopyFence() const noexcept
{
  return m_copyFence ? m_copyFence->GetCompletedValue() : 0;
}

void GpuDevice::WaitForCopies()
{
  if (!m_copyFence || m_copyFence->GetCompletedValue() >= m_copyFenceValue)
    return;
  check_hresult(m_copyFence->SetEventOnCompletion(m_copyFenceValue, m_copyEvent));
  WaitForSingleObject(m_copyEvent, INFINITE);
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuDevice::BackBufferView() const noexcept
{
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvStride;
  return rtv;
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuDevice::DepthView() const noexcept
{
  return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void GpuDevice::BeginFrame(Rgba _clear)
{
  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
  if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
  {
    check_hresult(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }

  check_hresult(m_allocators[m_frameIndex]->Reset());
  check_hresult(m_cmd->Reset(m_allocators[m_frameIndex].get(), nullptr));

  const D3D12_RESOURCE_BARRIER toTarget =
    Transition(m_backBuffers[m_frameIndex].get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  m_cmd->ResourceBarrier(1, &toTarget);

  D3D12_VIEWPORT vp = {};
  vp.TopLeftX = 0.0f;
  vp.TopLeftY = 0.0f;
  vp.Width = static_cast<float>(m_widthPx);
  vp.Height = static_cast<float>(m_heightPx);
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  D3D12_RECT scissor = {0, 0, static_cast<LONG>(m_widthPx), static_cast<LONG>(m_heightPx)};
  m_cmd->RSSetViewports(1, &vp);
  m_cmd->RSSetScissorRects(1, &scissor);

  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = BackBufferView();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
  m_cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  m_cmd->ClearRenderTargetView(rtv, &_clear.r, 0, nullptr);
  m_cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void GpuDevice::EndFrame()
{
  const D3D12_RESOURCE_BARRIER toPresent =
    Transition(m_backBuffers[m_frameIndex].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  m_cmd->ResourceBarrier(1, &toPresent);
  check_hresult(m_cmd->Close());

  ID3D12CommandList* lists[] = {m_cmd.get()};
  m_queue->ExecuteCommandLists(1, lists);

  // Logged rather than checked: a lost device is a real condition to recover from one day, and
  // until that handling exists a dropped frame is better than tearing the process down.
  const HRESULT presented = m_swapChain->Present(1, 0); // vsync on, always
  if (FAILED(presented))
    DebugTrace("Present failed: 0x{:08X}\n", static_cast<unsigned>(presented));

  const std::uint64_t target = ++m_fenceNext;
  check_hresult(m_queue->Signal(m_fence.get(), target));
  m_fenceValues[m_frameIndex] = target;
}
} // namespace Neuron
