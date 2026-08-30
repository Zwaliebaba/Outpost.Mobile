#include "pch.h"
#include "SceneRenderer.h"

#include "GpuHelpers.h"

// Shader bytecode, compiled by FXC at build time (AGENTS.md 3). Nothing here compiles HLSL at
// runtime: a shader mistake is a build error rather than a message box on somebody's machine, and
// the binary carries no dependency on d3dcompiler_47.dll.
#include "CompiledShaders/SceneVS.h"
#include "CompiledShaders/SceneInstancedVS.h"
#include "CompiledShaders/ScenePS.h"
#include "CompiledShaders/DecalVS.h"
#include "CompiledShaders/DecalPS.h"

namespace Neuron
{
namespace
{
// The decal pipelines share this layout with a Decal.hlsli VsIn that never declares RACE. That is
// legal -- an input element no shader consumes is ignored -- so do not add it there to fix a
// warning that will not come.
constexpr D3D12_INPUT_ELEMENT_DESC SCENE_ELEMENTS[] = {
  {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"RACE", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

// The same two per-vertex elements, plus the instance stream at slot 1. The offsets are
// MeshInstance's, spelled by hand here because nothing else could disagree with the struct -- which
// is what its two static_asserts are for.
constexpr D3D12_INPUT_ELEMENT_DESC SCENE_INSTANCED_ELEMENTS[] = {
  {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"RACE", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"INSTANCEWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  {"INSTANCEWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  {"INSTANCEWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  {"INSTANCEWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  {"INSTANCETINT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
};

// Instances the ring holds per frame. A fleet of this size is already past what the review projects
// a shard carrying, and it is 328 kB a frame; three of those is a megabyte, which is nothing beside
// what it saves. Past it, DrawMeshInstanced reports rather than the buffer growing.
constexpr std::uint32_t MAX_INSTANCES = 4096;

// A unit quad in the XZ plane, centred on the origin.
std::vector<MeshVertex> BuildUnitQuad()
{
  constexpr float h = 0.5f;
  return {
    MeshVertex{-h, 0.0f, -h, 1.0f, 1.0f, 1.0f}, MeshVertex{-h, 0.0f, h, 1.0f, 1.0f, 1.0f}, MeshVertex{h, 0.0f, h, 1.0f, 1.0f, 1.0f},
    MeshVertex{-h, 0.0f, -h, 1.0f, 1.0f, 1.0f}, MeshVertex{h, 0.0f, h, 1.0f, 1.0f, 1.0f},  MeshVertex{h, 0.0f, -h, 1.0f, 1.0f, 1.0f},
  };
}
} // namespace

void SceneRenderer::Init(GpuDevice& _gpu)
{
  CreateScenePipeline(_gpu);
  CreateDecalPipelines(_gpu);
  CreateInstanceRing(_gpu);

  // The quad's vertices go to a default heap now, so its copy has to be recorded into a list and
  // run. Bracketed here rather than left to the caller because this is the only upload Init does --
  // the same shape TextRenderer::Init already has.
  _gpu.BeginUploads();
  m_unitQuad = UploadMesh(_gpu, BuildUnitQuad());
  _gpu.ExecuteAndWait();
  DiscardStaging();
}

void SceneRenderer::CreateInstanceRing(GpuDevice& _gpu)
{
  for (std::uint32_t frame = 0; frame < GpuDevice::FRAME_COUNT; ++frame)
  {
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC rd = BufferDesc(static_cast<std::uint64_t>(MAX_INSTANCES) * sizeof(MeshInstance));
    check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(m_instanceBuffers[frame].put())));
    D3D12_RANGE noRead = {0, 0};
    check_hresult(m_instanceBuffers[frame]->Map(0, &noRead, reinterpret_cast<void**>(&m_instanceCpu[frame])));
  }
}

void SceneRenderer::DiscardStaging() noexcept
{
  m_staging.clear();
}

void SceneRenderer::CreateScenePipeline(GpuDevice& _gpu)
{
  // Two blocks of root constants and nothing else: 32 DWORDs of matrices for the vertex stage and
  // 12 of shading values for the pixel stage, well inside the 64-DWORD root signature budget. The
  // decal pipelines share this signature, so Decal.hlsli's block is the same 12 DWORDs (Decal.hlsli).
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.RegisterSpace = 0;
  params[0].Constants.Num32BitValues = 32;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.ShaderRegister = 1;
  params[1].Constants.RegisterSpace = 0;
  params[1].Constants.Num32BitValues = 12;
  // ALL rather than PIXEL: SceneVS reads baseColour now, so that it can write it into VsOut and let
  // one pixel shader serve both the instanced path and this one (Scene.hlsli). The decal pipelines
  // share this signature and read the block from the pixel stage only, which ALL also allows.
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.NumParameters = 2;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 0;
  rsDesc.pStaticSamplers = nullptr;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  m_sceneRs = CreateRootSignature(_gpu.Device(), rsDesc, "scene root signature");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = DefaultPipelineDesc();
  pso.pRootSignature = m_sceneRs.get();
  pso.VS.pShaderBytecode = g_pSceneVS;
  pso.VS.BytecodeLength = sizeof(g_pSceneVS);
  pso.PS.pShaderBytecode = g_pScenePS;
  pso.PS.BytecodeLength = sizeof(g_pScenePS);
  pso.DepthStencilState.DepthEnable = TRUE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  pso.InputLayout.pInputElementDescs = SCENE_ELEMENTS;
  pso.InputLayout.NumElements = static_cast<UINT>(std::size(SCENE_ELEMENTS));
  pso.DSVFormat = DEPTH_FORMAT;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_scenePso.put())));

  // The instanced hull path: same root signature, same pixel shader, same blend and depth state.
  // What differs is the vertex shader and one more input slot.
  pso.VS.pShaderBytecode = g_pSceneInstancedVS;
  pso.VS.BytecodeLength = sizeof(g_pSceneInstancedVS);
  pso.InputLayout.pInputElementDescs = SCENE_INSTANCED_ELEMENTS;
  pso.InputLayout.NumElements = static_cast<UINT>(std::size(SCENE_INSTANCED_ELEMENTS));
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_instancedPso.put())));
}

void SceneRenderer::CreateDecalPipelines(GpuDevice& _gpu)
{
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = DefaultPipelineDesc();
  pso.pRootSignature = m_sceneRs.get(); // same 32 + 20 root constants as the scene pass
  pso.VS.pShaderBytecode = g_pDecalVS;
  pso.VS.BytecodeLength = sizeof(g_pDecalVS);
  pso.PS.pShaderBytecode = g_pDecalPS;
  pso.PS.BytecodeLength = sizeof(g_pDecalPS);
  pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
  pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  // Tested against the scene so a ring is hidden by a hull in front of it, but never written, so
  // overlapping decals blend instead of fighting.
  pso.DepthStencilState.DepthEnable = TRUE;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pso.InputLayout.pInputElementDescs = SCENE_ELEMENTS;
  pso.InputLayout.NumElements = static_cast<UINT>(std::size(SCENE_ELEMENTS));
  pso.DSVFormat = DEPTH_FORMAT;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_decalPso.put())));
}

MeshHandle SceneRenderer::UploadMesh(GpuDevice& _gpu, const std::vector<MeshVertex>& _verts)
{
  GpuMesh mesh = {};
  const std::uint64_t bytes = static_cast<std::uint64_t>(_verts.size()) * sizeof(MeshVertex);
  if (bytes == 0)
  {
    m_meshes.push_back(std::move(mesh));
    return static_cast<MeshHandle>(m_meshes.size() - 1);
  }

  // A default heap, through a staging copy the caller's upload bracket runs. This used to be an
  // upload heap with a straight memcpy, and the comment beside it argued -- correctly, for the fleet
  // it was written against -- that a few thousand triangles do not justify a staging copy.
  //
  // What changed is not the upload, it is the reading. An upload heap is system memory, so the input
  // assembler crosses PCIe for it on every draw and, once instancing lands, on every *instance*. The
  // hulls in this tree average 32 kB: at a hundred ships that is 0.3 GB/s and the old comment holds,
  // at five hundred it is 1.4 and at two thousand 5.6, which is bandwidth-dead on the hardware
  // ADR 0019 targets (Design/MmoScalabilityReview.md G2). Instancing removes the draws; this removes
  // the fetch.
  GpuPtr<ID3D12Resource> staging;
  const std::span<const std::uint8_t> raw(reinterpret_cast<const std::uint8_t*>(_verts.data()), static_cast<std::size_t>(bytes));
  UploadStaticBuffer(_gpu, raw, mesh.vb, staging);
  m_staging.push_back(std::move(staging));

  mesh.vbv.BufferLocation = mesh.vb->GetGPUVirtualAddress();
  mesh.vbv.SizeInBytes = static_cast<UINT>(bytes);
  mesh.vbv.StrideInBytes = sizeof(MeshVertex);
  mesh.vertexCount = static_cast<std::uint32_t>(_verts.size());
  m_meshes.push_back(std::move(mesh));
  return static_cast<MeshHandle>(m_meshes.size() - 1);
}

void SceneRenderer::BeginScene(GpuDevice& _gpu, const SceneFrame& _frame)
{
  // The instance ring resets once a frame, here, because BeginScene is called once. Resetting it in
  // DrawMeshInstanced would overwrite instances the GPU has not read yet, which shows up as one hull
  // family wearing another's positions on a busy frame.
  const std::uint32_t frame = _gpu.FrameIndex();
  if (frame != m_instanceFrameIndex)
  {
    m_instanceFrameIndex = frame;
    m_instanceOffset = 0;
    m_droppedInstances = 0;
  }

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetPipelineState(m_scenePso.get());
  cmd->SetGraphicsRootSignature(m_sceneRs.get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // The world matrix occupies DWORDs 0..15 and changes per draw; viewProj is set once here.
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_frame.viewProj, 16);

  // Everything after baseColour, which is also per draw.
  const float shading[8] = {
    _frame.lightDir.x,  _frame.lightDir.y,  _frame.lightDir.z,  _frame.ambient,
    _frame.cameraPos.x, _frame.cameraPos.y, _frame.cameraPos.z, 0.0f,
  };
  cmd->SetGraphicsRoot32BitConstants(1, 8, shading, 4);
}

void SceneRenderer::DrawMesh(GpuDevice& _gpu, MeshHandle _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _baseColour, float _materialMix)
{
  if (_mesh >= m_meshes.size() || m_meshes[_mesh].vertexCount == 0)
    return;
  const GpuMesh& mesh = m_meshes[_mesh];
  const float base[4] = {_baseColour.r, _baseColour.g, _baseColour.b, _materialMix};

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  // Set here rather than relied on from BeginScene. DrawMeshInstanced switches the pipeline, so a
  // caller drawing one mesh after a batch of them would otherwise get the instanced input layout
  // with no instance buffer bound -- which is not an error anything reports, just wrong geometry.
  cmd->SetPipelineState(m_scenePso.get());
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_world, 0);
  cmd->SetGraphicsRoot32BitConstants(1, 4, base, 0);
  cmd->IASetVertexBuffers(0, 1, &mesh.vbv);
  cmd->DrawInstanced(mesh.vertexCount, 1, 0, 0);
}

void SceneRenderer::DrawMeshInstanced(GpuDevice& _gpu, MeshHandle _mesh, std::span<const MeshInstance> _instances)
{
  if (_mesh >= m_meshes.size() || m_meshes[_mesh].vertexCount == 0 || _instances.empty())
    return;

  const std::uint32_t wanted = static_cast<std::uint32_t>(_instances.size());
  const std::uint32_t count = std::min(wanted, MAX_INSTANCES - m_instanceOffset);
  if (count < wanted)
  {
    if (m_droppedInstances == 0) // once a frame: the ring is reset in BeginScene and so is this count
      DebugTrace("scene: the instance ring holds {} and this frame wanted more; {} dropped\n", MAX_INSTANCES, wanted - count);
    m_droppedInstances += wanted - count;
  }
  if (count == 0)
    return;

  const std::uint32_t frame = _gpu.FrameIndex();
  const std::size_t offsetBytes = static_cast<std::size_t>(m_instanceOffset) * sizeof(MeshInstance);
  std::memcpy(m_instanceCpu[frame] + offsetBytes, _instances.data(), static_cast<std::size_t>(count) * sizeof(MeshInstance));

  // One buffer, one view per draw: the offset is what keeps this family's instances clear of the
  // last one's, the same way FxRenderer's vertex ring works.
  D3D12_VERTEX_BUFFER_VIEW instanceView = {};
  instanceView.BufferLocation = m_instanceBuffers[frame]->GetGPUVirtualAddress() + offsetBytes;
  instanceView.SizeInBytes = static_cast<UINT>(count * sizeof(MeshInstance));
  instanceView.StrideInBytes = sizeof(MeshInstance);

  const D3D12_VERTEX_BUFFER_VIEW views[2] = {m_meshes[_mesh].vbv, instanceView};

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetPipelineState(m_instancedPso.get());
  cmd->IASetVertexBuffers(0, 2, views);
  cmd->DrawInstanced(m_meshes[_mesh].vertexCount, count, 0, 0);

  m_instanceOffset += count;
}

void SceneRenderer::BeginDecals(GpuDevice& _gpu, const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _cameraPos)
{
  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetGraphicsRootSignature(m_sceneRs.get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_viewProj, 16);
  const float eye[4] = {_cameraPos.x, _cameraPos.y, _cameraPos.z, 0.0f};
  cmd->SetGraphicsRoot32BitConstants(1, 4, eye, 8);
}

void SceneRenderer::DrawDecal(GpuDevice& _gpu, MeshHandle _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _colour, float _thickness,
                              float _fill)
{
  if (_mesh >= m_meshes.size() || m_meshes[_mesh].vertexCount == 0 || _colour.a <= 0.001f)
    return;
  const float colour[4] = {_colour.r, _colour.g, _colour.b, _colour.a};
  const float params[4] = {_thickness, _fill, 0.0f, 0.0f};

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetPipelineState(m_decalPso.get());
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_world, 0);
  cmd->SetGraphicsRoot32BitConstants(1, 4, colour, 0);
  cmd->SetGraphicsRoot32BitConstants(1, 4, params, 4);
  cmd->IASetVertexBuffers(0, 1, &m_meshes[_mesh].vbv);
  cmd->DrawInstanced(m_meshes[_mesh].vertexCount, 1, 0, 0);
}

} // namespace Neuron
