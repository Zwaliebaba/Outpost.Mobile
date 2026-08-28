#pragma once

#include "GpuDevice.h"
#include "RenderTypes.h"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace Neuron
{
// One font atlas on the GPU, and the arithmetic that turns a character into the cell it lives in.
//
// The layout is the whole contract, and it is rigid on purpose: sixteen columns of square cells in
// reading order from the space character, so the cell size is the image width over sixteen and the
// row count follows from the height. Nothing is stored beside the image. A proportional font would
// need a metrics file, and two files that have to agree are a content defect waiting to happen --
// this game's text is a HUD readout and in-world labels, and neither wants kerning that badly.
//
// The atlas is a coverage mask rather than colour. The pixel shader multiplies it by the vertex
// colour, which is what lets one atlas serve every tint the HUD draws in.
class BitmapFont
{
public:
  // The rectangle one character occupies, in texture coordinates.
  struct Cell
  {
    float u0, v0, u1, v1;
  };

  static constexpr std::uint32_t GLYPH_COLS = 16;
  static constexpr std::uint8_t FIRST_GLYPH = 32; // space, in the cell at index 0

  // 127 is the one code point inside the atlas's range that is defined not to print, so its cell is
  // overwritten solid at load and the untextured screen quads sample the middle of it. That is what
  // lets a selection box share the text pipeline rather than needing a second one kept in step with
  // it -- and it is why a literal DEL byte in a string draws a block.
  static constexpr std::uint8_t SOLID_GLYPH = 127;

  // Reads _fileName through FileSys, records the upload into the device's command list, and writes
  // the shader resource view to _srv. Reports false and traces on a font that cannot be read: a
  // missing atlas is a diagnostic, and the text queued on it simply does not draw.
  [[nodiscard]] bool Load(GpuDevice& _gpu, const std::wstring& _fileName, D3D12_CPU_DESCRIPTOR_HANDLE _srv);

  // Releases the staging buffer, which has to outlive Load because the copy has only been recorded.
  // Separate from Load rather than folded into it because GpuDevice::ExecuteAndWait closes the
  // command list: it can be called once for all the fonts, and never once per font.
  void DiscardStaging() noexcept
  {
    m_staging = nullptr;
  }

  [[nodiscard]] bool Ready() const noexcept
  {
    return m_atlas.get() != nullptr;
  }

  // Square cells and fixed pitch, so this one number is the quad, the pen step and the line height.
  [[nodiscard]] float CellPx() const noexcept
  {
    return m_cellPx;
  }

  [[nodiscard]] bool HasGlyph(std::uint8_t _ch) const noexcept
  {
    return Ready() && _ch >= FIRST_GLYPH && _ch <= m_lastGlyph;
  }
  [[nodiscard]] Cell GlyphCell(std::uint8_t _ch) const noexcept;

  // The middle of the solid cell. A point rather than a rectangle: an untextured quad wants one
  // texel held constant across it, not a stretch of the atlas.
  [[nodiscard]] float SolidU() const noexcept
  {
    return m_solidU;
  }
  [[nodiscard]] float SolidV() const noexcept
  {
    return m_solidV;
  }

private:
  GpuPtr<ID3D12Resource> m_atlas;
  GpuPtr<ID3D12Resource> m_staging;
  float m_cellPx = 0.0f;
  float m_uStep = 0.0f;
  float m_vStep = 0.0f;
  float m_solidU = 0.0f;
  float m_solidV = 0.0f;
  std::uint8_t m_lastGlyph = 0;
};
} // namespace Neuron
