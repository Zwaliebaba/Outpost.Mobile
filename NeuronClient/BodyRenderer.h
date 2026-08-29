#pragma once

#include "RenderTypes.h"

#include "BodyParams.h"
#include "ColourRamp.h"
#include "FxVertex.h"
#include "GpuDevice.h"

#include <d3d12.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Neuron
{
using BodyHandle = std::uint32_t;
inline constexpr BodyHandle INVALID_BODY = 0xFFFFFFFFu;

// The overlay material, which is the source's own: diffuse 1.2, specular 0.5, shininess 40. The
// fade is this tree's addition and has no source counterpart -- see BodyOverlayPS.
struct BodyOverlayParams
{
  float gain = 1.2f;
  float fade = 4.0f; // multiplies length(fwidth(uv)): 4 puts the outline out once a cell is under a quarter of a pixel
  float specular = 0.5f;
  float shininess = 40.0f;
};

// The GPU side of a planet or an asteroid: one outline texture, one root signature, two pipelines
// and the uploaded body meshes. It is the only file in the feature that includes <d3d12.h> --
// everything that decides where a vertex goes or what colour it is lives in BodyField and
// BodyMeshBuilder, which hold no device and are decided by tests.
//
// It has its own root signature rather than widening the scene's, for the reason the effect pass
// has its own: the scene's carries no descriptor table and no sampler, and adding both would touch
// every draw in the game to save one object.
//
// **Init order at boot.** Init records the outline's upload into the device's command list and does
// not submit it. The composition root opens the bracket, initialises this, uploads every starting
// body, submits once, and only then discards the staging buffers:
//
//     gpu.BeginUploads();
//     bodies.Init(gpu, desc);
//     handle = bodies.UploadBody(gpu, verts);   // as many as there are bodies
//     gpu.ExecuteAndWait();
//     bodies.DiscardStaging();
//
// One submission for every copy, which is TextRenderer::Init's shape stretched across two objects.
// Discarding earlier is a use-after-free that the debug layer reports as a removed device, at
// Present, in the following frame -- which is why DiscardStaging is a separate call rather than
// something UploadBody could be trusted to do.
class BodyRenderer
{
public:
  struct Desc
  {
    std::wstring outlineTexture; // TriangleOutline.dds: white rgb, the lines in alpha
  };

  void Init(GpuDevice& _gpu, const Desc& _desc);

  // Records the upload of one body's vertices; the buffer is usable after the next ExecuteAndWait.
  // Returns INVALID_BODY on an empty list. A handle is an index and stays valid for the run.
  [[nodiscard]] BodyHandle UploadBody(GpuDevice& _gpu, std::span<const FxVertex> _verts);

  // Records the bake of one body straight into a new default-heap buffer: three dispatches, two UAV
  // barriers, and a transition to VERTEX_AND_CONSTANT_BUFFER. Usable after the list runs, exactly
  // like UploadBody, and returning the same kind of handle -- the two producers make the same
  // vertices and everything downstream of them is identical (Design/PlanetRenderer.md 17).
  //
  // _gridPower is the body's, and the caller passes the ramp its class uses; the ocean is not baked,
  // because three thousand triangles through the scene pass buy nothing from a kernel.
  [[nodiscard]] BodyHandle BakeBody(GpuDevice& _gpu, const BodyParams& _params, const ColourRamp& _ramp);

  // Debug only, and the whole of slice 6's acceptance: copies a baked body back through a READBACK
  // heap so it can be compared with what BodyMeshBuilder makes of the same description. It opens and
  // closes its own upload bracket and blocks on the GPU, so it belongs at boot and nowhere else.
  void ReadBackBody(GpuDevice& _gpu, BodyHandle _body, std::vector<FxVertex>& _out);

  // Releases the staging buffers of every upload recorded since the last call. Only safe once the
  // command list carrying those copies has run.
  void DiscardStaging() noexcept;

  // Binds the heap, the root signature, the topology and this frame's constants for both passes.
  void Begin(GpuDevice& _gpu, const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _lightDir, float _ambient,
             const DirectX::XMFLOAT3& _cameraPos, const BodyOverlayParams& _overlay);

  // Two entry points rather than one that does both, so a caller draws every body's terrain and then
  // every body's outline: one pipeline switch per pass rather than two per body, and the outline of
  // one body tests against the depth of another (Design/PlanetRenderer.md 7.3).
  void DrawMain(GpuDevice& _gpu, BodyHandle _body, const DirectX::XMFLOAT4X4& _world);
  void DrawOverlay(GpuDevice& _gpu, BodyHandle _body, const DirectX::XMFLOAT4X4& _world);

  // False when the outline texture did not load. DrawOverlay then draws nothing and the terrain is
  // still there: a missing asset is a diagnostic, not a crash.
  [[nodiscard]] bool OutlineReady() const noexcept
  {
    return m_outlineReady;
  }

  [[nodiscard]] std::uint32_t BodyCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_bodies.size());
  }

private:
  void CreatePipelines(GpuDevice& _gpu);
  void CreateBakePipelines(GpuDevice& _gpu);
  void Draw(GpuDevice& _gpu, ID3D12PipelineState* _pso, BodyHandle _body, const DirectX::XMFLOAT4X4& _world);

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_mainPso;
  GpuPtr<ID3D12PipelineState> m_overlayPso;
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap; // one slot: the outline
  GpuPtr<ID3D12Resource> m_outline;
  GpuPtr<ID3D12Resource> m_outlineStaging;
  // The compute side. It has its own root signature for the reason the graphics side has its own: a
  // compute root signature cannot carry ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT and this one carries two
  // UAVs, which the drawing one has no use for.
  GpuPtr<ID3D12RootSignature> m_bakeRootSignature;
  GpuPtr<ID3D12PipelineState> m_bakeMaxPso;
  GpuPtr<ID3D12PipelineState> m_bakePso;
  std::uint32_t m_srvStride = 0;

  std::vector<GpuMesh> m_bodies;
  std::vector<GpuPtr<ID3D12Resource>> m_staging;         // until DiscardStaging
  std::vector<GpuPtr<ID3D12DescriptorHeap>> m_bakeHeaps; // one per bake, alive until its list has run
  bool m_outlineReady = false;
};
} // namespace Neuron
