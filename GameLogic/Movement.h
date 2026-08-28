#pragma once

#include "HullSpec.h"
#include "ShipState.h"

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

  // The sort key: centre distance less the neighbour's own bounding radius, so the list is the K
  // nearest by *surface* rather than by centre. With a 72:1 size ratio those are different
  // questions, and the centre answer is the wrong one -- a Structure whose wall a ship is touching
  // sits 263 m from that ship's centre and sorts below a dozen fighters a hundred metres clear.
  // Truncating that list at K puts the wall over the edge and the ship sails through it.
  float proximityMetres = 0.0f;
};

// One ship, one tick: turn towards the target at a limited rate, drive forward along the facing,
// slow down in time to stop on the point.
//
// prevPos and prevHeading are World's pass 0, not this function's: they are the start-of-tick
// snapshot every neighbour read in the tick depends on, so writing them here would make them
// depend on where in the array this ship sits.
//
// Separate from World because it is the piece worth testing in isolation -- given a ship and an
// order, does it arrive, and does it arrive the same way every time.
void StepShip(ShipState& _ship, const HullSpec& _hull) noexcept;
} // namespace Game
