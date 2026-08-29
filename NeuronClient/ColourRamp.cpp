#include "pch.h"
#include "ColourRamp.h"

using namespace DirectX;

namespace Neuron
{
bool ColourRamp::Load(const std::wstring& _fileName, ColourRamp& _outRamp)
{
  DdsImage image;
  if (!DdsImage::Load(_fileName, image))
    return false;

  if (!_outRamp.FromImage(image))
  {
    DebugTrace(L"ramp {} is not a {}x{} colour ramp; see the reason above\n", _fileName, SIDE, SIDE);
    return false;
  }

  return true;
}

bool ColourRamp::FromImage(const DdsImage& _image)
{
  m_loaded = false;

  if (_image.widthPx != SIDE || _image.heightPx != SIDE)
  {
    DebugTrace("ramp: {}x{} is not the {}x{} a colour ramp is\n", _image.widthPx, _image.heightPx, SIDE, SIDE);
    return false;
  }

  ByteBuffer pixels;
  if (!_image.TopMipAsBgra(pixels))
    return false;

  if (pixels.size() < static_cast<std::size_t>(SIDE) * SIDE * 4)
  {
    DebugTrace("ramp: the surface is {} bytes, short of the {} a {}x{} BGRA image is\n", pixels.size(),
               static_cast<std::size_t>(SIDE) * SIDE * 4, SIDE, SIDE);
    return false;
  }

  // B, G, R, A on disk; R, G, B as floats here. Alpha is dropped: a ramp is opaque by definition and
  // carrying a channel nothing reads would only invite somebody to read it.
  for (std::uint32_t texel = 0; texel < SIDE * SIDE; ++texel)
  {
    m_rgb[texel * 3 + 0] = pixels[texel * 4 + 2] * (1.0f / 255.0f);
    m_rgb[texel * 3 + 1] = pixels[texel * 4 + 1] * (1.0f / 255.0f);
    m_rgb[texel * 3 + 2] = pixels[texel * 4 + 0] * (1.0f / 255.0f);
  }

  m_loaded = true;
  return true;
}

XMFLOAT3 ColourRamp::Sample(float _u, float _v) const noexcept
{
  const float lastTexel = static_cast<float>(SIDE - 1);
  const float x = std::clamp(_u, 0.0f, 1.0f) * lastTexel;
  const float y = std::clamp(_v, 0.0f, 1.0f) * lastTexel;

  const std::uint32_t left = static_cast<std::uint32_t>(x);
  const std::uint32_t top = static_cast<std::uint32_t>(y);
  const std::uint32_t right = std::min(left + 1u, SIDE - 1u);
  const std::uint32_t bottom = std::min(top + 1u, SIDE - 1u);
  const float alongX = x - static_cast<float>(left);
  const float alongY = y - static_cast<float>(top);

  XMFLOAT3 colour(0.0f, 0.0f, 0.0f);
  float* const channels = &colour.x;
  for (std::uint32_t channel = 0; channel < 3; ++channel)
  {
    const float topRow = std::lerp(m_rgb[(top * SIDE + left) * 3 + channel], m_rgb[(top * SIDE + right) * 3 + channel], alongX);
    const float bottomRow = std::lerp(m_rgb[(bottom * SIDE + left) * 3 + channel], m_rgb[(bottom * SIDE + right) * 3 + channel], alongX);
    channels[channel] = std::lerp(topRow, bottomRow, alongY);
  }

  return colour;
}
} // namespace Neuron
