#include "pch.h"
#include "SceneRenderer.h"

#include "GpuHelpers.h"

// Shader bytecode, compiled by FXC at build time (AGENTS.md 3). Nothing here compiles HLSL at
// runtime: a shader mistake is a build error rather than a message box on somebody's machine, and
// the binary carries no dependency on d3dcompiler_47.dll.
#include "CompiledShaders/SceneVS.h"
#include "CompiledShaders/ScenePS.h"
#include "CompiledShaders/DecalVS.h"
#include "CompiledShaders/DecalPS.h"

namespace Neuron
{
namespace
{
constexpr D3D12_INPUT_ELEMENT_DESC SCENE_ELEMENTS[] = {
  {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},};

// A unit quad in the XZ plane, centred on the origin.
std::vector<MeshVertex> BuildUnitQuad()
{
  constexpr float h = 0.5f;
  return {MeshVertex{-h, 0.0f, -h, 1.0f, 1.0f, 1.0f}, MeshVertex{-h, 0.0f, h, 1.0f, 1.0f, 1.0f},
          MeshVertex{h, 0.0f, h, 1.0f, 1.0f, 1.0f},   MeshVertex{-h, 0.0f, -h, 1.0f, 1.0f, 1.0f},
          MeshVertex{h, 0.0f, h, 1.0f, 1.0f, 1.0f},   MeshVertex{h, 0.0f, -h, 1.0f, 1.0f, 1.0f},};
}
} // namespace

void SceneRenderer::Init(GpuDevice& _gpu)
{
  CreateScenePipeline(_gpu);
  CreateDecalPipelines(_gpu);
  m_unitQuad = UploadMesh(_gpu, BuildUnitQuad());
}

void SceneRenderer::CreateScenePipeline(GpuDevice& _gpu)
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

  // Same shader, added rather than blended, for thruster glow and trail.
  pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
  check_hresult(_gpu.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_glowPso.put())));
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

  const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC rd = BufferDesc(bytes);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(mesh.vb.put())));

  std::uint8_t* dst = nullptr;
  D3D12_RANGE noRead = {0, 0};
  check_hresult(mesh.vb->Map(0, &noRead, reinterpret_cast<void**>(&dst)));
  std::memcpy(dst, _verts.data(), static_cast<size_t>(bytes));
  mesh.vb->Unmap(0, nullptr);

  mesh.vbv.BufferLocation = mesh.vb->GetGPUVirtualAddress();
  mesh.vbv.SizeInBytes = static_cast<UINT>(bytes);
  mesh.vbv.StrideInBytes = sizeof(MeshVertex);
  mesh.vertexCount = static_cast<std::uint32_t>(_verts.size());
  m_meshes.push_back(std::move(mesh));
  return static_cast<MeshHandle>(m_meshes.size() - 1);
}

void SceneRenderer::BeginScene(GpuDevice& _gpu, const SceneFrame& _frame)
{
  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetPipelineState(m_scenePso.get());
  cmd->SetGraphicsRootSignature(m_sceneRs.get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  // The world matrix occupies DWORDs 0..15 and changes per draw; viewProj is set once here.
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_frame.viewProj, 16);

  // Everything after baseColour, which is also per draw.
  const float shading[16] = {_frame.lightDir.x,      _frame.lightDir.y,   _frame.lightDir.z,        _frame.ambient,
                             _frame.gridColour.r,    _frame.gridColour.g, _frame.gridColour.b,      _frame.gridColour.a,
                             _frame.gridSpacing,     _frame.gridLineWidthPx, _frame.gridFadeDistance, 0.0f,
                             _frame.cameraPos.x,     _frame.cameraPos.y,  _frame.cameraPos.z,       0.0f,};
  cmd->SetGraphicsRoot32BitConstants(1, 16, shading, 4);
}

void SceneRenderer::DrawMesh(GpuDevice& _gpu, MeshHandle _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _baseColour, float _materialMix,
                             bool _isGround)
{
  if (_mesh >= m_meshes.size() || m_meshes[_mesh].vertexCount == 0)
    return;
  const GpuMesh& mesh = m_meshes[_mesh];
  const float base[4] = {_baseColour.r, _baseColour.g, _baseColour.b, _materialMix};
  const float mode = _isGround ? 1.0f : 0.0f;

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_world, 0);
  cmd->SetGraphicsRoot32BitConstants(1, 4, base, 0);
  cmd->SetGraphicsRoot32BitConstants(1, 1, &mode, 15); // gridParams.w
  cmd->IASetVertexBuffers(0, 1, &mesh.vbv);
  cmd->DrawInstanced(mesh.vertexCount, 1, 0, 0);
}

void SceneRenderer::BeginDecals(GpuDevice& _gpu, const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _cameraPos)
{
  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetGraphicsRootSignature(m_sceneRs.get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_viewProj, 16);
  const float eye[4] = {_cameraPos.x, _cameraPos.y, _cameraPos.z, 0.0f};
  cmd->SetGraphicsRoot32BitConstants(1, 4, eye, 16);
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

void SceneRenderer::DrawGlow(GpuDevice& _gpu, MeshHandle _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _colour, float _falloff)
{
  if (_mesh >= m_meshes.size() || m_meshes[_mesh].vertexCount == 0 || _colour.a <= 0.001f)
    return;
  const float colour[4] = {_colour.r, _colour.g, _colour.b, _colour.a};
  const float params[4] = {0.0f, _falloff, 1.0f, 0.0f};

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();
  cmd->SetPipelineState(m_glowPso.get());
  cmd->SetGraphicsRoot32BitConstants(0, 16, &_world, 0);
  cmd->SetGraphicsRoot32BitConstants(1, 4, colour, 0);
  cmd->SetGraphicsRoot32BitConstants(1, 4, params, 4);
  cmd->IASetVertexBuffers(0, 1, &m_meshes[_mesh].vbv);
  cmd->DrawInstanced(m_meshes[_mesh].vertexCount, 1, 0, 0);
}
} // namespace Neuron
