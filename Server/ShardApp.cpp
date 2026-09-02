#include "pch.h"
#include "ShardApp.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <span>
#include <string>

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

  // The links, from the universe's own gates rather than from a layout this process does not have
  // (Design/ShardServer-slice-3.md 2.1). None of them has a transport yet, so none of them pumps
  // anything; slice 4 is what dials a neighbour.
  const std::uint32_t neighbours = m_links.Open(m_universe);
  if (neighbours == 0)
  {
    Say("LINKS | none | this shard borders no other, so nothing can cross");
  }
  else
  {
    std::string peers;
    for (std::uint32_t at = 0; at < neighbours; ++at)
      peers += std::format("{}{}", at ? ", " : "", static_cast<unsigned>(m_links.PeerAt(at)));
    Say(std::format("LINKS | {} | shard {} borders {} | no transport yet, so nothing crosses", neighbours, static_cast<unsigned>(m_shard),
                    peers));
  }

  // Last, because a port bound before the universe was read is a port held open by a process about
  // to exit -- and because everything above can fail without anything to close.
  return OpenListener();
}

bool ShardApp::OpenListener()
{
  // The development placeholder, and the reason it is set here rather than anywhere else: the
  // certificate this listener presents is one the process generated moments ago, and there is no
  // trust store to check it against. The connection is encrypted and it is not authenticated
  // (ADR 0023). A server on a real network needs a real certificate, and this is the line that has
  // to change when one exists.
  Neuron::QuicApi::Desc quicDesc;
  if (!m_quic.Open(quicDesc))
  {
    Say(std::format("QUIC REFUSED | the library would not open | {}", m_quic.Reason()));
    return false;
  }

  Neuron::QuicListener::Desc listenerDesc;
  listenerDesc.backlog = m_config.backlog;
  if (!m_listener.Start(m_quic, m_config.port, listenerDesc))
  {
    Say(std::format("PORT REFUSED | {} | {}", static_cast<unsigned>(m_config.port), m_listener.Reason()));
    m_quic.Close();
    return false;
  }

  m_listening = true;
  // Once, here, so PumpSessions allocates nothing per pass. Both are bounded by the backlog: the
  // listener never reports more live connections than it has slots, and a refusal is one of them.
  m_liveScratch.reserve(m_config.backlog);
  m_refusedScratch.reserve(m_config.backlog);
  Say(std::format("LISTENING | 127.0.0.1:{} | backlog {} | one session is served and the rest are refused",
                  static_cast<unsigned>(m_listener.Port()), static_cast<unsigned>(m_config.backlog)));
  return true;
}

void ShardApp::PumpSessions()
{
  if (!m_listening)
    return;

  m_listener.Poll();
  const std::span<Neuron::QuicTransport* const> accepted = m_listener.Accepted();

  // Every connection the listener still calls live gets polled, whether this server serves it or
  // refused it: a transport nobody polls never finishes the close it was asked for, and its slot is
  // never recycled.
  for (Neuron::QuicTransport* transport : accepted)
    transport->Poll();

  m_liveScratch.assign(accepted.begin(), accepted.end());
  const auto isLive = [this](const Neuron::Transport* _transport)
  { return std::find(m_liveScratch.begin(), m_liveScratch.end(), _transport) != m_liveScratch.end(); };

  // Gone first. A session whose transport has left Accepted() has had its slot recycled, and the
  // pointer this server holds names whatever the pool hands out next -- so it is dropped by
  // comparing against the span rather than by asking the transport anything (QuicListener.h).
  while (m_simulation.SessionCount() != 0)
  {
    Neuron::Transport* held = m_simulation.SessionTransportAt(0);
    if (isLive(held))
      break; // one session, so the first is the only one; a loop here is what lifts with the ceiling
    (void)m_simulation.CloseSession(held);
    Say(std::format("SESSION CLOSED | tick {} | {} subscriber(s)", m_universe.Tick(), m_simulation.SessionCount()));
  }

  // A refusal is forgotten once the listener has finished with the connection, and not before.
  std::erase_if(m_refusedScratch, [&isLive](const Neuron::Transport* _transport) { return !isLive(_transport); });

  // Then arrived. One is taken and the rest are closed, which is this slice's decision and not a
  // limit of the listener: there is no login, so every session would be OWNER_LOCAL and two clients
  // would give orders to one fleet (Design/ShardServer-slice-2.md 2.5).
  for (Neuron::QuicTransport* transport : accepted)
  {
    if (m_simulation.HasSessionOn(transport))
      continue;
    if (std::find(m_refusedScratch.begin(), m_refusedScratch.end(), transport) != m_refusedScratch.end())
      continue; // already refused and already closing; saying so again every pass is a log flood
    if (transport->State() != Neuron::ConnectionState::Connected)
      continue; // still handshaking; it is not a session until it can carry a datagram

    if (m_simulation.OpenSession(*transport, m_config))
    {
      Say(std::format("SESSION OPEN | tick {} | {} subscriber(s)", m_universe.Tick(), m_simulation.SessionCount()));
      continue;
    }

    Say(std::format("SESSION REFUSED | tick {} | this shard serves one session and has one | there is no login yet", m_universe.Tick()));
    m_refusedScratch.push_back(transport);
    transport->Close(); // its slot returns to the pool on a later Poll, so a refusal costs nothing lasting
  }
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

  // **Here, and nowhere else.** An acknowledgement asserts the entry is in this shard's FILE, so
  // nothing is durable until a write has actually succeeded (ADR 0066). The refusal above returns
  // before this line and that is the whole guard: a link told it was durable when the save was
  // refused would acknowledge a fleet into a file that does not contain it, and the departing shard
  // would then forget an entry nothing holds.
  m_links.NoteDurableThrough(m_universe.Tick());
  Say(std::format("SAVE | tick {} | {} bytes", m_universe.Tick(), file.size()));
}

std::uint64_t ShardApp::Run()
{
  Neuron::FrameClock frameClock;
  Say(std::format("RUNNING | {:.0f} Hz | saving every {} ticks | stop with Ctrl+C", 1.0f / m_host.TickDt(), m_config.saveEveryTicks));

  while (!StopRequested())
  {
    // Before the ticks, and once per PASS rather than once per tick. Accepting is not a tick's work,
    // and a pass that runs no ticks must still take a connection or a client that dials during a
    // quiet moment waits for the next one (Design/ShardServer-slice-2.md 2.4).
    PumpSessions();

    // The engine's accumulator, driven by real time and nothing else. There is no window to pump and
    // no frame to be in step with, which is the claim slice 1 existed to test -- ServerHost was
    // written as a fixed-rate loop over a Simulation and nothing in it refers to either.
    // Advance RUNS the ticks and reports how many it ran -- it is not a request for permission to.
    // Every tick it runs applies this pass's orders and publishes to every session, because that is
    // what ShardSimulation::Step does; nothing here reaches a subscriber directly.
    const int steps = m_host.Advance(frameClock.Tick());

    // After the ticks, which is the order Design/ShardServer.md 3.5 states: a handoff received here
    // lands in the inbox and is drained by the first tick of the next pass, on a stated tick. Once
    // per pass and not per tick, for the same reason the listener is: a transport's timing has no
    // business inside the replay contract.
    m_links.Pump(m_universe);

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

  // The listener first, which waits for its own callbacks and then closes every connection it
  // accepted; the registration only after, because it cannot close over a live connection
  // (QuicListener.h). The sessions are dropped before either, so nothing publishes into a transport
  // that is being torn down.
  if (m_listening)
  {
    while (m_simulation.SessionCount() != 0)
      (void)m_simulation.CloseSession(m_simulation.SessionTransportAt(0));
    m_listener.Stop();
    m_quic.Close();
    m_listening = false;
  }
  return m_universe.Tick();
}
} // namespace Shard
