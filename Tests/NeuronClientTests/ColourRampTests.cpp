#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
constexpr std::size_t PIXELS_OFFSET = 128;

void Put32(Neuron::ByteBuffer& _bytes, std::size_t _offset, std::uint32_t _value)
{
  _bytes[_offset + 0] = static_cast<std::uint8_t>(_value);
  _bytes[_offset + 1] = static_cast<std::uint8_t>(_value >> 8);
  _bytes[_offset + 2] = static_cast<std::uint8_t>(_value >> 16);
  _bytes[_offset + 3] = static_cast<std::uint8_t>(_value >> 24);
}

// A 32-bit A8R8G8B8 DDS built by hand, the masks the game's own ramps carry. Built here rather than
// read off disk so that the test states its own image: what a ramp means by "row 0" is the thing
// under test, and a file would only move the question somewhere a test cannot see it.
[[nodiscard]] Neuron::ByteBuffer MakeRampDds(std::uint32_t _widthPx, std::uint32_t _heightPx)
{
  Neuron::ByteBuffer bytes(PIXELS_OFFSET + static_cast<std::size_t>(_widthPx) * _heightPx * 4, 0);
  Put32(bytes, 0, 0x20534444u); // 'DDS '
  Put32(bytes, 4, 124);
  Put32(bytes, 8, 0x0002100fu); // caps, height, width, pitch, pixel format, mip count
  Put32(bytes, 12, _heightPx);
  Put32(bytes, 16, _widthPx);
  Put32(bytes, 20, _widthPx * 4);
  Put32(bytes, 28, 1);
  Put32(bytes, 76, 32);
  Put32(bytes, 80, 0x41u); // DDPF_RGB | DDPF_ALPHAPIXELS
  Put32(bytes, 88, 32);
  Put32(bytes, 92, 0x00ff0000u);
  Put32(bytes, 96, 0x0000ff00u);
  Put32(bytes, 100, 0x000000ffu);
  Put32(bytes, 104, 0xff000000u);

  // Red at (0, 0), green at (right, 0), blue at (0, bottom), white at (right, bottom), linear
  // between: one image that pins the corners, the orientation and the bilinear midpoint at once.
  const float lastColumn = static_cast<float>(_widthPx - 1);
  const float lastRow = static_cast<float>(_heightPx - 1);
  for (std::uint32_t row = 0; row < _heightPx; ++row)
  {
    for (std::uint32_t column = 0; column < _widthPx; ++column)
    {
      const float across = static_cast<float>(column) / lastColumn;
      const float down = static_cast<float>(row) / lastRow;
      const auto byte = [](float _value) { return static_cast<std::uint8_t>(std::lround(_value * 255.0f)); };

      const std::size_t texel = PIXELS_OFFSET + (static_cast<std::size_t>(row) * _widthPx + column) * 4;
      bytes[texel + 0] = byte(down);                                   // blue: none at the summit, full at sea level
      bytes[texel + 1] = byte(across);                                 // green: none on the flat, full on the cliff
      bytes[texel + 2] = byte(std::lerp(1.0f - across, across, down)); // red: the flat summit and the cliff sea floor
      bytes[texel + 3] = 255;
    }
  }

  return bytes;
}

[[nodiscard]] Neuron::ColourRamp MakeRamp(std::uint32_t _widthPx, std::uint32_t _heightPx, bool& _outLoaded)
{
  Neuron::DdsImage image;
  Assert::IsTrue(Neuron::DdsImage::Parse(MakeRampDds(_widthPx, _heightPx), image), L"the synthetic ramp did not parse");

  Neuron::ColourRamp ramp;
  _outLoaded = ramp.FromImage(image);
  return ramp;
}

void AssertColour(const XMFLOAT3& _expected, const XMFLOAT3& _actual, const wchar_t* _what)
{
  constexpr float ONE_TEXEL_LEVEL = 1.0f / 255.0f;
  Assert::AreEqual(_expected.x, _actual.x, ONE_TEXEL_LEVEL, _what);
  Assert::AreEqual(_expected.y, _actual.y, ONE_TEXEL_LEVEL, _what);
  Assert::AreEqual(_expected.z, _actual.z, ONE_TEXEL_LEVEL, _what);
}
} // namespace

TEST_CLASS(ColourRampTests)
{
public:
  TEST_METHOD(TheFourCornersComeBack)
  {
    // u is the slope axis and v the climate axis, and getting either the wrong way round produces a
    // world that is coloured -- plausibly, even -- and wrong everywhere.
    bool loaded = false;
    const Neuron::ColourRamp ramp = MakeRamp(Neuron::ColourRamp::SIDE, Neuron::ColourRamp::SIDE, loaded);
    Assert::IsTrue(loaded, L"a 64x64 ramp did not load");

    AssertColour(XMFLOAT3(1.0f, 0.0f, 0.0f), ramp.Sample(0.0f, 0.0f), L"the flat summit corner is not red");
    AssertColour(XMFLOAT3(0.0f, 1.0f, 0.0f), ramp.Sample(1.0f, 0.0f), L"the cliff summit corner is not green");
    AssertColour(XMFLOAT3(0.0f, 0.0f, 1.0f), ramp.Sample(0.0f, 1.0f), L"the flat sea-level corner is not blue");
    AssertColour(XMFLOAT3(1.0f, 1.0f, 1.0f), ramp.Sample(1.0f, 1.0f), L"the cliff sea-level corner is not white");
  }

  TEST_METHOD(RowZeroIsTheSummit)
  {
    // Named on its own, because it is the one thing about a ramp that cannot be seen by looking at
    // the ramp: every file in the tree was inspected and every one puts the summit on row 0
    // (Design/Archive/PlanetRenderer.md 6.1). A ramp authored upside down should fail here rather than
    // arrive as a planet with white beaches.
    bool loaded = false;
    const Neuron::ColourRamp ramp = MakeRamp(Neuron::ColourRamp::SIDE, Neuron::ColourRamp::SIDE, loaded);

    Assert::IsTrue(ramp.Sample(0.0f, 0.0f).x > ramp.Sample(0.0f, 1.0f).x, L"v = 0 is not the top row of the image");
    Assert::IsTrue(ramp.Sample(0.0f, 1.0f).z > ramp.Sample(0.0f, 0.0f).z, L"v = 1 is not the bottom row of the image");
  }

  TEST_METHOD(TheMidpointIsTheMeanOfItsNeighbours)
  {
    // Bilinear rather than nearest, which is the one place this class deliberately differs from the
    // source's GetPixel. Half way along a row that runs red to green is half of each.
    bool loaded = false;
    const Neuron::ColourRamp ramp = MakeRamp(Neuron::ColourRamp::SIDE, Neuron::ColourRamp::SIDE, loaded);

    AssertColour(XMFLOAT3(0.5f, 0.5f, 0.0f), ramp.Sample(0.5f, 0.0f), L"the midpoint of the top row is not the mean of its ends");
  }

  TEST_METHOD(SamplingOffTheEndClamps)
  {
    // The dither pushes v past both ends of the ramp by design, and every texel it lands on has to
    // be a real one: a wrap would put summit white into the sea.
    bool loaded = false;
    const Neuron::ColourRamp ramp = MakeRamp(Neuron::ColourRamp::SIDE, Neuron::ColourRamp::SIDE, loaded);

    AssertColour(ramp.Sample(0.0f, 1.0f), ramp.Sample(-1.0f, 2.0f), L"sampling off the bottom left did not clamp");
    AssertColour(ramp.Sample(1.0f, 0.0f), ramp.Sample(9.0f, -3.0f), L"sampling off the top right did not clamp");
  }

  TEST_METHOD(AnImageOfTheWrongSizeFailsClosed)
  {
    // A ramp is 64x64 and nothing else. An author's mistake is a diagnostic and a grey planet, not a
    // half-filled table read as if it were whole.
    bool loaded = true;
    const Neuron::ColourRamp ramp = MakeRamp(32, 32, loaded);

    Assert::IsFalse(loaded, L"a 32x32 image was accepted as a ramp");
    Assert::IsFalse(ramp.Loaded(), L"a ramp that failed to load reports itself as loaded");
  }
};
} // namespace NeuronClientTests
