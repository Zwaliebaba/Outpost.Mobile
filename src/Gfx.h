#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string_view>
#include <vector>

// Throwaway toy: one graphics object, plain fields, no wrappers. WRL's ComPtr is used rather than
// the tree's winrt::com_ptr because it needs nothing from the SDK beyond <wrl/client.h>.
using Microsoft::WRL::ComPtr;

void FatalHr(const char* _what, HRESULT _hr);
void DebugPrintf(const char* _fmt, ...);

#define CHECK_HR(expr)                                                                                                                     \
  do                                                                                                                                       \
  {                                                                                                                                        \
    const HRESULT checkHr = (expr);                                                                                                        \
    if (FAILED(checkHr))                                                                                                                   \
    {                                                                                                                                      \
      FatalHr(#expr, checkHr);                                                                                                             \
    }                                                                                                                                      \
  } while (false)

struct Rgba
{
  float r, g, b, a;
};

struct TextVertex
{
  float xPx, yPx;
  float u, v;
  Rgba colour;
};

struct Gfx
{
  static constexpr UINT FRAME_COUNT = 3;
  static constexpr UINT MAX_TEXT_VERTS = 24000; // 4000 glyphs; a multiple of 6

  void Init(HWND _hwnd);
  void Shutdown();
  void Resize(UINT _widthPx, UINT _heightPx);

  // BeginFrame clears colour and depth; EndFrame flushes queued text, presents, and waits for the
  // frame slot to come free.
  void BeginFrame(Rgba _clear);
  void EndFrame();

  // Queued during the frame, drawn on top of everything in EndFrame. '\n' starts a new line.
  void DrawTextLine(float _xPx, float _yPx, float _scale, Rgba _colour, std::string_view _text);
  float TextAdvancePx(float _scale) const { return m_advancePx * _scale; }
  float TextLineHeightPx(float _scale) const { return m_cellHPx * _scale; }

  UINT m_widthPx = 0;
  UINT m_heightPx = 0;

  ComPtr<IDXGIFactory6> m_factory;
  ComPtr<ID3D12Device> m_device;
  ComPtr<ID3D12CommandQueue> m_queue;
  ComPtr<IDXGISwapChain3> m_swapChain;
  ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
  ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
  ComPtr<ID3D12DescriptorHeap> m_srvHeap;
  ComPtr<ID3D12Resource> m_backBuffers[FRAME_COUNT];
  ComPtr<ID3D12Resource> m_depth;
  ComPtr<ID3D12CommandAllocator> m_allocators[FRAME_COUNT];
  ComPtr<ID3D12GraphicsCommandList> m_cmd;
  ComPtr<ID3D12Fence> m_fence;
  HANDLE m_fenceEvent = nullptr;
  UINT64 m_fenceValues[FRAME_COUNT] = {};
  UINT64 m_fenceNext = 0;
  UINT m_frameIndex = 0;
  UINT m_rtvStride = 0;

  ComPtr<ID3D12RootSignature> m_textRs;
  ComPtr<ID3D12PipelineState> m_textPso;
  ComPtr<ID3D12Resource> m_fontTex;
  ComPtr<ID3D12Resource> m_textVb[FRAME_COUNT];
  uint8_t* m_textVbCpu[FRAME_COUNT] = {};
  std::vector<TextVertex> m_textVerts;
  float m_cellWPx = 0.0f;  // atlas cell, i.e. the quad width
  float m_cellHPx = 0.0f;  // atlas cell, i.e. the line height
  float m_advancePx = 0.0f; // fixed pitch, i.e. the pen step
  float m_atlasWPx = 0.0f;
  float m_atlasHPx = 0.0f;

  void CreateSizedResources();
  void ReleaseSizedResources();
  void WaitForGpu();
  void BakeFontAtlas();
  void CreateTextPipeline();
};
