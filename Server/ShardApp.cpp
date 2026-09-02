#include "pch.h"
#include "ShardApp.h"

#include <atomic>
#include <cstdio>
#include <format>

namespace Shard
{
namespace
{
// Set from a console control handler, which runs on a thread of its own. Every other piece of state
// in this program belongs to the tick thread; this one does not, and is the only reason there is an
// atomic anywhere in the file.
std::atomic<bool> g_stopRequested{false};

// stdout, because a console has one and nothing else here can speak. `OutpostApp` puts these lines
// on screen through EventLog; a server has no screen and the same facts still have to be sayable.
void Say(const std::string& _line)
{
  std::printf("%s\n", _line.c_str());
  std::fflush(stdout); // a server's output is a log, and a log buffered past a crash is not one
}
} // namespace

void ShardApp::RequestStop() noexcept
{
  g_stopRequested.store(true, std::memory_order_relaxed);
}

bool ShardApp::StopRequested() const noexcept
{
  return g_stopRequested.load(std::memory_order_relaxed);
}

std::wstring ShardApp::SavePathOf(std::uint16_t _shard)
{
  // Universe.sav for shard 0, Universe.<n>.sav above it. The same rule Tools/UniverseGen writes by,
  // spelled the same way on purpose: a server and a tool that disagree about a filename fail as
  // completely as two that disagree about a format (Design/CrossShard-slice-1.md 2.3).
  if (_shard == 0)
    return std::wstring(Game::UNIVERSE_SAVE_FILE);

  const std::wstring base(Game::UNIVERSE_SAVE_FILE);
  const std::size_t dot = base.rfind(L'.');
  return base.substr(0, dot) + L"." + std::to_wstring(_shard) + base.substr(dot);
}

bool ShardApp::Boot(std::uint16_t _shard)
{
  m_shard = _shard;
  m_savePath = SavePathOf(_shard);

  // --- the configuration ---------------------------------------------------------------------------
  //
  // Read here and only here, which is what ADR 0043 means by "the composition root alone". A missing
  // file is not a failure: every field has a default taken from the library it configures, so a
  // deployment that wants those says nothing.
  if (Neuron::FileSys::Exists(Game::SERVER_CONFIG_FILE))
  {
    const Neuron::ByteBuffer bytes = Neuron::BinaryFile::ReadFile(Game::SERVER_CONFIG_FILE);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string error;
    if (!Game::ParseServerConfig(text, m_config, error))
    {
      Say(std::format("CONFIG REFUSED | {}", error));
      return false; // a config that says something wrong is worse than one that says nothing
    }
  }

  // --- the universe --------------------------------------------------------------------------------
  //
  // Two failures and no fallback, exactly as the game's boot has (ADR 0057). A server that could
  // invent a universe would be the second thing in the tree that can author one, which is precisely
  // what ADR 0058 exists to prevent -- and it would be worse here than in the game, because a shard
  // that generated its own would disagree with every other shard about where the stations are.
  std::string path;
  for (const wchar_t c : m_savePath)
    path.push_back(static_cast<char>(c));

  if (!Neuron::FileSys::Exists(m_savePath))
  {
    Say(std::format("NO UNIVERSE | {} is not there | run Tools/UniverseGen to write one", path));
    return false;
  }

  const Neuron::ByteBuffer file = Neuron::BinaryFile::ReadFile(m_savePath);
  if (file.empty())
  {
    Say(std::format("UNIVERSE REFUSED | {} is present and could not be read, or is empty", path));
    return false;
  }

  Game::SaveHeader header;
  if (!Game::ReadSaveFile(file, header, m_universe))
  {
    // Peeked for the sentence, because "not a universe this build can read" is a diagnosis only when
    // it names the byte -- the same reason OutpostApp::RestoreUniverse peeks. The peek is only a
    // diagnosis when a format is actually out of range: a file whose header reads fine and whose
    // body is short fails the same call, and naming formats this build does read would be a sentence
    // that contradicts itself.
    std::uint8_t fileFormat = 0;
    std::uint8_t stateFormat = 0;
    const bool peeked = Game::PeekSaveFormats(file, fileFormat, stateFormat);
    const bool readable = peeked && fileFormat >= Game::SAVE_FILE_FORMAT_OLDEST && fileFormat <= Game::SAVE_FILE_FORMAT &&
                          stateFormat >= Game::UNIVERSE_STATE_FORMAT_OLDEST && stateFormat <= Game::UNIVERSE_STATE_FORMAT;
    if (peeked && !readable)
    {
      Say(std::format("UNIVERSE REFUSED | {} is file format {} and state format {} | this build reads file {} to {} and state {} to {}",
                      path, static_cast<unsigned>(fileFormat), static_cast<unsigned>(stateFormat),
                      static_cast<unsigned>(Game::SAVE_FILE_FORMAT_OLDEST), static_cast<unsigned>(Game::SAVE_FILE_FORMAT),
                      static_cast<unsigned>(Game::UNIVERSE_STATE_FORMAT_OLDEST), static_cast<unsigned>(Game::UNIVERSE_STATE_FORMAT)));
    }
    else if (peeked)
    {
      Say(std::format("UNIVERSE REFUSED | {} is file format {} and state format {}, both of which this build reads | the file is torn: {} "
                      "bytes is not a whole universe",
                      path, static_cast<unsigned>(fileFormat), static_cast<unsigned>(stateFormat), file.size()));
    }
    else
    {
      Say(std::format("UNIVERSE REFUSED | {} is not a universe: the magic is wrong or the file is too short to hold a header", path));
    }
    return false;
  }

  // The shard the file says it is, against the shard this process was told to be. A mismatch is a
  // deployment that has pointed two servers at one file or one server at the wrong one, and it is
  // caught here rather than discovered when a handoff arrives for a shard nobody is running.
  if (header.shard != m_shard)
  {
    Say(std::format("UNIVERSE REFUSED | {} holds shard {} and this process was asked to be shard {}", path,
                    static_cast<unsigned>(header.shard), static_cast<unsigned>(m_shard)));
    return false;
  }

  m_galaxySeed = header.galaxySeed;
  m_lastSaveTick = m_universe.Tick();

  Neuron::ServerHost::Desc hostDesc;
  m_host.Init(hostDesc, m_simulation);

  Say(std::format("SHARD {} | {} | file format {} state format {} | tick {} | {} ships {} gates {} stations",
                  static_cast<unsigned>(m_shard), path, static_cast<unsigned>(header.fileFormat), static_cast<unsigned>(header.stateFormat),
                  m_universe.Tick(), m_universe.ShipCount(), m_universe.GateCount(), m_universe.StationCount()));
  if (header.stateFormat < Game::UNIVERSE_STATE_FORMAT)
    Say(std::format("MIGRATED | state format {} to {} | the next save writes the newer one", static_cast<unsigned>(header.stateFormat),
                    static_cast<unsigned>(Game::UNIVERSE_STATE_FORMAT)));
  return true;
}

void ShardApp::SaveUniverse()
{
  Game::SaveHeader header;
  header.galaxySeed = m_galaxySeed;
  header.shard = m_universe.Shard();

  std::vector<std::uint8_t> file;
  Game::WriteSaveFile(m_universe, header, file);
  if (!Neuron::BinaryFile::WriteFileAtomic(m_savePath, file))
  {
    // Said and carried on, for the game's reason: a running shard should not end because a save did,
    // and the previous save is still there.
    Say(std::format("SAVE REFUSED | tick {} | the previous save still stands", m_universe.Tick()));
    return;
  }
  m_lastSaveTick = m_universe.Tick();
  Say(std::format("SAVE | tick {} | {} bytes", m_universe.Tick(), file.size()));
}

std::uint64_t ShardApp::Run()
{
  Neuron::FrameClock frameClock;
  Say(std::format("RUNNING | {:.0f} Hz | saving every {} ticks | stop with Ctrl+C", 1.0f / m_host.TickDt(), m_config.saveEveryTicks));

  while (!StopRequested())
  {
    // The engine's accumulator, driven by real time and nothing else. There is no window to pump and
    // no frame to be in step with, which is the claim this slice exists to test -- ServerHost was
    // written as a fixed-rate loop over a Simulation and nothing in it refers to either.
    // Advance RUNS the ticks and reports how many it ran -- it is not a request for permission to.
    // The game's loop reads the count to pump the network once per tick; a shard has nothing to do
    // per tick this slice, so the count is only what decides whether to yield below.
    const int steps = m_host.Advance(frameClock.Tick());

    // Between ticks, which is the codec's whole contract: Advance ran every tick this pass owed and
    // the next cannot start until the next Advance, so the universe is at rest here.
    if (m_config.saveEveryTicks != 0 && m_universe.Tick() - m_lastSaveTick >= m_config.saveEveryTicks)
      SaveUniverse();

    // A server with nothing to do must not spin a core. One millisecond is a sixteenth of a tick at
    // 60 Hz, so it can never cost one, and this is the only place this program yields.
    if (steps == 0)
      ::Sleep(1);
  }

  Say(std::format("STOPPING | tick {}", m_universe.Tick()));
  SaveUniverse(); // a deliberate stop loses nothing, where a kill loses the ticks since the last save
  return m_universe.Tick();
}
} // namespace Shard
