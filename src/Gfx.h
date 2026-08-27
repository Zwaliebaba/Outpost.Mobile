#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <DirectXMath.h>

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

// One vertex format for ships and ground alike. No normal: the pixel shader takes the face normal
// from screen-space derivatives, which is exactly the flat shading wanted here.
struct SceneVertex
{
  float px, py, pz;
  float r, g, b;
};

struct GpuMesh
{
  ComPtr<ID3D12Resource> vb;
  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  UINT vertexCount = 0;
};

// Set once per frame, before any DrawMesh.
struct SceneFrame
{
  DirectX::XMFLOAT4X4 viewProj;
  DirectX::XMFLOAT3 lightDir;  // towards the light; normalised in the shader
  float ambient;
  Rgba gridColour;             // a = how far a grid line pulls away from the ground colour
  float gridSpacing;
  float gridLineWidthPx;
  float gridFadeDistance;
  DirectX::XMFLOAT3 cameraPos;
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

  // Static geometry, uploaded once at startup. The vertex buffer stays in an upload heap: a few
  // thousand triangles do not justify a staging copy and its barrier.
  UINT UploadMesh(const std::vector<SceneVertex>& _verts);
  void BeginScene(const SceneFrame& _frame);
  void DrawMesh(UINT _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _baseColour, float _materialMix, bool _isGround);

  // Queued during the frame, drawn on top of everything in EndFrame. '\n' starts a new line.
  void DrawTextLine(float _xPx, float _yPx, float _scale, Rgba _colour, std::string_view _text);

  // Screen-space quads, drawn through the text pipeline against a solid texel baked into the spare
  // atlas cell. Same queue as the text, so they land on top of the scene in EndFrame.
  void DrawScreenRect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, Rgba _colour);
  void DrawScreenLine(float _x0Px, float _y0Px, float _x1Px, float _y1Px, float _thicknessPx, Rgba _colour);
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

  ComPtr<ID3D12RootSignature> m_sceneRs;
  ComPtr<ID3D12PipelineState> m_scenePso;
  std::vector<GpuMesh> m_meshes;

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
  float m_solidU = 0.0f; // centre of the one atlas cell with no glyph in it
  float m_solidV = 0.0f;

  void CreateSizedResources();
  void ReleaseSizedResources();
  void WaitForGpu();
  void BakeFontAtlas();
  void CreateTextPipeline();
  void CreateScenePipeline();
};
