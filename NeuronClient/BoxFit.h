#pragma once

#include <algorithm>

namespace Neuron
{
// Fitting a source rectangle into a destination rectangle in pixels, with ONE scale for both axes.
//
// The isotropy is the whole content of this header. Fitting each axis independently is one line
// shorter and is what a map, a minimap or a thumbnail gets wrong silently: the picture still fills
// the box, nothing looks broken, and every distance read off it is wrong by the aspect ratio. A
// galaxy stretched to fill a 16:9 screen makes a player misjudge which of two gates is the long one,
// and there is no frame at which that failure announces itself.
//
// Header-only and free of every graphics type on purpose: it is arithmetic, so it is provable in the
// suite rather than by looking at a screenshot.
struct BoxFit
{
  // Destination pixels per source unit. One number, which is the point.
  float scale = 1.0f;

  // Where source (0, 0) lands. The fit is applied as origin + value * scale, never as a rectangle
  // remapped per axis.
  float originXPx = 0.0f;
  float originYPx = 0.0f;

  [[nodiscard]] float XPx(float _x) const noexcept
  {
    return originXPx + _x * scale;
  }

  [[nodiscard]] float YPx(float _y) const noexcept
  {
    return originYPx + _y * scale;
  }
};

// The largest isotropic fit of the source box into the destination box, centred in whichever axis
// has slack.
//
// A degenerate source -- one point, or every point on one line -- has no scale that means anything,
// so the fit that would divide by zero is refused rather than produced: the axis with no extent is
// ignored, and a source with no extent at all fits at scale 1 in the centre. A galaxy of one system
// is not a case worth a special screen, but it IS a case worth not dividing by zero in.
//
// A destination narrower than nothing (a window mid-resize) yields a non-positive scale, which draws
// nothing rather than drawing inside out.
[[nodiscard]] inline BoxFit FitBoxIsotropic(float _srcX0, float _srcY0, float _srcX1, float _srcY1, float _dstX0Px, float _dstY0Px,
                                            float _dstX1Px, float _dstY1Px) noexcept
{
  const float srcW = _srcX1 - _srcX0;
  const float srcH = _srcY1 - _srcY0;
  const float dstW = _dstX1Px - _dstX0Px;
  const float dstH = _dstY1Px - _dstY0Px;

  BoxFit fit;
  if (srcW > 0.0f && srcH > 0.0f)
    fit.scale = std::min(dstW / srcW, dstH / srcH);
  else if (srcW > 0.0f)
    fit.scale = dstW / srcW;
  else if (srcH > 0.0f)
    fit.scale = dstH / srcH;
  else
    fit.scale = 1.0f;

  // Centred: half the slack on each side, in both axes, from the same scale. An axis with no extent
  // centres its single value, which is what puts a one-system galaxy in the middle of the panel
  // rather than in a corner.
  fit.originXPx = _dstX0Px + (dstW - srcW * fit.scale) * 0.5f - _srcX0 * fit.scale;
  fit.originYPx = _dstY0Px + (dstH - srcH * fit.scale) * 0.5f - _srcY0 * fit.scale;
  return fit;
}
} // namespace Neuron
