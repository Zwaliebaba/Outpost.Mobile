#pragma once

// The NeuronServer umbrella. The authoritative half of the engine: it owns the tick loop and,
// later, sessions and snapshot fan-out. It knows nothing about the game it is ticking -- the
// simulation reaches it through Simulation, which the executable implements over GameLogic.
//
// It also knows nothing about the client half. The two never call each other; when they are
// separated they will talk over NeuronCore's Transport and nothing else.

#include "NeuronCore.h"

#include "Simulation.h"
#include "ServerHost.h"
