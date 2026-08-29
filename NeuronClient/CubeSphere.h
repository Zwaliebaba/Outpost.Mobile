#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace Neuron
{
enum class CubeFace : std::uint8_t
{
  PosX,
  NegX,
  PosY,
  NegY,
  PosZ,
  NegZ
};

inline constexpr std::uint32_t CUBE_FACE_COUNT = 6;

// The six-face grid a body is sampled on: sample (face, x, z) to a unit direction, with the usual
// tangent warp so the cells are close to equal area (Design/PlanetRenderer.md 5.1).
//
// A cube-sphere rather than a latitude/longitude sphere because the grid stays regular -- the
// neighbour rule for sea-level culling, one outline quad per cell and the per-cell dither seed all
// port face by face, with no pole to special-case.
//
// The two entry points are static members of an empty struct rather than free functions, which is
// what the design already calls them. This tree keeps three flat namespaces (AGENTS.md R9) and every
// one of them is pulled into scope by an umbrella header, so a free Direction() would be a global
// name in every translation unit in the solution; CubeSphere::Direction collides with nothing.
struct CubeSphere
{
  // N = 2^gridPower + 1 samples along each side of a face: a power of two cells, plus the shared
  // sample that closes the last one.
  [[nodiscard]] static constexpr std::uint32_t SamplesPerSide(std::uint32_t _gridPower) noexcept
  {
    return (1u << _gridPower) + 1u;
  }

  // The unit direction of sample (x, z) on a face, 0 <= x, z < _samplesPerSide.
  //
  // An edge sample of one face is the same direction as the matching edge sample of its neighbour,
  // to the bit, and that is what makes the height field seamless: h is evaluated from the direction,
  // so two faces that agree on the direction cannot disagree on the height. It holds by construction
  // rather than by luck. The in-face coordinate s = (2x - (n-1)) / (n-1) is exactly -1 at x = 0 and
  // exactly +1 at x = n-1; the warp is odd, so the set of warped coordinates is closed under
  // negation to the bit; and every face is written as "the two free coordinates range over that set,
  // the third is +-1". The edge samples of any two faces are therefore the same points. The test in
  // CubeSphereTests pins it; if it fails, one of those three properties moved.
  //
  // Opposite faces wind opposite ways under this scheme -- a quad on PosY and the same quad on NegY
  // cross-product to +Y and -Y respectively. That is deliberate and costs nothing: the mesh builder
  // flips a triangle normal to face outward anyway (Design/PlanetRenderer.md 8.2), and buying
  // consistent winding here would mean mirroring three of the six faces and losing the edge identity
  // above, which is the property that actually matters.
  [[nodiscard]] static constexpr DirectX::XMFLOAT3 Direction(CubeFace _face, std::uint32_t _x, std::uint32_t _z,
                                                             std::uint32_t _samplesPerSide) noexcept
  {
    const float u = Warp(InFace(_x, _samplesPerSide));
    const float v = Warp(InFace(_z, _samplesPerSide));

    // The face's two free coordinates take u and v in the world-axis order that skips the face's own
    // axis, so PosY's corner (0, 0) is the cube corner (-1, 1, -1).
    switch (_face)
    {
    case CubeFace::PosX:
      return Normalise(1.0f, v, u);
    case CubeFace::NegX:
      return Normalise(-1.0f, v, u);
    case CubeFace::PosY:
      return Normalise(u, 1.0f, v);
    case CubeFace::NegY:
      return Normalise(u, -1.0f, v);
    case CubeFace::PosZ:
      return Normalise(u, v, 1.0f);
    case CubeFace::NegZ:
    default:
      return Normalise(u, v, -1.0f);
    }
  }

private:
  static constexpr float PI_OVER_4 = 0.78539816339744830961f;

  // s = (2x - (n-1)) / (n-1), in this order and no other: it is exactly -1 and exactly +1 at the two
  // ends whatever n is, and s(i) is the exact negation of s(n-1-i), which is half of why two faces
  // agree along an edge.
  [[nodiscard]] static constexpr float InFace(std::uint32_t _index, std::uint32_t _samplesPerSide) noexcept
  {
    if (_samplesPerSide < 2u)
      return 0.0f;

    const float span = static_cast<float>(_samplesPerSide - 1u);
    return (2.0f * static_cast<float>(_index) - span) / span;
  }

  // tan(s * pi/4) for s in [-1, 1], the equal-area correction: without it a face's corner cell is
  // about 2.6 times the area of its centre cell.
  //
  // The two ends are returned as exactly +-1 rather than as the polynomial's answer for them. An
  // edge sample has to land on the cube's edge to the last bit or the two faces that share it
  // disagree and the sphere has a seam; the polynomial is out by about 1e-7 there, which is one bit
  // of a float and one seam.
  [[nodiscard]] static constexpr float Warp(float _s) noexcept
  {
    if (_s >= 1.0f)
      return 1.0f;
    if (_s <= -1.0f)
      return -1.0f;
    return Tan(_s * PI_OVER_4);
  }

  // tan on [-pi/4, pi/4] as a **degree-11 odd** polynomial -- six terms, Remez-fitted, maximum
  // absolute error 1.1e-7. std::tan is not constexpr at this language level and Direction has to be,
  // so the function is written out; state the degree here so that nobody "improves" it into a
  // different function and moves every pinned height in the tests. (The work order's degree 9 fits
  // to 1.5e-6, just over the 1e-6 it assumed, so one term was added.)
  //
  // Odd in form as well as in value: the coefficients multiply powers of t^2 and the result is
  // multiplied by t once at the end, so Tan(-t) is the exact negation of Tan(t) rather than merely
  // its neighbour. The edge identity above rests on that.
  [[nodiscard]] static constexpr float Tan(float _t) noexcept
  {
    const float t2 = _t * _t;
    float p = 0.0220568028f;
    p = p * t2 + 0.0097162480f;
    p = p * t2 + 0.0589079198f;
    p = p * t2 + 0.1323876022f;
    p = p * t2 + 0.3334098470f;
    p = p * t2 + 0.9999982415f;
    return p * _t;
  }

  // Newton's method in double, narrowed once. std::sqrt is not constexpr either, and a runtime call
  // would make Direction two functions -- one the compiler folds and one it does not -- which is
  // exactly the kind of pair that disagrees in the last bit. The argument is first scaled into
  // [1, 4) by powers of four, which are exact, so the scaling costs nothing and six iterations from
  // the linear seed always converge to the full precision of a double.
  [[nodiscard]] static constexpr float Sqrt(float _value) noexcept
  {
    if (!(_value > 0.0f))
      return 0.0f;

    double value = static_cast<double>(_value);
    double scale = 1.0;
    while (value >= 4.0)
    {
      value *= 0.25;
      scale *= 2.0;
    }
    while (value < 1.0)
    {
      value *= 4.0;
      scale *= 0.5;
    }

    double root = 0.5 * (1.0 + value);
    for (int i = 0; i < 6; ++i)
      root = 0.5 * (root + value / root);

    return static_cast<float>(root * scale);
  }

  [[nodiscard]] static constexpr DirectX::XMFLOAT3 Normalise(float _x, float _y, float _z) noexcept
  {
    const float length = Sqrt(_x * _x + _y * _y + _z * _z);
    if (length <= 0.0f)
      return DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);

    return DirectX::XMFLOAT3(_x / length, _y / length, _z / length);
  }
};
} // namespace Neuron
