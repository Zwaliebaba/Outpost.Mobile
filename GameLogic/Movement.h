#pragma once

#include "HullSpec.h"
#include "ShipState.h"

#include <span>

namespace Game
{
// One entry in a ship's sensed neighbourhood, captured at the top of the tick.
//
// The offsets and the velocity are start-of-tick values, copied here rather than read from the
// neighbour when they are wanted: by the time the intent pass runs, the neighbour's own live speed
// and heading may already have been advanced, and reading them would make the answer depend on
// array order. Copying is what makes the tick a Jacobi solve rather than a Gauss-Seidel one
// (Design/Collision.md 6).
struct Neighbour
{
  ShipId id = INVALID_SHIP_ID;
  float offsetX = 0.0f; // from this ship to that one, in metres
  float offsetZ = 0.0f;
  float velocityX = 0.0f;
  float velocityZ = 0.0f;
  float boundingRadiusMetres = 0.0f;
  float distanceSquared = 0.0f;

  // Carried so that avoidance yields on the same rule separation splits on. If the two disagreed
  // about who gives way, a ship would steer aside and then be shoved back into the place it left.
  float avoidanceAuthority = 1.0f;
  bool immovable = false;

  // The sort key: centre distance less the neighbour's own bounding radius, so the list is the K
  // nearest by *surface* rather than by centre. With a 72:1 size ratio those are different
  // questions, and the centre answer is the wrong one -- a Structure whose wall a ship is touching
  // sits 263 m from that ship's centre and sorts below a dozen fighters a hundred metres clear.
  // Truncating that list at K puts the wall over the edge and the ship sails through it.
  float proximityMetres = 0.0f;
};

// What the ship wants this tick, before the limiter has its say. It is the value passed between the
// three halves of the tick's motion, and it exists so that deciding *where to go* and deciding
// *how fast the hull can get there* are separately testable.
struct MotionIntent
{
  float desiredHeadingRad = 0.0f;
  float desiredSpeedMetresPerSec = 0.0f;

  // The heading the steering committed to, carried into the next tick as the continuity bias. It
  // travels in the intent rather than being written directly so that the two solving functions stay
  // free of side effects and IntegrateShip remains the one place a tick writes to a ship.
  float avoidHeadingRad = 0.0f;

  // Order transitions are reported rather than applied, for the same reason. Arriving is a decision
  // the order layer makes and the integrator enacts.
  OrderState nextOrder = OrderState::Idle;
};

// What the order wants: turn towards the target, and go no faster than can still be shed before it.
// The first half of what used to be one StepShip, and a pure function of one ship.
[[nodiscard]] MotionIntent SolveOrder(const ShipState& _ship, const HullSpec& _hull) noexcept;

// What the neighbourhood allows. Context steering: sample the headings this hull can actually reach,
// score each for interest and for danger, take the best.
//
// Not ORCA, and the reason is specific. ORCA is the quality standard for crowd avoidance and it
// assumes holonomic velocity control -- agents free to select any velocity each step. These hulls
// are turn-rate limited and cannot strafe, so adopting it would mean either replacing a motion model
// that already works and is tested, or spending every tick fighting the limiter that clamps ORCA's
// chosen velocity into something the hull can do. Context steering handles the constraint natively,
// because only reachable headings are scored in the first place (Design/Collision.md 10).
//
// This signature is the seam: if ORCA is ever wanted, it goes behind it and nothing above or below
// moves.
[[nodiscard]] MotionIntent AvoidNeighbours(const ShipState& _ship, const HullSpec& _hull, MotionIntent _intent,
                                           std::span<const Neighbour> _neighbours) noexcept;

// The turn-rate and acceleration limiter, unchanged from the day it was written, plus the two
// writes the solving halves deferred to it.
//
// prevPos and prevHeading are World's pass 0, not this function's: they are the start-of-tick
// snapshot every neighbour read in the tick depends on, so writing them here would make them depend
// on where in the array this ship sits.
void IntegrateShip(ShipState& _ship, const HullSpec& _hull, const MotionIntent& _intent) noexcept;
} // namespace Game
