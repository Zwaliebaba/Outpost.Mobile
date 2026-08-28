#include "pch.h"
#include "Movement.h"

#include "Collision.h"
#include "SimTuning.h"

using namespace DirectX;

namespace Game
{
namespace
{
// How threatening one neighbour is along a candidate heading, in [0, 1].
//
// Scored on time to closest approach rather than on separation distance, because distance gets the
// answer backwards the moment speeds differ: a ship 200 m away closing at 68 m/s matters more than
// one 40 m away closing at 2 m/s, and it matters more the wider the speed spread becomes. A
// neighbour that is not converging is ignored no matter how close it currently is, which is what
// lets ships pass each other closely and calmly (Design/Collision.md 10).
[[nodiscard]] float ThreatAlong(const Neighbour& _neighbour, const HullSpec& _hull, float _velocityX, float _velocityZ,
                                float _speedMetresPerSec) noexcept
{
  const float horizonSec = ThreatHorizonSec(_hull, _neighbour.boundingRadiusMetres, _speedMetresPerSec);
  const float relativeX = _neighbour.velocityX - _velocityX;
  const float relativeZ = _neighbour.velocityZ - _velocityZ;
  const float closingSquared = relativeX * relativeX + relativeZ * relativeZ;

  // Zero relative velocity is not an edge case: it is a formation flying in company, which is the
  // most ordinary arrangement the game has. The separation then never changes, so the closest
  // approach is now and it is the current distance. Dividing here instead produces a NaN in the
  // most common situation on the board.
  float timeToClosest = 0.0f;
  if (closingSquared > 1e-8f)
    timeToClosest = std::max(0.0f, -(_neighbour.offsetX * relativeX + _neighbour.offsetZ * relativeZ) / closingSquared);
  if (timeToClosest >= horizonSec)
    return 0.0f;

  const float missX = _neighbour.offsetX + relativeX * timeToClosest;
  const float missZ = _neighbour.offsetZ + relativeZ * timeToClosest;
  const float missDistance = std::sqrt(missX * missX + missZ * missZ);
  const float clearance = _hull.BoundingRadiusMetres() + _neighbour.boundingRadiusMetres + AVOID_MARGIN_METRES;
  if (missDistance >= clearance)
    return 0.0f;

  // Sooner and closer are both worse, and the product is what makes a distant near-miss score below
  // an imminent glancing one.
  return (1.0f - timeToClosest / horizonSec) * (1.0f - missDistance / clearance);
}
// The share of a converging pair's evasion this ship is the one to make -- the same split, from the
// same rule, that separation uses to decide who takes the correction.
//
// Without it, avoidance is symmetric and both parties steer: measured on a fighter crossing a
// Carrier's bow, the capital left its track by 121 m to the fighter's 124, which is a Carrier
// swerving for an Interceptor and reads as a bug to anyone watching it (Design/Collision.md 9, 16).
[[nodiscard]] float YieldShare(float _ownAuthority, const Neighbour& _neighbour) noexcept
{
  return SeparationSharesFor(_ownAuthority, false, _neighbour.avoidanceAuthority, _neighbour.immovable).a;
}
} // namespace

MotionIntent SolveOrder(const ShipState& _ship, const HullSpec& _hull) noexcept
{
  MotionIntent intent;
  intent.desiredHeadingRad = _ship.headingRad;
  intent.nextOrder = _ship.order;

  if (_ship.order == OrderState::Moving)
  {
    const float dx = OffsetX(_ship.posWorld, _ship.steerTargetPos);
    const float dz = OffsetZ(_ship.posWorld, _ship.steerTargetPos);
    const float distance = std::sqrt(dx * dx + dz * dz);
    const float arrival = ArrivalRadiusMetres(_hull);
    if (distance <= arrival)
      intent.nextOrder = _ship.orderHasFacing ? OrderState::Aligning : OrderState::Idle;
    else
    {
      intent.desiredHeadingRad = std::atan2(dx, dz);
      // Never faster than can still be shed before the point.
      intent.desiredSpeedMetresPerSec =
        std::min(_hull.maxSpeedMetresPerSec, std::sqrt(2.0f * _hull.decelerationMetresPerSec2 * (distance - arrival)));
    }
  }
  else if (_ship.order == OrderState::Aligning)
  {
    intent.desiredHeadingRad = _ship.orderFacingRad;
    if (std::fabs(XMScalarModAngle(intent.desiredHeadingRad - _ship.headingRad)) < ALIGNED_HEADING_RAD &&
        std::fabs(_ship.speed) < ALIGNED_SPEED)
      intent.nextOrder = OrderState::Idle;
  }

  intent.avoidHeadingRad = intent.desiredHeadingRad;
  return intent;
}

MotionIntent AvoidNeighbours(const ShipState& _ship, const HullSpec& _hull, MotionIntent _intent,
                             std::span<const Neighbour> _neighbours) noexcept
{
  if (_neighbours.empty())
    return _intent;

  // Score against the speed the ship is about to be doing, not the one it is doing: a ship
  // accelerating out of a standstill has to start giving way before it is moving.
  const float speed = std::max(_ship.speed, _intent.desiredSpeedMetresPerSec);

  // A clear sky must cost nothing and change nothing, or every phase before this one stops being
  // behaviour-neutral in the cases it was verified against.
  const float forwardX = std::sin(_ship.headingRad);
  const float forwardZ = std::cos(_ship.headingRad);
  const float ownAuthority = AvoidanceAuthorityOf(_hull, _ship.order);
  bool anyThreat = false;
  float headOnWeight = 0.0f;
  for (const Neighbour& neighbour : _neighbours)
  {
    const float threat = ThreatAlong(neighbour, _hull, forwardX * speed, forwardZ * speed, speed) * YieldShare(ownAuthority, neighbour);
    if (threat <= 0.0f)
      continue;
    anyThreat = true;

    // Two identical hulls meeting head-on mirror each other's correction exactly and deadlock, or
    // oscillate -- the pedestrian sidewalk dance, very visible at fleet scale. Break it with a rule
    // rather than a tie-break on ShipId: give way to starboard. It needs no shared state, neither
    // party has to know the other's id, and it reads on screen as seamanship rather than as jitter,
    // which is not an accident -- the problem is the same one (Design/Collision.md 9).
    //
    // The weight is continuous in bearing, in closing rate and in the threat itself, and that is
    // not tidiness. A bare predicate -- inside the cone and closing -- is a step of a third of the
    // interest range appearing and vanishing between ticks as the pair weaves, and a Frigate pair
    // measured fourteen heading reversals per second on it: the exact chatter the continuity bias
    // exists to prevent, reintroduced by the rule meant to break the symmetry.
    const float bearing = XMScalarModAngle(std::atan2(neighbour.offsetX, neighbour.offsetZ) - _ship.headingRad);
    const float onTheBow = std::max(0.0f, 1.0f - std::fabs(bearing) / AVOID_HEAD_ON_CONE_RAD);
    const float closing = -(neighbour.velocityX * forwardX + neighbour.velocityZ * forwardZ);
    const float opposed = std::clamp(closing / std::max(speed, 1.0f), 0.0f, 1.0f);
    headOnWeight = std::max(headOnWeight, std::min(1.0f, threat * AVOID_HEAD_ON_GAIN) * onTheBow * opposed);
  }
  if (!anyThreat)
    return _intent;

  // Only headings this hull can actually reach are scored, which is the whole reason this fits a
  // turn-rate-limited motion model where ORCA does not.
  const float fanHalfWidth = std::min(XM_PI, _hull.maxTurnRateRadPerSec * AVOID_STEER_REACH_SEC);

  float bestScore = -1e30f;
  float bestHeading = _intent.desiredHeadingRad;
  float bestDanger = 0.0f;
  for (int candidate = 0; candidate < AVOID_CANDIDATE_COUNT; ++candidate)
  {
    const float across = static_cast<float>(candidate) / static_cast<float>(AVOID_CANDIDATE_COUNT - 1) * 2.0f - 1.0f;
    const float heading = _ship.headingRad + across * fanHalfWidth;
    const float velocityX = std::sin(heading) * speed;
    const float velocityZ = std::cos(heading) * speed;

    float danger = 0.0f;
    for (const Neighbour& neighbour : _neighbours)
      danger = std::max(danger, ThreatAlong(neighbour, _hull, velocityX, velocityZ, speed) * YieldShare(ownAuthority, neighbour));

    float score = std::cos(XMScalarModAngle(heading - _intent.desiredHeadingRad)) - AVOID_DANGER_WEIGHT * danger;

    // A plain per-tick argmax chatters. Whenever two candidates score within noise of each other --
    // the normal condition for a symmetric head-on pair, which is exactly what the starboard rule
    // above creates -- the winner flips left, right, left on successive ticks and the ship shivers
    // down the middle. Scoring last tick's choice with a continuity bonus turns "does not
    // oscillate" from an aspiration into a property, and a ship that commits to its give-way turn
    // also looks decided rather than broken.
    score += AVOID_CONTINUITY_BONUS * std::max(0.0f, std::cos(XMScalarModAngle(heading - _ship.avoidHeadingRad)));
    // Weighted by the candidate's actual starboard component, not by where it sits in the fan. An
    // Interceptor's fan is the whole circle, so a fan-relative weight puts the bias's maximum on a
    // reversed heading -- where the interest term has already ruled it out -- and leaves nothing at
    // the small turns that decide a symmetric encounter. Measured: no starboard break at all.
    const float starboard = std::sin(XMScalarModAngle(heading - _ship.headingRad));
    if (starboard > 0.0f)
      score += AVOID_STARBOARD_BIAS * headOnWeight * starboard;

    if (score > bestScore)
    {
      bestScore = score;
      bestHeading = heading;
      bestDanger = danger;
    }
  }

  _intent.desiredHeadingRad = bestHeading;
  _intent.avoidHeadingRad = bestHeading;
  // Steering alone cannot always clear a contact -- a hull may be turn-limited past the point where
  // any reachable heading is safe -- so what is left is shed as speed.
  _intent.desiredSpeedMetresPerSec *= std::max(0.0f, 1.0f - AVOID_SPEED_SHED * bestDanger);
  return _intent;
}

void IntegrateShip(ShipState& _ship, const HullSpec& _hull, const MotionIntent& _intent) noexcept
{
  const float stopBlend = Neuron::HalfLifeBlend(TICK_DT, STOP_DAMPING_HALF_LIFE);
  const float speedBefore = _ship.speed;

  _ship.order = _intent.nextOrder;
  _ship.avoidHeadingRad = _intent.avoidHeadingRad;

  // Angular velocity accelerates towards whatever closes the error, capped both by the turn rate
  // and by what can still be brought to rest inside the angle that is left.
  const float headingError = XMScalarModAngle(_intent.desiredHeadingRad - _ship.headingRad);
  const float settleRate = std::sqrt(2.0f * _hull.turnAccelerationRadPerSec2 * std::fabs(headingError));
  float targetRate = std::clamp(headingError / TICK_DT, -_hull.maxTurnRateRadPerSec, _hull.maxTurnRateRadPerSec);
  targetRate = std::clamp(targetRate, -settleRate, settleRate);
  _ship.turnRateRadPerSec = Neuron::MoveTowards(_ship.turnRateRadPerSec, targetRate, _hull.turnAccelerationRadPerSec2 * TICK_DT);
  _ship.headingRad = XMScalarModAngle(_ship.headingRad + _ship.turnRateRadPerSec * TICK_DT);

  // Only drive hard while roughly pointed the right way, so ships arc round instead of pivoting on
  // the spot and then snapping into motion.
  const float desiredSpeed = _intent.desiredSpeedMetresPerSec * std::max(0.0f, std::cos(headingError));

  if (_ship.order == OrderState::Moving)
  {
    _ship.speed =
      Neuron::MoveTowards(_ship.speed, desiredSpeed,
                          (desiredSpeed > _ship.speed ? _hull.accelerationMetresPerSec2 : _hull.decelerationMetresPerSec2) * TICK_DT);
  }
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
