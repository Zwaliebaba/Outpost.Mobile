#pragma once

#include <DirectXMath.h>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace Neuron
{
// One vertex format for the effect passes -- tumbling hull fragments and camera-facing sprites --
// and for a body's terrain. Colour and alpha arrive already curved: the fade over a fragment's life
// and the zero alpha the darkening sprite blend needs are computed on the CPU, so the shaders do no
// fading and differ only in how they sample their texture.
//
// A sprite writes a zero normal and the sprite shader never reads it. Carrying the field anyway is
// what lets one input layout, one vertex buffer and one ring serve both passes.
//
// **Packed, 28 bytes.** The position stays float, because a planet is kilometres across and its
// facets are metres. Everything else is quantised to what it is worth: a per-triangle normal to
// 16-bit SNORM (a step of 3e-5, under any lighting difference a pixel can show), a colour to the
// eight bits the ramp it was read from has, and a uv to half precision, where a cell index up to
// 2 048 and a 0/1 corner are both exact. The input assembler expands them for free, so the vertex
// shaders read float3 / float4 / float2 as they always did, and what changed is that a 65-grid
// planet is 4.1 MB rather than 7.1 and the effect ring writes 1.3 MB a frame rather than 2.25
// (Design/Decisions/0018).
//
// The packing is spelled here, in integer arithmetic with one stated rounding rule each, and not
// through a library: BodyMeshTests pins the whole vertex list byte for byte on every machine, and a
// library's F16C path and its software path are two answers. The compute bake mirrors these three
// rules in BodyBake.hlsli, which is what lets its output be compared to this one.
struct FxVertex
{
  float px, py, pz;            // world (or object) position -- R32G32B32_FLOAT
  std::int16_t normalSnorm[4]; // normal, w unused -- R16G16B16A16_SNORM
  std::uint8_t colourUnorm[4]; // r, g, b, a -- R8G8B8A8_UNORM
  std::uint16_t uvHalf[2];     // u, v -- R16G16_FLOAT

  [[nodiscard]] static FxVertex Make(const DirectX::XMFLOAT3& _position, const DirectX::XMFLOAT3& _normal, const DirectX::XMFLOAT4& _colour,
                                     const DirectX::XMFLOAT2& _uv) noexcept
  {
    FxVertex vertex;
    vertex.px = _position.x;
    vertex.py = _position.y;
    vertex.pz = _position.z;
    vertex.normalSnorm[0] = PackSnorm16(_normal.x);
    vertex.normalSnorm[1] = PackSnorm16(_normal.y);
    vertex.normalSnorm[2] = PackSnorm16(_normal.z);
    vertex.normalSnorm[3] = 0;
    vertex.colourUnorm[0] = PackUnorm8(_colour.x);
    vertex.colourUnorm[1] = PackUnorm8(_colour.y);
    vertex.colourUnorm[2] = PackUnorm8(_colour.z);
    vertex.colourUnorm[3] = PackUnorm8(_colour.w);
    vertex.uvHalf[0] = PackHalf(_uv.x);
    vertex.uvHalf[1] = PackHalf(_uv.y);
    return vertex;
  }

  // What the input assembler hands the vertex shader, for a test or a reader that wants floats.
  [[nodiscard]] DirectX::XMFLOAT3 Position() const noexcept
  {
    return DirectX::XMFLOAT3(px, py, pz);
  }
  [[nodiscard]] DirectX::XMFLOAT3 Normal() const noexcept
  {
    return DirectX::XMFLOAT3(UnpackSnorm16(normalSnorm[0]), UnpackSnorm16(normalSnorm[1]), UnpackSnorm16(normalSnorm[2]));
  }
  [[nodiscard]] DirectX::XMFLOAT4 Colour() const noexcept
  {
    return DirectX::XMFLOAT4(UnpackUnorm8(colourUnorm[0]), UnpackUnorm8(colourUnorm[1]), UnpackUnorm8(colourUnorm[2]),
                             UnpackUnorm8(colourUnorm[3]));
  }
  [[nodiscard]] DirectX::XMFLOAT2 Uv() const noexcept
  {
    return DirectX::XMFLOAT2(UnpackHalf(uvHalf[0]), UnpackHalf(uvHalf[1]));
  }

  // UNORM8: saturate, scale, round half up. Half up rather than half to even because it is one
  // comparison and because HLSL's round() -- which BodyBake.hlsli uses for the same byte -- rounds
  // halves away from zero, which on a non-negative value is the same thing.
  [[nodiscard]] static constexpr std::uint8_t PackUnorm8(float _value) noexcept
  {
    const float clamped = (_value < 0.0f) ? 0.0f : ((_value > 1.0f) ? 1.0f : _value);
    return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
  }
  [[nodiscard]] static constexpr float UnpackUnorm8(std::uint8_t _value) noexcept
  {
    return static_cast<float>(_value) * (1.0f / 255.0f);
  }

  // SNORM16: clamp to [-1, 1], scale by 32767, round halves away from zero -- HLSL's round().
  [[nodiscard]] static constexpr std::int16_t PackSnorm16(float _value) noexcept
  {
    const float clamped = (_value < -1.0f) ? -1.0f : ((_value > 1.0f) ? 1.0f : _value);
    const float scaled = clamped * 32767.0f;
    return static_cast<std::int16_t>((scaled >= 0.0f) ? static_cast<int>(scaled + 0.5f) : -static_cast<int>(-scaled + 0.5f));
  }
  // -32768 decodes to -1 as well, as D3D specifies, so every 16-bit pattern is a legal normal.
  [[nodiscard]] static constexpr float UnpackSnorm16(std::int16_t _value) noexcept
  {
    const float value = static_cast<float>(_value) * (1.0f / 32767.0f);
    return (value < -1.0f) ? -1.0f : value;
  }

  // IEEE half, round to nearest even, which is what f32tof16 does on every D3D12 device. Written
  // out on the bits rather than through DirectXMath, whose conversion takes the F16C path on one
  // machine and a software path on another.
  [[nodiscard]] static constexpr std::uint16_t PackHalf(float _value) noexcept
  {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(_value);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23u) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu) // infinity or NaN, kept as such
      return static_cast<std::uint16_t>(sign | 0x7c00u | (mantissa != 0u ? 0x200u : 0u));

    const int halfExponent = static_cast<int>(exponent) - 127 + 15;
    if (halfExponent >= 31) // too large: infinity
      return static_cast<std::uint16_t>(sign | 0x7c00u);

    if (halfExponent <= 0) // subnormal in half, or zero
    {
      if (halfExponent < -10)
        return static_cast<std::uint16_t>(sign);

      mantissa |= 0x800000u;
      const std::uint32_t shift = static_cast<std::uint32_t>(14 - halfExponent);
      std::uint32_t half = mantissa >> shift;
      const std::uint32_t remainder = mantissa & ((1u << shift) - 1u);
      const std::uint32_t halfway = 1u << (shift - 1u);
      if (remainder > halfway || (remainder == halfway && (half & 1u) != 0u))
        ++half;
      return static_cast<std::uint16_t>(sign | half);
    }

    std::uint32_t half = sign | (static_cast<std::uint32_t>(halfExponent) << 10u) | (mantissa >> 13u);
    const std::uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0u))
      ++half; // a carry out of the mantissa lands in the exponent, which is the right answer
    return static_cast<std::uint16_t>(half);
  }
  [[nodiscard]] static constexpr float UnpackHalf(std::uint16_t _value) noexcept
  {
    const std::uint32_t sign = (static_cast<std::uint32_t>(_value) & 0x8000u) << 16u;
    std::uint32_t exponent = (_value >> 10u) & 0x1fu;
    std::uint32_t mantissa = _value & 0x3ffu;

    if (exponent == 0u)
    {
      if (mantissa == 0u)
        return std::bit_cast<float>(sign);

      // A subnormal half is a normal float: shift the leading one up into place.
      exponent = 1u;
      while ((mantissa & 0x400u) == 0u)
      {
        mantissa <<= 1u;
        --exponent;
      }
      mantissa &= 0x3ffu;
    }
    else if (exponent == 31u)
    {
      return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13u));
    }

    return std::bit_cast<float>(sign | ((exponent + 112u) << 23u) | (mantissa << 13u));
  }
};

// The input layouts in FxRenderer and BodyRenderer spell these offsets by hand, which is the only
// thing that could disagree with this struct.
static_assert(sizeof(FxVertex) == 28, "FxVertex is padded; the effect and body input layouts' offsets are wrong");
static_assert(offsetof(FxVertex, normalSnorm) == 12 && offsetof(FxVertex, colourUnorm) == 20 && offsetof(FxVertex, uvHalf) == 24,
              "FxVertex's fields moved; the input layouts spell 12, 20 and 24");
} // namespace Neuron
