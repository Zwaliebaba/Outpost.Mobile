#include "pch.h"
#include "BitmapFont.h"

#include "GpuHelpers.h"

namespace Neuron
{
bool BitmapFont::Load(GpuDevice& _gpu, const std::wstring& _fileName, D3D12_CPU_DESCRIPTOR_HANDLE _srv)
{
  DdsImage image;
  if (!DdsImage::Load(_fileName, image))
    return false;

  const std::uint32_t cellPx = image.widthPx / GLYPH_COLS;
  if (cellPx == 0 || cellPx * GLYPH_COLS != image.widthPx || image.heightPx % cellPx != 0)
  {
    DebugTrace(L"font {} is not sixteen columns of square cells\n", _fileName);
    return false;
  }

  // Cells run past the printable ASCII range into Latin-1 where the atlas is tall enough, and stop
  // at 255 where it is taller still: a character is one byte and there is no cell for a 256th.
  const std::uint32_t cells = (image.heightPx / cellPx) * GLYPH_COLS;
  if (cells <= SOLID_GLYPH - FIRST_GLYPH)
  {
    // Refused rather than clamped: the fill below would run off the end of the image, and an atlas
    // that stops short of 127 does not cover printable ASCII either.
    DebugTrace(L"font {} is too short to hold the printable glyphs and the solid cell\n", _fileName);
    return false;
  }
  m_lastGlyph = static_cast<std::uint8_t>(std::min<std::uint32_t>(FIRST_GLYPH + cells - 1, 0xffu));

  // The atlas is read texel by texel on the CPU, so it has to be an uncompressed 8-bit surface: a
  // block-compressed font would upload fine and then have no coverage to sample.
  ByteBuffer coverage;
  if (!CoverageOf(image, coverage))
  {
    DebugTrace(L"font {} is not an uncompressed 8-bit surface\n", _fileName);
    return false;
  }

  const std::uint32_t solidIndex = SOLID_GLYPH - FIRST_GLYPH;
  const std::uint32_t solidXPx = (solidIndex % GLYPH_COLS) * cellPx;
  const std::uint32_t solidYPx = (solidIndex / GLYPH_COLS) * cellPx;
  for (std::uint32_t y = 0; y < cellPx; ++y)
    std::memset(coverage.data() + static_cast<size_t>(solidYPx + y) * image.widthPx + solidXPx, 0xff, cellPx);

  UploadCoverageTexture(_gpu, image.widthPx, image.heightPx, coverage, _srv, m_atlas, m_staging);

  m_cellPx = static_cast<float>(cellPx);
  m_uStep = static_cast<float>(cellPx) / static_cast<float>(image.widthPx);
  m_vStep = static_cast<float>(cellPx) / static_cast<float>(image.heightPx);
  m_solidU = (static_cast<float>(solidXPx) + static_cast<float>(cellPx) * 0.5f) / static_cast<float>(image.widthPx);
  m_solidV = (static_cast<float>(solidYPx) + static_cast<float>(cellPx) * 0.5f) / static_cast<float>(image.heightPx);
  DebugTrace(L"font {}: atlas {}x{}, cell {}, glyphs {}..{}\n", _fileName, image.widthPx, image.heightPx, cellPx,
             static_cast<std::uint32_t>(FIRST_GLYPH), static_cast<std::uint32_t>(m_lastGlyph));
  return true;
}

BitmapFont::Cell BitmapFont::GlyphCell(std::uint8_t _ch) const noexcept
{
  const std::uint32_t index = static_cast<std::uint32_t>(_ch) - FIRST_GLYPH;
  const float u0 = static_cast<float>(index % GLYPH_COLS) * m_uStep;
  const float v0 = static_cast<float>(index / GLYPH_COLS) * m_vStep;
  return {u0, v0, u0 + m_uStep, v0 + m_vStep};
}
} // namespace Neuron
