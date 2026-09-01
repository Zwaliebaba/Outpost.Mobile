#pragma once

// The tool sees the game and the engine it needs to write a file, and nothing else. No client, no
// renderer, no window: a universe is generated on a machine that may not have a screen, which is the
// same reason NeuronServer is headless (AGENTS.md 2).
#include "GameLogic.h"
