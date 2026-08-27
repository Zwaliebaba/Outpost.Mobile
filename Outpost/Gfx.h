#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Throwaway toy: one graphics object, plain fields, no wrappers. Ownership and HRESULT checking
// come from C++/WinRT, the same as the rest of the tree.
using winrt::com_ptr;

void FatalHr(const char* _what, HRESULT _hr);

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
  com_ptr<ID3D12Resource> vb;
  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  UINT vertexCount = 0;
};

// Set once per frame, before any DrawMesh.
struct SceneFrame
{
  DirectX::XMFLOAT4X4 viewProj;
  DirectX::XMFLOAT3 lightDir; // towards the light; normalised in the shader
  float ambient;
  Rgba gridColour; // a = how far a grid line pulls away from the ground colour
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

  // Alpha-blended overlays drawn after the opaque pass: rings and order markers on the ground,
  // additive glows in the air. Both take the unit quad and shape it in the pixel shader, so ring
  // thickness and glow falloff stay plain parameters with no geometry to rebuild.
  void BeginDecals(const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _cameraPos);
  void DrawDecal(UINT _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _colour, float _thickness, float _fill);
  void DrawGlow(UINT _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _colour, float _falloff);

  // Queued during the frame, drawn on top of everything in EndFrame. '\n' starts a new line.
  void DrawTextLine(float _xPx, float _yPx, float _scale, Rgba _colour, std::string_view _text);

  // Screen-space quads, drawn through the text pipeline against a solid texel baked into the spare
  // atlas cell. Same queue as the text, so they land on top of the scene in EndFrame.
  void DrawScreenRect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, Rgba _colour);
  void DrawScreenLine(float _x0Px, float _y0Px, float _x1Px, float _y1Px, float _thicknessPx, Rgba _colour);

  float TextAdvancePx(float _scale) const
  {
    return m_advancePx * _scale;
  }

  float TextLineHeightPx(float _scale) const
  {
    return m_cellHPx * _scale;
  }

  UINT m_widthPx = 0;
  UINT m_heightPx = 0;

  com_ptr<IDXGIFactory6> m_factory;
  com_ptr<ID3D12Device> m_device;
  com_ptr<ID3D12CommandQueue> m_queue;
  com_ptr<IDXGISwapChain3> m_swapChain;
  com_ptr<ID3D12DescriptorHeap> m_rtvHeap;
  com_ptr<ID3D12DescriptorHeap> m_dsvHeap;
  com_ptr<ID3D12DescriptorHeap> m_srvHeap;
  com_ptr<ID3D12Resource> m_backBuffers[FRAME_COUNT];
  com_ptr<ID3D12Resource> m_depth;
  com_ptr<ID3D12CommandAllocator> m_allocators[FRAME_COUNT];
  com_ptr<ID3D12GraphicsCommandList> m_cmd;
  com_ptr<ID3D12Fence> m_fence;
  HANDLE m_fenceEvent = nullptr;
  UINT64 m_fenceValues[FRAME_COUNT] = {};
  UINT64 m_fenceNext = 0;
  UINT m_frameIndex = 0;
  UINT m_rtvStride = 0;

  com_ptr<ID3D12RootSignature> m_sceneRs;
  com_ptr<ID3D12PipelineState> m_scenePso;
  com_ptr<ID3D12PipelineState> m_decalPso; // alpha blended
  com_ptr<ID3D12PipelineState> m_glowPso;  // additive
  std::vector<GpuMesh> m_meshes;

  com_ptr<ID3D12RootSignature> m_textRs;
  com_ptr<ID3D12PipelineState> m_textPso;
  com_ptr<ID3D12Resource> m_fontTex;
  com_ptr<ID3D12Resource> m_textVb[FRAME_COUNT];
  uint8_t* m_textVbCpu[FRAME_COUNT] = {};
  std::vector<TextVertex> m_textVerts;
  float m_cellWPx = 0.0f;   // atlas cell, i.e. the quad width
  float m_cellHPx = 0.0f;   // atlas cell, i.e. the line height
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
  void CreateDecalPipelines();
};
