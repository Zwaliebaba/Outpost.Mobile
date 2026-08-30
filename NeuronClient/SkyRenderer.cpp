#include "pch.h"
#include "SkyRenderer.h"

#include "GpuHelpers.h"
#include "SkyVertex.h"

// Shader bytecode, compiled by DXC at build time (AGENTS.md 3).
#include "CompiledShaders/SkyVS.h"
#include "CompiledShaders/SkyPS.h"

using namespace DirectX;

namespace Neuron
{
namespace
{
// The offsets are spelled by hand here; SkyVertex.h's static_asserts on the struct's size and field
// offsets are what keep the two from disagreeing. The packed formats are expanded by the input
// assembler, so the vertex shader reads float3 / float2 / float4 / float4 / float2.
constexpr D3D12_INPUT_ELEMENT_DESC SKY_ELEMENTS[] = {
  {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"TEXCOORD", 0, DXGI_FORMAT_R16G16_SNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"TEXCOORD", 1, DXGI_FORMAT_R16G16_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

// viewProj, then camera right, up and eye, then the four frame scalars.
constexpr std::uint32_t SKY_CONSTANT_COUNT = 32;
} // namespace

void SkyRenderer::Init(GpuDevice& _gpu, const Desc& _desc)
{
  CreatePipeline(_gpu);

  LoadTexture(_gpu, SkyLayer::Nebula, _desc.nebulaTexture);
  LoadTexture(_gpu, SkyLayer::Star, _desc.starTexture);
  LoadTexture(_gpu, SkyLayer::Burst, _desc.burstTexture);

  m_texturesReady = m_textures[0] && m_textures[1] && m_textures[2];
  if (!m_texturesReady)
    DebugTrace("sky: a texture is missing, so the sky will not draw\n");
}

void SkyRenderer::LoadTexture(GpuDevice& _gpu, SkyLayer _layer, const std::wstring& _fileName)
{
  DdsImage image;
  if (!DdsImage::Load(_fileName, image))
  {
    DebugTrace(L"sky texture {} did not load\n", _fileName);
    return;
  }

  // Whatever the file holds goes up as it is; today all three are one BGRA8 mip.
  const std::uint32_t slot = static_cast<std::uint32_t>(_layer);
  UploadDdsTexture(_gpu, image, _gpu.SrvCpuHandle(m_slots[slot]), m_textures[slot], m_textureStaging[slot]);
}

void SkyRenderer::CreatePipeline(GpuDevice& _gpu)
{
  // Slots in the shared heap; the device null-filled every slot at creation.
  for (std::uint32_t at = 0; at < TEXTURE_COUNT; ++at)
    m_slots[at] = _gpu.SrvAllocator().Allocate();

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
  params[0].Constants.Num32BitValues = SKY_CONSTANT_COUNT;
  // Vertex only: the pixel stage reads an interpolated color and a texture, and nothing else.
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Linear and clamping. All three files are radial images whose edge is transparent, so wrapping
  // would bleed one side of a cloud onto the other, and a star magnified to seventeen pixels out of a
  // 128-pixel glow needs the filtering the effect's point-sampled sprites deliberately go without.
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
  rsDesc.NumParameters = 2;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 1;
  rsDesc.pStaticSamplers = &sampler;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  m_rootSignature = CreateRootSignature(_gpu.Device(), rsDesc, "sky root signature");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = DefaultPipelineDesc();
  pso.pRootSignature = m_rootSignature.get();
  pso.InputLayout.pInputElementDescs = SKY_ELEMENTS;
  pso.InputLayout.NumElements = static_cast<UINT>(std::size(SKY_ELEMENTS));
  pso.VS.pShaderBytecode = g_pSkyVS;
  pso.VS.BytecodeLength = sizeof(g_pSkyVS);
  pso.PS.pShaderBytecode = g_pSkyPS;
  pso.PS.BytecodeLength = sizeof(g_pSkyPS);

  // Additive, and unconditionally so: a star is light arriving, never light removed, and two stars
  // whose glows overlap are brighter where they do. Source alpha is not in it -- the intensity is in
  // the color the vertex carries -- so ONE, ONE says exactly what happens and nothing else can be
  // read into it. The destination's alpha is left alone, as the effect's sprite passes leave it.
  pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;

  // No depth at all, neither tested nor written. The sky is drawn before anything else and is behind
  // everything by construction, so there is nothing for a test to decide -- and a depth *write* would
  // be worse than useless, because the sphere sits at five kilometers and would occlude a planet that
  // is meant to be further away than the sky is. DSVFormat still names the depth buffer because the
  // frame has one bound and a pipeline that disagreed with the bound target is a debug-layer error.
  pso.DSVFormat = DEPTH_FORMAT;
  pso.DepthStencilState.DepthEnable = FALSE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_pso.put())));
}

void SkyRenderer::UploadField(GpuDevice& _gpu, const SkyMesh& _mesh)
{
  m_vertexCount = 0;
  for (std::uint32_t i = 0; i < SKY_LAYER_COUNT; ++i)
  {
    m_layerFirstVertex[i] = _mesh.LayerFirstVertex(static_cast<SkyLayer>(i));
    m_layerVertexCount[i] = _mesh.layerVertexCount[i];
  }

  if (_mesh.verts.empty())
  {
    DebugTrace("sky: an empty field was uploaded, so nothing will draw\n");
    return;
  }

  // put() asserts the pointer is empty rather than releasing what was there (AGENTS.md 5), so a
  // second sky has to release the first explicitly. Safe here and nowhere else: this is called
  // inside a copy bracket, and BeginCopies has waited for the previous batch before opening one --
  // which is what the first sky's copy was in. It no longer waits for *frames*, so a sky that is
  // still being drawn from is the case to keep in mind if this ever moves out of a reseed
  // (ADR 0044).
  m_vb = nullptr;
  m_vbStaging = nullptr;

  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(_mesh.verts.data()),
                                            _mesh.verts.size() * sizeof(SkyVertex));
  UploadStaticBuffer(_gpu, bytes, m_vb, m_vbStaging);

  m_vertexCount = static_cast<std::uint32_t>(_mesh.verts.size());
  m_vbv.BufferLocation = m_vb->GetGPUVirtualAddress();
  m_vbv.SizeInBytes = static_cast<UINT>(bytes.size());
  m_vbv.StrideInBytes = sizeof(SkyVertex);
}

void SkyRenderer::DiscardStaging() noexcept
{
  for (GpuPtr<ID3D12Resource>& staging : m_textureStaging)
    staging = nullptr;
  m_vbStaging = nullptr;
}

void SkyRenderer::Draw(GpuDevice& _gpu, const Frame& _frame)
{
  if (!Ready())
    return;

  float constants[SKY_CONSTANT_COUNT];
  std::memcpy(constants, &_frame.viewProj, sizeof(_frame.viewProj));
  constants[16] = _frame.cameraRight.x;
  constants[17] = _frame.cameraRight.y;
  constants[18] = _frame.cameraRight.z;
  constants[19] = 0.0f;
  constants[20] = _frame.cameraUp.x;
  constants[21] = _frame.cameraUp.y;
  constants[22] = _frame.cameraUp.z;
  constants[23] = 0.0f;
  constants[24] = _frame.cameraPos.x;
  constants[25] = _frame.cameraPos.y;
  constants[26] = _frame.cameraPos.z;
  constants[27] = 0.0f;
  constants[28] = _frame.radiusMetres;
  constants[29] = _frame.timeSec;
  constants[30] = _frame.intensity;
  constants[31] = _frame.twinkleMaxRateRadPerSec;

  // BeginFrame has bound both of these already, so today this is a restatement -- and the sky is the
  // first pass of the frame, so there is nothing before it to have rebound them. It stays for the
  // same reason FxRenderer's does: the day the passes are reordered, this is what stops the sky
  // inheriting a target somebody else set.
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _gpu.BackBufferView();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = _gpu.DepthView();

  ID3D12DescriptorHeap* heaps[] = {_gpu.SrvHeap()};
  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  cmd->SetGraphicsRootSignature(m_rootSignature.get());
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRoot32BitConstants(0, SKY_CONSTANT_COUNT, constants, 0);
  cmd->SetPipelineState(m_pso.get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->IASetVertexBuffers(0, 1, &m_vbv);

  // Faintest first: the nebulosity, then the stars over it, then the flares over those. Additive
  // blending is commutative, so the order buys nothing today -- it is the order the layers are laid
  // out in, and it is the order that stays right the day one of them stops being additive.
  for (std::uint32_t layer = 0; layer < SKY_LAYER_COUNT; ++layer)
  {
    if (m_layerVertexCount[layer] == 0)
      continue;

    cmd->SetGraphicsRootDescriptorTable(1, _gpu.SrvGpuHandle(m_slots[layer]));
    cmd->DrawInstanced(m_layerVertexCount[layer], 1, m_layerFirstVertex[layer], 0);
  }
}
} // namespace Neuron
