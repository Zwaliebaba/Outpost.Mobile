#include "pch.h"

#include "ShardApp.h"

#include <cstdio>
#include <cstdlib>

namespace
{
Shard::ShardApp* g_app = nullptr;

// Ctrl+C, Ctrl+Break and the console closing all mean the same thing here: finish the tick, save,
// exit. The handler runs on a thread of its own and does exactly one thing, which is why the flag it
// sets is the only atomic in this program.
BOOL WINAPI OnConsoleSignal(DWORD _signal)
{
  // Returned as 1 and 0 rather than TRUE and FALSE, which is not style: CheckProjectFiles bans those
  // two identifiers appearing after a type, and `return FALSE;` reads that way to a grep. The rule is
  // right to be conservative about them and this file has no business being the exception.
  const bool ours = _signal == CTRL_C_EVENT || _signal == CTRL_BREAK_EVENT || _signal == CTRL_CLOSE_EVENT;
  if (ours && g_app != nullptr)
    g_app->RequestStop();
  return ours ? 1 : 0;
}
} // namespace

// The shard server's entry point.
//
// **It reads argv, and the game still does not.** AGENTS.md 5 bans argv for the game and exempts a
// Tools/ command line on the argument that a library must not reach around its caller for
// configuration while a command-line tool IS its caller. A server is its caller in the same sense,
// and the alternative -- the shard number in Server.cfg -- means one near-identical config file per
// shard differing in one integer, and the day one is edited and another is not, two shards believe
// they are the same shard and both write Universe.2.sav
// (Design/Decisions/0067, Design/ShardServer-slice-1.md 3).
int wmain(int _argc, wchar_t** _argv)
{
  if (_argc > 1 && (std::wcscmp(_argv[1], L"--help") == 0 || std::wcscmp(_argv[1], L"-h") == 0))
  {
    std::printf("Server [shard]\n"
                "  shard  which shard to run, default 0. Shard 0 reads Universe.sav and shard n reads\n"
                "         Universe.n.sav, which is what Tools/UniverseGen writes.\n"
                "\n"
                "Reads Server.cfg beside itself if there is one. Saves on the configured cadence and\n"
                "once more on Ctrl+C.\n");
    return 0;
  }

  unsigned long shard = 0;
  if (_argc > 1)
  {
    wchar_t* end = nullptr;
    shard = std::wcstoul(_argv[1], &end, 10);
    if (end == _argv[1] || *end != L'\0' || shard > 0xFFFFu)
    {
      std::printf("Server: the shard must be a number from 0 to 65535\n");
      return 2;
    }
  }

  Shard::ShardApp app;
  g_app = &app;
  SetConsoleCtrlHandler(OnConsoleSignal, TRUE);

  if (!app.Boot(static_cast<std::uint16_t>(shard)))
    return 1;
  (void)app.Run();
  return 0;
}
