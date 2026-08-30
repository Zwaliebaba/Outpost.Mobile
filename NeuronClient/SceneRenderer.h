#pragma once

#include "GpuDevice.h"
#include "HandleStore.h"
#include "RenderTypes.h"

#include <span>

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

  // Static geometry, uploaded once at startup into a DEFAULT heap, through a staging copy the
  // caller's GpuDevice upload bracket runs -- so this must be called inside one, and DiscardStaging
  // after it.
  //
  // It was an upload heap and a straight memcpy, on the argument that a few thousand triangles do
  // not justify a staging copy. That was about the upload; the cost is in the reading. An upload
  // heap is system memory, so the input assembler crosses PCIe for these vertices on every draw and,
  // once instancing lands, on every instance.
  [[nodiscard]] MeshHandle UploadMesh(GpuDevice& _gpu, const std::vector<MeshVertex>& _verts);

  // A unit quad in the XZ plane, built at Init. Every ring and marker is this one mesh with a
  // different matrix and a different shader parameter, so none of them needs geometry rebuilt when
  // its size or thickness changes.
  [[nodiscard]] MeshHandle UnitQuad() const noexcept
  {
    return m_unitQuad;
  }

  // Opaque pass. Set once per frame, before any DrawMesh or DrawMeshInstanced.
  void BeginScene(GpuDevice& _gpu, const SceneFrame& _frame);
  // _livery multiplies the surfaces the model declared RaceTinted and reaches nothing else, so a
  // mesh whose vertices all carry race == 0 -- the ground quad, a decal -- draws as authored
  // whatever is passed. _highlight is a lift towards white in 0..1, applied after lighting.
  void DrawMesh(GpuDevice& _gpu, MeshHandle _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _livery, float _highlight);

  // One draw for every ship sharing a mesh. Five hundred hulls over five meshes were five hundred
  // draws, because a per-object matrix could only live in a root constant and a root constant is set
  // per draw (Design/MmoScalabilityReview.md G2). The instances go into a per-frame ring and are read
  // from input slot 1.
  //
  // Switches the pipeline, so a caller mixing this with DrawMesh pays a state change each way. Do the
  // instanced draws together.
  void DrawMeshInstanced(GpuDevice& _gpu, MeshHandle _mesh, std::span<const MeshInstance> _instances);

  // Retires a mesh: the handle stops resolving at once and the buffer is released at the next
  // DiscardStaging, which is the first point the GPU is known to be done with it. False if the
  // handle was already stale, so a double free is a no-op that says so rather than a second release
  // of a buffer the slot's new occupant now owns (ADR 0044).
  bool FreeMesh(MeshHandle _mesh) noexcept;

  // How many meshes are live. Not the slot count: a store that has freed anything has more slots
  // than meshes, and the slots are what make a handle survive the resource.
  [[nodiscard]] std::uint32_t MeshCount() const noexcept
  {
    return m_meshSlots.LiveCount();
  }

  // Releases the staging buffers UploadMesh recorded copies from. Call after the GpuDevice bracket
  // those uploads were recorded into has run, never before -- the copies read them.
  void DiscardStaging() noexcept;

  // Instances the ring had no room for, this frame. Reset by BeginScene, so it reports the frame in
  // front of the reader rather than the run.
  [[nodiscard]] std::uint32_t DroppedInstances() const noexcept
  {
    return m_droppedInstances;
  }

  // Overlay pass. The decal takes the unit quad and is shaped in the pixel shader, so ring thickness
  // and fill stay plain parameters with no geometry to rebuild.
  void BeginDecals(GpuDevice& _gpu, const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _cameraPos);
  void DrawDecal(GpuDevice& _gpu, MeshHandle _mesh, const DirectX::XMFLOAT4X4& _world, Rgba _colour, float _thickness, float _fill);

private:
  void CreateScenePipeline(GpuDevice& _gpu);
  void CreateDecalPipelines(GpuDevice& _gpu);
  void CreateInstanceRing(GpuDevice& _gpu);

  GpuPtr<ID3D12RootSignature> m_sceneRs;
  GpuPtr<ID3D12PipelineState> m_scenePso;
  GpuPtr<ID3D12PipelineState> m_instancedPso;
  GpuPtr<ID3D12PipelineState> m_decalPso; // alpha blended
  // Indexed by slot, never by handle: HandleStore is what turns one into the other, and what stops a
  // handle to a freed mesh reaching the mesh that took its place (ADR 0044).
  std::vector<GpuMesh> m_meshes;
  HandleStore m_meshSlots;

  // Buffers whose handles have been retired, held until DiscardStaging. A mesh freed while the GPU
  // is still drawing with it must not be released on the spot, and DiscardStaging is already the
  // call that means "the bracket has run".
  std::vector<GpuPtr<ID3D12Resource>> m_retired;
  MeshHandle m_unitQuad = INVALID_MESH;

  // Held only until the bracket the copies were recorded into has run; DiscardStaging drops them.
  std::vector<GpuPtr<ID3D12Resource>> m_staging;

  // The instance ring, one buffer per frame in flight -- FxRenderer's pattern, for the same reason:
  // a buffer the CPU writes and the GPU reads in the same frame cannot be one buffer.
  GpuPtr<ID3D12Resource> m_instanceBuffers[GpuDevice::FRAME_COUNT];
  std::uint8_t* m_instanceCpu[GpuDevice::FRAME_COUNT] = {};
  std::uint32_t m_instanceOffset = 0;
  std::uint32_t m_instanceFrameIndex = 0xFFFFFFFFu; // no frame yet, so the first BeginScene resets it
  std::uint32_t m_droppedInstances = 0;
};
} // namespace Neuron
