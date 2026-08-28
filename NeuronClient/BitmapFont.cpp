#include "pch.h"
#include "BitmapFont.h"

#include "GpuHelpers.h"

namespace Neuron
{
namespace
{
constexpr DXGI_FORMAT ATLAS_FORMAT = DXGI_FORMAT_R8_UNORM; // one channel, because the atlas is coverage and not colour

// Coverage comes from the alpha channel where the file has one and from the luminance where it does
// not, so a font authored white-on-black reads the same as one authored white-on-transparent. The
// alternative -- assuming one of the two -- turns the other into a solid block of ink.
std::vector<std::uint8_t> CoverageOf(const ByteBuffer& _bgraPixels, bool _hasAlpha)
{
  std::vector<std::uint8_t> coverage(_bgraPixels.size() / 4);
  for (size_t i = 0; i < coverage.size(); ++i)
  {
    const std::uint8_t* texel = _bgraPixels.data() + i * 4;
    coverage[i] = _hasAlpha ? texel[3] : std::max({texel[0], texel[1], texel[2]});
  }
  return coverage;
}
} // namespace

bool BitmapFont::Load(GpuDevice& _gpu, const std::wstring& _fileName, D3D12_CPU_DESCRIPTOR_HANDLE _srv)
{
  DdsImage image;
  if (!DdsImage::Load(_fileName, image))
    return false;

  // The atlas is read texel by texel on the CPU, so it has to be an uncompressed 8-bit surface: a
  // block-compressed font would upload fine and then have no coverage to sample.
  ByteBuffer pixels;
  if (!image.TopMipAsBgra(pixels))
  {
    DebugTrace(L"font {} is not an uncompressed 8-bit surface\n", _fileName);
    return false;
  }

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

  std::vector<std::uint8_t> coverage = CoverageOf(pixels, image.HasAlpha());

  const std::uint32_t solidIndex = SOLID_GLYPH - FIRST_GLYPH;
  const std::uint32_t solidXPx = (solidIndex % GLYPH_COLS) * cellPx;
  const std::uint32_t solidYPx = (solidIndex / GLYPH_COLS) * cellPx;
  for (std::uint32_t y = 0; y < cellPx; ++y)
    std::memset(coverage.data() + static_cast<size_t>(solidYPx + y) * image.widthPx + solidXPx, 0xff, cellPx);

  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Alignment = 0;
  td.Width = image.widthPx;
  td.Height = image.heightPx;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = ATLAS_FORMAT;
  td.SampleDesc.Count = 1;
  td.SampleDesc.Quality = 0;
  td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  td.Flags = D3D12_RESOURCE_FLAG_NONE;

  // put() asserts the pointer is empty rather than releasing what was there, so a font loaded twice
  // has to let go of the first atlas itself.
  m_atlas = nullptr;
  m_staging = nullptr;

  D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(m_atlas.put())));

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rowCount = 0;
  std::uint64_t rowBytes = 0;
  std::uint64_t totalBytes = 0;
  _gpu.Device()->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rowCount, &rowBytes, &totalBytes);

  hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC ud = BufferDesc(totalBytes);
  check_hresult(_gpu.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(m_staging.put())));

  std::uint8_t* dst = nullptr;
  D3D12_RANGE noRead = {0, 0};
  check_hresult(m_staging->Map(0, &noRead, reinterpret_cast<void**>(&dst)));
  for (UINT row = 0; row < rowCount; ++row)
    std::memcpy(dst + static_cast<size_t>(row) * footprint.Footprint.RowPitch, coverage.data() + static_cast<size_t>(row) * image.widthPx,
                static_cast<size_t>(rowBytes));
  m_staging->Unmap(0, nullptr);

  ID3D12GraphicsCommandList* cmd = _gpu.CommandList();

  D3D12_TEXTURE_COPY_LOCATION copyDst = {};
  copyDst.pResource = m_atlas.get();
  copyDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  copyDst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION copySrc = {};
  copySrc.pResource = m_staging.get();
  copySrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  copySrc.PlacedFootprint = footprint;
  cmd->CopyTextureRegion(&copyDst, 0, 0, 0, &copySrc, nullptr);

  const D3D12_RESOURCE_BARRIER toShader =
    Transition(m_atlas.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  cmd->ResourceBarrier(1, &toShader);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = ATLAS_FORMAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MostDetailedMip = 0;
  srv.Texture2D.MipLevels = 1;
  srv.Texture2D.PlaneSlice = 0;
  srv.Texture2D.ResourceMinLODClamp = 0.0f;
  _gpu.Device()->CreateShaderResourceView(m_atlas.get(), &srv, _srv);

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
  return Cell{u0, v0, u0 + m_uStep, v0 + m_vStep};
}
} // namespace Neuron
