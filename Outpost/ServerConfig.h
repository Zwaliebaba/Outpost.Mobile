#pragma once

#include "InterestSet.h"
#include "Publisher.h"
#include "ShipState.h"

#include "QuicListener.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Outpost
{
// What a composition root is told to be, as against what it compiles in.
//
// AGENTS.md 5 bans argv and the environment and says configuration is loaded by the composition
// root only; a file read there is exactly that, and not an exception to it (ADR 0043). Libraries go
// on taking plain Desc structs and none of them has ever seen a file.
//
// Every default below is taken from the library's own Desc{} rather than spelled again, so a
// default that moves in SimTuning.h or Publisher.h moves here with it and nobody has to remember to
// follow. The port is the exception, because it had no library to belong to: it was a constant in
// OutpostApp.cpp and this is now where it lives.
//
// A plain aggregate, so the fields carry no m_ (AGENTS.md 1 R8).
struct ServerConfig
{
  // Arbitrary and unregistered, on 127.0.0.1 only. Since the fallback link went (ADR 0028) this
  // number can be the reason the game does not start, which is why the boot failure names it.
  std::uint16_t port = 30081;

  // Transports pre-allocated to hand accepted connections to. One is enough for an executable that
  // is its own client; a dedicated server sizes this at the concurrency it means to carry, which is
  // what the recycling in ADR 0031 made a number rather than a rewrite.
  std::uint32_t backlog = Neuron::QuicListener::Desc{}.backlog;

  // What one subscriber sees and how often, and how much of its send rate it may convert into
  // server CPU. Per subscriber rather than global, which is the shape Publisher::Desc already has.
  float interestRadiusMetres = Game::InterestSet::Desc{}.radiusMetres;
  std::uint32_t interestUpdateEveryTicks = Game::InterestSet::Desc{}.updateEveryTicks;
  std::uint32_t ordersPerTick = Game::Publisher::Desc{}.ordersPerTick;

  // How often the universe is written to disk, in ticks. 1800 is thirty seconds at 60 Hz.
  //
  // It is here rather than compiled in for ADR 0043's reason exactly: how much progress a deployment
  // is willing to lose to a power cut is a property of the deployment, and nothing about the game
  // changes when it moves. It has no library Desc to default from -- the save is the composition
  // root's, because nothing in GameLogic or NeuronCore opens a file -- so, like the port, this is
  // where the number lives.
  //
  // 0 disables the PERIODIC save; the save at clean shutdown still happens. A period of zero has no
  // other sensible reading, and "never save at all" is a missing file rather than a setting.
  std::uint32_t saveEveryTicks = 1800;
};

// The file the root reads, relative to the home directory -- so it resolves under <exe>\Assets\ the
// way every mesh and font does (FileSys::ResolvePath).
inline constexpr const wchar_t* SERVER_CONFIG_FILE = L"Server.cfg";

// The universe on disk, resolved the same way and for the same reason: the composition root is the
// only thing in this tree that names a file, so the two names it knows live together.
//
// Under Assets\ because that is where ResolvePath puts a relative name and there is nowhere else
// yet. That is wrong for a real install -- a read-only program directory cannot be saved into -- and
// it is deliberately one constant, so the day there is a writable data directory this line moves and
// nothing else does (Design/Universe-slice-5.md 6).
inline constexpr const wchar_t* UNIVERSE_SAVE_FILE = L"Universe.sav";

// Reads `key = value` lines into _outConfig. `#` begins a comment; blank lines are ignored; both
// sides of the `=` are trimmed.
//
// Returns false and a message naming the line and the reason on ANY refusal -- an unknown key, a
// duplicate one, a missing `=`, a value that is not a number, one outside the key's range, or
// trailing rubbish after one. It never throws and never asserts, which is AGENTS.md 5's rule for
// anything that parses content.
//
// **Nothing is applied on a refusal**: _outConfig is untouched, so half the file and half the
// defaults -- with nothing saying which -- is a state that cannot exist. That is what "fails closed"
// means for a configuration file, and it is the whole reason this returns a bool rather than
// repairing what it can (Design/Archive/ServerConfig-work-order.md 2.1).
//
// What to DO about a refusal is the caller's, and deliberately not this function's: a game with a
// window logs it and boots on the defaults, because a typo in a tuning file should not be a black
// screen, while a headless root would print and exit non-zero. Same parser, two roots.
[[nodiscard]] bool ParseServerConfig(std::string_view _text, ServerConfig& _outConfig, std::string& _outError);
} // namespace Outpost
