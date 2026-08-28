#pragma once

#include "FileSys.h"

// The DDS DX10 header stores its format as a DXGI_FORMAT number, so that enum is the file format's
// own vocabulary and not a graphics dependency. <dxgiformat.h> is the enum alone -- no interfaces,
// no library -- which is what keeps this reader inside NeuronCore's no-graphics-API rule.
#include <dxgiformat.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Neuron
{
// A DDS file read into memory and described subresource by subresource, so whatever uploads it
// can walk the surfaces in D3D12 order without knowing what was in the header.
//
// Modelled on DirectXTex's DDSTextureLoader12: legacy pixel-format masks and fourCCs, the DX10
// extended header, block-compressed and packed formats, mip chains, arrays, cube maps and volumes
// are all read. The bytes are kept exactly as the file holds them -- nothing is decoded -- because
// the GPU takes every one of these formats as it stands. TopMipAsBgra is the one exception, for
// the callers that read texels on the CPU.
//
// A file it cannot read is the author's mistake, so it reports and fails closed rather than
// throwing (AGENTS.md 5).
struct DdsImage
{
  enum class Dimension
  {
    Texture1D,
    Texture2D,
    Texture3D,
  };

  // What the alpha channel means, from the DX10 header where the file has one and from the fourCC
  // where it does not. Unknown is what the legacy header says about every uncompressed surface.
  enum class AlphaMode
  {
    Unknown,
    Straight,
    Premultiplied,
    Opaque,
    Custom,
  };

  // One mip of one array slice (or cube face), located inside data. The pitches are the file's:
  // packed, with no D3D12 alignment applied, which the uploader supplies from GetCopyableFootprints.
  struct Subresource
  {
    std::uint32_t widthPx = 0;
    std::uint32_t heightPx = 0;
    std::uint32_t depth = 0;
    size_t offset = 0;
    size_t rowPitchBytes = 0;
    size_t slicePitchBytes = 0;
  };

  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  Dimension dimension = Dimension::Texture2D;
  AlphaMode alphaMode = AlphaMode::Unknown;
  bool isCubeMap = false;

  // The top mip. depth is 1 for anything but a volume.
  std::uint32_t widthPx = 0;
  std::uint32_t heightPx = 0;
  std::uint32_t depth = 0;
  std::uint32_t mipCount = 0;
  std::uint32_t arraySize = 0; // six per cube for a cube map, one otherwise

  // The surface bytes as they sit on disk, top mip of slice zero first.
  ByteBuffer data;

  // arraySize * mipCount entries, mip-major within each slice: entry [slice * mipCount + mip] is
  // D3D12 subresource index slice * mipCount + mip.
  std::vector<Subresource> subresources;

  [[nodiscard]] bool Empty() const noexcept
  {
    return data.empty();
  }

  // Whether the format carries an alpha channel that was read rather than filled in. A mask-style
  // texture has to know the difference: opaque-by-default and transparent-by-default read the
  // same otherwise.
  [[nodiscard]] bool HasAlpha() const noexcept;

  // The top mip of slice zero as widthPx * heightPx * 4 bytes -- B, G, R then A per texel, rows
  // tightly packed -- for callers that inspect texels on the CPU. Only the 8-bit-per-channel
  // uncompressed formats convert; anything block-compressed or wider reports false and traces,
  // because a decompressor that only one caller would use is not worth carrying.
  [[nodiscard]] bool TopMipAsBgra(ByteBuffer& _outPixels) const;

  // _fileName is relative to the asset root unless it carries a drive or a root; FileSys resolves
  // it. Reports false and traces on a file that is missing, truncated, or in a format this reader
  // does not decode, and leaves _outImage as it found it.
  [[nodiscard]] static bool Load(const std::wstring& _fileName, DdsImage& _outImage);

  // The decode on its own, so the format rules can be exercised without a file on disk.
  [[nodiscard]] static bool Parse(std::span<const std::uint8_t> _bytes, DdsImage& _outImage);
};
} // namespace Neuron
