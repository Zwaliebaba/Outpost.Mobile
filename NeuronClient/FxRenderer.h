#pragma once

#include "RenderTypes.h"

#include "FxVertex.h"
#include "GpuDevice.h"

#include <d3d12.h>

#include <cstdint>
#include <span>
#include <string>

namespace Neuron
{
// The GPU side of the explosion effect: three textures, two static samplers, three pipelines and
// one per-frame vertex ring. It is the only file in the effect that includes <d3d12.h> -- everything
// that decides where a fragment or a sprite goes is in MeshShatter and SpriteParticles, which hold
// no device and are decided by tests.
//
// It has its own root signature rather than widening the scene's. The scene's carries no descriptor
// table and no sampler, and adding both would touch every draw in the game to save one object.
class FxRenderer
{
public:
  // Three copies of a 500-fragment shatter is 4500 vertices per dead ship and a full 4096-particle
  // pool is 24576, so the ring holds five simultaneous deaths with the pool full. 1.3 MB a frame.
  static constexpr std::uint32_t MAX_FX_VERTS = 49152;

  struct Desc
  {
    std::wstring fragmentTexture; // the wireframe decal over a hull shard
    std::wstring spriteTexture;   // every billboard
    std::wstring flashTexture;    // loaded and slotted; nothing draws it yet
  };

  // Records the three texture uploads into the device's command list and submits them, so this runs
  // at boot, beside TextRenderer::Init, and before the first frame. A texture that cannot be read
  // traces and leaves Ready() false; the game boots without the effect rather than not at all.
  void Init(GpuDevice& _gpu, const Desc& _desc);

  // Binds the heap, the root signature and this frame's constants. Called once per pass the caller
  // wants -- the scene and decal passes rebind their own state in between -- and the vertex ring is
  // reset on the first Begin of a frame, not on every Begin.
  void Begin(GpuDevice& _gpu, const DirectX::XMFLOAT4X4& _viewProj, const DirectX::XMFLOAT3& _lightDir, float _ambient,
             const DirectX::XMFLOAT3& _cameraPos);

  void DrawFragments(GpuDevice& _gpu, std::span<const FxVertex> _verts);
  void DrawSpritesDark(GpuDevice& _gpu, std::span<const FxVertex> _verts);
  void DrawSpritesAdd(GpuDevice& _gpu, std::span<const FxVertex> _verts);

  // False until every texture has loaded. While it is false no Draw draws anything.
  [[nodiscard]] bool Ready() const noexcept
  {
    return m_ready;
  }
  // Vertices the ring had no room for, this frame. Reset by the first Begin of each frame, because
  // what a reader wants to know is whether the frame in front of them overflowed.
  [[nodiscard]] std::uint32_t DroppedVerts() const noexcept
  {
    return m_droppedVerts;
  }

private:
  static constexpr std::uint32_t FRAGMENT_SLOT = 0;
  static constexpr std::uint32_t SPRITE_SLOT = 1;
  static constexpr std::uint32_t FLASH_SLOT = 2;
  static constexpr std::uint32_t TEXTURE_COUNT = 3;

  void CreatePipelines(GpuDevice& _gpu);
  void LoadTexture(GpuDevice& _gpu, std::uint32_t _slot, const std::wstring& _fileName);
  void Draw(GpuDevice& _gpu, ID3D12PipelineState* _pso, std::uint32_t _srvSlot, std::span<const FxVertex> _verts);

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_fragmentPso;
  GpuPtr<ID3D12PipelineState> m_spriteDarkPso;
  GpuPtr<ID3D12PipelineState> m_spriteAddPso;
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap; // slot 0 fragment, 1 sprite, 2 flash
  GpuPtr<ID3D12Resource> m_textures[TEXTURE_COUNT];
  // Held only until Init's ExecuteAndWait has run the copies that read them; released there.
  GpuPtr<ID3D12Resource> m_staging[TEXTURE_COUNT];
  GpuPtr<ID3D12Resource> m_vb[GpuDevice::FRAME_COUNT];
  std::uint8_t* m_vbCpu[GpuDevice::FRAME_COUNT] = {};
  std::uint32_t m_ringOffsetVerts = 0;
  std::uint32_t m_ringFrameIndex = 0xFFFFFFFFu; // no frame yet, so the first Begin resets the ring
  std::uint32_t m_droppedVerts = 0;
  std::uint32_t m_srvStride = 0;
  bool m_ready = false;
};
} // namespace Neuron
