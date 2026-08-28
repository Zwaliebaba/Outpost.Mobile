#include "pch.h"
#include "TextRenderer.h"

#include "GpuHelpers.h"

// Shader bytecode, compiled by FXC at build time (AGENTS.md 3).
#include "CompiledShaders/TextVS.h"
#include "CompiledShaders/TextPS.h"

namespace Neuron
{
namespace
{
constexpr int FIRST_GLYPH = 32;
constexpr int LAST_GLYPH = 126;
constexpr int GLYPH_COLS = 16;
constexpr int GLYPH_ROWS = 6; // 96 cells for the 95 printable ASCII glyphs
constexpr int FONT_HEIGHT_PX = 16;

} // namespace

void TextRenderer::Init(GpuDevice& _gpu)
{
  CreatePipeline(_gpu);

  for (std::uint32_t i = 0; i < GpuDevice::FRAME_COUNT; ++i)
  {
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC rd = BufferDesc(static_cast<std::uint64_t>(MAX_VERTS) * sizeof(TextVertex));
    check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(m_vb[i].put())));
    D3D12_RANGE noRead = {0, 0};
    check_hresult(m_vb[i]->Map(0, &noRead, reinterpret_cast<void**>(&m_vbCpu[i]))); // mapped for the whole run
  }

  BakeFontAtlas(_gpu); // records into the device's command list, then executes and waits
  m_verts.reserve(MAX_VERTS);
}

void TextRenderer::CreatePipeline(GpuDevice& _gpu)
{
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1; // slot 0: the font atlas
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hd.NodeMask = 0;
  check_hresult(_gpu.Device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(m_srvHeap.put())));

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
  m_rootSignature = CreateRootSignature(_gpu.Device(), rsDesc, "text root signature");

  constexpr D3D12_INPUT_ELEMENT_DESC elements[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},};

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = DefaultPipelineDesc();
  pso.pRootSignature = m_rootSignature.get();
  pso.VS.pShaderBytecode = g_pTextVS;
  pso.VS.BytecodeLength = sizeof(g_pTextVS);
  pso.PS.pShaderBytecode = g_pTextPS;
  pso.PS.BytecodeLength = sizeof(g_pTextPS);
  pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  pso.InputLayout.pInputElementDescs = elements;
  pso.InputLayout.NumElements = static_cast<UINT>(std::size(elements));
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_pso.put())));
}

void TextRenderer::BakeFontAtlas(GpuDevice& _gpu)
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
    const wchar_t ch = static_cast<wchar_t>(FIRST_GLYPH + i);
    TextOutW(dc, (i % GLYPH_COLS) * cellW, (i / GLYPH_COLS) * cellH, &ch, 1);
  }
  // 95 printable glyphs sit in 96 cells. Fill the spare one solid so untextured screen quads --
  // selection boxes, order lines -- can share this pipeline instead of needing their own.
  constexpr int solidIndex = LAST_GLYPH - FIRST_GLYPH + 1;
  constexpr int solidCol = solidIndex % GLYPH_COLS;
  constexpr int solidRow = solidIndex / GLYPH_COLS;
  PatBlt(dc, solidCol * cellW, solidRow * cellH, cellW, cellH, WHITENESS);
  GdiFlush();

  // GDI leaves alpha at zero, so take coverage from the white-on-black luminance.
  std::vector<std::uint8_t> coverage(static_cast<size_t>(atlasW) * static_cast<size_t>(atlasH));
  auto src = static_cast<const std::uint8_t*>(bits);
  for (size_t i = 0; i < coverage.size(); ++i)
    coverage[i] = std::max({src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2]});

  SelectObject(dc, oldBmp);
  SelectObject(dc, oldFont);
  DeleteObject(dib);
  DeleteObject(font);
  DeleteDC(dc);

  m_cellWPx = static_cast<float>(cellW);
  m_cellHPx = static_cast<float>(cellH);
  m_advancePx = static_cast<float>(tm.tmAveCharWidth); // fixed pitch, so this is the true advance
  m_atlasWPx = static_cast<float>(atlasW);
  m_atlasHPx = static_cast<float>(atlasH);
  m_solidU = (static_cast<float>(solidCol) + 0.5f) * static_cast<float>(cellW) / static_cast<float>(atlasW);
  m_solidV = (static_cast<float>(solidRow) + 0.5f) * static_cast<float>(cellH) / static_cast<float>(atlasH);
  DebugTrace("font atlas {}x{}, cell {}x{}, advance {}\n", atlasW, atlasH, cellW, cellH, tm.tmAveCharWidth);

  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Alignment = 0;
  td.Width = static_cast<std::uint64_t>(atlasW);
  td.Height = static_cast<UINT>(atlasH);
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8_UNORM;
  td.SampleDesc.Count = 1;
  td.SampleDesc.Quality = 0;
  td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  td.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(m_fontTex.put())));

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rowCount = 0;
  std::uint64_t rowBytes = 0;
  std::uint64_t totalBytes = 0;
  _gpu.Device()->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rowCount, &rowBytes, &totalBytes);

  GpuPtr<ID3D12Resource> upload;
  hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC ud = BufferDesc(totalBytes);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(upload.put())));

  std::uint8_t* dst = nullptr;
  D3D12_RANGE noRead = {0, 0};
  check_hresult(upload->Map(0, &noRead, reinterpret_cast<void**>(&dst)));
  for (UINT row = 0; row < rowCount; ++row)
    std::memcpy(dst + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                coverage.data() + static_cast<size_t>(row) * static_cast<size_t>(atlasW), static_cast<size_t>(rowBytes));
  upload->Unmap(0, nullptr);

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();

  D3D12_TEXTURE_COPY_LOCATION copyDst = {};
  copyDst.pResource = m_fontTex.get();
  copyDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copyDst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copySrc = {};
  copySrc.pResource = upload.get();
  copySrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  copySrc.PlacedFootprint = footprint;
  cmd->CopyTextureRegion(&copyDst, 0, 0, 0, &copySrc, nullptr);

  const D3D12_RESOURCE_BARRIER toShader = Transition(m_fontTex.get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  cmd->ResourceBarrier(1, &toShader);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT_R8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MostDetailedMip = 0;
  srv.Texture2D.MipLevels = 1;
  srv.Texture2D.PlaneSlice = 0;
  srv.Texture2D.ResourceMinLODClamp = 0.0f;
  _gpu.Device()->CreateShaderResourceView(m_fontTex.get(), &srv, m_srvHeap->GetCPUDescriptorHandleForHeapStart());

  _gpu.ExecuteAndWait(); // the upload buffer stays alive until here
}

void TextRenderer::DrawTextLine(float _xPx, float _yPx, float _scale, Rgba _colour, std::string_view _text)
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
    if (m_verts.size() + 6 > MAX_VERTS)
      return;

    const int index = c - FIRST_GLYPH;
    const float u0 = static_cast<float>(index % GLYPH_COLS) * uStep;
    const float v0 = static_cast<float>(index / GLYPH_COLS) * vStep;
    const float u1 = u0 + uStep;
    const float v1 = v0 + vStep;
    const float x0 = penX;
    const float y0 = penY;
    const float x1 = penX + quadW;
    const float y1 = penY + quadH;

    m_verts.push_back({x0, y0, u0, v0, _colour});
    m_verts.push_back({x1, y0, u1, v0, _colour});
    m_verts.push_back({x0, y1, u0, v1, _colour});
    m_verts.push_back({x0, y1, u0, v1, _colour});
    m_verts.push_back({x1, y0, u1, v0, _colour});
    m_verts.push_back({x1, y1, u1, v1, _colour});
    penX += advance;
  }
}

void TextRenderer::DrawScreenRect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, Rgba _colour)
{
  if (m_verts.size() + 6 > MAX_VERTS)
    return;
  const float u = m_solidU;
  const float v = m_solidV;
  m_verts.push_back({_x0Px, _y0Px, u, v, _colour});
  m_verts.push_back({_x1Px, _y0Px, u, v, _colour});
  m_verts.push_back({_x0Px, _y1Px, u, v, _colour});
  m_verts.push_back({_x0Px, _y1Px, u, v, _colour});
  m_verts.push_back({_x1Px, _y0Px, u, v, _colour});
  m_verts.push_back({_x1Px, _y1Px, u, v, _colour});
}

void TextRenderer::DrawScreenLine(float _x0Px, float _y0Px, float _x1Px, float _y1Px, float _thicknessPx, Rgba _colour)
{
  const float dx = _x1Px - _x0Px;
  const float dy = _y1Px - _y0Px;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length < 1e-4f || m_verts.size() + 6 > MAX_VERTS)
    return;
  const float halfX = -dy / length * _thicknessPx * 0.5f;
  const float halfY = dx / length * _thicknessPx * 0.5f;
  const float u = m_solidU;
  const float v = m_solidV;
  m_verts.push_back({_x0Px + halfX, _y0Px + halfY, u, v, _colour});
  m_verts.push_back({_x1Px + halfX, _y1Px + halfY, u, v, _colour});
  m_verts.push_back({_x0Px - halfX, _y0Px - halfY, u, v, _colour});
  m_verts.push_back({_x0Px - halfX, _y0Px - halfY, u, v, _colour});
  m_verts.push_back({_x1Px + halfX, _y1Px + halfY, u, v, _colour});
  m_verts.push_back({_x1Px - halfX, _y1Px - halfY, u, v, _colour});
}

void TextRenderer::Flush(GpuDevice& _gpu)
{
  if (m_verts.empty())
    return;

  const std::uint32_t frame = _gpu.FrameIndex();
  const size_t count = std::min<size_t>(m_verts.size(), MAX_VERTS);
  std::memcpy(m_vbCpu[frame], m_verts.data(), count * sizeof(TextVertex));

  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  vbv.BufferLocation = m_vb[frame]->GetGPUVirtualAddress();
  vbv.SizeInBytes = static_cast<UINT>(count * sizeof(TextVertex));
  vbv.StrideInBytes = sizeof(TextVertex);

  const float invViewport[2] = {1.0f / static_cast<float>(_gpu.WidthPx()), 1.0f / static_cast<float>(_gpu.HeightPx())};
  ID3D12DescriptorHeap* heaps[] = {m_srvHeap.get()};
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _gpu.BackBufferView();

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr); // the overlay sits on top, no depth
  cmd->SetPipelineState(m_pso.get());
  cmd->SetGraphicsRootSignature(m_rootSignature.get());
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRoot32BitConstants(0, 2, invViewport, 0);
  cmd->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->IASetVertexBuffers(0, 1, &vbv);
  cmd->DrawInstanced(static_cast<UINT>(count), 1, 0, 0);
}
} // namespace Neuron
