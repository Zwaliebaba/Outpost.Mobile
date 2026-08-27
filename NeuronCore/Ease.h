#pragma once

#include <algorithm>
#include <cmath>

namespace Neuron
{
// Framerate-independent easing. Everything in this tree that eases uses the half-life form rather
// than a per-frame constant, so the same tuning value produces the same motion at 30 Hz and at
// 240 Hz. A half-life is also the one easing parameter a person can reason about: it is the time
// the remaining error takes to halve.

// The fraction of the remaining distance to cover this step.
[[nodiscard]] inline float HalfLifeBlend(float _dtSec, float _halfLifeSec) noexcept
{
  return (_halfLifeSec <= 0.0f) ? 1.0f : 1.0f - std::exp2(-_dtSec / _halfLifeSec);
}

[[nodiscard]] inline float SmoothTowards(float _current, float _target, float _dtSec, float _halfLifeSec) noexcept
{
  if (_halfLifeSec <= 0.0f)
    return _target;
  return _current + (_target - _current) * HalfLifeBlend(_dtSec, _halfLifeSec);
}

// Linear approach, capped at _maxDelta per call. Used where a rate limit is what is wanted (a turn
// rate, a fade ramp) rather than an exponential settle.
[[nodiscard]] inline float MoveTowards(float _current, float _target, float _maxDelta) noexcept
{
  const float delta = _target - _current;
  if (std::fabs(delta) <= _maxDelta)
    return _target;
  return _current + ((delta > 0.0f) ? _maxDelta : -_maxDelta);
}

[[nodiscard]] inline float Distance2D(float _ax, float _ay, float _bx, float _by) noexcept
{
  const float dx = _bx - _ax;
  const float dy = _by - _ay;
  return std::sqrt(dx * dx + dy * dy);
}

// A value that overshoots is a spring, so use one. The peak overshoot fixes the damping ratio and
// the settle half-life then fixes the frequency, which makes both values mean exactly what they
// say rather than being two knobs on the same vague curve.
inline void SpringTowards(float& _value, float& _velocity, float _target, float _overshoot, float _settleHalfLifeSec,
                          float _dtSec) noexcept
{
  constexpr float PI = 3.14159265358979323846f;
  constexpr float LN2 = 0.6931472f;

  const float overshoot = std::clamp(_overshoot - 1.0f, 0.002f, 0.95f);
  const float logOvershoot = std::log(overshoot);
  const float damping = -logOvershoot / std::sqrt(PI * PI + logOvershoot * logOvershoot);
  // The envelope decays as exp(-damping * omega * t), so this omega puts its half-life exactly on
  // the tuned one. Capped against the step size so dragging the tuning to nothing cannot blow up.
  float omega = LN2 / (std::max(0.001f, _settleHalfLifeSec) * std::max(0.05f, damping));
  omega = std::min(omega, 0.8f / std::max(1e-5f, _dtSec));

  _velocity += (omega * omega * (_target - _value) - 2.0f * damping * omega * _velocity) * _dtSec;
  _value += _velocity * _dtSec;
}
} // namespace Neuron
