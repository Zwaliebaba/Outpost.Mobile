#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NeuronCoreTests
{
namespace
{
constexpr size_t PIXELS_OFFSET = 128;

// The header offsets, named the way the reader names them so a test can bend one field and say
// which one it bent.
constexpr size_t OFFSET_MAGIC = 0;
constexpr size_t OFFSET_HEADER_SIZE = 4;
constexpr size_t OFFSET_HEADER_FLAGS = 8;
constexpr size_t OFFSET_HEIGHT = 12;
constexpr size_t OFFSET_WIDTH = 16;
constexpr size_t OFFSET_PITCH = 20;
constexpr size_t OFFSET_MIP_COUNT = 28;
constexpr size_t OFFSET_FORMAT_SIZE = 76;
constexpr size_t OFFSET_FORMAT_FLAGS = 80;
constexpr size_t OFFSET_FOUR_CC = 84;
constexpr size_t OFFSET_BIT_COUNT = 88;
constexpr size_t OFFSET_RED_MASK = 92;
constexpr size_t OFFSET_GREEN_MASK = 96;
constexpr size_t OFFSET_BLUE_MASK = 100;
constexpr size_t OFFSET_ALPHA_MASK = 104;

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
Neuron::ByteBuffer MakeDds(std::uint32_t _widthPx, std::uint32_t _heightPx)
{
  Neuron::ByteBuffer bytes(PIXELS_OFFSET + static_cast<size_t>(_widthPx) * _heightPx * 4, 0);
  Put32(bytes, OFFSET_MAGIC, 0x20534444u); // 'DDS '
  Put32(bytes, OFFSET_HEADER_SIZE, 124);
  Put32(bytes, OFFSET_HEADER_FLAGS, 0x0002100fu); // caps, height, width, pitch, pixel format, mip count
  Put32(bytes, OFFSET_HEIGHT, _heightPx);
  Put32(bytes, OFFSET_WIDTH, _widthPx);
  Put32(bytes, OFFSET_PITCH, _widthPx * 4); // one packed row
  Put32(bytes, OFFSET_MIP_COUNT, 1);
  Put32(bytes, OFFSET_FORMAT_SIZE, 32);
  Put32(bytes, OFFSET_FORMAT_FLAGS, 0x41u); // DDPF_RGB | DDPF_ALPHAPIXELS
  Put32(bytes, OFFSET_BIT_COUNT, 32);
  Put32(bytes, OFFSET_RED_MASK, 0x00ff0000u);
  Put32(bytes, OFFSET_GREEN_MASK, 0x0000ff00u);
  Put32(bytes, OFFSET_BLUE_MASK, 0x000000ffu);
  Put32(bytes, OFFSET_ALPHA_MASK, 0xff000000u);
  return bytes;
}

unsigned At(const Neuron::DdsImage& _image, size_t _texel, size_t _channel)
{
  return static_cast<unsigned>(_image.pixels[_texel * 4 + _channel]);
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
    Assert::AreEqual(2u, image.widthPx, L"width");
    Assert::AreEqual(1u, image.heightPx, L"height");
    Assert::IsTrue(image.hasAlpha, L"the alpha channel was declared and went unnoticed");
    Assert::AreEqual(8u, static_cast<unsigned>(image.pixels.size()), L"one texel per pixel, four bytes each");
    Assert::AreEqual(0x11u, At(image, 0, 0), L"blue");
    Assert::AreEqual(0x22u, At(image, 0, 1), L"green");
    Assert::AreEqual(0x33u, At(image, 0, 2), L"red");
    Assert::AreEqual(0x44u, At(image, 0, 3), L"alpha");
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
    Assert::AreEqual(0x11u, At(image, 0, 0), L"blue");
    Assert::AreEqual(0x22u, At(image, 0, 1), L"green");
    Assert::AreEqual(0x33u, At(image, 0, 2), L"red");
    Assert::AreEqual(0x44u, At(image, 0, 3), L"alpha");
  }

  TEST_METHOD(ASurfaceWithNoAlphaChannelReadsOpaque)
  {
    // The fourth byte is undefined where the format declares no alpha, so it is filled in rather
    // than read. Trusting it would turn a font authored white-on-black into nothing at all.
    Neuron::ByteBuffer bytes = MakeDds(1, 1);
    Put32(bytes, OFFSET_FORMAT_FLAGS, 0x40u); // DDPF_RGB, no DDPF_ALPHAPIXELS
    bytes[PIXELS_OFFSET + 3] = 0x00;

    Neuron::DdsImage image;
    Assert::IsTrue(Neuron::DdsImage::Parse(bytes, image), L"a surface without alpha was rejected");
    Assert::IsFalse(image.hasAlpha, L"an alpha channel was reported where the format declared none");
    Assert::AreEqual(0xffu, At(image, 0, 3), L"the filled-in alpha is not opaque");
  }

  TEST_METHOD(ACompressedSurfaceIsRejected)
  {
    // This reader carries no decompressor on purpose, so a block-compressed file has to be refused
    // rather than read as though its bytes were texels.
    Neuron::ByteBuffer bytes = MakeDds(4, 4);
    Put32(bytes, OFFSET_FORMAT_FLAGS, 0x4u);   // DDPF_FOURCC
    Put32(bytes, OFFSET_FOUR_CC, 0x31545844u); // 'DXT1'

    Neuron::DdsImage image;
    Assert::IsFalse(Neuron::DdsImage::Parse(bytes, image), L"a DXT1 surface reported success");
    Assert::IsTrue(image.Empty(), L"a rejected surface left pixels behind");
  }

  TEST_METHOD(AnUnhandledLayoutIsRejected)
  {
    Neuron::DdsImage image;

    Neuron::ByteBuffer notADds = MakeDds(1, 1);
    Put32(notADds, OFFSET_HEADER_SIZE, 100);
    Assert::IsFalse(Neuron::DdsImage::Parse(notADds, image), L"a header of the wrong size reported success");

    Neuron::ByteBuffer sixteenBit = MakeDds(1, 1);
    Put32(sixteenBit, OFFSET_BIT_COUNT, 16);
    Assert::IsFalse(Neuron::DdsImage::Parse(sixteenBit, image), L"a 16-bit surface reported success");

    Neuron::ByteBuffer unaligned = MakeDds(1, 1);
    Put32(unaligned, OFFSET_RED_MASK, 0x0000f800u);
    Assert::IsFalse(Neuron::DdsImage::Parse(unaligned, image), L"a mask that is not byte aligned reported success");

    Neuron::ByteBuffer truncated = MakeDds(4, 4);
    truncated.resize(truncated.size() - 1);
    Assert::IsFalse(Neuron::DdsImage::Parse(truncated, image), L"a surface short of a byte reported success");

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
    Assert::AreEqual(32u, static_cast<unsigned>(image.pixels.size()), L"the failed parse disturbed the pixels");
  }
};
} // namespace NeuronCoreTests
