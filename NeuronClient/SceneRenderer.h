#pragma once

#include "GpuDevice.h"
#include "RenderTypes.h"

#include "MeshData.h"

#include <vector>

namespace Neuron
{
// The world pass: opaque geometry, then the alpha-blended ground decals that sit on top of it. Two
// pipelines, one root signature, one vertex format -- the cost of adding a new kind of ground decal
// is a constant, not a pipeline.
//
// Thruster glows were a third pipeline here and are not any more. A decal is drawn a handful of
// times a frame and a glow was drawn thousands, which is a different problem: it moved to
// FxRenderer's vertex ring, where the whole frame's worth is one draw.
//
// It owns the uploaded meshes because they are only meaningful to it. Callers hold a MeshHandle,
// which stays valid for the run and survives the renderer rehousing its buffers.
class SceneRenderer
{
public:
  void Init(GpuDevice& _gpu);

  // Static geometry, uploaded once at startup. The vertex buffer stays in an upload heap: a few
  // thousand triangles do not justify a staging copy and its barrier.
  [[nodiscard]] MeshHandle UploadMesh(GpuDevice& _gpu, const std::vector<MeshVertex>& _verts);

  // A unit quad in the XZ plane, built at Init. Every ring and marker is this one mesh with a
  // different matrix and a different shader parameter, so none of them needs geometry rebuilt when
  // its size or thickness changes.
  [[nodiscard]] MeshHandle UnitQuad() const noexcept
  {
    return m_unitQuad;
  }

  // Opaque pass. Set once per frame, before any DrawMesh.
  void BeginScene(GpuDevice& _gpu, const SceneFrame& _frame);
  void DrawMesh(GpuDevice& _gpu, MeshHandle _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _baseColour, float _materialMix);

  // Overlay pass. The decal takes the unit quad and is shaped in the pixel shader, so ring thickness
  // and fill stay plain parameters with no geometry to rebuild.
  void BeginDecals(GpuDevice& _gpu, const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _cameraPos);
  void DrawDecal(GpuDevice& _gpu, MeshHandle _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _colour, float _thickness, float _fill);

private:
  void CreateScenePipeline(GpuDevice& _gpu);
  void CreateDecalPipelines(GpuDevice& _gpu);

  GpuPtr<ID3D12RootSignature> m_sceneRs;
  GpuPtr<ID3D12PipelineState> m_scenePso;
  GpuPtr<ID3D12PipelineState> m_decalPso; // alpha blended
  std::vector<GpuMesh> m_meshes;
  MeshHandle m_unitQuad = INVALID_MESH;
};
} // namespace Neuron
