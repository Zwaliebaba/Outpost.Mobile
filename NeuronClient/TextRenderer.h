#pragma once

#include "BitmapFont.h"
#include "GpuDevice.h"
#include "RenderTypes.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Neuron
{
// Which atlas a queued draw takes its glyphs from. The engine names the two roles and the
// composition root binds each to a file through TextRenderer::Desc -- a library that spelled the
// name of a font would be a library with content in it.
enum class FontId : std::uint32_t
{
  Ui,   // the HUD and editor text, and the atlas the untextured quads take their solid texel from
  Scene // text drawn over the world
};

// The overlay pass: bitmap font atlases loaded at startup, and screen-space quads that share them.
// Everything queued during a frame is drawn on top of the scene with no depth, one draw call per
// run of quads on the same font -- so the HUD costs one pipeline change however much of it there is.
//
// Untextured shapes go through the same pipeline: every atlas carries one cell filled solid, so a
// selection box or an order line is a quad with the same shader rather than a second pipeline that
// would have to be kept in step with this one.
class TextRenderer
{
public:
  static constexpr std::uint32_t MAX_VERTS = 24000; // 4000 glyphs; a multiple of 6
  static constexpr std::uint32_t FONT_COUNT = 2;

  struct Desc
  {
    std::wstring uiFont;    // the atlas behind FontId::Ui
    std::wstring sceneFont; // the atlas behind FontId::Scene
  };

  // Records the atlas uploads into the device's command list and submits them, so call this after
  // the device is up and before the first frame. A font that cannot be read traces and draws
  // nothing; a missing atlas does not fail boot.
  void Init(GpuDevice& _gpu, const Desc& _desc);

  // Drops last frame's queue. Called once per frame, after GpuDevice::BeginFrame.
  void BeginFrame() noexcept
  {
    m_verts.clear();
    m_batches.clear();
  }

  // Queued during the frame, drawn in Flush. '\n' starts a new line. Bytes outside the atlas's
  // range advance the pen and draw nothing, so a stray one costs a space rather than a wrong glyph.
  void DrawTextLine(FontId _font, float _xPx, float _yPx, float _scale, Rgba _colour, std::string_view _text);
  void DrawScreenRect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, Rgba _colour);
  void DrawScreenLine(float _x0Px, float _y0Px, float _x1Px, float _y1Px, float _thicknessPx, Rgba _colour);

  // Draws everything queued this frame. Must run before GpuDevice::EndFrame.
  void Flush(GpuDevice& _gpu);

  // Square cells and fixed pitch, so these are the same number. Both are here because a caller
  // measuring a string and a caller stacking lines are asking different questions.
  [[nodiscard]] float AdvancePx(FontId _font, float _scale) const noexcept
  {
    return m_fonts[static_cast<std::uint32_t>(_font)].CellPx() * _scale;
  }
  [[nodiscard]] float LineHeightPx(FontId _font, float _scale) const noexcept
  {
    return m_fonts[static_cast<std::uint32_t>(_font)].CellPx() * _scale;
  }

private:
  // One run of vertices on one font. Runs are cut where the font changes rather than gathered per
  // font, so Flush replays the frame in the order it was queued: a HUD drawn after a selection box
  // has to land on top of it.
  struct Batch
  {
    FontId font = FontId::Ui;
    std::uint32_t firstVert = 0;
    std::uint32_t vertCount = 0;
  };

  void CreatePipeline(GpuDevice& _gpu);
  void LoadFont(GpuDevice& _gpu, FontId _font, const std::wstring& _fileName);

  // Every shape this pass draws is one quad, so they share the split into two triangles and the run
  // bookkeeping. Corners are top-left, top-right, bottom-left, bottom-right of the quad's own axes,
  // which for a line means either side of each end.
  void PushQuad(FontId _font, const TextVertex& _a, const TextVertex& _b, const TextVertex& _c, const TextVertex& _d);

  GpuPtr<ID3D12RootSignature> m_rootSignature;
  GpuPtr<ID3D12PipelineState> m_pso;
  GpuPtr<ID3D12DescriptorHeap> m_srvHeap; // one slot per font, in FontId order
  GpuPtr<ID3D12Resource> m_vb[GpuDevice::FRAME_COUNT];
  std::uint8_t* m_vbCpu[GpuDevice::FRAME_COUNT] = {};
  BitmapFont m_fonts[FONT_COUNT];
  std::vector<TextVertex> m_verts;
  std::vector<Batch> m_batches;
  std::uint32_t m_srvStride = 0;
};
} // namespace Neuron
