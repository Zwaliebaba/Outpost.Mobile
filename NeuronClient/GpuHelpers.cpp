#include "pch.h"
#include "GpuHelpers.h"

#include <d3dcompiler.h>

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

GpuPtr<ID3DBlob> CompileShader(const char* _source, const char* _entry, const char* _target)
{
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
  GpuPtr<ID3DBlob> code;
  GpuPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(_source, std::strlen(_source), _entry, nullptr, nullptr, _entry, _target, flags, 0, code.put(),
                                errors.put());
  if (FAILED(hr))
  {
    // The compiler's own message is the only useful part; check_hresult then carries the failure
    // to the composition root, which is where it becomes something a person reads.
    DebugTrace("shader {} failed: {}\n", _entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
    check_hresult(hr);
  }
  return code;
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
} // namespace Neuron
