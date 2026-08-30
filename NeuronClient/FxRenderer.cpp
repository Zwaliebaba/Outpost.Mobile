#include "pch.h"
#include "FxRenderer.h"

#include "GpuHelpers.h"

// Shader bytecode, compiled by DXC at build time (AGENTS.md 3).
#include "CompiledShaders/FxFragmentVS.h"
#include "CompiledShaders/FxFragmentPS.h"
#include "CompiledShaders/FxSpriteVS.h"
#include "CompiledShaders/FxSpritePS.h"
#include "CompiledShaders/FxGlowPS.h"

using namespace DirectX;

namespace Neuron
{
namespace
{
// The offsets are spelled by hand here and in BodyRenderer; FxVertex.h's static_asserts on the
// struct's size and field offsets are what keep them from disagreeing. The packed formats are
// expanded by the input assembler, so the vertex shaders read float3 / float4 / float2.
constexpr D3D12_INPUT_ELEMENT_DESC FX_ELEMENTS[] = {
  {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"NORMAL", 0, DXGI_FORMAT_R16G16B16A16_SNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
} // namespace

void FxRenderer::Init(GpuDevice& _gpu, const Desc& _desc)
{
  CreatePipelines(_gpu);

  for (std::uint32_t i = 0; i < GpuDevice::FRAME_COUNT; ++i)
  {
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC rd = BufferDesc(static_cast<std::uint64_t>(MAX_FX_VERTS) * sizeof(FxVertex));
    check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(m_vb[i].put())));
    D3D12_RANGE noRead = {0, 0};
    check_hresult(m_vb[i]->Map(0, &noRead, reinterpret_cast<void**>(&m_vbCpu[i]))); // mapped for the whole run
  }

  // Everything the glow pass needs now exists: pipelines, the slots CreatePipelines allocated, and
  // the ring. The textures below are the explosion's, and a glow reads none of them.
  m_ringReady = true;

  // The loads ride the copy queue now, in one bracket of this pass's own -- Init runs outside the
  // composition root's load brackets, which is why it carried a direct bracket before. Boot is the
  // one place waiting is acceptable: the staging buffers may only be released once the copies have
  // actually run.
  _gpu.BeginCopies();
  LoadTexture(_gpu, FRAGMENT_SLOT, _desc.fragmentTexture);
  LoadTexture(_gpu, SPRITE_SLOT, _desc.spriteTexture);
  LoadTexture(_gpu, FLASH_SLOT, _desc.flashTexture);
  _gpu.SubmitCopies();
  _gpu.WaitForCopies();
  for (GpuPtr<ID3D12Resource>& staging : m_staging)
    staging = nullptr; // the staging buffers stayed alive until the copies had run

  m_ready = m_textures[FRAGMENT_SLOT] && m_textures[SPRITE_SLOT] && m_textures[FLASH_SLOT];
  if (!m_ready)
    DebugTrace("fx: a texture is missing, so the explosion effect will not draw\n");
}

void FxRenderer::LoadTexture(GpuDevice& _gpu, std::uint32_t _slot, const std::wstring& _fileName)
{
  DdsImage image;
  if (!DdsImage::Load(_fileName, image))
  {
    DebugTrace(L"fx texture {} did not load\n", _fileName);
    return;
  }

  // Whatever the file holds -- today one BGRA8 mip, tomorrow a baked BC chain -- goes up as it is.
  UploadDdsTexture(_gpu, image, _gpu.SrvCpuHandle(m_slots[_slot]), m_textures[_slot], m_staging[_slot]);
}

void FxRenderer::CreatePipelines(GpuDevice& _gpu)
{
  // Slots in the shared heap rather than a heap of this pass's own. The device wrote a null SRV
  // into every slot at creation, so an unloaded texture reads zero -- which the glow pass relies
  // on, since it binds the table and samples nothing (Design/CompressedTextures-work-order.md 2.1).
  for (std::uint32_t at = 0; at < TEXTURE_COUNT; ++at)
    m_slots[at] = _gpu.SrvAllocator().Allocate();

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
  params[0].Constants.Num32BitValues = 16; // viewProj, and no world matrix: the vertices are already in world space
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.ShaderRegister = 1;
  params[1].Constants.RegisterSpace = 0;
  params[1].Constants.Num32BitValues = 8; // lightDirAmbient, cameraPos
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &range;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
  // s0, the fragment decal: linear and wrapping, because the wireframe is a 128-texel picture with
  // real lines in it and a shard's uv runs off the edge of it.
  samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  // s1, the sprites: point magnification, as the source effect had it. Particle.dds is a 16x16 flat
  // tile with a lighter rim; its softness comes from the blend, not from filtering the texture.
  // MIN_LINEAR_MAG_MIP_POINT is D3D12's spelling of "linear minification, point magnification";
  // there is no MIN_LINEAR_MAG_POINT_MIP_POINT in the enum, only this.
  samplers[1].Filter = D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
  samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  for (std::uint32_t i = 0; i < 2; ++i)
  {
    samplers[i].MipLODBias = 0.0f;
    samplers[i].MaxAnisotropy = 1;
    samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[i].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplers[i].MinLOD = 0.0f;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ShaderRegister = i;
    samplers[i].RegisterSpace = 0;
    samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.NumParameters = 3;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 2;
  rsDesc.pStaticSamplers = samplers;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  m_rootSignature = CreateRootSignature(_gpu.Device(), rsDesc, "fx root signature");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = DefaultPipelineDesc();
  pso.pRootSignature = m_rootSignature.get();
  pso.InputLayout.pInputElementDescs = FX_ELEMENTS;
  pso.InputLayout.NumElements = static_cast<UINT>(std::size(FX_ELEMENTS));
  pso.DSVFormat = DEPTH_FORMAT; // all three draw into the scene's depth, unlike the overlay
  pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
  pso.DepthStencilState.DepthEnable = TRUE;

  // Fragments: ordinary transparency, and they *write* depth, so a shard occludes the smoke behind
  // it. The two sprite passes below do not, so overlapping smoke blends instead of fighting.
  pso.VS.pShaderBytecode = g_pFxFragmentVS;
  pso.VS.BytecodeLength = sizeof(g_pFxFragmentVS);
  pso.PS.pShaderBytecode = g_pFxFragmentPS;
  pso.PS.BytecodeLength = sizeof(g_pFxFragmentPS);
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_fragmentPso.put())));

  pso.VS.pShaderBytecode = g_pFxSpriteVS;
  pso.VS.BytecodeLength = sizeof(g_pFxSpriteVS);
  pso.PS.pShaderBytecode = g_pFxSpritePS;
  pso.PS.BytecodeLength = sizeof(g_pFxSpritePS);
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  // The back buffer's alpha is never read, so the sprite passes leave the destination's alone and
  // say what they mean about colour only.
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;

  // INV_SRC_COLOR, not INV_SRC_ALPHA, and this is the difference between smoke and nothing at all.
  // The vertices carry a zero alpha (SpriteParticles::Build), so the source term vanishes and the
  // frame becomes dest * (1 - src.rgb): a light sprite darkens what is behind it. Written with
  // INV_SRC_ALPHA the sprite is simply invisible, which reads as a missing texture and sends
  // whoever is looking at it to the wrong file.
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_spriteDarkPso.put())));

  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE; // the fireball
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_spriteAddPso.put())));

  // Thruster glows and trails. The blend and depth state below is SceneRenderer's retired glow
  // pipeline field for field -- additive colour, tested against the scene and never written -- so
  // the only thing that changed about a glow is how many draws a frame of them costs.
  pso.PS.pShaderBytecode = g_pFxGlowPS;
  pso.PS.BytecodeLength = sizeof(g_pFxGlowPS);
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_glowPso.put())));
}

void FxRenderer::Begin(GpuDevice& _gpu, const XMFLOAT4X4& _viewProj, const XMFLOAT3& _lightDir, float _ambient, const XMFLOAT3& _cameraPos)
{
  if (!m_ringReady)
    return;

  // The ring resets once a frame and not once a Begin. The pass is begun twice -- fragments before
  // the decals, sprites after -- and resetting on the second would overwrite vertices the GPU has
  // not read yet, in a way that only shows up as flicker on a busy frame.
  const std::uint32_t frame = _gpu.FrameIndex();
  if (frame != m_ringFrameIndex)
  {
    m_ringFrameIndex = frame;
    m_ringOffsetVerts = 0;
    m_droppedVerts = 0;
  }

  const float shading[8] = {_lightDir.x, _lightDir.y, _lightDir.z, _ambient, _cameraPos.x, _cameraPos.y, _cameraPos.z, 0.0f};

  ID3D12DescriptorHeap* heaps[] = {_gpu.SrvHeap()};
  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetGraphicsRootSignature(m_rootSignature.get());
  // Heaps are per command list and the overlay pass sets its own later in the frame, so this cannot
  // be assumed to have survived from the last Begin.
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_viewProj, 0);
  cmd->SetGraphicsRoot32BitConstants(1, 8, shading, 0);
}

// The three passes below sample a texture, so each is gated on every texture having loaded. The
// guard sits here rather than in Draw because DrawGlows shares Draw and has no such requirement.
void FxRenderer::DrawFragments(GpuDevice& _gpu, std::span<const FxVertex> _verts)
{
  if (!m_ready)
    return;
  Draw(_gpu, m_fragmentPso.get(), FRAGMENT_SLOT, _verts);
}

void FxRenderer::DrawSpritesDark(GpuDevice& _gpu, std::span<const FxVertex> _verts)
{
  if (!m_ready)
    return;
  Draw(_gpu, m_spriteDarkPso.get(), SPRITE_SLOT, _verts);
}

void FxRenderer::DrawSpritesAdd(GpuDevice& _gpu, std::span<const FxVertex> _verts)
{
  if (!m_ready)
    return;
  Draw(_gpu, m_spriteAddPso.get(), SPRITE_SLOT, _verts);
}

void FxRenderer::DrawGlows(GpuDevice& _gpu, std::span<const FxVertex> _verts, float _falloff)
{
  if (!m_ringReady || _verts.empty())
    return;

  // The falloff is the last DWORD of the pixel constants -- cameraPos.w, which Begin left at zero
  // and the textured passes never read. Set here rather than in Begin because it belongs to this
  // pass alone, and once per frame either way.
  _gpu.CommandList()->SetGraphicsRoot32BitConstants(1, 1, &_falloff, 7);
  // The descriptor table is still bound from Begin. FxGlowPS declares no texture, so which slot it
  // points at does not matter; SPRITE_SLOT keeps it pointing at something real.
  Draw(_gpu, m_glowPso.get(), SPRITE_SLOT, _verts);
}

void FxRenderer::Draw(GpuDevice& _gpu, ID3D12PipelineState* _pso, std::uint32_t _srvSlot, std::span<const FxVertex> _verts)
{
  if (!m_ringReady || _verts.empty())
    return;

  const std::uint32_t wanted = static_cast<std::uint32_t>(_verts.size());
  std::uint32_t count = std::min(wanted, MAX_FX_VERTS - m_ringOffsetVerts);
  count -= count % 3; // whole triangles: half a triangle is worse than none of one
  if (count < wanted)
  {
    const std::uint32_t lost = wanted - count;
    const std::uint32_t capacity = MAX_FX_VERTS;
    if (m_droppedVerts == 0) // once a frame: the ring is reset in Begin and so is this count
      DebugTrace("fx: the vertex ring holds {} verts and this frame wanted more; {} dropped\n", capacity, lost);
    m_droppedVerts += lost;
  }
  if (count == 0)
    return;

  const std::uint32_t frame = _gpu.FrameIndex();
  const std::size_t offsetBytes = static_cast<std::size_t>(m_ringOffsetVerts) * sizeof(FxVertex);
  std::memcpy(m_vbCpu[frame] + offsetBytes, _verts.data(), static_cast<std::size_t>(count) * sizeof(FxVertex));

  // One buffer, one view per draw: the offset is what keeps this draw's vertices clear of the last.
  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  vbv.BufferLocation = m_vb[frame]->GetGPUVirtualAddress() + offsetBytes;
  vbv.SizeInBytes = static_cast<UINT>(count * sizeof(FxVertex));
  vbv.StrideInBytes = sizeof(FxVertex);

  const D3D12_GPU_DESCRIPTOR_HANDLE srv = _gpu.SrvGpuHandle(m_slots[_srvSlot]);

  // BeginFrame has bound both of these already, so today this is a restatement. The overlay's Flush
  // rebinds the target *without* depth, and the day the passes are reordered this is what stops the
  // effect inheriting that.
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _gpu.BackBufferView();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = _gpu.DepthView();

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  cmd->SetPipelineState(_pso);
  cmd->SetGraphicsRootDescriptorTable(2, srv);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->IASetVertexBuffers(0, 1, &vbv);
  cmd->DrawInstanced(count, 1, 0, 0);

  m_ringOffsetVerts += count;
}
} // namespace Neuron
