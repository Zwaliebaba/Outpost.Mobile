#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
// The camera basis used throughout: right along +X, up along +Y. A billboard built with these lies
// in the XY plane, which makes every expected corner readable as a pair of numbers.
constexpr XMFLOAT3 RIGHT{1.0f, 0.0f, 0.0f};
constexpr XMFLOAT3 UP{0.0f, 1.0f, 0.0f};

[[nodiscard]] Neuron::GlowSample Glow(float _x, float _y, float _z, float _radius, float _alpha = 1.0f)
{
  return Neuron::GlowSample{.posWorld = XMFLOAT3(_x, _y, _z), .radiusMetres = _radius, .colour = Neuron::Rgba{1.0f, 1.0f, 1.0f, _alpha}};
}

[[nodiscard]] bool NearlyEqual(float _a, float _b, float _tolerance = 1e-4f) noexcept
{
  return std::fabs(_a - _b) <= _tolerance;
}
} // namespace

TEST_CLASS(GlowBillboardsTests)
{
public:
  TEST_METHOD(EachSampleBecomesTwoTrianglesOfOneQuad)
  {
    std::vector<Neuron::FxVertex> verts;
    const Neuron::GlowSample samples[] = {Glow(0.0f, 0.0f, 0.0f, 1.0f), Glow(10.0f, 0.0f, 0.0f, 2.0f)};
    Neuron::BuildGlowBillboards(samples, RIGHT, UP, verts);

    Assert::AreEqual(size_t{2} * Neuron::GLOW_VERTS_PER_SAMPLE, verts.size(), L"a sample is not six vertices");

    // The two triangles share the diagonal 0-2, so exactly four distinct positions are used and two
    // of them appear twice. That is what makes it one quad rather than two loose triangles.
    int shared = 0;
    for (std::uint32_t a = 0; a < Neuron::GLOW_VERTS_PER_SAMPLE; ++a)
      for (std::uint32_t b = a + 1; b < Neuron::GLOW_VERTS_PER_SAMPLE; ++b)
      {
        const XMFLOAT3 one = verts[a].Position();
        const XMFLOAT3 other = verts[b].Position();
        if (NearlyEqual(one.x, other.x) && NearlyEqual(one.y, other.y) && NearlyEqual(one.z, other.z))
          ++shared;
      }
    Assert::AreEqual(2, shared, L"the two triangles do not share exactly the quad's diagonal");
  }

  TEST_METHOD(TheCornersSitAtOneRadiusAlongTheCameraBasis)
  {
    // The retired path put cameraRight * radius * 2 and cameraUp * radius * 2 in a matrix that a
    // unit quad spanning +-0.5 was drawn through, so a corner landed one radius out along each axis.
    // These corners have to be those corners, or the plume changes size.
    std::vector<Neuron::FxVertex> verts;
    const Neuron::GlowSample samples[] = {Glow(5.0f, 7.0f, -3.0f, 4.0f)};
    Neuron::BuildGlowBillboards(samples, RIGHT, UP, verts);

    for (const Neuron::FxVertex& vertex : verts)
    {
      const XMFLOAT3 position = vertex.Position();
      Assert::IsTrue(NearlyEqual(std::fabs(position.x - 5.0f), 4.0f), L"a corner is not one radius out along right");
      Assert::IsTrue(NearlyEqual(std::fabs(position.y - 7.0f), 4.0f), L"a corner is not one radius out along up");
      Assert::IsTrue(NearlyEqual(position.z, -3.0f), L"a corner left the billboard plane");
    }
  }

  TEST_METHOD(TheUvIsTheCornerTheShaderMeasuresFrom)
  {
    // FxGlowPS computes length(uv) and fades over [0, 1]. That only reproduces DecalVS's
    // `i.pos.xz * 2.0` if each corner carries its own sign pair, so this is the contract between the
    // builder and the shader and it is worth pinning.
    std::vector<Neuron::FxVertex> verts;
    const Neuron::GlowSample samples[] = {Glow(0.0f, 0.0f, 0.0f, 3.0f)};
    Neuron::BuildGlowBillboards(samples, RIGHT, UP, verts);

    for (const Neuron::FxVertex& vertex : verts)
    {
      const XMFLOAT2 uv = vertex.Uv();
      const XMFLOAT3 position = vertex.Position();
      Assert::IsTrue(NearlyEqual(std::fabs(uv.x), 1.0f), L"the uv's x is not a corner sign");
      Assert::IsTrue(NearlyEqual(std::fabs(uv.y), 1.0f), L"the uv's y is not a corner sign");
      // And it is *this* corner: the sign has to follow the offset, or the disc is mirrored.
      Assert::IsTrue(NearlyEqual(uv.x * 3.0f, position.x), L"the uv's x does not name the corner it is on");
      Assert::IsTrue(NearlyEqual(uv.y * 3.0f, position.y), L"the uv's y does not name the corner it is on");
    }
  }

  TEST_METHOD(ABillboardTurnsWithTheCamera)
  {
    // The whole point of building on the CPU: the quad faces wherever the eye is. With the basis
    // rotated a quarter turn, the quad lies in ZY instead of XY.
    std::vector<Neuron::FxVertex> verts;
    const Neuron::GlowSample samples[] = {Glow(0.0f, 0.0f, 0.0f, 1.0f)};
    Neuron::BuildGlowBillboards(samples, XMFLOAT3(0.0f, 0.0f, 1.0f), UP, verts);

    for (const Neuron::FxVertex& vertex : verts)
    {
      const XMFLOAT3 position = vertex.Position();
      Assert::IsTrue(NearlyEqual(position.x, 0.0f), L"the quad did not turn with the camera's right");
      Assert::IsTrue(NearlyEqual(std::fabs(position.z), 1.0f), L"the quad is not one radius out along the new right");
    }
  }

  TEST_METHOD(NothingIsBuiltForAGlowThatWouldNotDraw)
  {
    // SceneRenderer::DrawGlow returned early on both of these, so neither reached the GPU before.
    // Keeping that here is what stops the ring filling with quads that draw nothing.
    std::vector<Neuron::FxVertex> verts;
    const Neuron::GlowSample samples[] = {Glow(0.0f, 0.0f, 0.0f, 1.0f, 0.0f), Glow(0.0f, 0.0f, 0.0f, 0.0f, 1.0f),
                                          Glow(0.0f, 0.0f, 0.0f, 1.0f, 1.0f)};
    Neuron::BuildGlowBillboards(samples, RIGHT, UP, verts);
    Assert::AreEqual(size_t{Neuron::GLOW_VERTS_PER_SAMPLE}, verts.size(), L"a glow with no alpha or no radius was built");
  }

  TEST_METHOD(BuildingAppendsRatherThanReplacing)
  {
    // WorldView clears once and builds once, but the ring pattern is append-shaped and a caller that
    // batches two sources into one draw must not lose the first.
    std::vector<Neuron::FxVertex> verts;
    const Neuron::GlowSample first[] = {Glow(0.0f, 0.0f, 0.0f, 1.0f)};
    const Neuron::GlowSample second[] = {Glow(20.0f, 0.0f, 0.0f, 1.0f)};
    Neuron::BuildGlowBillboards(first, RIGHT, UP, verts);
    Neuron::BuildGlowBillboards(second, RIGHT, UP, verts);

    Assert::AreEqual(size_t{2} * Neuron::GLOW_VERTS_PER_SAMPLE, verts.size(), L"the second build did not append");
    Assert::IsTrue(NearlyEqual(verts[0].Position().x, -1.0f), L"the first build's vertices were overwritten");
    Assert::IsTrue(NearlyEqual(verts[Neuron::GLOW_VERTS_PER_SAMPLE].Position().x, 19.0f), L"the second build landed in the wrong place");
  }

  TEST_METHOD(AnEmptyPlumeBuildsNothing)
  {
    std::vector<Neuron::FxVertex> verts;
    Neuron::BuildGlowBillboards({}, RIGHT, UP, verts);
    Assert::IsTrue(verts.empty(), L"an empty sample list produced vertices");
  }
};
} // namespace NeuronClientTests
