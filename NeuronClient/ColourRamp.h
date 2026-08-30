#pragma once

#include "DdsImage.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>

namespace Neuron
{
// A 64x64 colour lookup, read off a DDS and kept in system memory as floats. It is the port of the
// source's landscape palettes: the X axis is slope, flat on the left and cliff on the right, and the
// Y axis is climate height, the summit on row 0 and sea level on row 63
// (Design/Archive/PlanetRenderer.md 6.1).
//
// It is a lookup table and never a texture. The colour of a body is baked into its vertices on the
// CPU, one colour per triangle, because a pixel-shader lookup would lose the per-triangle dither
// that gives the surface its grain -- so nothing here uploads anything, and this class includes no
// graphics header.
//
// Sampled bilinear where the source read the nearest texel. The ramps are soft gradients, the two
// are indistinguishable on them, and this is stated so that a later change to a ramp is not blamed
// on the sampler.
class ColourRamp
{
public:
  static constexpr std::uint32_t SIDE = 64;

  // Reports false and traces on anything that is not a 64x64 surface this tree can read as BGRA. A
  // ramp that fails to load leaves the object unloaded rather than half filled, and the builder
  // draws BODY_FALLBACK_GREY: a missing asset is a diagnostic, not a crash.
  [[nodiscard]] static bool Load(const std::wstring& _fileName, ColourRamp& _outRamp);

  // The decode on its own, so the size and format rules can be exercised without a file on disk.
  [[nodiscard]] bool FromImage(const DdsImage& _image);

  // _u: 0 flat .. 1 cliff. _v: 0 summit .. 1 sea level. Both clamped, so a dithered v that ran past
  // the end of the ramp reads the last row rather than wrapping to the first.
  [[nodiscard]] DirectX::XMFLOAT3 Sample(float _u, float _v) const noexcept;

  // The table back as the bytes it was read from, for a caller that needs it as a texture -- the
  // compute bake samples the ramp on the GPU where everything else on this side samples it here.
  // The round trip is exact: a channel came in as a byte over 255 and goes back as that byte.
  void AsBgra(ByteBuffer& _outPixels) const;

  [[nodiscard]] bool Loaded() const noexcept
  {
    return m_loaded;
  }

private:
  float m_rgb[SIDE * SIDE * 3] = {};
  bool m_loaded = false;
};
} // namespace Neuron
