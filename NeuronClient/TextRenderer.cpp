#include "pch.h"
#include "TextRenderer.h"

#include "GpuHelpers.h"

// Shader bytecode, compiled by FXC at build time (AGENTS.md 3).
#include "CompiledShaders/TextVS.h"
#include "CompiledShaders/TextPS.h"

namespace Neuron
{
void TextRenderer::Init(GpuDevice& _gpu, const Desc& _desc)
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

  m_srvStride = _gpu.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // Both atlases record into the device's command list and are submitted together: ExecuteAndWait
  // closes the list, so there is one flush here and not one per font.
  LoadFont(_gpu, FontId::Ui, _desc.uiFont);
  LoadFont(_gpu, FontId::Scene, _desc.sceneFont);
  if (_desc.images.size() > MAX_IMAGES)
    DebugTrace("text: {} images listed and only {} slots; the rest are dropped\n", _desc.images.size(), MAX_IMAGES);
  for (std::uint32_t i = 0; i < _desc.images.size() && i < MAX_IMAGES; ++i)
    LoadImage(_gpu, i, _desc.images[i]);
  _gpu.ExecuteAndWait();
  for (BitmapFont& font : m_fonts)
    font.DiscardStaging(); // the staging buffers stayed alive until the copies had run
  for (ScreenImage& image : m_images)
    image.DiscardStaging();

  m_verts.reserve(MAX_VERTS);
}

D3D12_CPU_DESCRIPTOR_HANDLE TextRenderer::SrvHandle(std::uint32_t _slot) const noexcept
{
  D3D12_CPU_DESCRIPTOR_HANDLE srv = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
  srv.ptr += static_cast<SIZE_T>(_slot) * m_srvStride;
  return srv;
}

void TextRenderer::LoadFont(GpuDevice& _gpu, FontId _font, const std::wstring& _fileName)
{
  const std::uint32_t index = static_cast<std::uint32_t>(_font);
  if (!m_fonts[index].Load(_gpu, _fileName, SrvHandle(index)))
    DebugTrace(L"font {} did not load; text queued on it will not draw\n", _fileName);
}

void TextRenderer::LoadImage(GpuDevice& _gpu, ImageId _image, const std::wstring& _fileName)
{
  if (!m_images[_image].Load(_gpu, _fileName, SrvHandle(FONT_COUNT + _image)))
    DebugTrace(L"image {} did not load; quads queued on it will not draw\n", _fileName);
}

void TextRenderer::CreatePipeline(GpuDevice& _gpu)
{
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = FONT_COUNT + MAX_IMAGES; // one atlas per font, in FontId order, then the images
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
  // Point, not linear. These atlases are one-bit pixel fonts: filtering turns a two-texel stem into
  // a grey smear, and there is no mip chain for it to interpolate towards anyway.
  samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
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
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

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

void TextRenderer::PushQuad(std::uint32_t _srvSlot, const TextVertex& _a, const TextVertex& _b, const TextVertex& _c, const TextVertex& _d)
{
  if (m_verts.size() + 6 > MAX_VERTS)
    return;

  if (m_batches.empty() || m_batches.back().srvSlot != _srvSlot)
    m_batches.push_back({_srvSlot, static_cast<std::uint32_t>(m_verts.size()), 0});

  m_verts.push_back(_a);
  m_verts.push_back(_b);
  m_verts.push_back(_c);
  m_verts.push_back(_c);
  m_verts.push_back(_b);
  m_verts.push_back(_d);
  m_batches.back().vertCount += 6;
}

void TextRenderer::DrawTextLine(FontId _font, float _xPx, float _yPx, float _scale, Rgba _colour, std::string_view _text)
{
  const BitmapFont& font = m_fonts[static_cast<std::uint32_t>(_font)];
  if (!font.Ready())
    return;

  // Square cells and fixed pitch, so one number is the quad, the pen step and the line height.
  const float cellPx = font.CellPx() * _scale;

  float penX = _xPx;
  float penY = _yPx;
  for (const char c : _text)
  {
    if (c == '\n')
    {
      penX = _xPx;
      penY += cellPx;
      continue;
    }

    // Read as a byte, not as a char: the atlas runs past the printable ASCII range into Latin-1,
    // and char is signed here, so the second half would come out negative.
    const std::uint8_t glyph = static_cast<std::uint8_t>(c);
    if (!font.HasGlyph(glyph))
    {
      penX += cellPx;
      continue;
    }

    const BitmapFont::Cell cell = font.GlyphCell(glyph);
    const float x1 = penX + cellPx;
    const float y1 = penY + cellPx;
    PushQuad(static_cast<std::uint32_t>(_font), {penX, penY, cell.u0, cell.v0, _colour}, {x1, penY, cell.u1, cell.v0, _colour},
             {penX, y1, cell.u0, cell.v1, _colour}, {x1, y1, cell.u1, cell.v1, _colour});
    penX += cellPx;
  }
}

void TextRenderer::DrawScreenRect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, Rgba _colour)
{
  // Shapes take the UI atlas's solid cell, so they queue in the same run as the HUD around them.
  const BitmapFont& font = m_fonts[static_cast<std::uint32_t>(FontId::Ui)];
  if (!font.Ready())
    return;

  const float u = font.SolidU();
  const float v = font.SolidV();
  PushQuad(static_cast<std::uint32_t>(FontId::Ui), {_x0Px, _y0Px, u, v, _colour}, {_x1Px, _y0Px, u, v, _colour},
           {_x0Px, _y1Px, u, v, _colour}, {_x1Px, _y1Px, u, v, _colour});
}

void TextRenderer::DrawScreenLine(float _x0Px, float _y0Px, float _x1Px, float _y1Px, float _thicknessPx, Rgba _colour)
{
  const BitmapFont& font = m_fonts[static_cast<std::uint32_t>(FontId::Ui)];
  if (!font.Ready())
    return;

  const float dx = _x1Px - _x0Px;
  const float dy = _y1Px - _y0Px;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length < 1e-4f)
    return;

  const float halfX = -dy / length * _thicknessPx * 0.5f;
  const float halfY = dx / length * _thicknessPx * 0.5f;
  const float u = font.SolidU();
  const float v = font.SolidV();
  PushQuad(static_cast<std::uint32_t>(FontId::Ui), {_x0Px + halfX, _y0Px + halfY, u, v, _colour},
           {_x1Px + halfX, _y1Px + halfY, u, v, _colour}, {_x0Px - halfX, _y0Px - halfY, u, v, _colour},
           {_x1Px - halfX, _y1Px - halfY, u, v, _colour});
}

void TextRenderer::DrawScreenImage(ImageId _image, float _x0Px, float _y0Px, float _x1Px, float _y1Px, Rgba _colour)
{
  if (!ImageReady(_image))
    return;

  PushQuad(FONT_COUNT + _image, {_x0Px, _y0Px, 0.0f, 0.0f, _colour}, {_x1Px, _y0Px, 1.0f, 0.0f, _colour},
           {_x0Px, _y1Px, 0.0f, 1.0f, _colour}, {_x1Px, _y1Px, 1.0f, 1.0f, _colour});
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
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->IASetVertexBuffers(0, 1, &vbv);

  // One draw per run, in the order the runs were queued. Only the texture changes between them, so
  // switching font or image costs a descriptor table and nothing else.
  const D3D12_GPU_DESCRIPTOR_HANDLE srvStart = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
  for (const Batch& batch : m_batches)
  {
    D3D12_GPU_DESCRIPTOR_HANDLE srv = srvStart;
    srv.ptr += static_cast<UINT64>(batch.srvSlot) * m_srvStride;
    cmd->SetGraphicsRootDescriptorTable(1, srv);
    cmd->DrawInstanced(batch.vertCount, 1, batch.firstVert, 0);
  }
}
} // namespace Neuron
