#pragma once

// The GameLogic umbrella. This library is the game: the deterministic simulation, and nothing
// else. It depends on NeuronCore and on nothing above it -- no renderer, no window, no transport
// implementation, no engine library that knows what a frame is.
//
// Determinism is the property that makes it worth being a library at all, because it is what lets
// the same code be unit-tested, replayed, and eventually run on a server that has no GPU:
//
//   * no wall clock -- the only time in here is TICK_DT and the tick count;
//   * no OS entropy -- randomness, when it arrives, is one seeded generator held by World;
//   * no pointers as keys, and no iteration order that is not dense-array order;
//   * no presentation state -- ring fades, camera lag and thruster glow live in the view.
//
// The step from "runs on one machine" to "runs on a server" is then a matter of moving World, not
// of untangling it.

#include "NeuronCore.h"

#include "SimTuning.h"
#include "ShipState.h"
#include "Formation.h"
#include "Movement.h"
#include "World.h"
