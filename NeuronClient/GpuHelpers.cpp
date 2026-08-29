#include "pch.h"
#include "GpuHelpers.h"

#include "GpuDevice.h"

namespace Neuron
{
D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE _type) noexcept
{
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = _type;
  hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  hp.CreationNodeMask = 1;
  hp.VisibleNodeMask = 1;
  return hp;
}

D3D12_RESOURCE_DESC BufferDesc(std::uint64_t _bytes) noexcept
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

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* _resource, D3D12_RESOURCE_STATES _before, D3D12_RESOURCE_STATES _after) noexcept
{
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  b.Transition.pResource = _resource;
  b.Transition.StateBefore = _before;
  b.Transition.StateAfter = _after;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return b;
}

GpuPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* _device, const D3D12_ROOT_SIGNATURE_DESC& _desc, const char* _what)
{
  GpuPtr<ID3DBlob> blob;
  GpuPtr<ID3DBlob> errors;
  const HRESULT hr = D3D12SerializeRootSignature(&_desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put());
  if (FAILED(hr))
  {
    DebugTrace("root signature {}: {}\n", _what, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
    check_hresult(hr);
  }

  GpuPtr<ID3D12RootSignature> signature;
  check_hresult(_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(signature.put())));
  return signature;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC DefaultPipelineDesc() noexcept
{
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
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
  // Cull nothing: the OBJ import flips Z, which reverses winding, and several hulls carry
  // single-sided panels that should be visible from both sides anyway.
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
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = BACK_BUFFER_FORMAT;
  pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
  pso.SampleDesc.Count = 1;
  pso.SampleDesc.Quality = 0;
  pso.NodeMask = 0;
  pso.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
  return pso;
}

bool CoverageOf(const DdsImage& _image, ByteBuffer& _outCoverage)
{
  ByteBuffer pixels;
  if (!_image.TopMipAsBgra(pixels))
    return false;

  const bool hasAlpha = _image.HasAlpha();
  ByteBuffer coverage(pixels.size() / 4);
  for (size_t i = 0; i < coverage.size(); ++i)
  {
    const std::uint8_t* texel = pixels.data() + i * 4;
    coverage[i] = hasAlpha ? texel[3] : std::max({texel[0], texel[1], texel[2]});
  }
  _outCoverage = std::move(coverage);
  return true;
}

void UploadCoverageTexture(GpuDevice& _gpu, std::uint32_t _widthPx, std::uint32_t _heightPx, const ByteBuffer& _coverage,
                           D3D12_CPU_DESCRIPTOR_HANDLE _srv, GpuPtr<ID3D12Resource>& _outTexture, GpuPtr<ID3D12Resource>& _outStaging)
{
  constexpr DXGI_FORMAT COVERAGE_FORMAT = DXGI_FORMAT_R8_UNORM; // one channel, because it is coverage and not colour

  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Alignment = 0;
  td.Width = _widthPx;
  td.Height = _heightPx;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = COVERAGE_FORMAT;
  td.SampleDesc.Count = 1;
  td.SampleDesc.Quality = 0;
  td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  td.Flags = D3D12_RESOURCE_FLAG_NONE;

  // put() asserts the pointer is empty rather than releasing what was there, so a texture loaded
  // twice has to let go of the first one itself.
  _outTexture = nullptr;
  _outStaging = nullptr;

  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(_outTexture.put())));

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rowCount = 0;
  std::uint64_t rowBytes = 0;
  std::uint64_t totalBytes = 0;
  _gpu.Device()->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rowCount, &rowBytes, &totalBytes);

  hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC ud = BufferDesc(totalBytes);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(_outStaging.put())));

  std::uint8_t* dst = nullptr;
  D3D12_RANGE noRead = {0, 0};
  check_hresult(_outStaging->Map(0, &noRead, reinterpret_cast<void**>(&dst)));
  for (UINT row = 0; row < rowCount; ++row)
    std::memcpy(dst + static_cast<size_t>(row) * footprint.Footprint.RowPitch, _coverage.data() + static_cast<size_t>(row) * _widthPx,
                static_cast<size_t>(rowBytes));
  _outStaging->Unmap(0, nullptr);

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();

  D3D12_TEXTURE_COPY_LOCATION copyDst = {};
  copyDst.pResource = _outTexture.get();
  copyDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copyDst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copySrc = {};
  copySrc.pResource = _outStaging.get();
  copySrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  copySrc.PlacedFootprint = footprint;
  cmd->CopyTextureRegion(&copyDst, 0, 0, 0, &copySrc, nullptr);

  const D3D12_RESOURCE_BARRIER toShader =
    Transition(_outTexture.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  cmd->ResourceBarrier(1, &toShader);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = COVERAGE_FORMAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MostDetailedMip = 0;
  srv.Texture2D.MipLevels = 1;
  srv.Texture2D.PlaneSlice = 0;
  srv.Texture2D.ResourceMinLODClamp = 0.0f;
  _gpu.Device()->CreateShaderResourceView(_outTexture.get(), &srv, _srv);
}

void UploadColourTexture(GpuDevice& _gpu, std::uint32_t _widthPx, std::uint32_t _heightPx, const ByteBuffer& _pixels,
                         D3D12_CPU_DESCRIPTOR_HANDLE _srv, GpuPtr<ID3D12Resource>& _outTexture, GpuPtr<ID3D12Resource>& _outStaging)
{
  constexpr DXGI_FORMAT COLOUR_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM; // four channels, because it is colour and not coverage
  constexpr std::uint32_t BYTES_PER_TEXEL = 4;

  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Alignment = 0;
  td.Width = _widthPx;
  td.Height = _heightPx;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = COLOUR_FORMAT;
  td.SampleDesc.Count = 1;
  td.SampleDesc.Quality = 0;
  td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  td.Flags = D3D12_RESOURCE_FLAG_NONE;

  // put() asserts the pointer is empty rather than releasing what was there, so a texture loaded
  // twice has to let go of the first one itself.
  _outTexture = nullptr;
  _outStaging = nullptr;

  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(_outTexture.put())));

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rowCount = 0;
  std::uint64_t rowBytes = 0;
  std::uint64_t totalBytes = 0;
  _gpu.Device()->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rowCount, &rowBytes, &totalBytes);

  hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC ud = BufferDesc(totalBytes);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(_outStaging.put())));

  std::uint8_t* dst = nullptr;
  D3D12_RANGE noRead = {0, 0};
  check_hresult(_outStaging->Map(0, &noRead, reinterpret_cast<void**>(&dst)));
  for (UINT row = 0; row < rowCount; ++row)
    std::memcpy(dst + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                _pixels.data() + static_cast<size_t>(row) * _widthPx * BYTES_PER_TEXEL, static_cast<size_t>(rowBytes));
  _outStaging->Unmap(0, nullptr);

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();

  D3D12_TEXTURE_COPY_LOCATION copyDst = {};
  copyDst.pResource = _outTexture.get();
  copyDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copyDst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copySrc = {};
  copySrc.pResource = _outStaging.get();
  copySrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  copySrc.PlacedFootprint = footprint;
  cmd->CopyTextureRegion(&copyDst, 0, 0, 0, &copySrc, nullptr);

  const D3D12_RESOURCE_BARRIER toShader =
    Transition(_outTexture.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  cmd->ResourceBarrier(1, &toShader);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = COLOUR_FORMAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MostDetailedMip = 0;
  srv.Texture2D.MipLevels = 1;
  srv.Texture2D.PlaneSlice = 0;
  srv.Texture2D.ResourceMinLODClamp = 0.0f;
  _gpu.Device()->CreateShaderResourceView(_outTexture.get(), &srv, _srv);
}

void UploadStaticBuffer(GpuDevice& _gpu, std::span<const std::uint8_t> _bytes, GpuPtr<ID3D12Resource>& _outBuffer,
                        GpuPtr<ID3D12Resource>& _outStaging)
{
  // put() asserts the pointer is empty rather than releasing what was there, so a buffer filled
  // twice has to let go of the first one itself.
  _outBuffer = nullptr;
  _outStaging = nullptr;
  if (_bytes.empty())
    return;

  const std::uint64_t bytes = static_cast<std::uint64_t>(_bytes.size());

  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  const D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(_outBuffer.put())));

  hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(_outStaging.put())));

  std::uint8_t* dst = nullptr;
  D3D12_RANGE noRead = {0, 0};
  check_hresult(_outStaging->Map(0, &noRead, reinterpret_cast<void**>(&dst)));
  std::memcpy(dst, _bytes.data(), static_cast<size_t>(bytes));
  _outStaging->Unmap(0, nullptr);

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->CopyBufferRegion(_outBuffer.get(), 0, _outStaging.get(), 0, bytes);

  const D3D12_RESOURCE_BARRIER toVertices =
    Transition(_outBuffer.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  cmd->ResourceBarrier(1, &toVertices);
}
} // namespace Neuron
