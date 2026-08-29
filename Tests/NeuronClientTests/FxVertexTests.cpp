#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
// The packing in FxVertex.h is spelled by hand so that it is the same on every machine, and these
// pin it: the three rounding rules, the round trips, and the bytes of one vertex. The compute bake
// (Shaders/BodyBake.hlsli) writes the same bytes with HLSL's round() and f32tof16(), and its
// readback comparison is only meaningful if this side holds still.
TEST_CLASS(FxVertexTests)
{
public:
  TEST_METHOD(TheStructIsTwentyEightBytesInTheInputLayoutsOrder)
  {
    Assert::AreEqual(static_cast<std::size_t>(28), sizeof(Neuron::FxVertex), L"the vertex is not the size the input layouts assume");
    Assert::AreEqual(static_cast<std::size_t>(12), offsetof(Neuron::FxVertex, normalSnorm), L"the normal is not at offset 12");
    Assert::AreEqual(static_cast<std::size_t>(20), offsetof(Neuron::FxVertex, colourUnorm), L"the colour is not at offset 20");
    Assert::AreEqual(static_cast<std::size_t>(24), offsetof(Neuron::FxVertex, uvHalf), L"the uv is not at offset 24");
  }

  TEST_METHOD(Unorm8RoundsHalfUpAndSaturates)
  {
    Assert::AreEqual<int>(0, Neuron::FxVertex::PackUnorm8(-0.5f), L"below zero is not clamped to zero");
    Assert::AreEqual<int>(255, Neuron::FxVertex::PackUnorm8(1.5f), L"above one is not clamped to 255");
    Assert::AreEqual<int>(128, Neuron::FxVertex::PackUnorm8(0.5f), L"0.5 * 255 = 127.5 must round up to 128");
    Assert::AreEqual<int>(45, Neuron::FxVertex::PackUnorm8(45.0f / 255.0f), L"a value that is a whole step does not round trip");
    for (int i = 0; i < 256; ++i)
    {
      const std::uint8_t byte = static_cast<std::uint8_t>(i);
      Assert::AreEqual<int>(i, Neuron::FxVertex::PackUnorm8(Neuron::FxVertex::UnpackUnorm8(byte)), L"a byte does not round trip");
    }
  }

  TEST_METHOD(Snorm16RoundsHalvesAwayFromZeroAndClampsBothEnds)
  {
    Assert::AreEqual<int>(32767, Neuron::FxVertex::PackSnorm16(1.0f), L"one is not the top code");
    Assert::AreEqual<int>(-32767, Neuron::FxVertex::PackSnorm16(-1.0f), L"minus one is not the bottom code");
    Assert::AreEqual<int>(32767, Neuron::FxVertex::PackSnorm16(3.0f), L"above one is not clamped");
    Assert::AreEqual<int>(0, Neuron::FxVertex::PackSnorm16(0.0f), L"zero is not zero");
    Assert::AreEqual<int>(1, Neuron::FxVertex::PackSnorm16(0.5f / 32767.0f), L"a positive half does not round away from zero");
    Assert::AreEqual<int>(-1, Neuron::FxVertex::PackSnorm16(-0.5f / 32767.0f), L"a negative half does not round away from zero");
    Assert::AreEqual(-1.0f, Neuron::FxVertex::UnpackSnorm16(-32768), L"the spare bottom code does not decode to minus one");
    for (int i = -32767; i <= 32767; i += 97)
    {
      const std::int16_t code = static_cast<std::int16_t>(i);
      Assert::AreEqual<int>(i, Neuron::FxVertex::PackSnorm16(Neuron::FxVertex::UnpackSnorm16(code)), L"a code does not round trip");
    }
  }

  TEST_METHOD(HalfIsIeeeRoundToNearestEven)
  {
    // The known bit patterns: 0, 1, -2, the largest finite half, a subnormal, and the two sides
    // of a tie.
    Assert::AreEqual<int>(0x0000, Neuron::FxVertex::PackHalf(0.0f), L"zero");
    Assert::AreEqual<int>(0x3c00, Neuron::FxVertex::PackHalf(1.0f), L"one");
    Assert::AreEqual<int>(0xc000, Neuron::FxVertex::PackHalf(-2.0f), L"minus two");
    Assert::AreEqual<int>(0x7bff, Neuron::FxVertex::PackHalf(65504.0f), L"the largest finite half");
    Assert::AreEqual<int>(0x7c00, Neuron::FxVertex::PackHalf(70000.0f), L"overflow is infinity");
    Assert::AreEqual<int>(0x0001, Neuron::FxVertex::PackHalf(5.960464477539063e-8f), L"the smallest subnormal");
    // 1 + 2^-11 sits exactly between 1.0 (0x3c00) and the next half (0x3c01); even wins.
    Assert::AreEqual<int>(0x3c00, Neuron::FxVertex::PackHalf(1.0f + 1.0f / 2048.0f), L"a tie must round to even (down)");
    // 1 + 3 * 2^-11 sits between 0x3c01 and 0x3c02; even is 0x3c02.
    Assert::AreEqual<int>(0x3c02, Neuron::FxVertex::PackHalf(1.0f + 3.0f / 2048.0f), L"a tie must round to even (up)");

    // Every integer a uv can be -- a cell index up to 2 048 -- is exact, and so are the corners.
    for (int i = 0; i <= 2048; ++i)
      Assert::AreEqual(static_cast<float>(i), Neuron::FxVertex::UnpackHalf(Neuron::FxVertex::PackHalf(static_cast<float>(i))),
                       L"an integer uv does not round trip");
    for (int i = 0; i < 0x7c00; i += 13)
    {
      const std::uint16_t half = static_cast<std::uint16_t>(i);
      Assert::AreEqual<int>(i, Neuron::FxVertex::PackHalf(Neuron::FxVertex::UnpackHalf(half)), L"a half does not round trip");
    }
  }

  TEST_METHOD(MakeWritesTheBytesTheInputAssemblerReads)
  {
    const Neuron::FxVertex vertex = Neuron::FxVertex::Make(XMFLOAT3(1.5f, -2.0f, 3.25f), XMFLOAT3(0.0f, 1.0f, 0.0f),
                                                           XMFLOAT4(1.0f, 0.5f, 0.0f, 0.25f), XMFLOAT2(3.0f, 1.0f));

    Assert::AreEqual(1.5f, vertex.px);
    Assert::AreEqual(-2.0f, vertex.py);
    Assert::AreEqual(3.25f, vertex.pz);
    Assert::AreEqual<int>(0, vertex.normalSnorm[0]);
    Assert::AreEqual<int>(32767, vertex.normalSnorm[1]);
    Assert::AreEqual<int>(0, vertex.normalSnorm[2]);
    Assert::AreEqual<int>(0, vertex.normalSnorm[3]);
    Assert::AreEqual<int>(255, vertex.colourUnorm[0]);
    Assert::AreEqual<int>(128, vertex.colourUnorm[1]);
    Assert::AreEqual<int>(0, vertex.colourUnorm[2]);
    Assert::AreEqual<int>(64, vertex.colourUnorm[3]);
    Assert::AreEqual<int>(0x4200, vertex.uvHalf[0]);
    Assert::AreEqual<int>(0x3c00, vertex.uvHalf[1]);

    const XMFLOAT3 normal = vertex.Normal();
    Assert::AreEqual(1.0f, normal.y, L"the packed normal does not decode to one");
    const XMFLOAT4 colour = vertex.Colour();
    Assert::AreEqual(128.0f / 255.0f, colour.y, 1e-6f, L"the packed colour does not decode to its byte");
    const XMFLOAT2 uv = vertex.Uv();
    Assert::AreEqual(3.0f, uv.x);
    Assert::AreEqual(1.0f, uv.y);
  }
};
} // namespace NeuronClientTests
