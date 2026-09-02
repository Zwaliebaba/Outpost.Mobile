#pragma once

// The shard server sees three layers, because it is a composition root and the only other thing in
// the tree entitled to (AGENTS.md 3). It does NOT see NeuronClient: a server that could name a
// graphics type is a server that will eventually hold one, and the layer check holds this.

#include "NeuronCore.h"
#include "NeuronServer.h"
#include "GameLogic.h"
