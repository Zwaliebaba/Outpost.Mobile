#pragma once

#include "RenderTypes.h"

#include "GpuDevice.h"
#include "SkyField.h"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace Neuron
{
// The GPU side of the sky: three textures, one static sampler, one pipeline, and one static vertex
// buffer holding the whole celestial sphere. It is the only file in the feature that includes
// <d3d12.h> -- what a star looks like is decided in SkyField, which holds no device and is decided by
// tests.
//
// **One buffer, three draws, no per-frame CPU work at all.** The vertices carry directions rather
// than positions and the billboards are expanded in the vertex shader, so nothing about the sky
// changes when the camera moves and there is nothing to rebuild. The three draws are the three
// textures -- nebulosity, stars, flares -- and between them the pass changes one descriptor
// (Design/Archive/Skybox.md 6).
//
// It has its own root signature for the reason the effect and body passes have their own: the
// scene's carries no descriptor table and no sampler, and adding both would touch every draw in the
// game to save one object.
//
// **Init order at boot.** Init records the three texture uploads into the device's command list and
// does not submit it; UploadField records the vertex buffer's copy the same way. The composition
// root opens the bracket, does both, submits once, and only then discards the staging buffers:
//
//     gpu.BeginUploads();
//     sky.Init(gpu, desc);
//     sky.UploadField(gpu, mesh);
//     gpu.ExecuteAndWait();
//     sky.DiscardStaging();
//
// which is BodyRenderer's shape, and for the same reason. Discarding earlier is a use-after-free the
// debug layer reports as a removed device, at Present, in the following frame.
class SkyRenderer
{
public:
  struct Desc
  {
    std::wstring nebulaTexture; // CloudyGlow.dds
    std::wstring starTexture;   // Glow.dds
    std::wstring burstTexture;  // Starburst.dds: the flare over the brightest stars
  };

  // Everything the pass needs for one frame. The camera basis is here rather than derived from the
  // matrix because the camera already has it and inverting a view matrix to get back what the caller
  // is holding is work done to lose precision.
  struct Frame
  {
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT3 cameraRight;
    DirectX::XMFLOAT3 cameraUp;
    DirectX::XMFLOAT3 cameraPos;
    // Where the sphere is put. It only has to sit between the near and far planes -- the pass draws
    // first with the depth test off, so nothing in the scene is ever tested against it.
    float radiusMetres = 5000.0f;
    float timeSec = 0.0f; // real time, not sim time: pausing the game does not stop the sky
    float intensity = 1.0f;
    float twinkleMaxRateRadPerSec = 4.5f; // a vertex carries its own rate as a fraction of this
  };

  // Records the three texture uploads into the device's command list and does not submit them; the
  // caller brackets. A texture that cannot be read traces and leaves Ready() false; the game boots
  // with a plain background rather than not at all.
  void Init(GpuDevice& _gpu, const Desc& _desc);

  // Records the copy of a whole generated sky into a new default-heap buffer, usable after the list
  // has run. Called again for a different sky, which releases the buffer the last one used -- safe
  // only inside an upload bracket, because BeginUploads drains the GPU before it resets the list.
  void UploadField(GpuDevice& _gpu, const SkyMesh& _mesh);

  // Releases the staging buffers of every upload recorded since the last call. Only safe once the
  // command list carrying those copies has run.
  void DiscardStaging() noexcept;

  // The whole pass: state, constants, and three draws. Called once per frame, before anything else
  // in the scene, and it neither tests nor writes depth -- so whatever draws next covers it, however
  // far away it nominally is.
  void Draw(GpuDevice& _gpu, const Frame& _frame);

  // False until all three textures and a sky have loaded. While it is false Draw draws nothing.
  [[nodiscard]] bool Ready() const noexcept
  {
    return m_texturesReady && m_vertexCount > 0;
  }

  [[nodiscard]] std::uint32_t VertexCount() const noexcept
  {
    return m_vertexCount;
  }

private:
  static constexpr std::uint32_t TEXTURE_COUNT = SKY_LAYER_COUNT;

  void CreatePipeline(GpuDevice& _gpu);
  void LoadTexture(GpuDevice& _gpu, SkyLayer _layer, const std::wstring& _fileName);

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_pso;
  std::uint32_t m_slots[TEXTURE_COUNT] = {}; // shared-heap slots, one per SkyLayer in that order
  GpuPtr<ID3D12Resource> m_textures[TEXTURE_COUNT];
  GpuPtr<ID3D12Resource> m_textureStaging[TEXTURE_COUNT];

  GpuPtr<ID3D12Resource> m_vb;
  GpuPtr<ID3D12Resource> m_vbStaging;
  D3D12_VERTEX_BUFFER_VIEW m_vbv = {};
  std::uint32_t m_layerFirstVertex[SKY_LAYER_COUNT] = {};
  std::uint32_t m_layerVertexCount[SKY_LAYER_COUNT] = {};
  std::uint32_t m_vertexCount = 0;
  bool m_texturesReady = false;
};
} // namespace Neuron
