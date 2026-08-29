#pragma once

#include "FxVertex.h"

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>

namespace Neuron
{
// One vertex of the sky: a corner of one billboard on the celestial sphere, whether that billboard
// is a star, a bright star's flare, or a patch of nebulosity.
//
// **It carries a direction, not a position.** The quad is expanded in the vertex shader, against the
// camera's own right and up and around a center at cameraPos + dir * radius, which is what lets the
// whole sky be one static buffer uploaded once: a star does not move when the camera turns, so
// nothing about it changes per frame and the CPU has no per-frame work to do at all. Every other
// billboard in this tree is built on the CPU (SpriteParticles::Build), because every other billboard
// moves; this one is the case FxSpriteVS's comment anticipated (Design/Decisions/0021).
//
// **Packed, 28 bytes**, on the same terms FxVertex is (Design/Decisions/0019) and reusing its three
// packing rules rather than restating them -- one rounding rule per format, in integer arithmetic,
// so a vertex list is the same bytes on every machine. The direction stays float because a
// half-precision direction moves a star by up to a tenth of a degree, which at these angular sizes
// is several times the star's own width. Everything else is quantized to what it is worth: a corner
// is exactly +/-1 so SNORM16 is exact, a color is the eight bits the blend will resolve, and the
// three twinkle numbers are each a fraction of a range the frame constants carry.
struct SkyVertex
{
  float dirX, dirY, dirZ;       // world-space unit direction to the billboard's center -- R32G32B32_FLOAT
  std::int16_t cornerSnorm[2];  // the quad corner before the roll, exactly +/-1 -- R16G16_SNORM
  std::uint8_t colourUnorm[4];  // rgb the hue already multiplied by intensity, a unused -- R8G8B8A8_UNORM
  std::uint8_t twinkleUnorm[4]; // r amount, g rate as a fraction of the frame's maximum, b phase / 2pi, a unused
  std::uint16_t sizeHalf[2];    // x angular half-size in radians, y roll in radians -- R16G16_FLOAT

  // _corner is the unrolled corner and must be one of the four (+/-1, +/-1); _roll turns the quad in
  // its own plane, which is what stops a hundred copies of one cloud texture reading as a hundred
  // copies of one cloud. A star passes zero and the shader's rotation collapses to the identity.
  [[nodiscard]] static SkyVertex Make(const DirectX::XMFLOAT3& _direction, const DirectX::XMFLOAT2& _corner,
                                      const DirectX::XMFLOAT3& _colour, float _halfAngleRad, float _rollRad, float _twinkleAmount,
                                      float _twinkleRateFraction, float _twinklePhaseFraction) noexcept
  {
    SkyVertex vertex;
    vertex.dirX = _direction.x;
    vertex.dirY = _direction.y;
    vertex.dirZ = _direction.z;
    vertex.cornerSnorm[0] = FxVertex::PackSnorm16(_corner.x);
    vertex.cornerSnorm[1] = FxVertex::PackSnorm16(_corner.y);
    vertex.colourUnorm[0] = FxVertex::PackUnorm8(_colour.x);
    vertex.colourUnorm[1] = FxVertex::PackUnorm8(_colour.y);
    vertex.colourUnorm[2] = FxVertex::PackUnorm8(_colour.z);
    vertex.colourUnorm[3] = 0;
    vertex.twinkleUnorm[0] = FxVertex::PackUnorm8(_twinkleAmount);
    vertex.twinkleUnorm[1] = FxVertex::PackUnorm8(_twinkleRateFraction);
    vertex.twinkleUnorm[2] = FxVertex::PackUnorm8(_twinklePhaseFraction);
    vertex.twinkleUnorm[3] = 0;
    vertex.sizeHalf[0] = FxVertex::PackHalf(_halfAngleRad);
    vertex.sizeHalf[1] = FxVertex::PackHalf(_rollRad);
    return vertex;
  }

  // What the input assembler hands the vertex shader, for a test or a reader that wants floats.
  [[nodiscard]] DirectX::XMFLOAT3 Direction() const noexcept
  {
    return DirectX::XMFLOAT3(dirX, dirY, dirZ);
  }
  [[nodiscard]] DirectX::XMFLOAT2 Corner() const noexcept
  {
    return DirectX::XMFLOAT2(FxVertex::UnpackSnorm16(cornerSnorm[0]), FxVertex::UnpackSnorm16(cornerSnorm[1]));
  }
  [[nodiscard]] DirectX::XMFLOAT3 Colour() const noexcept
  {
    return DirectX::XMFLOAT3(FxVertex::UnpackUnorm8(colourUnorm[0]), FxVertex::UnpackUnorm8(colourUnorm[1]),
                             FxVertex::UnpackUnorm8(colourUnorm[2]));
  }
  [[nodiscard]] float HalfAngleRad() const noexcept
  {
    return FxVertex::UnpackHalf(sizeHalf[0]);
  }
  [[nodiscard]] float RollRad() const noexcept
  {
    return FxVertex::UnpackHalf(sizeHalf[1]);
  }
  [[nodiscard]] float TwinkleAmount() const noexcept
  {
    return FxVertex::UnpackUnorm8(twinkleUnorm[0]);
  }
  [[nodiscard]] float TwinkleRateFraction() const noexcept
  {
    return FxVertex::UnpackUnorm8(twinkleUnorm[1]);
  }
  [[nodiscard]] float TwinklePhaseFraction() const noexcept
  {
    return FxVertex::UnpackUnorm8(twinkleUnorm[2]);
  }
};

// SkyRenderer's input layout spells these offsets by hand, which is the only thing that could
// disagree with this struct.
static_assert(sizeof(SkyVertex) == 28, "SkyVertex is padded; the sky input layout's offsets are wrong");
static_assert(offsetof(SkyVertex, cornerSnorm) == 12 && offsetof(SkyVertex, colourUnorm) == 16 && offsetof(SkyVertex, twinkleUnorm) == 20 &&
                offsetof(SkyVertex, sizeHalf) == 24,
              "SkyVertex's fields moved; the sky input layout spells 12, 16, 20 and 24");
} // namespace Neuron
