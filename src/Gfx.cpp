#include "Gfx.h"

#include <d3dcompiler.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace
{

constexpr int FIRST_GLYPH = 32;
constexpr int LAST_GLYPH = 126;
constexpr int GLYPH_COLS = 16;
constexpr int GLYPH_ROWS = 6; // 96 cells for the 95 printable ASCII glyphs
constexpr int FONT_HEIGHT_PX = 16;

constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;

const char* const TEXT_SHADER = R"HLSL(
cbuffer Root : register(b0)
{
  float2 invViewportPx;
};

Texture2D<float> Atlas : register(t0);
SamplerState Samp : register(s0);

struct VsIn  { float2 posPx : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };

VsOut VsMain(VsIn i)
{
  VsOut o;
  o.pos = float4(i.posPx.x * invViewportPx.x * 2.0 - 1.0, 1.0 - i.posPx.y * invViewportPx.y * 2.0, 0.0, 1.0);
  o.uv = i.uv;
  o.col = i.col;
  return o;
}

float4 PsMain(VsOut i) : SV_Target
{
  float coverage = Atlas.Sample(Samp, i.uv);
  return float4(i.col.rgb, i.col.a * coverage);
}
)HLSL";

// Ships and ground share this. `row_major` matches XMFLOAT4X4's storage so mul(rowVector, matrix)
// means what DirectXMath means by it.
const char* const SCENE_SHADER = R"HLSL(
cbuffer VsConstants : register(b0)
{
  row_major float4x4 world;
  row_major float4x4 viewProj;
};

cbuffer PsConstants : register(b1)
{
  float4 baseColour;       // rgb base colour, w material mix
  float4 lightDirAmbient;  // xyz towards the light, w ambient level
  float4 gridColour;       // rgb line colour, a strength
  float4 gridParams;       // x spacing, y line width px, z fade distance, w 1 for the ground
  float4 cameraPos;        // xyz eye
};

struct VsIn  { float3 pos : POSITION; float3 col : COLOR0; };
struct VsOut { float4 clip : SV_Position; float3 worldPos : TEXCOORD0; float3 col : COLOR0; };

VsOut VsMain(VsIn i)
{
  VsOut o;
  float4 wp = mul(float4(i.pos, 1.0), world);
  o.worldPos = wp.xyz;
  o.clip = mul(wp, viewProj);
  o.col = i.col;
  return o;
}

float4 PsMain(VsOut i) : SV_Target
{
  // Flat shading from screen-space derivatives, then faced towards the eye so triangle winding
  // cannot matter.
  float3 crossed = cross(ddx(i.worldPos), ddy(i.worldPos));
  float3 faceNormal = (dot(crossed, crossed) > 1e-12) ? normalize(crossed) : float3(0.0, 1.0, 0.0);
  if (dot(faceNormal, cameraPos.xyz - i.worldPos) < 0.0)
  {
    faceNormal = -faceNormal;
  }

  float3 albedo = lerp(baseColour.rgb, i.col, baseColour.w);
  float3 normal = faceNormal;

  if (gridParams.w > 0.5)
  {
    normal = float3(0.0, 1.0, 0.0);
    float2 cell = i.worldPos.xz / max(gridParams.x, 0.001);
    float2 toLine = abs(frac(cell - 0.5) - 0.5) / max(fwidth(cell), 1e-6);
    float onLine = 1.0 - saturate(min(toLine.x, toLine.y) - gridParams.y * 0.5);
    float fade = saturate(1.0 - length(i.worldPos.xz - cameraPos.xz) / max(gridParams.z, 0.001));
    albedo = lerp(baseColour.rgb, gridColour.rgb, onLine * fade * gridColour.a);
  }

  float lambert = saturate(dot(normal, normalize(lightDirAmbient.xyz)));
  float3 lit = albedo * (lightDirAmbient.w + (1.0 - lightDirAmbient.w) * lambert);
  return float4(lit, 1.0);
}
)HLSL";

// Rings, order markers and thruster glows. Shares the scene root signature -- 32 vertex DWORDs of
// matrices, 20 pixel DWORDs -- and reads the same unit quad, shaping it entirely in the pixel
// shader so radius, thickness and falloff are all just constants.
const char* const DECAL_SHADER = R"HLSL(
cbuffer VsConstants : register(b0)
{
  row_major float4x4 world;
  row_major float4x4 viewProj;
};

cbuffer PsConstants : register(b1)
{
  float4 decalColour; // rgb, a
  float4 decalParams; // x thickness as a fraction of the radius, y fill or glow falloff, z 1 for a glow
  float4 unusedA;
  float4 unusedB;
  float4 cameraPos;
};

struct VsIn  { float3 pos : POSITION; float3 col : COLOR0; };
struct VsOut { float4 clip : SV_Position; float2 local : TEXCOORD0; };

VsOut VsMain(VsIn i)
{
  VsOut o;
  float4 wp = mul(float4(i.pos, 1.0), world);
  o.clip = mul(wp, viewProj);
  o.local = i.pos.xz * 2.0; // the unit quad spans +-0.5, so this lands on +-1
  return o;
}

float4 PsMain(VsOut i) : SV_Target
{
  float d = length(i.local);

  if (decalParams.z > 0.5) // thruster glow: soft radial falloff, no edge
  {
    float glow = pow(saturate(1.0 - d), max(1.0, decalParams.y));
    clip(glow - 0.002);
    return float4(decalColour.rgb, glow * decalColour.a);
  }

  // Ring, antialiased against its own screen-space width so it stays crisp at any zoom and never
  // thins away to nothing.
  float aa = fwidth(d) + 1e-5;
  float halfWidth = max(decalParams.x, aa) * 0.5;
  float centre = 1.0 - halfWidth;
  float ring = 1.0 - smoothstep(halfWidth - aa, halfWidth + aa, abs(d - centre));
  float fill = decalParams.y * (1.0 - smoothstep(1.0 - aa, 1.0 + aa, d));
  float alpha = saturate(ring + fill) * decalColour.a;
  clip(alpha - 0.002);
  return float4(decalColour.rgb, alpha);
}
)HLSL";

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE _type)
{
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = _type;
  hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  hp.CreationNodeMask = 1;
  hp.VisibleNodeMask = 1;
  return hp;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 _bytes)
{
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Alignment = 0;
  rd.Width = _bytes;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_UNKNOWN;
  rd.SampleDesc.Count = 1;
  rd.SampleDesc.Quality = 0;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.Flags = D3D12_RESOURCE_FLAG_NONE;
  return rd;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* _res, D3D12_RESOURCE_STATES _before, D3D12_RESOURCE_STATES _after)
{
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  b.Transition.pResource = _res;
  b.Transition.StateBefore = _before;
  b.Transition.StateAfter = _after;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return b;
}

ComPtr<ID3DBlob> CompileShader(const char* _source, const char* _entry, const char* _target)
{
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  const HRESULT hr =
      D3DCompile(_source, std::strlen(_source), "ShipFeel", nullptr, nullptr, _entry, _target, flags, 0, &code, &errors);
  if (FAILED(hr))
  {
    DebugPrintf("shader %s failed: %s\n", _entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
    FatalHr(_entry, hr);
  }
  return code;
}

} // namespace

void FatalHr(const char* _what, HRESULT _hr)
{
  char msg[512] = {};
  std::snprintf(msg, sizeof(msg), "Fatal: %s\nHRESULT 0x%08X", _what, static_cast<unsigned>(_hr));
  DebugPrintf("%s\n", msg);
  wchar_t wide[512] = {};
  MultiByteToWideChar(CP_UTF8, 0, msg, -1, wide, 512);
  MessageBoxW(nullptr, wide, L"ShipFeel", MB_OK | MB_ICONERROR);
  ExitProcess(1);
}

void DebugPrintf(const char* _fmt, ...)
{
  char msg[1024] = {};
  va_list args;
  va_start(args, _fmt);
  std::vsnprintf(msg, sizeof(msg), _fmt, args);
  va_end(args);
  wchar_t wide[1024] = {};
  MultiByteToWideChar(CP_UTF8, 0, msg, -1, wide, 1024);
  OutputDebugStringW(wide);
}

void Gfx::Init(HWND _hwnd)
{
  UINT factoryFlags = 0;
#ifndef NDEBUG
  {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
      debug->EnableDebugLayer();
      factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }
#endif

  CHECK_HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0;; ++i)
  {
    if (m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
    {
      break;
    }
    DXGI_ADAPTER_DESC1 ad = {};
    adapter->GetDesc1(&ad);
    if ((ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
    {
      continue;
    }
    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
    {
      DebugPrintf("adapter: %S\n", ad.Description);
      break;
    }
  }
  if (!m_device)
  {
    FatalHr("no D3D12 feature level 11_0 adapter", DXGI_ERROR_NOT_FOUND);
  }

  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  qd.NodeMask = 0;
  CHECK_HR(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)));

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

  ComPtr<IDXGISwapChain1> swapChain1;
  CHECK_HR(m_factory->CreateSwapChainForHwnd(m_queue.Get(), _hwnd, &scd, nullptr, nullptr, &swapChain1));
  CHECK_HR(m_factory->MakeWindowAssociation(_hwnd, DXGI_MWA_NO_ALT_ENTER)); // windowed only, no exclusive mode
  CHECK_HR(swapChain1.As(&m_swapChain));

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = FRAME_COUNT;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  hd.NodeMask = 0;
  CHECK_HR(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)));
  m_rtvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = 1;
  CHECK_HR(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap)));

  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1; // slot 0: the font atlas
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  CHECK_HR(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)));

  for (UINT i = 0; i < FRAME_COUNT; ++i)
  {
    CHECK_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocators[i])));
  }
  CHECK_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_cmd)));

  CHECK_HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent)
  {
    FatalHr("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
  }

  CreateSizedResources();
  CreateTextPipeline();
  CreateScenePipeline();
  CreateDecalPipelines();

  for (UINT i = 0; i < FRAME_COUNT; ++i)
  {
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC rd = BufferDesc(UINT64(MAX_TEXT_VERTS) * sizeof(TextVertex));
    CHECK_HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&m_textVb[i])));
    D3D12_RANGE noRead = {0, 0};
    CHECK_HR(m_textVb[i]->Map(0, &noRead, reinterpret_cast<void**>(&m_textVbCpu[i]))); // mapped for the whole run
  }

  BakeFontAtlas(); // records into m_cmd, then executes and waits
  m_textVerts.reserve(MAX_TEXT_VERTS);
}

void Gfx::Shutdown()
{
  if (m_device)
  {
    WaitForGpu();
  }
  if (m_fenceEvent)
  {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }
}

void Gfx::CreateSizedResources()
{
  DXGI_SWAP_CHAIN_DESC1 scd = {};
  CHECK_HR(m_swapChain->GetDesc1(&scd));
  m_widthPx = scd.Width;
  m_heightPx = scd.Height;

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < FRAME_COUNT; ++i)
  {
    CHECK_HR(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
    m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
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
  CHECK_HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                             IID_PPV_ARGS(&m_depth)));

  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
  dsvDesc.Format = DEPTH_FORMAT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
  m_device->CreateDepthStencilView(m_depth.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void Gfx::ReleaseSizedResources()
{
  for (UINT i = 0; i < FRAME_COUNT; ++i)
  {
    m_backBuffers[i].Reset();
  }
  m_depth.Reset();
}

void Gfx::Resize(UINT _widthPx, UINT _heightPx)
{
  if (!m_device || _widthPx == 0 || _heightPx == 0)
  {
    return;
  }
  if (_widthPx == m_widthPx && _heightPx == m_heightPx)
  {
    return;
  }
  WaitForGpu();
  ReleaseSizedResources();
  CHECK_HR(m_swapChain->ResizeBuffers(FRAME_COUNT, _widthPx, _heightPx, BACK_BUFFER_FORMAT, 0));
  CreateSizedResources();
}

void Gfx::WaitForGpu()
{
  if (!m_queue || !m_fence)
  {
    return;
  }
  const UINT64 target = ++m_fenceNext;
  CHECK_HR(m_queue->Signal(m_fence.Get(), target));
  if (m_fence->GetCompletedValue() < target)
  {
    CHECK_HR(m_fence->SetEventOnCompletion(target, m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
  for (UINT i = 0; i < FRAME_COUNT; ++i)
  {
    m_fenceValues[i] = target;
  }
}

void Gfx::BeginFrame(Rgba _clear)
{
  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
  if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
  {
    CHECK_HR(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }

  m_textVerts.clear();
  CHECK_HR(m_allocators[m_frameIndex]->Reset());
  CHECK_HR(m_cmd->Reset(m_allocators[m_frameIndex].Get(), nullptr));

  const D3D12_RESOURCE_BARRIER toTarget =
      Transition(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  m_cmd->ResourceBarrier(1, &toTarget);

  D3D12_VIEWPORT vp = {};
  vp.TopLeftX = 0.0f;
  vp.TopLeftY = 0.0f;
  vp.Width = float(m_widthPx);
  vp.Height = float(m_heightPx);
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  D3D12_RECT scissor = {0, 0, LONG(m_widthPx), LONG(m_heightPx)};
  m_cmd->RSSetViewports(1, &vp);
  m_cmd->RSSetScissorRects(1, &scissor);

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(m_frameIndex) * m_rtvStride;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
  m_cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  m_cmd->ClearRenderTargetView(rtv, &_clear.r, 0, nullptr);
  m_cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void Gfx::EndFrame()
{
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(m_frameIndex) * m_rtvStride;

  if (!m_textVerts.empty())
  {
    const size_t count = std::min<size_t>(m_textVerts.size(), MAX_TEXT_VERTS);
    std::memcpy(m_textVbCpu[m_frameIndex], m_textVerts.data(), count * sizeof(TextVertex));

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = m_textVb[m_frameIndex]->GetGPUVirtualAddress();
    vbv.SizeInBytes = UINT(count * sizeof(TextVertex));
    vbv.StrideInBytes = sizeof(TextVertex);

    const float invViewport[2] = {1.0f / float(m_widthPx), 1.0f / float(m_heightPx)};
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};

    m_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr); // HUD sits on top, no depth
    m_cmd->SetPipelineState(m_textPso.Get());
    m_cmd->SetGraphicsRootSignature(m_textRs.Get());
    m_cmd->SetDescriptorHeaps(1, heaps);
    m_cmd->SetGraphicsRoot32BitConstants(0, 2, invViewport, 0);
    m_cmd->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmd->IASetVertexBuffers(0, 1, &vbv);
    m_cmd->DrawInstanced(UINT(count), 1, 0, 0);
  }

  if (m_captureRequested)
  {
    RecordCaptureCopy();
  }

  const D3D12_RESOURCE_STATES from = m_captureRequested ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  const D3D12_RESOURCE_BARRIER toPresent = Transition(m_backBuffers[m_frameIndex].Get(), from, D3D12_RESOURCE_STATE_PRESENT);
  m_cmd->ResourceBarrier(1, &toPresent);
  CHECK_HR(m_cmd->Close());

  ID3D12CommandList* lists[] = {m_cmd.Get()};
  m_queue->ExecuteCommandLists(1, lists);
  CHECK_HR(m_swapChain->Present(1, 0)); // vsync on, always

  const UINT64 target = ++m_fenceNext;
  CHECK_HR(m_queue->Signal(m_fence.Get(), target));
  m_fenceValues[m_frameIndex] = target;

  if (m_captureRequested)
  {
    WaitForGpu(); // the readback has to have landed before it can be encoded
    WriteCapturePng();
    m_captureRequested = false;
  }
}

void Gfx::BakeFontAtlas()
{
  HDC screenDc = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screenDc);
  ReleaseDC(nullptr, screenDc);

  HFONT font = CreateFontW(-FONT_HEIGHT_PX, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
  HGDIOBJ oldFont = SelectObject(dc, font);

  TEXTMETRICW tm = {};
  GetTextMetricsW(dc, &tm);
  const int cellW = tm.tmMaxCharWidth + 1; // a pixel of slack so a wide glyph cannot bleed into its neighbour
  const int cellH = tm.tmHeight;
  const int atlasW = cellW * GLYPH_COLS;
  const int atlasH = cellH * GLYPH_ROWS;

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = atlasW;
  bmi.bmiHeader.biHeight = -atlasH; // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ oldBmp = SelectObject(dc, dib);

  PatBlt(dc, 0, 0, atlasW, atlasH, BLACKNESS);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  SetTextAlign(dc, TA_LEFT | TA_TOP);
  for (int i = 0; i <= LAST_GLYPH - FIRST_GLYPH; ++i)
  {
    const wchar_t ch = wchar_t(FIRST_GLYPH + i);
    TextOutW(dc, (i % GLYPH_COLS) * cellW, (i / GLYPH_COLS) * cellH, &ch, 1);
  }
  // 95 printable glyphs sit in 96 cells. Fill the spare one solid so untextured screen quads --
  // selection boxes, order lines -- can share the text pipeline instead of needing their own.
  const int solidIndex = LAST_GLYPH - FIRST_GLYPH + 1;
  const int solidCol = solidIndex % GLYPH_COLS;
  const int solidRow = solidIndex / GLYPH_COLS;
  PatBlt(dc, solidCol * cellW, solidRow * cellH, cellW, cellH, WHITENESS);
  GdiFlush();

  // GDI leaves alpha at zero, so take coverage from the white-on-black luminance.
  std::vector<uint8_t> coverage(size_t(atlasW) * size_t(atlasH));
  const uint8_t* src = static_cast<const uint8_t*>(bits);
  for (size_t i = 0; i < coverage.size(); ++i)
  {
    coverage[i] = std::max({src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2]});
  }

  SelectObject(dc, oldBmp);
  SelectObject(dc, oldFont);
  DeleteObject(dib);
  DeleteObject(font);
  DeleteDC(dc);

  m_cellWPx = float(cellW);
  m_cellHPx = float(cellH);
  m_advancePx = float(tm.tmAveCharWidth); // fixed pitch, so this is the true advance
  m_atlasWPx = float(atlasW);
  m_atlasHPx = float(atlasH);
  m_solidU = (float(solidCol) + 0.5f) * float(cellW) / float(atlasW);
  m_solidV = (float(solidRow) + 0.5f) * float(cellH) / float(atlasH);
  DebugPrintf("font atlas %dx%d, cell %dx%d, advance %d\n", atlasW, atlasH, cellW, cellH, tm.tmAveCharWidth);

  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Alignment = 0;
  td.Width = UINT64(atlasW);
  td.Height = UINT(atlasH);
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8_UNORM;
  td.SampleDesc.Count = 1;
  td.SampleDesc.Quality = 0;
  td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  td.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  CHECK_HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&m_fontTex)));

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rowCount = 0;
  UINT64 rowBytes = 0;
  UINT64 totalBytes = 0;
  m_device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rowCount, &rowBytes, &totalBytes);

  ComPtr<ID3D12Resource> upload;
  hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC ud = BufferDesc(totalBytes);
  CHECK_HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&upload)));

  uint8_t* dst = nullptr;
  D3D12_RANGE noRead = {0, 0};
  CHECK_HR(upload->Map(0, &noRead, reinterpret_cast<void**>(&dst)));
  for (UINT row = 0; row < rowCount; ++row)
  {
    std::memcpy(dst + size_t(row) * footprint.Footprint.RowPitch, coverage.data() + size_t(row) * size_t(atlasW), size_t(rowBytes));
  }
  upload->Unmap(0, nullptr);

  D3D12_TEXTURE_COPY_LOCATION copyDst = {};
  copyDst.pResource = m_fontTex.Get();
  copyDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copyDst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copySrc = {};
  copySrc.pResource = upload.Get();
  copySrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  copySrc.PlacedFootprint = footprint;
  m_cmd->CopyTextureRegion(&copyDst, 0, 0, 0, &copySrc, nullptr);

  const D3D12_RESOURCE_BARRIER toShader =
      Transition(m_fontTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  m_cmd->ResourceBarrier(1, &toShader);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT_R8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MostDetailedMip = 0;
  srv.Texture2D.MipLevels = 1;
  srv.Texture2D.PlaneSlice = 0;
  srv.Texture2D.ResourceMinLODClamp = 0.0f;
  m_device->CreateShaderResourceView(m_fontTex.Get(), &srv, m_srvHeap->GetCPUDescriptorHandleForHeapStart());

  CHECK_HR(m_cmd->Close());
  ID3D12CommandList* lists[] = {m_cmd.Get()};
  m_queue->ExecuteCommandLists(1, lists);
  WaitForGpu(); // upload stays alive until here
}

void Gfx::CreateTextPipeline()
{
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  range.RegisterSpace = 0;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.RegisterSpace = 0;
  params[0].Constants.Num32BitValues = 2;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samp = {};
  samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.MipLODBias = 0.0f;
  samp.MaxAnisotropy = 1;
  samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  samp.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  samp.MinLOD = 0.0f;
  samp.MaxLOD = D3D12_FLOAT32_MAX;
  samp.ShaderRegister = 0;
  samp.RegisterSpace = 0;
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.NumParameters = 2;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 1;
  rsDesc.pStaticSamplers = &samp;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rsBlob;
  ComPtr<ID3DBlob> rsError;
  const HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError);
  if (FAILED(hr))
  {
    DebugPrintf("root signature: %s\n", rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : "(no message)");
    FatalHr("D3D12SerializeRootSignature", hr);
  }
  CHECK_HR(m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&m_textRs)));

  ComPtr<ID3DBlob> vs = CompileShader(TEXT_SHADER, "VsMain", "vs_5_1");
  ComPtr<ID3DBlob> ps = CompileShader(TEXT_SHADER, "PsMain", "ps_5_1");

  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_textRs.Get();
  pso.VS.pShaderBytecode = vs->GetBufferPointer();
  pso.VS.BytecodeLength = vs->GetBufferSize();
  pso.PS.pShaderBytecode = ps->GetBufferPointer();
  pso.PS.BytecodeLength = ps->GetBufferSize();
  pso.BlendState.AlphaToCoverageEnable = FALSE;
  pso.BlendState.IndependentBlendEnable = FALSE;
  pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
  pso.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  pso.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = 0xFFFFFFFFu;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.FrontCounterClockwise = FALSE;
  pso.RasterizerState.DepthBias = 0;
  pso.RasterizerState.DepthBiasClamp = 0.0f;
  pso.RasterizerState.SlopeScaledDepthBias = 0.0f;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.RasterizerState.MultisampleEnable = FALSE;
  pso.RasterizerState.AntialiasedLineEnable = FALSE;
  pso.RasterizerState.ForcedSampleCount = 0;
  pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
  pso.DepthStencilState.DepthEnable = FALSE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.StencilEnable = FALSE;
  pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
  pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
  pso.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
  pso.InputLayout.pInputElementDescs = elements;
  pso.InputLayout.NumElements = UINT(std::size(elements));
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = BACK_BUFFER_FORMAT;
  pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
  pso.SampleDesc.Count = 1;
  pso.SampleDesc.Quality = 0;
  pso.NodeMask = 0;
  pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
  CHECK_HR(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_textPso)));
}

void Gfx::DrawTextLine(float _xPx, float _yPx, float _scale, Rgba _colour, std::string_view _text)
{
  const float advance = m_advancePx * _scale;
  const float quadW = m_cellWPx * _scale;
  const float quadH = m_cellHPx * _scale;
  const float uStep = m_cellWPx / m_atlasWPx;
  const float vStep = m_cellHPx / m_atlasHPx;

  float penX = _xPx;
  float penY = _yPx;
  for (const char c : _text)
  {
    if (c == '\n')
    {
      penX = _xPx;
      penY += quadH;
      continue;
    }
    if (c < FIRST_GLYPH || c > LAST_GLYPH)
    {
      penX += advance;
      continue;
    }
    if (m_textVerts.size() + 6 > MAX_TEXT_VERTS)
    {
      return;
    }

    const int index = c - FIRST_GLYPH;
    const float u0 = float(index % GLYPH_COLS) * uStep;
    const float v0 = float(index / GLYPH_COLS) * vStep;
    const float u1 = u0 + uStep;
    const float v1 = v0 + vStep;
    const float x0 = penX;
    const float y0 = penY;
    const float x1 = penX + quadW;
    const float y1 = penY + quadH;

    m_textVerts.push_back({x0, y0, u0, v0, _colour});
    m_textVerts.push_back({x1, y0, u1, v0, _colour});
    m_textVerts.push_back({x0, y1, u0, v1, _colour});
    m_textVerts.push_back({x0, y1, u0, v1, _colour});
    m_textVerts.push_back({x1, y0, u1, v0, _colour});
    m_textVerts.push_back({x1, y1, u1, v1, _colour});
    penX += advance;
  }
}

void Gfx::CreateScenePipeline()
{
  // Two blocks of root constants and nothing else: 32 DWORDs of matrices for the vertex stage and
  // 20 of shading values for the pixel stage, well inside the 64-DWORD root signature budget.
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.RegisterSpace = 0;
  params[0].Constants.Num32BitValues = 32;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.ShaderRegister = 1;
  params[1].Constants.RegisterSpace = 0;
  params[1].Constants.Num32BitValues = 20;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.NumParameters = 2;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 0;
  rsDesc.pStaticSamplers = nullptr;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> rsBlob;
  ComPtr<ID3DBlob> rsError;
  const HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError);
  if (FAILED(hr))
  {
    DebugPrintf("scene root signature: %s\n", rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : "(no message)");
    FatalHr("D3D12SerializeRootSignature (scene)", hr);
  }
  CHECK_HR(m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&m_sceneRs)));

  ComPtr<ID3DBlob> vs = CompileShader(SCENE_SHADER, "VsMain", "vs_5_1");
  ComPtr<ID3DBlob> ps = CompileShader(SCENE_SHADER, "PsMain", "ps_5_1");

  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_sceneRs.Get();
  pso.VS.pShaderBytecode = vs->GetBufferPointer();
  pso.VS.BytecodeLength = vs->GetBufferSize();
  pso.PS.pShaderBytecode = ps->GetBufferPointer();
  pso.PS.BytecodeLength = ps->GetBufferSize();
  pso.BlendState.AlphaToCoverageEnable = FALSE;
  pso.BlendState.IndependentBlendEnable = FALSE;
  pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
  pso.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
  pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
  pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  pso.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = 0xFFFFFFFFu;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // Cull nothing: the import flips Z, which reverses winding, and several hulls carry single-sided
  // panels that should be visible from both sides anyway.
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.FrontCounterClockwise = FALSE;
  pso.RasterizerState.DepthBias = 0;
  pso.RasterizerState.DepthBiasClamp = 0.0f;
  pso.RasterizerState.SlopeScaledDepthBias = 0.0f;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.RasterizerState.MultisampleEnable = FALSE;
  pso.RasterizerState.AntialiasedLineEnable = FALSE;
  pso.RasterizerState.ForcedSampleCount = 0;
  pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
  pso.DepthStencilState.DepthEnable = TRUE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  pso.DepthStencilState.StencilEnable = FALSE;
  pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
  pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
  pso.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
  pso.InputLayout.pInputElementDescs = elements;
  pso.InputLayout.NumElements = UINT(std::size(elements));
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = BACK_BUFFER_FORMAT;
  pso.DSVFormat = DEPTH_FORMAT;
  pso.SampleDesc.Count = 1;
  pso.SampleDesc.Quality = 0;
  pso.NodeMask = 0;
  pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
  CHECK_HR(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_scenePso)));
}

UINT Gfx::UploadMesh(const std::vector<SceneVertex>& _verts)
{
  GpuMesh mesh = {};
  const UINT64 bytes = UINT64(_verts.size()) * sizeof(SceneVertex);
  if (bytes == 0)
  {
    m_meshes.push_back(std::move(mesh));
    return UINT(m_meshes.size() - 1);
  }

  const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC rd = BufferDesc(bytes);
  CHECK_HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&mesh.vb)));

  uint8_t* dst = nullptr;
  D3D12_RANGE noRead = {0, 0};
  CHECK_HR(mesh.vb->Map(0, &noRead, reinterpret_cast<void**>(&dst)));
  std::memcpy(dst, _verts.data(), size_t(bytes));
  mesh.vb->Unmap(0, nullptr);

  mesh.vbv.BufferLocation = mesh.vb->GetGPUVirtualAddress();
  mesh.vbv.SizeInBytes = UINT(bytes);
  mesh.vbv.StrideInBytes = sizeof(SceneVertex);
  mesh.vertexCount = UINT(_verts.size());
  m_meshes.push_back(std::move(mesh));
  return UINT(m_meshes.size() - 1);
}

void Gfx::BeginScene(const SceneFrame& _frame)
{
  m_cmd->SetPipelineState(m_scenePso.Get());
  m_cmd->SetGraphicsRootSignature(m_sceneRs.Get());
  m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // The world matrix occupies DWORDs 0..15 and changes per draw; viewProj is set once here.
  m_cmd->SetGraphicsRoot32BitConstants(0, 16, &_frame.viewProj, 16);

  // Everything after baseColour, which is also per draw.
  const float shading[16] = {
      _frame.lightDir.x,      _frame.lightDir.y,       _frame.lightDir.z,      _frame.ambient,
      _frame.gridColour.r,    _frame.gridColour.g,     _frame.gridColour.b,    _frame.gridColour.a,
      _frame.gridSpacing,     _frame.gridLineWidthPx,  _frame.gridFadeDistance, 0.0f,
      _frame.cameraPos.x,     _frame.cameraPos.y,      _frame.cameraPos.z,     0.0f,
  };
  m_cmd->SetGraphicsRoot32BitConstants(1, 16, shading, 4);
}

void Gfx::DrawMesh(UINT _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _baseColour, float _materialMix, bool _isGround)
{
  if (_mesh >= m_meshes.size() || m_meshes[_mesh].vertexCount == 0)
  {
    return;
  }
  const GpuMesh& mesh = m_meshes[_mesh];
  const float base[4] = {_baseColour.r, _baseColour.g, _baseColour.b, _materialMix};
  const float mode = _isGround ? 1.0f : 0.0f;

  m_cmd->SetGraphicsRoot32BitConstants(0, 16, &_world, 0);
  m_cmd->SetGraphicsRoot32BitConstants(1, 4, base, 0);
  m_cmd->SetGraphicsRoot32BitConstants(1, 1, &mode, 15); // gridParams.w
  m_cmd->IASetVertexBuffers(0, 1, &mesh.vbv);
  m_cmd->DrawInstanced(mesh.vertexCount, 1, 0, 0);
}

void Gfx::DrawScreenRect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, Rgba _colour)
{
  if (m_textVerts.size() + 6 > MAX_TEXT_VERTS)
  {
    return;
  }
  const float u = m_solidU;
  const float v = m_solidV;
  m_textVerts.push_back({_x0Px, _y0Px, u, v, _colour});
  m_textVerts.push_back({_x1Px, _y0Px, u, v, _colour});
  m_textVerts.push_back({_x0Px, _y1Px, u, v, _colour});
  m_textVerts.push_back({_x0Px, _y1Px, u, v, _colour});
  m_textVerts.push_back({_x1Px, _y0Px, u, v, _colour});
  m_textVerts.push_back({_x1Px, _y1Px, u, v, _colour});
}

void Gfx::DrawScreenLine(float _x0Px, float _y0Px, float _x1Px, float _y1Px, float _thicknessPx, Rgba _colour)
{
  const float dx = _x1Px - _x0Px;
  const float dy = _y1Px - _y0Px;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length < 1e-4f || m_textVerts.size() + 6 > MAX_TEXT_VERTS)
  {
    return;
  }
  const float halfX = -dy / length * _thicknessPx * 0.5f;
  const float halfY = dx / length * _thicknessPx * 0.5f;
  const float u = m_solidU;
  const float v = m_solidV;
  m_textVerts.push_back({_x0Px + halfX, _y0Px + halfY, u, v, _colour});
  m_textVerts.push_back({_x1Px + halfX, _y1Px + halfY, u, v, _colour});
  m_textVerts.push_back({_x0Px - halfX, _y0Px - halfY, u, v, _colour});
  m_textVerts.push_back({_x0Px - halfX, _y0Px - halfY, u, v, _colour});
  m_textVerts.push_back({_x1Px + halfX, _y1Px + halfY, u, v, _colour});
  m_textVerts.push_back({_x1Px - halfX, _y1Px - halfY, u, v, _colour});
}

void Gfx::CreateDecalPipelines()
{
  ComPtr<ID3DBlob> vs = CompileShader(DECAL_SHADER, "VsMain", "vs_5_1");
  ComPtr<ID3DBlob> ps = CompileShader(DECAL_SHADER, "PsMain", "ps_5_1");

  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_sceneRs.Get(); // same 32 + 20 root constants as the scene pass
  pso.VS.pShaderBytecode = vs->GetBufferPointer();
  pso.VS.BytecodeLength = vs->GetBufferSize();
  pso.PS.pShaderBytecode = ps->GetBufferPointer();
  pso.PS.BytecodeLength = ps->GetBufferSize();
  pso.BlendState.AlphaToCoverageEnable = FALSE;
  pso.BlendState.IndependentBlendEnable = FALSE;
  pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
  pso.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  pso.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = 0xFFFFFFFFu;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.FrontCounterClockwise = FALSE;
  pso.RasterizerState.DepthBias = 0;
  pso.RasterizerState.DepthBiasClamp = 0.0f;
  pso.RasterizerState.SlopeScaledDepthBias = 0.0f;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.RasterizerState.MultisampleEnable = FALSE;
  pso.RasterizerState.AntialiasedLineEnable = FALSE;
  pso.RasterizerState.ForcedSampleCount = 0;
  pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
  // Tested against the scene so a ring is hidden by a hull in front of it, but never written, so
  // overlapping decals blend instead of fighting.
  pso.DepthStencilState.DepthEnable = TRUE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pso.DepthStencilState.StencilEnable = FALSE;
  pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
  pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
  pso.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
  pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
  pso.InputLayout.pInputElementDescs = elements;
  pso.InputLayout.NumElements = UINT(std::size(elements));
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = BACK_BUFFER_FORMAT;
  pso.DSVFormat = DEPTH_FORMAT;
  pso.SampleDesc.Count = 1;
  pso.SampleDesc.Quality = 0;
  pso.NodeMask = 0;
  pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
  CHECK_HR(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_decalPso)));

  // Same shader, added rather than blended, for thruster glow and trail.
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
  CHECK_HR(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_glowPso)));
}

void Gfx::BeginDecals(const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _cameraPos)
{
  m_cmd->SetGraphicsRootSignature(m_sceneRs.Get());
  m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_cmd->SetGraphicsRoot32BitConstants(0, 16, &_viewProj, 16);
  const float eye[4] = {_cameraPos.x, _cameraPos.y, _cameraPos.z, 0.0f};
  m_cmd->SetGraphicsRoot32BitConstants(1, 4, eye, 16);
}

void Gfx::DrawDecal(UINT _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _colour, float _thickness, float _fill)
{
  if (_mesh >= m_meshes.size() || m_meshes[_mesh].vertexCount == 0 || _colour.a <= 0.001f)
  {
    return;
  }
  const float colour[4] = {_colour.r, _colour.g, _colour.b, _colour.a};
  const float params[4] = {_thickness, _fill, 0.0f, 0.0f};
  m_cmd->SetPipelineState(m_decalPso.Get());
  m_cmd->SetGraphicsRoot32BitConstants(0, 16, &_world, 0);
  m_cmd->SetGraphicsRoot32BitConstants(1, 4, colour, 0);
  m_cmd->SetGraphicsRoot32BitConstants(1, 4, params, 4);
  m_cmd->IASetVertexBuffers(0, 1, &m_meshes[_mesh].vbv);
  m_cmd->DrawInstanced(m_meshes[_mesh].vertexCount, 1, 0, 0);
}

void Gfx::DrawGlow(UINT _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _colour, float _falloff)
{
  if (_mesh >= m_meshes.size() || m_meshes[_mesh].vertexCount == 0 || _colour.a <= 0.001f)
  {
    return;
  }
  const float colour[4] = {_colour.r, _colour.g, _colour.b, _colour.a};
  const float params[4] = {0.0f, _falloff, 1.0f, 0.0f};
  m_cmd->SetPipelineState(m_glowPso.Get());
  m_cmd->SetGraphicsRoot32BitConstants(0, 16, &_world, 0);
  m_cmd->SetGraphicsRoot32BitConstants(1, 4, colour, 0);
  m_cmd->SetGraphicsRoot32BitConstants(1, 4, params, 4);
  m_cmd->IASetVertexBuffers(0, 1, &m_meshes[_mesh].vbv);
  m_cmd->DrawInstanced(m_meshes[_mesh].vertexCount, 1, 0, 0);
}

void Gfx::RequestCapture(const std::wstring& _pngPath)
{
  m_capturePath = _pngPath;
  m_captureRequested = true;
}

void Gfx::RecordCaptureCopy()
{
  const D3D12_RESOURCE_DESC backBuffer = m_backBuffers[m_frameIndex]->GetDesc();
  UINT64 totalBytes = 0;
  m_device->GetCopyableFootprints(&backBuffer, 0, 1, 0, &m_captureFootprint, &m_captureRows, nullptr, &totalBytes);

  const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
  const D3D12_RESOURCE_DESC rd = BufferDesc(totalBytes);
  m_captureReadback.Reset();
  CHECK_HR(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&m_captureReadback)));

  const D3D12_RESOURCE_BARRIER toCopy =
      Transition(m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_cmd->ResourceBarrier(1, &toCopy);

  D3D12_TEXTURE_COPY_LOCATION destination = {};
  destination.pResource = m_captureReadback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint = m_captureFootprint;
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = m_backBuffers[m_frameIndex].Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source.SubresourceIndex = 0;
  m_cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  // The caller transitions COPY_SOURCE straight to PRESENT.
}

void Gfx::WriteCapturePng()
{
  const UINT width = m_captureFootprint.Footprint.Width;
  const UINT height = m_captureFootprint.Footprint.Height;
  const UINT rowPitch = m_captureFootprint.Footprint.RowPitch;

  uint8_t* mapped = nullptr;
  D3D12_RANGE readRange = {0, SIZE_T(rowPitch) * SIZE_T(m_captureRows)};
  if (FAILED(m_captureReadback->Map(0, &readRange, reinterpret_cast<void**>(&mapped))))
  {
    DebugPrintf("capture: could not map the readback buffer\n");
    return;
  }

  // The back buffer is RGBA and the PNG encoder wants BGRA, and the readback rows are padded to
  // 256 bytes, so repack into a tight buffer. Alpha is forced opaque: the swapchain's is not
  // meaningful and a half-transparent screenshot is no use.
  std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
  for (UINT y = 0; y < height; ++y)
  {
    const uint8_t* sourceRow = mapped + size_t(y) * rowPitch;
    uint8_t* destinationRow = pixels.data() + size_t(y) * size_t(width) * 4;
    for (UINT x = 0; x < width; ++x)
    {
      destinationRow[x * 4 + 0] = sourceRow[x * 4 + 2];
      destinationRow[x * 4 + 1] = sourceRow[x * 4 + 1];
      destinationRow[x * 4 + 2] = sourceRow[x * 4 + 0];
      destinationRow[x * 4 + 3] = 255;
    }
  }
  const D3D12_RANGE wroteNothing = {0, 0};
  m_captureReadback->Unmap(0, &wroteNothing);

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
  {
    DebugPrintf("capture: no WIC factory\n");
    return;
  }

  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> frameProperties;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;

  if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(m_capturePath.c_str(), GENERIC_WRITE)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &frameProperties)) ||
      FAILED(frame->Initialize(frameProperties.Get())) || FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)))
  {
    DebugPrintf("capture: could not start the PNG encoder for %S\n", m_capturePath.c_str());
    return;
  }
  if (FAILED(frame->WritePixels(height, width * 4, UINT(pixels.size()), pixels.data())) || FAILED(frame->Commit()) ||
      FAILED(encoder->Commit()))
  {
    DebugPrintf("capture: could not write %S\n", m_capturePath.c_str());
    return;
  }
  DebugPrintf("captured %S (%u x %u)\n", m_capturePath.c_str(), width, height);
}
