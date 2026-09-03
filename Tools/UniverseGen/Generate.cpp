#include "pch.h"

#include <charconv>
#include <cstdio>
#include <string>
#include <vector>

// UniverseGen: writes a universe for the game to run.
//
// A universe is content, and content is authored rather than discovered at boot. Until slice 5b the
// game generated one on its first run, which worked because there was one program; a dedicated
// server has no business running genesis either, and two programs generating "the same" universe is
// two chances to disagree (ADR 0058).
//
// So this is the one thing in the tree that authors a universe, and Outpost only ever loads one.
//
// It reads argv, which the game may not (AGENTS.md 5). The rule is about the game and the reason is
// that a library must not reach around its caller for configuration; a command-line tool IS its
// caller, and a generator you cannot point at a seed is a generator you have to rebuild to use.
namespace
{
constexpr const wchar_t* USAGE = L"UniverseGen -- writes a universe file for Outpost: Frontier.\n"
                                 L"\n"
                                 L"  UniverseGen [seed] [path] [shards]\n"
                                 L"\n"
                                 L"  seed    the galaxy seed, decimal or 0x-prefixed hex. Default: the shipped galaxy.\n"
                                 L"  path    where to write it. A bare name resolves under this tool's own Assets directory;\n"
                                 L"          a rooted path is taken as it stands. Default: Universe.sav.\n"
                                 L"  shards  how many shards to split the galaxy into. Default: 1, the shipped game.\n"
                                 L"\n"
                                 L"One shard writes one file at the path given, exactly as it always did. More than one writes\n"
                                 L"a file per shard, numbered before the extension -- Universe.0.sav, Universe.1.sav -- because a\n"
                                 L"shard's universe is a different universe and not a version of one. Every shard is built in the\n"
                                 L"same pass: a gate leading to another shard carries that shard's identity for its far end, and\n"
                                 L"the only way to know one is to have built it.\n"
                                 L"\n"
                                 L"A count that would leave a shard with no system is refused, and says which: the partition is a\n"
                                 L"band of lattice columns, so a count near the lattice's width can cut a band that holds nothing.\n"
                                 L"\n"
                                 L"The tool and the game are two executables in two output directories, so a bare name lands\n"
                                 L"beside the TOOL and not where the game will look. To feed a build of the game, give it the\n"
                                 L"game's path:\n"
                                 L"\n"
                                 L"  UniverseGen 0 \"...\\Outpost\\Assets\\Universe.sav\"\n"
                                 L"\n"
                                 L"The write is atomic: a sibling temporary renamed over the target, so an interruption leaves\n"
                                 L"whatever was there before rather than half of a new universe.\n";

// Decimal, or hex behind an 0x. from_chars for both, so a value that is not a number is a refusal
// rather than a zero -- strtoull's habit of returning 0 for rubbish is exactly the failure a seed
// must not have, because the resulting galaxy would look perfectly plausible.
[[nodiscard]] bool ReadSeed(const std::wstring& _text, std::uint64_t& _outSeed)
{
  std::string narrow;
  narrow.reserve(_text.size());
  for (const wchar_t at : _text)
  {
    if (at > 127)
      return false;
    narrow.push_back(static_cast<char>(at));
  }

  std::string_view digits = narrow;
  int base = 10;
  if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
  {
    digits.remove_prefix(2);
    base = 16;
  }

  const char* const begin = digits.data();
  const char* const end = begin + digits.size();
  const std::from_chars_result result = std::from_chars(begin, end, _outSeed, base);
  return result.ec == std::errc{} && result.ptr == end;
}

// Where one shard's file goes. One shard writes the path it was given and nothing else, so every
// existing invocation means exactly what it meant; more than one numbers the file before its
// extension, because a shard's universe is a different universe rather than a version of one.
[[nodiscard]] std::wstring ShardPath(const std::wstring& _path, std::uint32_t _shard, std::uint32_t _count)
{
  if (_count <= 1)
    return _path;

  const std::size_t dot = _path.find_last_of(L'.');
  const std::size_t slash = _path.find_last_of(L"\\/");
  const bool hasExtension = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
  const std::wstring number = L"." + std::to_wstring(_shard);
  return hasExtension ? _path.substr(0, dot) + number + _path.substr(dot) : _path + number;
}
} // namespace

int wmain(int _argc, wchar_t** _argv)
{
  // Assets sit beside the executable, the same rule the game follows (Neuron::FileSys::ResolvePath)
  // -- but THIS executable, which is not the game. A bare name therefore lands beside the tool, and
  // feeding a build of the game means passing its path. Said here because it is the one thing about
  // this tool that surprises people, and USAGE says it too.
  wchar_t modulePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
  std::wstring directory(modulePath);
  directory = directory.substr(0, directory.find_last_of(L'\\'));
  Neuron::FileSys::SetHomeDirectory(directory);

  std::uint64_t seed = Game::STARTING_GALAXY_SEED;
  std::wstring path = Game::UNIVERSE_SAVE_FILE;
  std::uint64_t shardCount = 1;

  for (int at = 1; at < _argc; ++at)
  {
    const std::wstring argument = _argv[at];
    if (argument == L"-h" || argument == L"--help" || argument == L"/?")
    {
      std::fputws(USAGE, stdout);
      return 0;
    }
    if (at == 1)
    {
      if (!ReadSeed(argument, seed))
      {
        std::fwprintf(stderr, L"UniverseGen: '%ls' is not a seed.\n\n", argument.c_str());
        std::fputws(USAGE, stderr);
        return 1;
      }
      continue;
    }
    if (at == 2)
    {
      path = argument;
      continue;
    }
    if (at == 3)
    {
      if (!ReadSeed(argument, shardCount) || shardCount == 0 || shardCount > 0xFFFFu)
      {
        std::fwprintf(stderr, L"UniverseGen: '%ls' is not a shard count.\n\n", argument.c_str());
        std::fputws(USAGE, stderr);
        return 1;
      }
      continue;
    }
    std::fputws(L"UniverseGen: too many arguments.\n\n", stderr);
    std::fputws(USAGE, stderr);
    return 1;
  }

  // The galaxy, then everything that stands in it. Both pure functions of the seed, which is what
  // makes this tool's output reproducible: the same seed writes the same file, on any machine and
  // any build (ADR 0055, ADR 0058).
  Game::GalaxyDesc desc = Game::STARTING_GALAXY;
  desc.shardCount = static_cast<std::uint32_t>(shardCount);

  const Game::GalaxyLayout galaxy = Game::LayOutGalaxy(seed, Game::UniversePos{}, desc, Game::GALAXY_PINS);

  // Refused before anything is written, and against the layout that was actually drawn rather than
  // against a ceiling: occupancy is not monotonic in the count, so only the drawn galaxy can answer
  // (Game::OccupiedShardCount). A shard with no system is a process that would boot with nothing.
  const std::uint32_t occupied = Game::OccupiedShardCount(galaxy.systems, desc);
  if (occupied != desc.shardCount)
  {
    std::fwprintf(stderr, L"UniverseGen: %u shards would leave %u of them with no system in a galaxy of %zu.\n", desc.shardCount,
                  desc.shardCount - occupied, galaxy.systems.size());
    std::fwprintf(stderr, L"             The partition is a band of lattice columns; try a smaller count.\n");
    return 1;
  }

  std::vector<Game::Universe> shards(desc.shardCount);
  Game::BuildStartingGalaxy(galaxy, desc, shards);

  std::wprintf(L"  seed      0x%016llX\n", static_cast<unsigned long long>(seed));
  std::wprintf(L"  systems   %zu\n", galaxy.systems.size());
  std::wprintf(L"  shards    %u\n", desc.shardCount);

  for (std::uint32_t at = 0; at < desc.shardCount; ++at)
  {
    Game::SaveHeader header;
    header.galaxySeed = seed;
    header.shard = shards[at].Shard();
    header.shardCount = desc.shardCount;

    std::vector<std::uint8_t> file;
    Game::WriteSaveFile(shards[at], header, file);

    const std::wstring target = ShardPath(path, at, desc.shardCount);
    if (!Neuron::BinaryFile::WriteFileAtomic(target, file))
    {
      std::fwprintf(stderr, L"UniverseGen: could not write %ls\n", Neuron::FileSys::ResolvePath(target).c_str());
      return 1;
    }

    std::wprintf(L"%ls\n", Neuron::FileSys::ResolvePath(target).c_str());
    std::wprintf(L"  gates     %u\n", shards[at].GateCount());
    std::wprintf(L"  stations  %u\n", shards[at].StationCount());
    std::wprintf(L"  ships     %u\n", shards[at].ShipCount());
    std::wprintf(L"  bytes     %zu\n", file.size());
  }
  return 0;
}
