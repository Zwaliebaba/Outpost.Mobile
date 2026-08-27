#pragma once

#include "GpuDevice.h"
#include "RenderTypes.h"

#include <string_view>
#include <vector>

namespace Neuron
{
// The overlay pass: a fixed-pitch font baked to one atlas at startup, and screen-space quads that
// share it. Everything queued during a frame is drawn once, on top of the scene, in one draw call
// with no depth -- so the HUD costs one pipeline change however much of it there is.
//
// Untextured shapes go through the same pipeline: the atlas has 96 cells for 95 printable glyphs,
// and the spare one is filled solid so a selection box or an order line is just a quad with the
// same shader, rather than a second pipeline that would have to be kept in step with this one.
class TextRenderer
{
public:
  static constexpr std::uint32_t MAX_VERTS = 24000; // 4000 glyphs; a multiple of 6

  // Records the atlas upload into the device's command list and submits it, so call this after the
  // device is up and before the first frame.
  void Init(GpuDevice& _gpu);

  // Drops last frame's queue. Called once per frame, after GpuDevice::BeginFrame.
  void BeginFrame() noexcept { m_verts.clear(); }

  // Queued during the frame, drawn in Flush. '\n' starts a new line.
  void DrawTextLine(float _xPx, float _yPx, float _scale, Rgba _colour, std::string_view _text);
  void DrawScreenRect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, Rgba _colour);
  void DrawScreenLine(float _x0Px, float _y0Px, float _x1Px, float _y1Px, float _thicknessPx, Rgba _colour);

  // Draws everything queued this frame. Must run before GpuDevice::EndFrame.
  void Flush(GpuDevice& _gpu);

  [[nodiscard]] float AdvancePx(float _scale) const noexcept { return m_advancePx * _scale; }
  [[nodiscard]] float LineHeightPx(float _scale) const noexcept { return m_cellHPx * _scale; }

private:
  void CreatePipeline(GpuDevice& _gpu);
  void BakeFontAtlas(GpuDevice& _gpu);

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_pso;
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap; // slot 0: the font atlas
  GpuPtr<ID3D12Resource> m_fontTex;
  GpuPtr<ID3D12Resource> m_vb[GpuDevice::FRAME_COUNT];
  std::uint8_t* m_vbCpu[GpuDevice::FRAME_COUNT] = {};
  std::vector<TextVertex> m_verts;

  float m_cellWPx = 0.0f;   // atlas cell, i.e. the quad width
  float m_cellHPx = 0.0f;   // atlas cell, i.e. the line height
  float m_advancePx = 0.0f; // fixed pitch, i.e. the pen step
  float m_atlasWPx = 0.0f;
  float m_atlasHPx = 0.0f;
  float m_solidU = 0.0f; // centre of the one atlas cell with no glyph in it
  float m_solidV = 0.0f;
};
} // namespace Neuron
