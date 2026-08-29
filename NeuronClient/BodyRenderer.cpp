#include "pch.h"
#include "BodyRenderer.h"

#include "CubeSphere.h"
#include "GpuHelpers.h"

// Shader bytecode, compiled by FXC at build time (AGENTS.md 3).
#include "CompiledShaders/BodyVS.h"
#include "CompiledShaders/BodyPS.h"
#include "CompiledShaders/BodyOverlayPS.h"
#include "CompiledShaders/BodyBakeMaxCS.h"
#include "CompiledShaders/BodyBakeCS.h"

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

// The bake's own numbers. The thread-group size is the kernels' [numthreads(64,1,1)]; the two have
// to agree and there is nowhere for a shader to tell C++ what it chose.
constexpr UINT BAKE_GROUP_SIZE = 64;
constexpr UINT BAKE_CONTROL_DWORDS = 4;
constexpr UINT BAKE_DESCRIPTORS = 3; // the ramp SRV, the vertex UAV, the maxima UAV
constexpr UINT BAKE_PASS_TILE_MAXIMA = 0;
constexpr UINT BAKE_PASS_HEIGHT_MAXIMUM = 1;

// The cbuffer in BodyBake.hlsli is BodyParams field for field, and nothing but this line notices if
// one of them is edited without the other. The number is written out rather than computed so that a
// change to either side has to be looked at rather than absorbed.
static_assert(sizeof(BodyParams) == 2784, "BodyParams and BodyBake.hlsli's cbuffer have drifted apart");

[[nodiscard]] UINT GroupsFor(UINT _threads) noexcept
{
  return (_threads + BAKE_GROUP_SIZE - 1u) / BAKE_GROUP_SIZE;
}

// The order-preserving uint image of a float, the inverse of the shader's OrderedBits. The maxima
// buffer is seeded with the same starting values BodyField's constructor uses, so the reduction and
// the loop begin from the same place.
[[nodiscard]] std::uint32_t OrderedBits(float _value) noexcept
{
  std::uint32_t bits = 0;
  std::memcpy(&bits, &_value, sizeof(bits));
  const std::uint32_t sign = (bits & 0x80000000u) ? 0xFFFFFFFFu : 0x80000000u;
  return bits ^ sign;
}
} // namespace

void BodyRenderer::Init(GpuDevice& _gpu, const Desc& _desc)
{
  m_srvStride = _gpu.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  CreatePipelines(_gpu);
  CreateBakePipelines(_gpu);

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

void BodyRenderer::CreateBakePipelines(GpuDevice& _gpu)
{
  // One table holding the ramp and the two UAVs, so a bake binds one thing. The ranges are separate
  // because SRV and UAV are separate range types, but they sit in one heap in this order.
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0; // t0, the ramp
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 2;
  ranges[1].BaseShaderRegister = 0; // u0 the vertices, u1 the maxima
  ranges[1].OffsetInDescriptorsFromTableStart = 1;

  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0; // b0, the BodyParams block
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.ShaderRegister = 1; // b1, which pass this dispatch is
  params[1].Constants.Num32BitValues = BAKE_CONTROL_DWORDS;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 2;
  params[2].DescriptorTable.pDescriptorRanges = ranges;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // Bilinear and clamped, which is the filter ColourRamp implements on the CPU. The half-texel
  // correction that makes the two agree is in the shader, not here.
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // No ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT: there is no input assembler in a compute pipeline, and
  // the flag is rejected rather than ignored.
  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.NumParameters = 3;
  rsDesc.pParameters = params;
  rsDesc.NumStaticSamplers = 1;
  rsDesc.pStaticSamplers = &sampler;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  m_bakeRootSignature = CreateRootSignature(_gpu.Device(), rsDesc, "body bake root signature");

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_bakeRootSignature.get();
  pso.CS.pShaderBytecode = g_pBodyBakeMaxCS;
  pso.CS.BytecodeLength = sizeof(g_pBodyBakeMaxCS);
  check_hresult(_gpu.Device()->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_bakeMaxPso.put())));

  pso.CS.pShaderBytecode = g_pBodyBakeCS;
  pso.CS.BytecodeLength = sizeof(g_pBodyBakeCS);
  check_hresult(_gpu.Device()->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_bakePso.put())));
}

BodyHandle BodyRenderer::BakeBody(GpuDevice& _gpu, const BodyParams& _params, const ColourRamp& _ramp)
{
  const std::uint32_t gridPower = static_cast<std::uint32_t>(_params.outsideMaxHeightGrid.z);
  const std::uint32_t samplesPerSide = (1u << gridPower) + 1u;
  const std::uint32_t cells = samplesPerSide - 1u;
  const std::uint32_t vertexCount = CUBE_FACE_COUNT * cells * cells * 6u;
  const std::uint64_t vertexBytes = static_cast<std::uint64_t>(vertexCount) * sizeof(FxVertex);

  ID3D12Device* const device = _gpu.Device();
  ID3D12GraphicsCommandList* const cmd = _gpu.CommandList();

  // The vertex buffer the kernel writes and the input assembler later reads. It carries the UAV flag
  // where UploadStaticBuffer's does not, which is the only difference between what the two producers
  // hand back; a committed buffer is created in COMMON either way and is promoted on first use.
  GpuMesh mesh = {};
  D3D12_RESOURCE_DESC bufferDesc = BufferDesc(vertexBytes);
  bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  check_hresult(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(mesh.vb.put())));

  // The maxima: one slot per tile and one for the field, seeded with the values BodyField's
  // constructor starts its two loops from -- zero for a tile, outsideHeight for the field.
  constexpr std::uint32_t MAXIMA_COUNT = BodyParams::MAX_TILES + 1u;
  GpuPtr<ID3D12Resource> maxima;
  D3D12_RESOURCE_DESC maximaDesc = BufferDesc(MAXIMA_COUNT * sizeof(std::uint32_t));
  maximaDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  check_hresult(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &maximaDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(maxima.put())));

  std::uint32_t seed[MAXIMA_COUNT] = {};
  for (std::uint32_t i = 0; i < BodyParams::MAX_TILES; ++i)
    seed[i] = OrderedBits(0.0f);
  seed[BodyParams::MAX_TILES] = OrderedBits(_params.outsideMaxHeightGrid.x);

  GpuPtr<ID3D12Resource> maximaSeed;
  GpuPtr<ID3D12Resource> unusedStaging;
  UploadStaticBuffer(_gpu, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(seed), sizeof(seed)), maximaSeed,
                     unusedStaging);
  cmd->CopyBufferRegion(maxima.get(), 0, maximaSeed.get(), 0, sizeof(seed));

  // The parameter block, in an upload heap and read as a root CBV. Constant buffers are sized in
  // 256-byte units whatever the struct is.
  constexpr std::uint64_t CONSTANT_ALIGNMENT = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  const std::uint64_t paramsBytes = (sizeof(BodyParams) + CONSTANT_ALIGNMENT - 1u) & ~(CONSTANT_ALIGNMENT - 1u);
  GpuPtr<ID3D12Resource> paramsBuffer;
  const D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC paramsDesc = BufferDesc(paramsBytes);
  check_hresult(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &paramsDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(paramsBuffer.put())));
  std::uint8_t* mapped = nullptr;
  D3D12_RANGE noRead = {0, 0};
  check_hresult(paramsBuffer->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
  std::memcpy(mapped, &_params, sizeof(BodyParams));
  paramsBuffer->Unmap(0, nullptr);

  // A heap of its own per bake, rather than one slot rewritten between them. Every body at boot goes
  // into one command list and one submission, so a rewritten slot would be read by whichever
  // dispatch happened to run after it -- and the fence that would make rewriting safe is the
  // submission this arrangement exists to avoid splitting.
  GpuPtr<ID3D12DescriptorHeap> bakeHeap;
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = BAKE_DESCRIPTORS;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check_hresult(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(bakeHeap.put())));

  D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot = bakeHeap->GetCPUDescriptorHandleForHeapStart();

  ByteBuffer rampPixels;
  _ramp.AsBgra(rampPixels);
  GpuPtr<ID3D12Resource> rampTexture;
  GpuPtr<ID3D12Resource> rampStaging;
  UploadColourTexture(_gpu, ColourRamp::SIDE, ColourRamp::SIDE, rampPixels, cpuSlot, rampTexture, rampStaging);

  cpuSlot.ptr += m_srvStride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC vertexUav = {};
  vertexUav.Format = DXGI_FORMAT_UNKNOWN; // a structured buffer names its stride, not a format
  vertexUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  vertexUav.Buffer.NumElements = vertexCount;
  vertexUav.Buffer.StructureByteStride = sizeof(FxVertex);
  device->CreateUnorderedAccessView(mesh.vb.get(), nullptr, &vertexUav, cpuSlot);

  cpuSlot.ptr += m_srvStride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC maximaUav = {};
  maximaUav.Format = DXGI_FORMAT_R32_UINT; // a typed buffer, which is what InterlockedMax needs
  maximaUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  maximaUav.Buffer.NumElements = MAXIMA_COUNT;
  device->CreateUnorderedAccessView(maxima.get(), nullptr, &maximaUav, cpuSlot);

  ID3D12DescriptorHeap* heaps[] = {bakeHeap.get()};
  cmd->SetDescriptorHeaps(1, heaps);
  cmd->SetComputeRootSignature(m_bakeRootSignature.get());
  cmd->SetComputeRootConstantBufferView(0, paramsBuffer->GetGPUVirtualAddress());
  cmd->SetComputeRootDescriptorTable(2, bakeHeap->GetGPUDescriptorHandleForHeapStart());

  const UINT sampleGroups = GroupsFor(CUBE_FACE_COUNT * samplesPerSide * samplesPerSide);
  const UINT cellGroups = GroupsFor(CUBE_FACE_COUNT * cells * cells);

  // The barrier between the two reduction passes is not optional. The second reads what the first
  // wrote, and without it some hardware reads zeros and the implementer's reads the right answer.
  D3D12_RESOURCE_BARRIER maximaBarrier = {};
  maximaBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  maximaBarrier.UAV.pResource = maxima.get();

  cmd->SetPipelineState(m_bakeMaxPso.get());
  UINT control[BAKE_CONTROL_DWORDS] = {BAKE_PASS_TILE_MAXIMA, 0, 0, 0};
  cmd->SetComputeRoot32BitConstants(1, BAKE_CONTROL_DWORDS, control, 0);
  cmd->Dispatch(sampleGroups, 1, 1);
  cmd->ResourceBarrier(1, &maximaBarrier);

  control[0] = BAKE_PASS_HEIGHT_MAXIMUM;
  cmd->SetComputeRoot32BitConstants(1, BAKE_CONTROL_DWORDS, control, 0);
  cmd->Dispatch(sampleGroups, 1, 1);
  cmd->ResourceBarrier(1, &maximaBarrier);

  cmd->SetPipelineState(m_bakePso.get());
  cmd->Dispatch(cellGroups, 1, 1);

  const D3D12_RESOURCE_BARRIER toVertices =
    Transition(mesh.vb.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  cmd->ResourceBarrier(1, &toVertices);

  mesh.vbv.BufferLocation = mesh.vb->GetGPUVirtualAddress();
  mesh.vbv.SizeInBytes = static_cast<UINT>(vertexBytes);
  mesh.vbv.StrideInBytes = sizeof(FxVertex);
  mesh.vertexCount = vertexCount;

  // Everything the dispatches read has to outlive the submission, which is what DiscardStaging is
  // called after and for.
  m_staging.push_back(std::move(maximaSeed));
  m_staging.push_back(std::move(unusedStaging));
  m_staging.push_back(std::move(maxima));
  m_staging.push_back(std::move(paramsBuffer));
  m_staging.push_back(std::move(rampTexture));
  m_staging.push_back(std::move(rampStaging));
  m_bakeHeaps.push_back(std::move(bakeHeap));

  m_bodies.push_back(std::move(mesh));
  return static_cast<BodyHandle>(m_bodies.size() - 1);
}

void BodyRenderer::ReadBackBody(GpuDevice& _gpu, BodyHandle _body, std::vector<FxVertex>& _out)
{
  _out.clear();
  if (_body >= m_bodies.size() || m_bodies[_body].vertexCount == 0)
    return;

  const GpuMesh& mesh = m_bodies[_body];
  const std::uint64_t bytes = static_cast<std::uint64_t>(mesh.vertexCount) * sizeof(FxVertex);

  GpuPtr<ID3D12Resource> readback;
  const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_READBACK);
  const D3D12_RESOURCE_DESC desc = BufferDesc(bytes);
  check_hresult(_gpu.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(readback.put())));

  _gpu.BeginUploads();
  ID3D12GraphicsCommandList* const cmd = _gpu.CommandList();
  const D3D12_RESOURCE_BARRIER toSource =
    Transition(mesh.vb.get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_SOURCE);
  cmd->ResourceBarrier(1, &toSource);
  cmd->CopyBufferRegion(readback.get(), 0, mesh.vb.get(), 0, bytes);
  const D3D12_RESOURCE_BARRIER toVertices =
    Transition(mesh.vb.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  cmd->ResourceBarrier(1, &toVertices);
  _gpu.ExecuteAndWait();

  _out.resize(mesh.vertexCount);
  std::uint8_t* mapped = nullptr;
  const D3D12_RANGE readEverything = {0, static_cast<SIZE_T>(bytes)};
  check_hresult(readback->Map(0, &readEverything, reinterpret_cast<void**>(&mapped)));
  std::memcpy(_out.data(), mapped, static_cast<std::size_t>(bytes));
  const D3D12_RANGE wroteNothing = {0, 0};
  readback->Unmap(0, &wroteNothing);
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
  m_bakeHeaps.clear();
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
