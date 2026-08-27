#pragma once

// The executable sees every layer, because it is the composition root and the only thing that is
// allowed to. Nothing below it includes more than one of these.

#include "NeuronClient.h"
#include "NeuronServer.h"
#include "GameLogic.h"
