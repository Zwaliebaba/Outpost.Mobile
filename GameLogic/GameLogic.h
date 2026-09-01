#pragma once

// The GameLogic umbrella. This library is the game: the deterministic simulation, and nothing
// else. It depends on NeuronCore and on nothing above it -- no renderer, no window, no transport
// implementation, no engine library that knows what a frame is.
//
// Determinism is the property that makes it worth being a library at all, because it is what lets
// the same code be unit-tested, replayed, and eventually run on a server that has no GPU:
//
//   * no wall clock -- the only time in here is TICK_DT and the tick count;
//   * no OS entropy -- the one generator is Neuron::Pcg32 and nothing seeds itself from the
//     machine. Randomness has arrived, in UniverseLayout: a pure function of a seed its caller
//     supplies, called at boot, whose output is then ordinary spawn input. Nothing inside Step
//     draws at all, so the replay contract sees positions and never a generator;
//   * no pointers as keys, and no iteration order that is not dense-array order;
//   * no presentation state -- ring fades, camera lag and thruster glow live in the view.
//
// The step from "runs on one machine" to "runs on a server" is then a matter of moving Universe, not
// of untangling it.

#include "NeuronCore.h"

#include "SimTuning.h"
#include "UniversePos.h"
#include "ShipState.h"
#include "DeviceSpec.h"
#include "HullSpec.h"
#include "Collision.h"
#include "PathGrid.h"
#include "PathIslands.h"
#include "SpatialIndex.h"
#include "InterestSet.h"
#include "Formation.h"
#include "Patrol.h"
#include "UniverseLayout.h"
#include "Movement.h"
#include "Universe.h"
#include "UniverseSnapshot.h"
#include "Publisher.h"
