#pragma once

#include "ShipState.h"

namespace Game
{
// One ship, one tick: turn towards the target at a limited rate, drive forward along the facing,
// slow down in time to stop on the point. No pathfinding and no avoidance, by design.
//
// Separate from World because it is the piece worth testing in isolation -- given a ship and an
// order, does it arrive, and does it arrive the same way every time.
void StepShip(ShipState& _ship) noexcept;
} // namespace Game
