#include "pch.h"
#include "Movement.h"

#include "SimTuning.h"

using namespace DirectX;

namespace Game
{
void StepShip(ShipState& _ship, const HullSpec& _hull) noexcept
{
  const float maxTurnRate = _hull.maxTurnRateRadPerSec;
  const float turnAcceleration = _hull.turnAccelerationRadPerSec2;
  const float stopBlend = Neuron::HalfLifeBlend(TICK_DT, STOP_DAMPING_HALF_LIFE);

  _ship.prevPos = _ship.posWorld;
  _ship.prevHeading = _ship.headingRad;
  const float speedBefore = _ship.speed;

  float desiredHeading = _ship.headingRad;
  float desiredSpeed = 0.0f;

  if (_ship.order == OrderState::Moving)
  {
    const float dx = OffsetX(_ship.posWorld, _ship.orderPos);
    const float dz = OffsetZ(_ship.posWorld, _ship.orderPos);
    const float distance = std::sqrt(dx * dx + dz * dz);
    if (distance <= ARRIVAL_RADIUS)
      _ship.order = _ship.orderHasFacing ? OrderState::Aligning : OrderState::Idle;
    else
    {
      desiredHeading = std::atan2(dx, dz);
      // Never faster than can still be shed before the point.
      desiredSpeed = std::min(_hull.maxSpeedMetresPerSec, std::sqrt(2.0f * _hull.decelerationMetresPerSec2 * (distance - ARRIVAL_RADIUS)));
    }
  }
  else if (_ship.order == OrderState::Aligning)
  {
    desiredHeading = _ship.orderFacingRad;
    if (std::fabs(XMScalarModAngle(desiredHeading - _ship.headingRad)) < ALIGNED_HEADING_RAD && std::fabs(_ship.speed) < ALIGNED_SPEED)
      _ship.order = OrderState::Idle;
  }

  // Angular velocity accelerates towards whatever closes the error, capped both by the turn rate
  // and by what can still be brought to rest inside the angle that is left.
  const float headingError = XMScalarModAngle(desiredHeading - _ship.headingRad);
  const float settleRate = std::sqrt(2.0f * turnAcceleration * std::fabs(headingError));
  float targetRate = std::clamp(headingError / TICK_DT, -maxTurnRate, maxTurnRate);
  targetRate = std::clamp(targetRate, -settleRate, settleRate);
  _ship.turnRateRadPerSec = Neuron::MoveTowards(_ship.turnRateRadPerSec, targetRate, turnAcceleration * TICK_DT);
  _ship.headingRad = XMScalarModAngle(_ship.headingRad + _ship.turnRateRadPerSec * TICK_DT);

  // Only drive hard while roughly pointed the right way, so ships arc round instead of pivoting on
  // the spot and then snapping into motion.
  desiredSpeed *= std::max(0.0f, std::cos(headingError));

  if (_ship.order == OrderState::Moving)
    _ship.speed =
      Neuron::MoveTowards(_ship.speed, desiredSpeed,
                          (desiredSpeed > _ship.speed ? _hull.accelerationMetresPerSec2 : _hull.decelerationMetresPerSec2) * TICK_DT);
  else
  {
    _ship.speed -= _ship.speed * stopBlend; // half-life damping down to a standstill
    if (std::fabs(_ship.speed) < STOPPED_SPEED)
      _ship.speed = 0.0f;
  }

  Translate(_ship.posWorld, std::sin(_ship.headingRad) * _ship.speed * TICK_DT, std::cos(_ship.headingRad) * _ship.speed * TICK_DT);

  // What the view's thruster glow and trail are driven by.
  _ship.accelSample = (_ship.speed - speedBefore) / TICK_DT;
}
} // namespace Game
