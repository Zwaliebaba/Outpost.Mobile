#include "pch.h"
#include "BodyRenderer.h"

#include "GpuHelpers.h"

// Shader bytecode, compiled by FXC at build time (AGENTS.md 3).
#include "CompiledShaders/BodyVS.h"
#include "CompiledShaders/BodyPS.h"
#include "CompiledShaders/BodyOverlayPS.h"

using namespace DirectX;

namespace Neuron
{
namespace
{
// Spelled here even though FxRenderer spells the same four elements: the two renderers share a
// vertex format, not an array. The day they share one array is a RenderTypes.h change with a reason
// behind it. FxVertex.h's static_assert on the struct's size is what keeps either from drifting.
constexpr D3D12_INPUT_ELEMENT_DESC BODY_ELEMENTS[] = {
  {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

// The vertex stage's root constants are the scene's layout: world at DWORD 0, viewProj at 16. Keep
// them, and SetGraphicsRoot32BitConstants(0, 16, &world, 0) reads the same here as it does in
// SceneRenderer::DrawMesh. Write world at the wrong offset and it lands on viewProj: the body
// vanishes, with no error anywhere.
constexpr UINT VS_CONSTANT_DWORDS = 32;
constexpr UINT WORLD_OFFSET_DWORDS = 0;
constexpr UINT VIEW_PROJ_OFFSET_DWORDS = 16;
constexpr UINT PS_CONSTANT_DWORDS = 12; // lightDirAmbient, cameraPos, overlayParams
} // namespace

void BodyRenderer::Init(GpuDevice& _gpu, const Desc& _desc)
{
  CreatePipelines(_gpu);

  // The table is bound on every draw, terrain included, and a descriptor heap starts uninitialised:
  // a slot that never gets a view is a handle the debug layer reports the moment anything binds it.
  // The null view is overwritten by the real one below when the file loads.
  D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
  nullSrv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nullSrv.Texture2D.MipLevels = 1;
  _gpu.Device()->CreateShaderResourceView(nullptr, &nullSrv, m_srvHeap->GetCPUDescriptorHandleForHeapStart());

  DdsImage image;
  if (!DdsImage::Load(_desc.outlineTexture, image))
  {
    DebugTrace(L"body outline {} did not load; bodies will draw without their wire frame\n", _desc.outlineTexture);
    return;
  }

  // One mip, BGRA8, real alpha: a copy rather than a conversion, and no mip chain to build. The
  // shader's fwidth fade stands in for the mips the tree does not generate.
  ByteBuffer pixels;
  if (!image.TopMipAsBgra(pixels))
  {
    DebugTrace(L"body outline {} is not an uncompressed 8-bit surface\n", _desc.outlineTexture);
    return;
  }

  // Recorded into whatever list the composition root opened, and deliberately not submitted here:
  // the bodies' vertex copies go into the same list and one ExecuteAndWait carries all of them.
  const D3D12_CPU_DESCRIPTOR_HANDLE srv = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
  UploadColourTexture(_gpu, image.widthPx, image.heightPx, pixels, srv, m_outline, m_outlineStaging);
  m_outlineReady = m_outline != nullptr;
}

void BodyRenderer::CreatePipelines(GpuDevice& _gpu)
{
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1; // the outline, and nothing else: a body's colour is in its vertices
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hd.NodeMask = 0;
  check_hresult(_gpu.Device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(m_srvHeap.put())));

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  range.RegisterSpace = 0;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.RegisterSpace = 0;
  params[0].Constants.Num32BitValues = VS_CONSTANT_DWORDS;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.ShaderRegister = 1;
  params[1].Constants.RegisterSpace = 0;
  params[1].Constants.Num32BitValues = PS_CONSTANT_DWORDS;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &range;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Linear and wrapping: the outline is a 128-texel picture with real lines in it, and a body's uv
  // is the grid cell index, so it runs from zero to sixty-four across a face.
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.MipLODBias = 0.0f;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  sampler.MinLOD = 0.0f;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.RegisterSpace = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.NumParameters = 3;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 1;
  rsDesc.pStaticSamplers = &sampler;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  m_rootSignature = CreateRootSignature(_gpu.Device(), rsDesc, "body root signature");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = DefaultPipelineDesc();
  pso.pRootSignature = m_rootSignature.get();
  pso.InputLayout.pInputElementDescs = BODY_ELEMENTS;
  pso.InputLayout.NumElements = static_cast<UINT>(std::size(BODY_ELEMENTS));
  pso.DSVFormat = DEPTH_FORMAT;
  pso.VS.pShaderBytecode = g_pBodyVS;
  pso.VS.BytecodeLength = sizeof(g_pBodyVS);

  // The terrain: opaque, depth tested and written, so a body occludes a hull behind it and its own
  // far side is rejected. Cull comes from DefaultPipelineDesc, which is NONE for the whole tree;
  // at fifty thousand triangles a planet that is a second sphere rasterised for nothing, and
  // whether it can be turned off is a measurement on the winding, not an assumption.
  pso.PS.pShaderBytecode = g_pBodyPS;
  pso.PS.BytecodeLength = sizeof(g_pBodyPS);
  pso.DepthStencilState.DepthEnable = TRUE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_mainPso.put())));

  // The outline: additive, depth tested but not written, and LESS_EQUAL so that it lands exactly on
  // the terrain it belongs to rather than being rejected by it.
  pso.PS.pShaderBytecode = g_pBodyOverlayPS;
  pso.PS.BytecodeLength = sizeof(g_pBodyOverlayPS);
  pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_overlayPso.put())));
}

BodyHandle BodyRenderer::UploadBody(GpuDevice& _gpu, std::span<const FxVertex> _verts)
{
  if (_verts.empty())
  {
    DebugTrace("body: an empty vertex list was offered for upload and refused\n");
    return INVALID_BODY;
  }

  GpuMesh mesh = {};
  GpuPtr<ID3D12Resource> staging;
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(_verts.data()), _verts.size() * sizeof(FxVertex));
  UploadStaticBuffer(_gpu, bytes, mesh.vb, staging);

  mesh.vbv.BufferLocation = mesh.vb->GetGPUVirtualAddress();
  mesh.vbv.SizeInBytes = static_cast<UINT>(bytes.size());
  mesh.vbv.StrideInBytes = sizeof(FxVertex);
  mesh.vertexCount = static_cast<std::uint32_t>(_verts.size());

  m_staging.push_back(std::move(staging));
  m_bodies.push_back(std::move(mesh));
  return static_cast<BodyHandle>(m_bodies.size() - 1);
}

void BodyRenderer::DiscardStaging() noexcept
{
  m_staging.clear();
  m_outlineStaging = nullptr;
}

void BodyRenderer::Begin(GpuDevice& _gpu, const XMFLOAT4X4& _viewProj, const XMFLOAT3& _lightDir, float _ambient,
                         const XMFLOAT3& _cameraPos, const BodyOverlayParams& _overlay)
{
  const float shading[PS_CONSTANT_DWORDS] = {_lightDir.x,   _lightDir.y,   _lightDir.z,       _ambient,
                                             _cameraPos.x,  _cameraPos.y,  _cameraPos.z,      0.0f,
                                             _overlay.gain, _overlay.fade, _overlay.specular, _overlay.shininess};

  ID3D12DescriptorHeap* heaps[] = {m_srvHeap.get()};
  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetGraphicsRootSignature(m_rootSignature.get());
  // Heaps are per command list and other passes set their own later in the frame, so this cannot be
  // assumed to have survived from the last Begin.
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_viewProj, VIEW_PROJ_OFFSET_DWORDS);
  cmd->SetGraphicsRoot32BitConstants(1, PS_CONSTANT_DWORDS, shading, 0);
}

void BodyRenderer::DrawMain(GpuDevice& _gpu, BodyHandle _body, const XMFLOAT4X4& _world)
{
  Draw(_gpu, m_mainPso.get(), _body, _world);
}

void BodyRenderer::DrawOverlay(GpuDevice& _gpu, BodyHandle _body, const XMFLOAT4X4& _world)
{
  if (!m_outlineReady)
    return;

  Draw(_gpu, m_overlayPso.get(), _body, _world);
}

void BodyRenderer::Draw(GpuDevice& _gpu, ID3D12PipelineState* _pso, BodyHandle _body, const XMFLOAT4X4& _world)
{
  if (_body >= m_bodies.size() || m_bodies[_body].vertexCount == 0)
    return;

  const GpuMesh& mesh = m_bodies[_body];

  // BeginFrame has bound both of these already, so today this is a restatement. The text overlay's
  // Flush rebinds the target *without* depth, and the day the passes are reordered this is what
  // stops a body inheriting that and drawing through everything.
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _gpu.BackBufferView();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = _gpu.DepthView();

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  cmd->SetPipelineState(_pso);
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_world, WORLD_OFFSET_DWORDS);
  cmd->SetGraphicsRootDescriptorTable(2, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
  cmd->IASetVertexBuffers(0, 1, &mesh.vbv);
  cmd->DrawInstanced(mesh.vertexCount, 1, 0, 0);
}
} // namespace Neuron
