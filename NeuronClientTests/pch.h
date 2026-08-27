#pragma once

// Test projects link the library under test and see its umbrella header, nothing more. A test that
// needs another layer to compile is telling you the layers are not separate.
#include "NeuronClient.h"

#include "CppUnitTest.h"
