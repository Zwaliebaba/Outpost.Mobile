#include "pch.h"
#include "DdsImage.h"

namespace
{
// Byte offsets into the file. Named rather than mapped onto a struct: the layout on disk is
// little-endian and byte-packed, and a struct only reproduces that by accident of one compiler's
// alignment rules.
constexpr size_t OFFSET_MAGIC = 0;
constexpr size_t OFFSET_HEADER_SIZE = 4;
constexpr size_t OFFSET_HEADER_FLAGS = 8;
constexpr size_t OFFSET_HEIGHT = 12;
constexpr size_t OFFSET_WIDTH = 16;
constexpr size_t OFFSET_PITCH = 20;
constexpr size_t OFFSET_FORMAT_SIZE = 76;
constexpr size_t OFFSET_FORMAT_FLAGS = 80;
constexpr size_t OFFSET_FOUR_CC = 84;
constexpr size_t OFFSET_BIT_COUNT = 88;
constexpr size_t OFFSET_RED_MASK = 92;
constexpr size_t OFFSET_GREEN_MASK = 96;
constexpr size_t OFFSET_BLUE_MASK = 100;
constexpr size_t OFFSET_ALPHA_MASK = 104;
constexpr size_t OFFSET_PIXELS = 128; // the four magic bytes plus a 124-byte header

constexpr std::uint32_t DDS_MAGIC = 0x20534444u; // 'DDS ', little-endian
constexpr std::uint32_t HEADER_SIZE = 124;
constexpr std::uint32_t FORMAT_SIZE = 32;
constexpr std::uint32_t BYTES_PER_TEXEL = 4;

constexpr std::uint32_t HEADER_HAS_PITCH = 0x8u;
constexpr std::uint32_t FORMAT_HAS_ALPHA = 0x1u;
constexpr std::uint32_t FORMAT_IS_FOUR_CC = 0x4u;
constexpr std::uint32_t FORMAT_IS_RGB = 0x40u;

std::uint32_t ReadU32(std::span<const std::uint8_t> _bytes, size_t _offset) noexcept
{
  return static_cast<std::uint32_t>(_bytes[_offset]) | (static_cast<std::uint32_t>(_bytes[_offset + 1]) << 8) |
         (static_cast<std::uint32_t>(_bytes[_offset + 2]) << 16) | (static_cast<std::uint32_t>(_bytes[_offset + 3]) << 24);
}

// Which byte of a texel a channel mask selects, or -1 for a mask this reader cannot honour. Only
// byte-aligned 8-bit channels are accepted; the packed 16-bit formats would need a different loop
// and nothing in this game's content is in one.
int ByteIndexOf(std::uint32_t _mask) noexcept
{
  switch (_mask)
  {
  case 0x000000ffu:
    return 0;
  case 0x0000ff00u:
    return 1;
  case 0x00ff0000u:
    return 2;
  case 0xff000000u:
    return 3;
  default:
    return -1;
  }
}

// A fourCC is four characters that were meant to be read, so the diagnostic prints them rather
// than the number: "DXT5" says what to do about it and 0x35545844 does not.
std::string FourCcText(std::uint32_t _fourCc)
{
  std::string text(4, '?');
  for (size_t i = 0; i < text.size(); ++i)
  {
    const char letter = static_cast<char>((_fourCc >> (i * 8)) & 0xffu);
    if (letter >= 32 && letter < 127)
      text[i] = letter;
  }
  return text;
}
} // namespace

namespace Neuron
{
bool DdsImage::Load(const std::wstring& _fileName, DdsImage& _outImage)
{
  const ByteBuffer file = BinaryFile::ReadFile(_fileName);
  if (file.empty())
  {
    DebugTrace(L"texture {} could not be read\n", _fileName);
    return false;
  }

  if (!Parse(file, _outImage))
  {
    DebugTrace(L"texture {} was rejected; see the reason above\n", _fileName);
    return false;
  }

  return true;
}

bool DdsImage::Parse(std::span<const std::uint8_t> _bytes, DdsImage& _outImage)
{
  if (_bytes.size() < OFFSET_PIXELS)
  {
    DebugTrace("dds: {} bytes is shorter than a header\n", _bytes.size());
    return false;
  }

  if (ReadU32(_bytes, OFFSET_MAGIC) != DDS_MAGIC || ReadU32(_bytes, OFFSET_HEADER_SIZE) != HEADER_SIZE ||
      ReadU32(_bytes, OFFSET_FORMAT_SIZE) != FORMAT_SIZE)
  {
    DebugTrace("dds: no DDS header here\n");
    return false;
  }

  const std::uint32_t formatFlags = ReadU32(_bytes, OFFSET_FORMAT_FLAGS);
  if ((formatFlags & FORMAT_IS_FOUR_CC) != 0)
  {
    // Both the block-compressed formats and the DX10 extended header land here.
    DebugTrace("dds: '{}' is not an uncompressed surface\n", FourCcText(ReadU32(_bytes, OFFSET_FOUR_CC)));
    return false;
  }

  const std::uint32_t bitCount = ReadU32(_bytes, OFFSET_BIT_COUNT);
  if ((formatFlags & FORMAT_IS_RGB) == 0 || bitCount != BYTES_PER_TEXEL * 8)
  {
    DebugTrace("dds: {} bits per texel, and this reader takes 32-bit RGB surfaces only\n", bitCount);
    return false;
  }

  const bool hasAlpha = (formatFlags & FORMAT_HAS_ALPHA) != 0;
  const int red = ByteIndexOf(ReadU32(_bytes, OFFSET_RED_MASK));
  const int green = ByteIndexOf(ReadU32(_bytes, OFFSET_GREEN_MASK));
  const int blue = ByteIndexOf(ReadU32(_bytes, OFFSET_BLUE_MASK));
  const int alpha = hasAlpha ? ByteIndexOf(ReadU32(_bytes, OFFSET_ALPHA_MASK)) : -1;
  if (red < 0 || green < 0 || blue < 0 || (hasAlpha && alpha < 0))
  {
    DebugTrace("dds: the channel masks are not byte aligned\n");
    return false;
  }

  const std::uint32_t widthPx = ReadU32(_bytes, OFFSET_WIDTH);
  const std::uint32_t heightPx = ReadU32(_bytes, OFFSET_HEIGHT);
  if (widthPx == 0 || heightPx == 0)
  {
    DebugTrace("dds: {}x{} is not a surface\n", widthPx, heightPx);
    return false;
  }

  // The widest row the file could hold. Every size test below divides by the height rather than
  // multiplying by it, because a header is untrusted input and two numbers out of one can overflow
  // a product but never a quotient.
  const size_t surfaceBytes = _bytes.size() - OFFSET_PIXELS;
  const size_t rowBudget = surfaceBytes / heightPx;

  // dwPitchOrLinearSize is honoured only where the header claims it, it is wider than a packed row,
  // and the file is actually that big: writers disagree about the field, and more than one puts the
  // whole surface size in it.
  const size_t packedRowBytes = static_cast<size_t>(widthPx) * BYTES_PER_TEXEL;
  size_t rowBytes = packedRowBytes;
  if ((ReadU32(_bytes, OFFSET_HEADER_FLAGS) & HEADER_HAS_PITCH) != 0)
  {
    const size_t claimed = ReadU32(_bytes, OFFSET_PITCH);
    if (claimed > packedRowBytes && claimed <= rowBudget)
      rowBytes = claimed;
  }

  // Only the top mip is read. Whatever mip chain the authoring tool wrote sits after it, and
  // nothing downstream wants one: the atlases are sampled at their own scale and nothing else.
  if (rowBytes > rowBudget)
  {
    DebugTrace("dds: {}x{} needs {} rows of {} bytes and the file carries {}\n", widthPx, heightPx, heightPx, rowBytes, surfaceBytes);
    return false;
  }

  ByteBuffer pixels(packedRowBytes * heightPx);
  for (std::uint32_t y = 0; y < heightPx; ++y)
  {
    const std::uint8_t* src = _bytes.data() + OFFSET_PIXELS + static_cast<size_t>(y) * rowBytes;
    std::uint8_t* dst = pixels.data() + static_cast<size_t>(y) * packedRowBytes;
    for (std::uint32_t x = 0; x < widthPx; ++x)
    {
      dst[0] = src[static_cast<size_t>(blue)];
      dst[1] = src[static_cast<size_t>(green)];
      dst[2] = src[static_cast<size_t>(red)];
      dst[3] = hasAlpha ? src[static_cast<size_t>(alpha)] : 0xffu;
      src += BYTES_PER_TEXEL;
      dst += BYTES_PER_TEXEL;
    }
  }

  // Written last, so a rejected file leaves the caller's image exactly as it was.
  _outImage.widthPx = widthPx;
  _outImage.heightPx = heightPx;
  _outImage.hasAlpha = hasAlpha;
  _outImage.pixels = std::move(pixels);
  return true;
}
} // namespace Neuron
