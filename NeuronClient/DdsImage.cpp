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
constexpr size_t OFFSET_DEPTH = 24;
constexpr size_t OFFSET_MIP_COUNT = 28;
constexpr size_t OFFSET_FORMAT_SIZE = 76;
constexpr size_t OFFSET_FORMAT_FLAGS = 80;
constexpr size_t OFFSET_FOUR_CC = 84;
constexpr size_t OFFSET_BIT_COUNT = 88;
constexpr size_t OFFSET_RED_MASK = 92;
constexpr size_t OFFSET_GREEN_MASK = 96;
constexpr size_t OFFSET_BLUE_MASK = 100;
constexpr size_t OFFSET_ALPHA_MASK = 104;
constexpr size_t OFFSET_CAPS2 = 112;
constexpr size_t OFFSET_HEADER_END = 128; // the four magic bytes plus a 124-byte header

// The DX10 extended header, relative to OFFSET_HEADER_END.
constexpr size_t OFFSET_DX10_FORMAT = 0;
constexpr size_t OFFSET_DX10_DIMENSION = 4;
constexpr size_t OFFSET_DX10_MISC_FLAG = 8;
constexpr size_t OFFSET_DX10_ARRAY_SIZE = 12;
constexpr size_t OFFSET_DX10_MISC_FLAGS2 = 16;
constexpr size_t DX10_HEADER_BYTES = 20;

constexpr std::uint32_t DDS_MAGIC = 0x20534444u; // 'DDS ', little-endian
constexpr std::uint32_t HEADER_SIZE = 124;
constexpr std::uint32_t FORMAT_SIZE = 32;

constexpr std::uint32_t HEADER_HAS_DEPTH = 0x800000u;
constexpr std::uint32_t FORMAT_IS_ALPHA_ONLY = 0x2u;
constexpr std::uint32_t FORMAT_IS_FOUR_CC = 0x4u;
constexpr std::uint32_t FORMAT_IS_RGB = 0x40u;
constexpr std::uint32_t FORMAT_IS_LUMINANCE = 0x20000u;
constexpr std::uint32_t FORMAT_IS_BUMP_DUDV = 0x80000u;
constexpr std::uint32_t CAPS2_CUBEMAP = 0x200u;
constexpr std::uint32_t CAPS2_CUBEMAP_ALL_FACES = 0xfc00u;
constexpr std::uint32_t CAPS2_VOLUME = 0x200000u;

constexpr std::uint32_t DX10_DIMENSION_TEXTURE1D = 2;
constexpr std::uint32_t DX10_DIMENSION_TEXTURE2D = 3;
constexpr std::uint32_t DX10_DIMENSION_TEXTURE3D = 4;
constexpr std::uint32_t DX10_MISC_TEXTURECUBE = 0x4u;
constexpr std::uint32_t DX10_MISC2_ALPHA_MODE_MASK = 0x7u;

// D3D12 resource limits. A header past these is either corrupt or for hardware that does not
// exist, and either way the size arithmetic below is only safe because they are enforced first.
constexpr std::uint32_t MAX_TEXTURE_DIMENSION = 16384;
constexpr std::uint32_t MAX_TEXTURE3D_DIMENSION = 2048;
constexpr std::uint32_t MAX_ARRAY_SIZE = 2048;
constexpr std::uint32_t MAX_MIP_COUNT = 15; // log2(16384) + 1

constexpr std::uint32_t CUBE_FACES = 6;
constexpr std::uint32_t BGRA_BYTES_PER_TEXEL = 4;

constexpr std::uint32_t MakeFourCc(char _a, char _b, char _c, char _d) noexcept
{
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(_a)) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(_b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(_c)) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(_d)) << 24);
}

constexpr std::uint32_t FOUR_CC_DX10 = MakeFourCc('D', 'X', '1', '0');

std::uint32_t ReadU32(std::span<const std::uint8_t> _bytes, size_t _offset) noexcept
{
  return static_cast<std::uint32_t>(_bytes[_offset]) | (static_cast<std::uint32_t>(_bytes[_offset + 1]) << 8) |
         (static_cast<std::uint32_t>(_bytes[_offset + 2]) << 16) | (static_cast<std::uint32_t>(_bytes[_offset + 3]) << 24);
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

// The legacy pixel format to DXGI, as DDSTextureLoader12 resolves it. The order of the mask tests
// matters where masks overlap, and the R10G10B10A2 entry reads the masks the way D3DX wrote them,
// which is byte-swapped from what the name says: a file that says A2B10G10R10 is really
// R10G10B10A2 on disk, and honouring the label instead would cross the channels.
DXGI_FORMAT FormatOfLegacyPixelFormat(std::uint32_t _flags, std::uint32_t _bitCount, std::uint32_t _redMask, std::uint32_t _greenMask,
                                      std::uint32_t _blueMask, std::uint32_t _alphaMask) noexcept
{
  const auto masks = [&](std::uint32_t _r, std::uint32_t _g, std::uint32_t _b, std::uint32_t _a) noexcept
  { return _redMask == _r && _greenMask == _g && _blueMask == _b && _alphaMask == _a; };

  if ((_flags & FORMAT_IS_RGB) != 0)
  {
    switch (_bitCount)
    {
    case 32:
      if (masks(0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u))
        return DXGI_FORMAT_R8G8B8A8_UNORM;
      if (masks(0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u))
        return DXGI_FORMAT_B8G8R8A8_UNORM;
      if (masks(0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0u))
        return DXGI_FORMAT_B8G8R8X8_UNORM;
      if (masks(0x3ff00000u, 0x000ffc00u, 0x000003ffu, 0xc0000000u))
        return DXGI_FORMAT_R10G10B10A2_UNORM;
      if (masks(0x0000ffffu, 0xffff0000u, 0u, 0u))
        return DXGI_FORMAT_R16G16_UNORM;
      if (masks(0xffffffffu, 0u, 0u, 0u))
        return DXGI_FORMAT_R32_FLOAT; // D3DX writes D3DFMT_R32F this way
      break;

    // 24-bit falls to the default: there is no 24-bit DXGI format to map it onto.
    case 16:
      if (masks(0x7c00u, 0x03e0u, 0x001fu, 0x8000u))
        return DXGI_FORMAT_B5G5R5A1_UNORM;
      if (masks(0xf800u, 0x07e0u, 0x001fu, 0u))
        return DXGI_FORMAT_B5G6R5_UNORM;
      if (masks(0x0f00u, 0x00f0u, 0x000fu, 0xf000u))
        return DXGI_FORMAT_B4G4R4A4_UNORM;
      if (masks(0x00ffu, 0u, 0u, 0xff00u))
        return DXGI_FORMAT_R8G8_UNORM; // A8L8 written with the RGB flag
      if (masks(0xffffu, 0u, 0u, 0u))
        return DXGI_FORMAT_R16_UNORM; // L16 written with the RGB flag
      break;

    case 8:
      if (masks(0xffu, 0u, 0u, 0u))
        return DXGI_FORMAT_R8_UNORM; // L8 written with the RGB flag
      break;

    default:
      break;
    }
  }
  else if ((_flags & FORMAT_IS_LUMINANCE) != 0)
  {
    switch (_bitCount)
    {
    case 16:
      if (masks(0xffffu, 0u, 0u, 0u))
        return DXGI_FORMAT_R16_UNORM;
      if (masks(0x00ffu, 0u, 0u, 0xff00u))
        return DXGI_FORMAT_R8G8_UNORM;
      break;

    case 8:
      if (masks(0xffu, 0u, 0u, 0u))
        return DXGI_FORMAT_R8_UNORM;
      if (masks(0x00ffu, 0u, 0u, 0xff00u))
        return DXGI_FORMAT_R8G8_UNORM; // some writers put A8L8 under an 8-bit count
      break;

    default:
      break;
    }
  }
  else if ((_flags & FORMAT_IS_ALPHA_ONLY) != 0)
  {
    if (_bitCount == 8)
      return DXGI_FORMAT_A8_UNORM;
  }
  else if ((_flags & FORMAT_IS_BUMP_DUDV) != 0)
  {
    switch (_bitCount)
    {
    case 32:
      if (masks(0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u))
        return DXGI_FORMAT_R8G8B8A8_SNORM; // Q8W8V8U8
      if (masks(0x0000ffffu, 0xffff0000u, 0u, 0u))
        return DXGI_FORMAT_R16G16_SNORM; // V16U16
      break;

    case 16:
      if (masks(0x00ffu, 0xff00u, 0u, 0u))
        return DXGI_FORMAT_R8G8_SNORM; // V8U8
      break;

    default:
      break;
    }
  }

  return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT FormatOfFourCc(std::uint32_t _fourCc) noexcept
{
  switch (_fourCc)
  {
  case MakeFourCc('D', 'X', 'T', '1'):
    return DXGI_FORMAT_BC1_UNORM;
  case MakeFourCc('D', 'X', 'T', '2'):
  case MakeFourCc('D', 'X', 'T', '3'):
    return DXGI_FORMAT_BC2_UNORM;
  case MakeFourCc('D', 'X', 'T', '4'):
  case MakeFourCc('D', 'X', 'T', '5'):
    return DXGI_FORMAT_BC3_UNORM;
  case MakeFourCc('A', 'T', 'I', '1'):
  case MakeFourCc('B', 'C', '4', 'U'):
    return DXGI_FORMAT_BC4_UNORM;
  case MakeFourCc('B', 'C', '4', 'S'):
    return DXGI_FORMAT_BC4_SNORM;
  case MakeFourCc('A', 'T', 'I', '2'):
  case MakeFourCc('B', 'C', '5', 'U'):
    return DXGI_FORMAT_BC5_UNORM;
  case MakeFourCc('B', 'C', '5', 'S'):
    return DXGI_FORMAT_BC5_SNORM;
  case MakeFourCc('R', 'G', 'B', 'G'):
    return DXGI_FORMAT_R8G8_B8G8_UNORM;
  case MakeFourCc('G', 'R', 'B', 'G'):
    return DXGI_FORMAT_G8R8_G8B8_UNORM;
  case MakeFourCc('Y', 'U', 'Y', '2'):
    return DXGI_FORMAT_YUY2;
  case 36: // D3DFMT_A16B16G16R16
    return DXGI_FORMAT_R16G16B16A16_UNORM;
  case 110: // D3DFMT_Q16W16V16U16
    return DXGI_FORMAT_R16G16B16A16_SNORM;
  case 111: // D3DFMT_R16F
    return DXGI_FORMAT_R16_FLOAT;
  case 112: // D3DFMT_G16R16F
    return DXGI_FORMAT_R16G16_FLOAT;
  case 113: // D3DFMT_A16B16G16R16F
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case 114: // D3DFMT_R32F
    return DXGI_FORMAT_R32_FLOAT;
  case 115: // D3DFMT_G32R32F
    return DXGI_FORMAT_R32G32_FLOAT;
  case 116: // D3DFMT_A32B32G32R32F
    return DXGI_FORMAT_R32G32B32A32_FLOAT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

// Bits per texel for the formats a DDS can carry, or 0 for one it cannot (typeless views, video
// formats with no fixed layout, and the enum's gaps). Block-compressed formats report their
// average, which is what the surface-size arithmetic below wants.
std::uint32_t BitsPerPixel(DXGI_FORMAT _format) noexcept
{
  switch (_format)
  {
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return 128;

  case DXGI_FORMAT_R32G32B32_TYPELESS:
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
    return 96;

  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R32G32_TYPELESS:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
  case DXGI_FORMAT_Y416:
  case DXGI_FORMAT_Y210:
  case DXGI_FORMAT_Y216:
    return 64;

  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R11G11B10_FLOAT:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_R16G16_TYPELESS:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SNORM:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R32_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
  case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
  case DXGI_FORMAT_R8G8_B8G8_UNORM:
  case DXGI_FORMAT_G8R8_G8B8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_TYPELESS:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_AYUV:
  case DXGI_FORMAT_Y410:
  case DXGI_FORMAT_YUY2:
    return 32;

  case DXGI_FORMAT_P010:
  case DXGI_FORMAT_P016:
    return 24;

  case DXGI_FORMAT_R8G8_TYPELESS:
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SNORM:
  case DXGI_FORMAT_R8G8_SINT:
  case DXGI_FORMAT_R16_TYPELESS:
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_D16_UNORM:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SNORM:
  case DXGI_FORMAT_R16_SINT:
  case DXGI_FORMAT_B5G6R5_UNORM:
  case DXGI_FORMAT_B5G5R5A1_UNORM:
  case DXGI_FORMAT_A8P8:
  case DXGI_FORMAT_B4G4R4A4_UNORM:
    return 16;

  case DXGI_FORMAT_NV12:
  case DXGI_FORMAT_420_OPAQUE:
  case DXGI_FORMAT_NV11:
    return 12;

  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
  case DXGI_FORMAT_A8_UNORM:
  case DXGI_FORMAT_BC2_TYPELESS:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_TYPELESS:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC5_TYPELESS:
  case DXGI_FORMAT_BC5_UNORM:
  case DXGI_FORMAT_BC5_SNORM:
  case DXGI_FORMAT_BC6H_TYPELESS:
  case DXGI_FORMAT_BC6H_UF16:
  case DXGI_FORMAT_BC6H_SF16:
  case DXGI_FORMAT_BC7_TYPELESS:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
  case DXGI_FORMAT_AI44:
  case DXGI_FORMAT_IA44:
  case DXGI_FORMAT_P8:
    return 8;

  case DXGI_FORMAT_R1_UNORM:
    return 1;

  case DXGI_FORMAT_BC1_TYPELESS:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS:
  case DXGI_FORMAT_BC4_UNORM:
  case DXGI_FORMAT_BC4_SNORM:
    return 4;

  default:
    return 0;
  }
}

// How a format lays its texels out in memory, which is what decides the row and slice pitch of a
// surface. Bytes per unit is per 4x4 block for the compressed layouts and per texel pair for the
// packed ones.
struct SurfaceLayout
{
  enum class Kind
  {
    Linear,
    BlockCompressed,
    Packed,
    Planar,
    Nv11,
  };

  Kind kind = Kind::Linear;
  std::uint32_t bytesPerUnit = 0;
};

SurfaceLayout LayoutOf(DXGI_FORMAT _format) noexcept
{
  switch (_format)
  {
  case DXGI_FORMAT_BC1_TYPELESS:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS:
  case DXGI_FORMAT_BC4_UNORM:
  case DXGI_FORMAT_BC4_SNORM:
    return {SurfaceLayout::Kind::BlockCompressed, 8};

  case DXGI_FORMAT_BC2_TYPELESS:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_TYPELESS:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC5_TYPELESS:
  case DXGI_FORMAT_BC5_UNORM:
  case DXGI_FORMAT_BC5_SNORM:
  case DXGI_FORMAT_BC6H_TYPELESS:
  case DXGI_FORMAT_BC6H_UF16:
  case DXGI_FORMAT_BC6H_SF16:
  case DXGI_FORMAT_BC7_TYPELESS:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return {SurfaceLayout::Kind::BlockCompressed, 16};

  case DXGI_FORMAT_R8G8_B8G8_UNORM:
  case DXGI_FORMAT_G8R8_G8B8_UNORM:
  case DXGI_FORMAT_YUY2:
    return {SurfaceLayout::Kind::Packed, 4};

  case DXGI_FORMAT_Y210:
  case DXGI_FORMAT_Y216:
    return {SurfaceLayout::Kind::Packed, 8};

  case DXGI_FORMAT_NV12:
  case DXGI_FORMAT_420_OPAQUE:
    return {SurfaceLayout::Kind::Planar, 2};

  case DXGI_FORMAT_P010:
  case DXGI_FORMAT_P016:
    return {SurfaceLayout::Kind::Planar, 4};

  case DXGI_FORMAT_NV11:
    return {SurfaceLayout::Kind::Nv11, 4};

  default:
    return {SurfaceLayout::Kind::Linear, 0};
  }
}

// Row and slice pitch of one mip, the way DDSTextureLoader12's GetSurfaceInfo works them out.
// Every product here is bounded by the dimension limits Parse enforced first, so size_t cannot
// overflow.
bool SurfacePitch(DXGI_FORMAT _format, std::uint32_t _widthPx, std::uint32_t _heightPx, size_t& _outRowPitch,
                  size_t& _outSlicePitch) noexcept
{
  const SurfaceLayout layout = LayoutOf(_format);
  const size_t width = _widthPx;
  const size_t height = _heightPx;

  switch (layout.kind)
  {
  case SurfaceLayout::Kind::BlockCompressed:
  {
    const size_t blocksWide = width > 0 ? std::max<size_t>(1, (width + 3) / 4) : 0;
    const size_t blocksHigh = height > 0 ? std::max<size_t>(1, (height + 3) / 4) : 0;
    _outRowPitch = blocksWide * layout.bytesPerUnit;
    _outSlicePitch = _outRowPitch * blocksHigh;
    return true;
  }

  case SurfaceLayout::Kind::Packed:
    _outRowPitch = ((width + 1) >> 1) * layout.bytesPerUnit;
    _outSlicePitch = _outRowPitch * height;
    return true;

  case SurfaceLayout::Kind::Planar:
    _outRowPitch = ((width + 1) >> 1) * layout.bytesPerUnit;
    _outSlicePitch = (_outRowPitch * height) + ((_outRowPitch * height + 1) >> 1);
    return true;

  case SurfaceLayout::Kind::Nv11:
    _outRowPitch = ((width + 3) >> 2) * layout.bytesPerUnit;
    _outSlicePitch = _outRowPitch * height * 2; // the luma rows, then the chroma rows
    return true;

  case SurfaceLayout::Kind::Linear:
  {
    const size_t bitsPerPixel = BitsPerPixel(_format);
    if (bitsPerPixel == 0)
      return false;
    _outRowPitch = (width * bitsPerPixel + 7) / 8;
    _outSlicePitch = _outRowPitch * height;
    return true;
  }
  }

  return false;
}

// Which byte of a 32-bit texel each channel sits in, for the formats TopMipAsBgra converts.
// -1 marks a channel the format does not carry.
struct ChannelBytes
{
  int red = -1;
  int green = -1;
  int blue = -1;
  int alpha = -1;
  std::uint32_t bytesPerTexel = 0;
};

bool ChannelBytesOf(DXGI_FORMAT _format, ChannelBytes& _out) noexcept
{
  switch (_format)
  {
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    _out = {2, 1, 0, 3, 4};
    return true;
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    _out = {2, 1, 0, -1, 4};
    return true;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    _out = {0, 1, 2, 3, 4};
    return true;
  case DXGI_FORMAT_R8G8_UNORM:
    _out = {0, 1, -1, -1, 2};
    return true;
  case DXGI_FORMAT_R8_UNORM:
    _out = {0, -1, -1, -1, 1};
    return true;
  case DXGI_FORMAT_A8_UNORM:
    _out = {-1, -1, -1, 0, 1};
    return true;
  default:
    return false;
  }
}
} // namespace

namespace Neuron
{
bool DdsImage::HasAlpha() const noexcept
{
  switch (format)
  {
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_A8_UNORM:
  case DXGI_FORMAT_BC1_TYPELESS:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC2_TYPELESS:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_TYPELESS:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_B5G5R5A1_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_BC7_TYPELESS:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
  case DXGI_FORMAT_AYUV:
  case DXGI_FORMAT_Y410:
  case DXGI_FORMAT_Y416:
  case DXGI_FORMAT_AI44:
  case DXGI_FORMAT_IA44:
  case DXGI_FORMAT_A8P8:
  case DXGI_FORMAT_B4G4R4A4_UNORM:
    return true;
  default:
    return false;
  }
}

bool DdsImage::TopMipAsBgra(ByteBuffer& _outPixels) const
{
  ChannelBytes channels;
  if (Empty() || subresources.empty() || !ChannelBytesOf(format, channels))
  {
    DebugTrace("dds: format {} does not convert to BGRA on the CPU\n", static_cast<int>(format));
    return false;
  }

  const Subresource& top = subresources.front();
  const size_t packedRowBytes = static_cast<size_t>(top.widthPx) * BGRA_BYTES_PER_TEXEL;
  ByteBuffer pixels(packedRowBytes * top.heightPx);
  for (std::uint32_t y = 0; y < top.heightPx; ++y)
  {
    const std::uint8_t* src = data.data() + top.offset + static_cast<size_t>(y) * top.rowPitchBytes;
    std::uint8_t* dst = pixels.data() + static_cast<size_t>(y) * packedRowBytes;
    for (std::uint32_t x = 0; x < top.widthPx; ++x)
    {
      // A single-channel format is grey: R8 replicates into all three colour bytes and A8 sits on
      // white, so a mask-style texture reads the same whichever channel its author put it in.
      const std::uint8_t red = channels.red >= 0 ? src[static_cast<size_t>(channels.red)] : 0xffu;
      dst[0] = channels.blue >= 0 ? src[static_cast<size_t>(channels.blue)] : red;
      dst[1] = channels.green >= 0 ? src[static_cast<size_t>(channels.green)] : red;
      dst[2] = red;
      dst[3] = channels.alpha >= 0 ? src[static_cast<size_t>(channels.alpha)] : 0xffu;
      src += channels.bytesPerTexel;
      dst += BGRA_BYTES_PER_TEXEL;
    }
  }

  _outPixels = std::move(pixels);
  return true;
}

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
  if (_bytes.size() < OFFSET_HEADER_END)
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

  const std::uint32_t headerFlags = ReadU32(_bytes, OFFSET_HEADER_FLAGS);
  const std::uint32_t formatFlags = ReadU32(_bytes, OFFSET_FORMAT_FLAGS);
  const std::uint32_t fourCc = ReadU32(_bytes, OFFSET_FOUR_CC);
  const std::uint32_t caps2 = ReadU32(_bytes, OFFSET_CAPS2);
  const bool hasDx10Header = (formatFlags & FORMAT_IS_FOUR_CC) != 0 && fourCc == FOUR_CC_DX10;

  size_t dataOffset = OFFSET_HEADER_END;
  if (hasDx10Header)
  {
    dataOffset += DX10_HEADER_BYTES;
    if (_bytes.size() < dataOffset)
    {
      DebugTrace("dds: the DX10 header is cut short\n");
      return false;
    }
  }

  std::uint32_t widthPx = ReadU32(_bytes, OFFSET_WIDTH);
  std::uint32_t heightPx = ReadU32(_bytes, OFFSET_HEIGHT);
  std::uint32_t depth = ReadU32(_bytes, OFFSET_DEPTH);
  std::uint32_t mipCount = std::max<std::uint32_t>(1, ReadU32(_bytes, OFFSET_MIP_COUNT));
  std::uint32_t arraySize = 1;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  Dimension dimension = Dimension::Texture2D;
  AlphaMode alphaMode = AlphaMode::Unknown;
  bool isCubeMap = false;

  if (hasDx10Header)
  {
    const std::span<const std::uint8_t> dx10 = _bytes.subspan(OFFSET_HEADER_END, DX10_HEADER_BYTES);
    format = static_cast<DXGI_FORMAT>(ReadU32(dx10, OFFSET_DX10_FORMAT));
    arraySize = ReadU32(dx10, OFFSET_DX10_ARRAY_SIZE);
    if (arraySize == 0)
    {
      DebugTrace("dds: the DX10 header declares an array of nothing\n");
      return false;
    }

    // DXGI_FORMAT_UNKNOWN has no size, and the enum's gaps and reserved values are not formats the
    // GPU can be asked for. BitsPerPixel is the list of what can actually be laid out.
    if (BitsPerPixel(format) == 0)
    {
      DebugTrace("dds: DXGI format {} is not one a texture can hold\n", static_cast<int>(format));
      return false;
    }

    switch (ReadU32(dx10, OFFSET_DX10_DIMENSION))
    {
    case DX10_DIMENSION_TEXTURE1D:
      // A 1D texture with a height other than one is a 2D texture with the wrong label.
      if ((headerFlags & HEADER_HAS_DEPTH) != 0 || heightPx != 1)
      {
        DebugTrace("dds: a 1D texture cannot be {} texels high\n", heightPx);
        return false;
      }
      dimension = Dimension::Texture1D;
      heightPx = 1;
      depth = 1;
      break;

    case DX10_DIMENSION_TEXTURE2D:
      if ((ReadU32(dx10, OFFSET_DX10_MISC_FLAG) & DX10_MISC_TEXTURECUBE) != 0)
      {
        arraySize *= CUBE_FACES;
        isCubeMap = true;
      }
      dimension = Dimension::Texture2D;
      depth = 1;
      break;

    case DX10_DIMENSION_TEXTURE3D:
      if ((headerFlags & HEADER_HAS_DEPTH) == 0)
      {
        DebugTrace("dds: a volume texture declares no depth\n");
        return false;
      }
      if (arraySize > 1)
      {
        DebugTrace("dds: there is no such thing as an array of volumes\n");
        return false;
      }
      dimension = Dimension::Texture3D;
      break;

    default:
      DebugTrace("dds: resource dimension {} is not a texture\n", ReadU32(dx10, OFFSET_DX10_DIMENSION));
      return false;
    }

    switch (ReadU32(dx10, OFFSET_DX10_MISC_FLAGS2) & DX10_MISC2_ALPHA_MODE_MASK)
    {
    case 1:
      alphaMode = AlphaMode::Straight;
      break;
    case 2:
      alphaMode = AlphaMode::Premultiplied;
      break;
    case 3:
      alphaMode = AlphaMode::Opaque;
      break;
    case 4:
      alphaMode = AlphaMode::Custom;
      break;
    default:
      alphaMode = AlphaMode::Unknown;
      break;
    }
  }
  else
  {
    if ((formatFlags & FORMAT_IS_FOUR_CC) != 0)
    {
      format = FormatOfFourCc(fourCc);
      if (format == DXGI_FORMAT_UNKNOWN)
      {
        DebugTrace("dds: '{}' is not a fourCC this reader knows\n", FourCcText(fourCc));
        return false;
      }
      // DXT2 and DXT4 are DXT3 and DXT5 with the colour already multiplied through, and the fourCC
      // is the only place the file says so.
      if (fourCc == MakeFourCc('D', 'X', 'T', '2') || fourCc == MakeFourCc('D', 'X', 'T', '4'))
        alphaMode = AlphaMode::Premultiplied;
    }
    else
    {
      format = FormatOfLegacyPixelFormat(formatFlags, ReadU32(_bytes, OFFSET_BIT_COUNT), ReadU32(_bytes, OFFSET_RED_MASK),
                                         ReadU32(_bytes, OFFSET_GREEN_MASK), ReadU32(_bytes, OFFSET_BLUE_MASK),
                                         ReadU32(_bytes, OFFSET_ALPHA_MASK));
      if (format == DXGI_FORMAT_UNKNOWN)
      {
        DebugTrace("dds: {} bits per texel with masks {:08x}/{:08x}/{:08x}/{:08x} is not a layout this reader maps\n",
                   ReadU32(_bytes, OFFSET_BIT_COUNT), ReadU32(_bytes, OFFSET_RED_MASK), ReadU32(_bytes, OFFSET_GREEN_MASK),
                   ReadU32(_bytes, OFFSET_BLUE_MASK), ReadU32(_bytes, OFFSET_ALPHA_MASK));
        return false;
      }
    }

    if ((headerFlags & HEADER_HAS_DEPTH) != 0)
    {
      dimension = Dimension::Texture3D;
    }
    else
    {
      if ((caps2 & CAPS2_CUBEMAP) != 0)
      {
        // A partial cube is not something D3D12 can be handed, so all six faces are required.
        if ((caps2 & CAPS2_CUBEMAP_ALL_FACES) != CAPS2_CUBEMAP_ALL_FACES)
        {
          DebugTrace("dds: a cube map is missing faces\n");
          return false;
        }
        arraySize = CUBE_FACES;
        isCubeMap = true;
      }
      dimension = Dimension::Texture2D;
      depth = 1;
    }

    // A legacy header that claims a volume without the caps bit is a writer's slip, not a volume.
    if (dimension == Dimension::Texture3D && (caps2 & CAPS2_VOLUME) == 0)
      DebugTrace("dds: the header has a depth but is not flagged as a volume; reading it as one anyway\n");
  }

  if (widthPx == 0 || heightPx == 0 || depth == 0)
  {
    DebugTrace("dds: {}x{}x{} is not a surface\n", widthPx, heightPx, depth);
    return false;
  }

  if (mipCount > MAX_MIP_COUNT)
  {
    DebugTrace("dds: {} mips is more than any texture can hold\n", mipCount);
    return false;
  }

  switch (dimension)
  {
  case Dimension::Texture1D:
    if (arraySize > MAX_ARRAY_SIZE || widthPx > MAX_TEXTURE_DIMENSION)
    {
      DebugTrace("dds: a 1D texture of {} texels x {} slices is past the D3D12 limit\n", widthPx, arraySize);
      return false;
    }
    break;

  case Dimension::Texture2D:
    if (arraySize > MAX_ARRAY_SIZE || widthPx > MAX_TEXTURE_DIMENSION || heightPx > MAX_TEXTURE_DIMENSION)
    {
      DebugTrace("dds: a 2D texture of {}x{} x {} slices is past the D3D12 limit\n", widthPx, heightPx, arraySize);
      return false;
    }
    break;

  case Dimension::Texture3D:
    if (widthPx > MAX_TEXTURE3D_DIMENSION || heightPx > MAX_TEXTURE3D_DIMENSION || depth > MAX_TEXTURE3D_DIMENSION)
    {
      DebugTrace("dds: a volume of {}x{}x{} is past the D3D12 limit\n", widthPx, heightPx, depth);
      return false;
    }
    break;
  }

  // Lay the subresources out in D3D12 order and total them up before touching a byte, so a file
  // short of its own chain is refused whole rather than read up to the gap.
  std::vector<Subresource> subresources;
  subresources.reserve(static_cast<size_t>(arraySize) * mipCount);
  size_t totalBytes = 0;
  for (std::uint32_t slice = 0; slice < arraySize; ++slice)
  {
    std::uint32_t mipWidthPx = widthPx;
    std::uint32_t mipHeightPx = heightPx;
    std::uint32_t mipDepth = depth;
    for (std::uint32_t mip = 0; mip < mipCount; ++mip)
    {
      Subresource sub;
      sub.widthPx = mipWidthPx;
      sub.heightPx = mipHeightPx;
      sub.depth = mipDepth;
      sub.offset = totalBytes;
      if (!SurfacePitch(format, mipWidthPx, mipHeightPx, sub.rowPitchBytes, sub.slicePitchBytes))
      {
        DebugTrace("dds: DXGI format {} has no surface layout this reader can size\n", static_cast<int>(format));
        return false;
      }
      subresources.push_back(sub);

      totalBytes += sub.slicePitchBytes * mipDepth;
      mipWidthPx = std::max<std::uint32_t>(1, mipWidthPx >> 1);
      mipHeightPx = std::max<std::uint32_t>(1, mipHeightPx >> 1);
      mipDepth = std::max<std::uint32_t>(1, mipDepth >> 1);
    }
  }

  const size_t surfaceBytes = _bytes.size() - dataOffset;
  if (totalBytes > surfaceBytes)
  {
    DebugTrace("dds: {}x{} with {} mips and {} slices needs {} bytes and the file carries {}\n", widthPx, heightPx, mipCount, arraySize,
               totalBytes, surfaceBytes);
    return false;
  }

  // Written last, so a rejected file leaves the caller's image exactly as it was.
  _outImage.format = format;
  _outImage.dimension = dimension;
  _outImage.alphaMode = alphaMode;
  _outImage.isCubeMap = isCubeMap;
  _outImage.widthPx = widthPx;
  _outImage.heightPx = heightPx;
  _outImage.depth = depth;
  _outImage.mipCount = mipCount;
  _outImage.arraySize = arraySize;
  _outImage.data.assign(_bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                        _bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset + totalBytes));
  _outImage.subresources = std::move(subresources);
  return true;
}
} // namespace Neuron
