#pragma once

// The exception to the rule the other four suites keep.
//
// A test project sees the umbrella header of the library it tests and nothing more, because a test
// that needs another layer to compile is telling you the layers are not separate. `Server` is not a
// library: it is the tree's second composition root
// ([ADR 0067](../../Design/Decisions/0067-the-tree-has-a-second-composition-root.md)), and a root is
// the one kind of thing entitled to see several layers -- so a suite over one sees what it sees.
//
// It reaches `Server\` by include path and shares no source file with it. That works because the
// testable half of `Server/` is header-only by construction -- `ShardSimulation.h`, `ShardSession.h`
// and `ShardLinks.h` -- while `ShardApp.cpp` is boot, listener and run loop, which are run rather
// than tested (Design/ShardServer-slice-3.md 2.4). Keeping that split is what keeps this suite
// possible; the day a testable thing lands in a .cpp here, it has landed in the wrong file.
#include "NeuronCore.h"
#include "NeuronServer.h"
#include "GameLogic.h"

#include "CppUnitTest.h"
