#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronClientTests
{
namespace
{
constexpr size_t PIXELS_OFFSET = 128;
constexpr size_t DX10_HEADER_BYTES = 20;

// The header offsets, named the way the reader names them so a test can bend one field and say
// which one it bent.
constexpr size_t OFFSET_MAGIC = 0;
constexpr size_t OFFSET_HEADER_SIZE = 4;
constexpr size_t OFFSET_HEADER_FLAGS = 8;
constexpr size_t OFFSET_HEIGHT = 12;
constexpr size_t OFFSET_WIDTH = 16;
constexpr size_t OFFSET_PITCH = 20;
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

constexpr size_t OFFSET_DX10_FORMAT = PIXELS_OFFSET + 0;
constexpr size_t OFFSET_DX10_DIMENSION = PIXELS_OFFSET + 4;
constexpr size_t OFFSET_DX10_MISC_FLAG = PIXELS_OFFSET + 8;
constexpr size_t OFFSET_DX10_ARRAY_SIZE = PIXELS_OFFSET + 12;
constexpr size_t OFFSET_DX10_MISC_FLAGS2 = PIXELS_OFFSET + 16;

constexpr std::uint32_t HEADER_FLAGS_BASIC = 0x0002100fu; // caps, height, width, pitch, pixel format, mip count
constexpr std::uint32_t HEADER_HAS_DEPTH = 0x800000u;
constexpr std::uint32_t FORMAT_FLAGS_RGBA = 0x41u; // DDPF_RGB | DDPF_ALPHAPIXELS
constexpr std::uint32_t FORMAT_FLAGS_FOUR_CC = 0x4u;
constexpr std::uint32_t CAPS2_CUBEMAP_ALL = 0xfe00u;
constexpr std::uint32_t CAPS2_VOLUME = 0x200000u;
constexpr std::uint32_t FOUR_CC_DXT1 = 0x31545844u;
constexpr std::uint32_t FOUR_CC_DXT5 = 0x35545844u;
constexpr std::uint32_t FOUR_CC_DX10 = 0x30315844u;

void Put32(Neuron::ByteBuffer& _bytes, size_t _offset, std::uint32_t _value)
{
  _bytes[_offset + 0] = static_cast<std::uint8_t>(_value);
  _bytes[_offset + 1] = static_cast<std::uint8_t>(_value >> 8);
  _bytes[_offset + 2] = static_cast<std::uint8_t>(_value >> 16);
  _bytes[_offset + 3] = static_cast<std::uint8_t>(_value >> 24);
}

// A well-formed 32-bit DDS with room for one surface, built by hand rather than read off disk so a
// test can bend exactly one field and watch what the reader does about it. The masks are the
// A8R8G8B8 ordering the game's own content carries, which puts blue in the first byte.
Neuron::ByteBuffer MakeDds(std::uint32_t _widthPx, std::uint32_t _heightPx, size_t _surfaceBytes)
{
  Neuron::ByteBuffer bytes(PIXELS_OFFSET + _surfaceBytes, 0);
  Put32(bytes, OFFSET_MAGIC, 0x20534444u); // 'DDS '
  Put32(bytes, OFFSET_HEADER_SIZE, 124);
  Put32(bytes, OFFSET_HEADER_FLAGS, HEADER_FLAGS_BASIC);
  Put32(bytes, OFFSET_HEIGHT, _heightPx);
  Put32(bytes, OFFSET_WIDTH, _widthPx);
  Put32(bytes, OFFSET_PITCH, _widthPx * 4); // one packed row
  Put32(bytes, OFFSET_MIP_COUNT, 1);
  Put32(bytes, OFFSET_FORMAT_SIZE, 32);
  Put32(bytes, OFFSET_FORMAT_FLAGS, FORMAT_FLAGS_RGBA);
  Put32(bytes, OFFSET_BIT_COUNT, 32);
  Put32(bytes, OFFSET_RED_MASK, 0x00ff0000u);
  Put32(bytes, OFFSET_GREEN_MASK, 0x0000ff00u);
  Put32(bytes, OFFSET_BLUE_MASK, 0x000000ffu);
  Put32(bytes, OFFSET_ALPHA_MASK, 0xff000000u);
  return bytes;
}

Neuron::ByteBuffer MakeDds(std::uint32_t _widthPx, std::uint32_t _heightPx)
{
  return MakeDds(_widthPx, _heightPx, static_cast<size_t>(_widthPx) * _heightPx * 4);
}

// The same file with a DX10 header in front of the surface, which is where a modern writer puts
// the format, the dimension and the array size.
Neuron::ByteBuffer MakeDx10Dds(std::uint32_t _widthPx, std::uint32_t _heightPx, DXGI_FORMAT _format, std::uint32_t _arraySize,
                               size_t _surfaceBytes)
{
  Neuron::ByteBuffer bytes = MakeDds(_widthPx, _heightPx, DX10_HEADER_BYTES + _surfaceBytes);
  Put32(bytes, OFFSET_FORMAT_FLAGS, FORMAT_FLAGS_FOUR_CC);
  Put32(bytes, OFFSET_FOUR_CC, FOUR_CC_DX10);
  Put32(bytes, OFFSET_BIT_COUNT, 0);
  Put32(bytes, OFFSET_DX10_FORMAT, static_cast<std::uint32_t>(_format));
  Put32(bytes, OFFSET_DX10_DIMENSION, 3); // DDS_DIMENSION_TEXTURE2D
  Put32(bytes, OFFSET_DX10_MISC_FLAG, 0);
  Put32(bytes, OFFSET_DX10_ARRAY_SIZE, _arraySize);
  Put32(bytes, OFFSET_DX10_MISC_FLAGS2, 0);
  return bytes;
}

Neuron::ByteBuffer Bgra(const Neuron::DdsImage& _image)
{
  Neuron::ByteBuffer pixels;
  Assert::IsTrue(_image.TopMipAsBgra(pixels), L"the top mip did not convert to BGRA");
  return pixels;
}

unsigned At(const Neuron::ByteBuffer& _pixels, size_t _texel, size_t _channel)
{
  return static_cast<unsigned>(_pixels[_texel * 4 + _channel]);
}
} // namespace

TEST_CLASS(DdsImageTests)
{
public:
  TEST_METHOD(AMissingTextureFailsClosed)
  {
    // Content errors are diagnostics, never crashes. A texture that is not there reports false and
    // leaves the output alone; it does not throw, and it does not half-fill the image.
    Neuron::DdsImage image;
    Assert::IsFalse(Neuron::DdsImage::Load(L"Fonts\\NoSuchFontExists.dds", image), L"a missing texture reported success");
    Assert::IsTrue(image.Empty(), L"a failed load left pixels behind");
  }

  TEST_METHOD(AnAlphaFirstSurfaceKeepsItsByteOrder)
  {
    // A8R8G8B8 is little-endian, so blue already sits in the first byte and the decode is a copy.
    // If this one ever needs a swizzle, every texture in the tree has been reading wrong.
    Neuron::ByteBuffer bytes = MakeDds(2, 1);
    bytes[PIXELS_OFFSET + 0] = 0x11; // blue
    bytes[PIXELS_OFFSET + 1] = 0x22; // green
    bytes[PIXELS_OFFSET + 2] = 0x33; // red
    bytes[PIXELS_OFFSET + 3] = 0x44; // alpha

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a well-formed surface was rejected");
    Assert::IsTrue(DXGI_FORMAT_B8G8R8A8_UNORM == image.format, L"the masks did not resolve to B8G8R8A8");
    Assert::AreEqual(2u, image.widthPx, L"width");
    Assert::AreEqual(1u, image.heightPx, L"height");
    Assert::AreEqual(1u, image.mipCount, L"mip count");
    Assert::AreEqual(1u, image.arraySize, L"array size");
    Assert::IsTrue(image.HasAlpha(), L"the alpha channel was declared and went unnoticed");
    Assert::AreEqual(8u, static_cast<unsigned>(image.data.size()), L"one texel per pixel, four bytes each");

    const Neuron::ByteBuffer pixels = Bgra(image);
    Assert::AreEqual(8u, static_cast<unsigned>(pixels.size()), L"BGRA is four bytes a texel");
    Assert::AreEqual(0x11u, At(pixels, 0, 0), L"blue");
    Assert::AreEqual(0x22u, At(pixels, 0, 1), L"green");
    Assert::AreEqual(0x33u, At(pixels, 0, 2), L"red");
    Assert::AreEqual(0x44u, At(pixels, 0, 3), L"alpha");
  }

  TEST_METHOD(AnRgbaSurfaceIsSwizzledToBgra)
  {
    // The masks say which byte is which, so a file written the other way round decodes to the same
    // layout as everything else rather than coming out with its channels crossed.
    Neuron::ByteBuffer bytes = MakeDds(1, 1);
    Put32(bytes, OFFSET_RED_MASK, 0x000000ffu);
    Put32(bytes, OFFSET_BLUE_MASK, 0x00ff0000u);
    bytes[PIXELS_OFFSET + 0] = 0x33; // red
    bytes[PIXELS_OFFSET + 1] = 0x22; // green
    bytes[PIXELS_OFFSET + 2] = 0x11; // blue
    bytes[PIXELS_OFFSET + 3] = 0x44; // alpha

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"an RGBA-ordered surface was rejected");
    Assert::IsTrue(DXGI_FORMAT_R8G8B8A8_UNORM == image.format, L"the masks did not resolve to R8G8B8A8");

    const Neuron::ByteBuffer pixels = Bgra(image);
    Assert::AreEqual(0x11u, At(pixels, 0, 0), L"blue");
    Assert::AreEqual(0x22u, At(pixels, 0, 1), L"green");
    Assert::AreEqual(0x33u, At(pixels, 0, 2), L"red");
    Assert::AreEqual(0x44u, At(pixels, 0, 3), L"alpha");
  }

  TEST_METHOD(ASurfaceWithNoAlphaChannelReadsOpaque)
  {
    // The fourth byte is undefined where the format declares no alpha, so it is filled in rather
    // than read. Trusting it would turn a font authored white-on-black into nothing at all.
    Neuron::ByteBuffer bytes = MakeDds(1, 1);
    Put32(bytes, OFFSET_FORMAT_FLAGS, 0x40u); // DDPF_RGB, no DDPF_ALPHAPIXELS
    Put32(bytes, OFFSET_ALPHA_MASK, 0);
    bytes[PIXELS_OFFSET + 3] = 0x00;

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a surface without alpha was rejected");
    Assert::IsTrue(DXGI_FORMAT_B8G8R8X8_UNORM == image.format, L"the masks did not resolve to B8G8R8X8");
    Assert::IsFalse(image.HasAlpha(), L"an alpha channel was reported where the format declared none");
    Assert::AreEqual(0xffu, At(Bgra(image), 0, 3), L"the filled-in alpha is not opaque");
  }

  TEST_METHOD(ASingleChannelSurfaceReadsAsGrey)
  {
    // An 8-bit luminance atlas is the natural way to author a font, and it has to read the same as
    // a white-on-black 32-bit one: grey in the colour bytes, opaque alpha.
    Neuron::ByteBuffer bytes = MakeDds(2, 1, 2);
    Put32(bytes, OFFSET_FORMAT_FLAGS, 0x20000u); // DDPF_LUMINANCE
    Put32(bytes, OFFSET_BIT_COUNT, 8);
    Put32(bytes, OFFSET_RED_MASK, 0xffu);
    Put32(bytes, OFFSET_GREEN_MASK, 0);
    Put32(bytes, OFFSET_BLUE_MASK, 0);
    Put32(bytes, OFFSET_ALPHA_MASK, 0);
    bytes[PIXELS_OFFSET + 0] = 0x80;
    bytes[PIXELS_OFFSET + 1] = 0x20;

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"an L8 surface was rejected");
    Assert::IsTrue(DXGI_FORMAT_R8_UNORM == image.format, L"L8 did not resolve to R8");
    Assert::IsFalse(image.HasAlpha(), L"a luminance surface reported alpha");
    Assert::AreEqual(2u, static_cast<unsigned>(image.subresources.front().rowPitchBytes), L"one byte a texel");

    const Neuron::ByteBuffer pixels = Bgra(image);
    Assert::AreEqual(0x80u, At(pixels, 0, 0), L"blue takes the luminance");
    Assert::AreEqual(0x80u, At(pixels, 0, 1), L"green takes the luminance");
    Assert::AreEqual(0x80u, At(pixels, 0, 2), L"red takes the luminance");
    Assert::AreEqual(0xffu, At(pixels, 0, 3), L"alpha is opaque");
    Assert::AreEqual(0x20u, At(pixels, 1, 2), L"the second texel");
  }

  TEST_METHOD(ABlockCompressedSurfaceIsLaidOutInBlocks)
  {
    // BC1 is half a byte a texel in 8-byte 4x4 blocks, and a mip narrower than a block still takes
    // a whole one. A 8x4 top mip is two blocks; 4x2, 2x1 and 1x1 are one block each.
    Neuron::ByteBuffer bytes = MakeDds(8, 4, 16 + 8 + 8 + 8);
    Put32(bytes, OFFSET_FORMAT_FLAGS, FORMAT_FLAGS_FOUR_CC);
    Put32(bytes, OFFSET_FOUR_CC, FOUR_CC_DXT1);
    Put32(bytes, OFFSET_MIP_COUNT, 4);

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a DXT1 surface was rejected");
    Assert::IsTrue(DXGI_FORMAT_BC1_UNORM == image.format, L"DXT1 did not resolve to BC1");
    Assert::AreEqual(4u, image.mipCount, L"mip count");
    Assert::AreEqual(4u, static_cast<unsigned>(image.subresources.size()), L"one subresource per mip");
    Assert::AreEqual(16u, static_cast<unsigned>(image.subresources[0].rowPitchBytes), L"two blocks across the top mip");
    Assert::AreEqual(16u, static_cast<unsigned>(image.subresources[0].slicePitchBytes), L"one block row in the top mip");
    Assert::AreEqual(16u, static_cast<unsigned>(image.subresources[1].offset), L"mip 1 follows mip 0");
    Assert::AreEqual(8u, static_cast<unsigned>(image.subresources[1].slicePitchBytes), L"4x2 is one block");
    Assert::AreEqual(1u, image.subresources[3].widthPx, L"the last mip is one texel");
    Assert::AreEqual(8u, static_cast<unsigned>(image.subresources[3].slicePitchBytes), L"1x1 is still one block");
    Assert::AreEqual(40u, static_cast<unsigned>(image.data.size()), L"the whole chain was kept");

    // Compressed texels cannot be read on the CPU, and a caller that needs them must be told so
    // rather than handed the block bytes as though they were colours.
    Neuron::ByteBuffer pixels;
    Assert::IsFalse(image.TopMipAsBgra(pixels), L"a BC1 surface converted to BGRA");
    Assert::IsTrue(pixels.empty(), L"a refused conversion left pixels behind");
  }

  TEST_METHOD(ADxt4SurfaceIsPremultiplied)
  {
    // DXT2 and DXT4 differ from DXT3 and DXT5 only in what the alpha means, and the fourCC is the
    // one place that is recorded. Losing it would blend those textures wrong.
    Neuron::ByteBuffer bytes = MakeDds(4, 4, 16);
    Put32(bytes, OFFSET_FORMAT_FLAGS, FORMAT_FLAGS_FOUR_CC);
    Put32(bytes, OFFSET_FOUR_CC, 0x34545844u); // 'DXT4'

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a DXT4 surface was rejected");
    Assert::IsTrue(DXGI_FORMAT_BC3_UNORM == image.format, L"DXT4 did not resolve to BC3");
    Assert::IsTrue(Neuron::DdsImage::AlphaMode::Premultiplied == image.alphaMode, L"DXT4's premultiplied alpha was lost");

    Put32(bytes, OFFSET_FOUR_CC, FOUR_CC_DXT5);
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a DXT5 surface was rejected");
    Assert::IsTrue(Neuron::DdsImage::AlphaMode::Unknown == image.alphaMode, L"DXT5 claimed an alpha mode it does not record");
  }

  TEST_METHOD(ADx10HeaderCarriesTheFormatAndArraySize)
  {
    // The DX10 header names a DXGI format outright and can hold an array; the surfaces then run
    // slice by slice, each with its own mip chain, which is D3D12's subresource order.
    Neuron::ByteBuffer bytes = MakeDx10Dds(2, 2, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3, (16 + 4) * 3);
    Put32(bytes, OFFSET_MIP_COUNT, 2);
    Put32(bytes, OFFSET_DX10_MISC_FLAGS2, 1);            // DDS_ALPHA_MODE_STRAIGHT
    bytes[PIXELS_OFFSET + DX10_HEADER_BYTES + 0] = 0x33; // red, slice 0, texel 0

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a DX10 surface was rejected");
    Assert::IsTrue(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB == image.format, L"the DX10 format was not taken as it stands");
    Assert::IsTrue(Neuron::DdsImage::Dimension::Texture2D == image.dimension, L"dimension");
    Assert::IsTrue(Neuron::DdsImage::AlphaMode::Straight == image.alphaMode, L"the alpha mode was not read");
    Assert::AreEqual(3u, image.arraySize, L"array size");
    Assert::AreEqual(2u, image.mipCount, L"mip count");
    Assert::AreEqual(6u, static_cast<unsigned>(image.subresources.size()), L"slices times mips");
    Assert::AreEqual(20u, static_cast<unsigned>(image.subresources[2].offset), L"slice 1 begins after slice 0's whole chain");
    Assert::AreEqual(1u, image.subresources[3].widthPx, L"slice 1, mip 1 is one texel wide");
    Assert::AreEqual(60u, static_cast<unsigned>(image.data.size()), L"three chains of twenty bytes");
    Assert::AreEqual(0x33u, At(Bgra(image), 0, 2), L"the surface starts after the DX10 header");
  }

  TEST_METHOD(ACubeMapHasSixFaces)
  {
    // A cube is six 2D slices to D3D12, and one short of six is not a cube at all.
    Neuron::ByteBuffer legacy = MakeDds(1, 1, 4 * 6);
    Put32(legacy, OFFSET_CAPS2, CAPS2_CUBEMAP_ALL);

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(legacy, image), L"a legacy cube map was rejected");
    Assert::IsTrue(image.isCubeMap, L"the cube map flag was not read");
    Assert::AreEqual(6u, image.arraySize, L"a cube is six faces");
    Assert::AreEqual(6u, static_cast<unsigned>(image.subresources.size()), L"one subresource per face");

    Put32(legacy, OFFSET_CAPS2, CAPS2_CUBEMAP_ALL & ~0x8000u); // drop the -Z face
    Assert::IsFalse(Neuron::DdsImage::Parse(legacy, image), L"a five-faced cube reported success");

    Neuron::ByteBuffer dx10 = MakeDx10Dds(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 2, 4 * 12);
    Put32(dx10, OFFSET_DX10_MISC_FLAG, 0x4u); // DDS_RESOURCE_MISC_TEXTURECUBE
    Assert::IsTrue(Neuron::DdsImage::Parse(dx10, image), L"a DX10 cube array was rejected");
    Assert::IsTrue(image.isCubeMap, L"the DX10 cube flag was not read");
    Assert::AreEqual(12u, image.arraySize, L"two cubes are twelve faces");
  }

  TEST_METHOD(AVolumeHalvesItsDepthDownTheChain)
  {
    Neuron::ByteBuffer bytes = MakeDds(2, 2, (16 * 2) + 4);
    Put32(bytes, OFFSET_HEADER_FLAGS, HEADER_FLAGS_BASIC | HEADER_HAS_DEPTH);
    Put32(bytes, OFFSET_DEPTH, 2);
    Put32(bytes, OFFSET_MIP_COUNT, 2);
    Put32(bytes, OFFSET_CAPS2, CAPS2_VOLUME);

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a volume was rejected");
    Assert::IsTrue(Neuron::DdsImage::Dimension::Texture3D == image.dimension, L"dimension");
    Assert::AreEqual(2u, image.depth, L"depth");
    Assert::AreEqual(2u, image.subresources[0].depth, L"the top mip keeps its depth");
    Assert::AreEqual(32u, static_cast<unsigned>(image.subresources[1].offset), L"mip 1 follows both slices of mip 0");
    Assert::AreEqual(1u, image.subresources[1].depth, L"depth halves with the rest");
  }

  TEST_METHOD(APackedSixteenBitSurfaceIsMappedNotDecoded)
  {
    // B5G6R5 is a real DXGI format, so the file is accepted and described; it is the CPU-side
    // conversion that declines, because the packed layouts are the GPU's to unpack.
    Neuron::ByteBuffer bytes = MakeDds(3, 1, 6);
    Put32(bytes, OFFSET_FORMAT_FLAGS, 0x40u); // DDPF_RGB
    Put32(bytes, OFFSET_BIT_COUNT, 16);
    Put32(bytes, OFFSET_RED_MASK, 0xf800u);
    Put32(bytes, OFFSET_GREEN_MASK, 0x07e0u);
    Put32(bytes, OFFSET_BLUE_MASK, 0x001fu);
    Put32(bytes, OFFSET_ALPHA_MASK, 0);

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a B5G6R5 surface was rejected");
    Assert::IsTrue(DXGI_FORMAT_B5G6R5_UNORM == image.format, L"the 16-bit masks did not resolve to B5G6R5");
    Assert::AreEqual(6u, static_cast<unsigned>(image.subresources.front().rowPitchBytes), L"two bytes a texel");

    Neuron::ByteBuffer pixels;
    Assert::IsFalse(image.TopMipAsBgra(pixels), L"a 16-bit surface converted to BGRA");
  }

  TEST_METHOD(AnUnhandledLayoutIsRejected)
  {
    Neuron::DdsImage image;

    Neuron::ByteBuffer notADds = MakeDds(1, 1);
    Put32(notADds, OFFSET_HEADER_SIZE, 100);
    Assert::IsFalse(Neuron::DdsImage::Parse(notADds, image), L"a header of the wrong size reported success");

    Neuron::ByteBuffer twentyFourBit = MakeDds(1, 1);
    Put32(twentyFourBit, OFFSET_FORMAT_FLAGS, 0x40u);
    Put32(twentyFourBit, OFFSET_BIT_COUNT, 24);
    Assert::IsFalse(Neuron::DdsImage::Parse(twentyFourBit, image), L"a 24-bit surface reported success; DXGI has no such format");

    Neuron::ByteBuffer unaligned = MakeDds(1, 1);
    Put32(unaligned, OFFSET_RED_MASK, 0x0000f800u);
    Assert::IsFalse(Neuron::DdsImage::Parse(unaligned, image), L"a mask set no DXGI format matches reported success");

    Neuron::ByteBuffer unknownFourCc = MakeDds(4, 4);
    Put32(unknownFourCc, OFFSET_FORMAT_FLAGS, FORMAT_FLAGS_FOUR_CC);
    Put32(unknownFourCc, OFFSET_FOUR_CC, 0x31435445u); // 'ETC1'
    Assert::IsFalse(Neuron::DdsImage::Parse(unknownFourCc, image), L"a fourCC this reader does not know reported success");

    Neuron::ByteBuffer unknownDxgi = MakeDx10Dds(1, 1, DXGI_FORMAT_UNKNOWN, 1, 4);
    Assert::IsFalse(Neuron::DdsImage::Parse(unknownDxgi, image), L"DXGI_FORMAT_UNKNOWN reported success");

    Neuron::ByteBuffer emptyArray = MakeDx10Dds(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 4);
    Assert::IsFalse(Neuron::DdsImage::Parse(emptyArray, image), L"an array of nothing reported success");

    Neuron::ByteBuffer shortDx10 = MakeDds(1, 1, 8);
    Put32(shortDx10, OFFSET_FORMAT_FLAGS, FORMAT_FLAGS_FOUR_CC);
    Put32(shortDx10, OFFSET_FOUR_CC, FOUR_CC_DX10);
    Assert::IsFalse(Neuron::DdsImage::Parse(shortDx10, image), L"a file that ends inside the DX10 header reported success");

    Neuron::ByteBuffer tooWide = MakeDds(16385, 1, 4);
    Assert::IsFalse(Neuron::DdsImage::Parse(tooWide, image), L"a surface wider than D3D12 allows reported success");

    Neuron::ByteBuffer truncated = MakeDds(4, 4);
    truncated.resize(truncated.size() - 1);
    Assert::IsFalse(Neuron::DdsImage::Parse(truncated, image), L"a surface short of a byte reported success");

    Neuron::ByteBuffer shortChain = MakeDds(4, 4, 64 + 15); // the second mip is a byte short
    Put32(shortChain, OFFSET_MIP_COUNT, 2);
    Assert::IsFalse(Neuron::DdsImage::Parse(shortChain, image), L"a mip chain the file does not carry reported success");

    Assert::IsFalse(Neuron::DdsImage::Parse(Neuron::ByteBuffer{}, image), L"an empty buffer reported success");
  }

  TEST_METHOD(ARejectedSurfaceDoesNotDisturbTheOneAlreadyRead)
  {
    // Callers reuse an image across loads, and a reader that half-wrote its output on the way to
    // failing would hand back a texture spliced out of two files.
    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(MakeDds(4, 2), image), L"a well-formed surface was rejected");

    Neuron::ByteBuffer truncated = MakeDds(8, 8);
    truncated.resize(PIXELS_OFFSET + 4);
    Assert::IsFalse(Neuron::DdsImage::Parse(truncated, image), L"a truncated surface reported success");
    Assert::AreEqual(4u, image.widthPx, L"the failed parse moved the width");
    Assert::AreEqual(2u, image.heightPx, L"the failed parse moved the height");
    Assert::AreEqual(32u, static_cast<unsigned>(image.data.size()), L"the failed parse disturbed the pixels");
    Assert::AreEqual(1u, static_cast<unsigned>(image.subresources.size()), L"the failed parse disturbed the subresources");
  }
};
} // namespace NeuronClientTests
