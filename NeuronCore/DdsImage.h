#pragma once

#include "FileSys.h"

#include <cstdint>
#include <span>
#include <string>

namespace Neuron
{
// A DDS decoded to one uncompressed surface, so whatever uploads it has a plain block of bytes to
// copy and never has to know what was in the header.
//
// Deliberately narrow: uncompressed 32-bit surfaces, the top mip only, every one of them
// normalised to BGRA byte order. That is what this game's content is, and a reader that also
// carried the block-compressed formats would be carrying a decompressor nothing calls. Widen it
// the day a file needs it, not before.
//
// A file it cannot read is the author's mistake, so it reports and fails closed rather than
// throwing (AGENTS.md 5).
struct DdsImage
{
  std::uint32_t widthPx = 0;
  std::uint32_t heightPx = 0;

  // False when the pixel format declared no alpha channel, in which case the A bytes below were
  // filled in rather than read. A mask-style texture has to know the difference: opaque-by-default
  // and transparent-by-default read the same otherwise.
  bool hasAlpha = false;

  // widthPx * heightPx * 4 bytes -- B, G, R then A per texel, rows tightly packed.
  ByteBuffer pixels;

  [[nodiscard]] bool Empty() const noexcept
  {
    return pixels.empty();
  }

  // _fileName is relative to the asset root unless it carries a drive or a root; FileSys resolves
  // it. Reports false and traces on a file that is missing, truncated, or in a format this reader
  // does not decode, and leaves _outImage as it found it.
  [[nodiscard]] static bool Load(const std::wstring& _fileName, DdsImage& _outImage);

  // The decode on its own, so the format rules can be exercised without a file on disk.
  [[nodiscard]] static bool Parse(std::span<const std::uint8_t> _bytes, DdsImage& _outImage);
};
} // namespace Neuron
