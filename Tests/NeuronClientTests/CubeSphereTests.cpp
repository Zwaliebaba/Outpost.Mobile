#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
// Evaluated at compile time, which is half of what this pins: Direction has to be usable in a
// constant expression, and it is only usable in one if the polynomial tan and the Newton sqrt beside
// it are too. The other half is the face convention -- PosY's (0, 0) is the cube corner (-1, 1, -1) --
// which every other face's orientation is defined against.
constexpr XMFLOAT3 POS_Y_CORNER = Neuron::CubeSphere::Direction(Neuron::CubeFace::PosY, 0, 0, 3);
static_assert(POS_Y_CORNER.y == 0.57735026f, "the corner of PosY is no longer one over root three");
static_assert(POS_Y_CORNER.x == -POS_Y_CORNER.y, "the corner of PosY is no longer normalize(-1, 1, -1)");
static_assert(POS_Y_CORNER.z == -POS_Y_CORNER.y, "the corner of PosY is no longer normalize(-1, 1, -1)");
static_assert(Neuron::CubeSphere::SamplesPerSide(6) == 65u, "a grid power of six is no longer sixty-five samples a side");

[[nodiscard]] float Dot(const XMFLOAT3& _a, const XMFLOAT3& _b)
{
  return _a.x * _b.x + _a.y * _b.y + _a.z * _b.z;
}

[[nodiscard]] bool Identical(const XMFLOAT3& _a, const XMFLOAT3& _b)
{
  return _a.x == _b.x && _a.y == _b.y && _a.z == _b.z;
}

// The area of the cell whose corner is (x, z), as the parallelogram of its two edges. Approximate,
// and equally approximate at both ends of the comparison it is used for.
[[nodiscard]] float CellArea(std::uint32_t _x, std::uint32_t _z, std::uint32_t _samplesPerSide)
{
  const XMFLOAT3 origin = Neuron::CubeSphere::Direction(Neuron::CubeFace::PosY, _x, _z, _samplesPerSide);
  const XMFLOAT3 alongX = Neuron::CubeSphere::Direction(Neuron::CubeFace::PosY, _x + 1, _z, _samplesPerSide);
  const XMFLOAT3 alongZ = Neuron::CubeSphere::Direction(Neuron::CubeFace::PosY, _x, _z + 1, _samplesPerSide);

  const XMVECTOR edgeX = XMVectorSubtract(XMLoadFloat3(&alongX), XMLoadFloat3(&origin));
  const XMVECTOR edgeZ = XMVectorSubtract(XMLoadFloat3(&alongZ), XMLoadFloat3(&origin));
  return XMVectorGetX(XMVector3Length(XMVector3Cross(edgeX, edgeZ)));
}
} // namespace

TEST_CLASS(CubeSphereTests)
{
public:
  TEST_METHOD(EveryDirectionIsUnitLength)
  {
    // The height field is evaluated from the direction and the vertex is placed along it, so a
    // direction that is not unit length is a vertex at the wrong radius -- a dent in the sphere that
    // no amount of staring at the noise would explain.
    const std::uint32_t samples = Neuron::CubeSphere::SamplesPerSide(4);

    float worst = 0.0f;
    for (std::uint32_t face = 0; face < Neuron::CUBE_FACE_COUNT; ++face)
    {
      for (std::uint32_t x = 0; x < samples; ++x)
      {
        for (std::uint32_t z = 0; z < samples; ++z)
        {
          const XMFLOAT3 direction = Neuron::CubeSphere::Direction(static_cast<Neuron::CubeFace>(face), x, z, samples);
          worst = std::max(worst, std::fabs(std::sqrt(Dot(direction, direction)) - 1.0f));
        }
      }
    }

    Assert::IsTrue(worst < 1e-6f, L"a direction is not unit length");
  }

  TEST_METHOD(AdjacentFacesShareTheirEdgeSamplesToTheBit)
  {
    // This is what makes the sphere seamless, and it is a bit-for-bit claim rather than a tolerance
    // one: the height is a function of the direction, so two faces that agree exactly on the
    // direction cannot disagree at all on the height, and any tolerance here would let a seam of one
    // last bit through (Design/Archive/PlanetRenderer.md 5.1).
    //
    // The check is the count rather than an index-by-index walk, because two faces traverse their
    // shared edge in whichever directions their own orientations give them. A shared edge is n
    // samples; there is nothing else two faces can have in common, and opposite faces have nothing.
    const std::uint32_t samples = Neuron::CubeSphere::SamplesPerSide(5);

    std::vector<XMFLOAT3> boundary[Neuron::CUBE_FACE_COUNT];
    for (std::uint32_t face = 0; face < Neuron::CUBE_FACE_COUNT; ++face)
    {
      for (std::uint32_t x = 0; x < samples; ++x)
      {
        for (std::uint32_t z = 0; z < samples; ++z)
        {
          if (x == 0 || z == 0 || x == samples - 1 || z == samples - 1)
            boundary[face].push_back(Neuron::CubeSphere::Direction(static_cast<Neuron::CubeFace>(face), x, z, samples));
        }
      }
    }

    for (std::uint32_t a = 0; a < Neuron::CUBE_FACE_COUNT; ++a)
    {
      for (std::uint32_t b = a + 1; b < Neuron::CUBE_FACE_COUNT; ++b)
      {
        std::uint32_t shared = 0;
        for (const XMFLOAT3& first : boundary[a])
        {
          for (const XMFLOAT3& second : boundary[b])
          {
            if (Identical(first, second))
              ++shared;
          }
        }

        // The enumeration pairs the two faces of an axis, so a / 2 == b / 2 is exactly "opposite".
        const std::uint32_t expected = (a / 2 == b / 2) ? 0u : samples;
        Assert::AreEqual(expected, shared, L"two faces do not share exactly the samples along the edge between them");
      }
    }
  }

  TEST_METHOD(TheWarpEvensOutTheCellAreas)
  {
    // Without the tangent warp a face's corner cell is a fifth of the area of its centre cell -- the
    // measured figure is 0.20 -- which puts five times the triangles on the middle of a face as on
    // its corner and shows up as a visible change of detail across a planet. With it the smaller cell
    // is three quarters of the larger, which is 0.75 measured and is what the 0.70 below allows for.
    const std::uint32_t samples = Neuron::CubeSphere::SamplesPerSide(5);
    const float centre = CellArea(samples / 2, samples / 2, samples);
    const float corner = CellArea(0, 0, samples);

    Assert::IsTrue(centre > 0.0f && corner > 0.0f, L"a cell has no area");
    Assert::IsTrue(std::min(centre, corner) / std::max(centre, corner) > 0.70f,
                   L"the corner and centre cells of a face are more than thirty per cent apart");
  }

  TEST_METHOD(TheConstexprTanIsTanToASixthDecimal)
  {
    // Direction cannot call std::tan, which is not constexpr at this language level, so it carries a
    // degree-eleven odd polynomial of its own. The polynomial is recoverable through the public
    // interface: a PosX direction is Normalise(1, v, u), so u is d.z / d.x.
    const std::uint32_t samples = Neuron::CubeSphere::SamplesPerSide(5);
    const float span = static_cast<float>(samples - 1);

    float worst = 0.0f;
    for (std::uint32_t x = 0; x < samples; ++x)
    {
      const XMFLOAT3 direction = Neuron::CubeSphere::Direction(Neuron::CubeFace::PosX, x, 0, samples);
      const float inFace = (2.0f * static_cast<float>(x) - span) / span;
      worst = std::max(worst, std::fabs(direction.z / direction.x - std::tan(inFace * XM_PIDIV4)));
    }

    Assert::IsTrue(worst < 1e-6f, L"the polynomial tan has drifted away from std::tan");
  }

  TEST_METHOD(TheEndsOfAFaceLandExactlyOnTheCube)
  {
    // The polynomial is out by about 1e-7 at pi/4, and one bit is one seam: the ends are returned as
    // exactly plus and minus one instead, which is what the sharing test above rests on. Checked on
    // the cube point rather than on the warp, because that is where it matters.
    const std::uint32_t samples = Neuron::CubeSphere::SamplesPerSide(3);
    const XMFLOAT3 corner = Neuron::CubeSphere::Direction(Neuron::CubeFace::PosZ, 0, samples - 1, samples);

    Assert::AreEqual(-corner.z, corner.x, L"a face corner is not on the cube's diagonal");
    Assert::AreEqual(corner.z, corner.y, L"a face corner is not on the cube's diagonal");
  }
};
} // namespace NeuronClientTests
