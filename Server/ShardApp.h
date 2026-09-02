#pragma once

#include "ShardSimulation.h"

#include "ServerHost.h"

#include "ServerConfig.h"

#include "Universe.h"

#include <cstdint>
#include <string>

namespace Shard
{
// The shard server's composition root: a universe with no window
// ([`Design/ShardServer.md`](../Design/ShardServer.md) §3).
//
// It is the tree's SECOND composition root and deliberately not a shared one
// ([ADR 0067](../Design/Decisions/0067-the-tree-has-a-second-composition-root.md)). `OutpostApp`'s
// shape, in the same order, because a reader who knows one should recognise the other -- and the
// duplication that costs is the thing the design says to watch: if a change to boot ordering ever has
// to be made twice, that record was wrong.
//
// What it owns is ordering and lifetime. Every rule is `GameLogic`'s, the pacing is `ServerHost`'s,
// and the universe it runs was written by a tool (ADR 0058).
class ShardApp
{
public:
  // Reads the config and the save, in that order, and says why it will not start when it will not.
  // False means the process should exit non-zero having printed a reason -- the boot has two
  // failures and no fallback, exactly as the game's does (ADR 0057, 0058).
  [[nodiscard]] bool Boot(std::uint16_t _shard);

  // Ticks until Stop is asked for, saving on the cadence. Returns the tick it finished on.
  std::uint64_t Run();

  // Asked from a console control handler, on another thread, and therefore the one piece of state
  // here that is not single-threaded. It is a flag and nothing else: the loop finishes the tick it
  // is in, writes one save and returns, so a deliberate stop loses nothing where a kill loses the
  // ticks since the last save.
  void RequestStop() noexcept;

  [[nodiscard]] bool StopRequested() const noexcept;

private:
  // Universe.sav for shard 0, Universe.<n>.sav above it -- the same rule Tools/UniverseGen writes by,
  // and it has to be spelled the same way here: a server and a tool that disagree about a filename
  // fail as completely as two that disagree about a format (Design/CrossShard-slice-1.md 2.3).
  [[nodiscard]] static std::wstring SavePathOf(std::uint16_t _shard);

  void SaveUniverse();

  Game::Universe m_universe;
  ShardSimulation m_simulation{m_universe};
  Neuron::ServerHost m_host;

  Game::ServerConfig m_config;
  std::uint16_t m_shard = 0;
  std::wstring m_savePath;
  std::uint64_t m_lastSaveTick = 0;
  std::uint64_t m_galaxySeed = Game::STARTING_GALAXY_SEED;
};
} // namespace Shard
