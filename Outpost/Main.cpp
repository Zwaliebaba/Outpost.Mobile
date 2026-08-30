#include "pch.h"

#include "OutpostApp.h"

// The entry point, and the one exception handler in the tree.
//
// Everything below throws rather than reporting: check_hresult for an HRESULT that had to succeed,
// throw_last_error for a Win32 call that had to succeed, ASSERT for a broken invariant. They all
// arrive here, which is the only place that knows there is a person watching and a window to put a
// message box over. A library that caught its own errors would have to decide what to do about
// them, and there is nothing it could sensibly decide.
//
// There is no argv and there are no environment variables, deliberately (see AGENTS.md 5): the
// composition root is the only thing that reads configuration, and it hands libraries plain structs.

int WINAPI wWinMain(HINSTANCE _instance, HINSTANCE, LPWSTR, int)
{
  Outpost::OutpostApp app;
  try
  {
    // Assets sit beside the executable, which is what the MSIX package lays out.
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    std::wstring directory(modulePath);
    directory = directory.substr(0, directory.find_last_of(L'\\'));
    Neuron::FileSys::SetHomeDirectory(directory);

    app.Init(_instance);
    app.Run();
    app.Shutdown();
    return 0;
  }
  catch (const winrt::hresult_error& error)
  {
    app.Shutdown();
    const winrt::hstring message = error.message();
    Neuron::DebugTrace(L"fatal: {}\n", std::wstring_view(message));
    MessageBoxW(nullptr, message.c_str(), L"Outpost: Frontier", MB_OK | MB_ICONERROR);
    return 1;
  }
  catch (const std::exception& error)
  {
    app.Shutdown();
    Neuron::DebugTrace("fatal: {}\n", error.what());
    wchar_t wide[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, wide, static_cast<int>(std::size(wide)));
    MessageBoxW(nullptr, wide, L"Outpost: Frontier", MB_OK | MB_ICONERROR);
    return 1;
  }
}
