#include "Gfx.h"
#include "Scene.h"
#include "Tuning.h"

#include <cmath>
#include <cstdio>

namespace
{

Gfx g_gfx;
Scene g_scene;
bool g_running = true;

// Framerate-independent easing, used for everything that eases, HUD readouts included.
float SmoothTowards(float _current, float _target, float _dtSec, float _halfLifeSec)
{
  if (_halfLifeSec <= 0.0f)
  {
    return _target;
  }
  const float t = 1.0f - std::exp2(-_dtSec / _halfLifeSec);
  return _current + (_target - _current) * t;
}

LRESULT CALLBACK WndProc(HWND _hwnd, UINT _msg, WPARAM _wparam, LPARAM _lparam)
{
  switch (_msg)
  {
  case WM_SIZE:
    if (_wparam != SIZE_MINIMIZED)
    {
      g_gfx.Resize(LOWORD(_lparam), HIWORD(_lparam));
    }
    return 0;

  case WM_DPICHANGED:
  {
    const RECT* suggested = reinterpret_cast<const RECT*>(_lparam);
    SetWindowPos(_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                 suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }

  case WM_KEYDOWN:
    if (_wparam == VK_ESCAPE)
    {
      PostMessageW(_hwnd, WM_CLOSE, 0, 0);
    }
    else if (_wparam == VK_F2)
    {
      TuningToggleWindow();
    }
    return 0;

  case WM_CLOSE:
    g_running = false;
    return 0;

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;

  default:
    break;
  }
  return DefWindowProcW(_hwnd, _msg, _wparam, _lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE _instance, HINSTANCE, LPWSTR, int)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = _instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"ShipFeelWindow";
  RegisterClassExW(&wc);

  const DWORD style = WS_OVERLAPPEDWINDOW;
  RECT rect = {0, 0, 1600, 900};
  AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());

  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ShipFeel", style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                              rect.bottom - rect.top, nullptr, nullptr, _instance, nullptr);
  if (!hwnd)
  {
    FatalHr("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
  }

  // Mouse arrives as WM_POINTER too, so one input path covers mouse and touch.
  EnableMouseInPointer(TRUE);

  g_gfx.Init(hwnd);
  TuningInit(_instance, hwnd); // before the scene: ship placement reads the tuning values
  g_scene.Init(g_gfx);
  ShowWindow(hwnd, SW_SHOW);

  LARGE_INTEGER qpcFreq = {};
  QueryPerformanceFrequency(&qpcFreq);
  LARGE_INTEGER qpcPrev = {};
  QueryPerformanceCounter(&qpcPrev);

  float fpsSmoothed = 0.0f;
  float frameMsSmoothed = 0.0f;
  uint64_t frameCount = 0;

  while (g_running)
  {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_QUIT)
      {
        g_running = false;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!g_running)
    {
      break;
    }

    TuningPoll(); // picks up an edited tuning.ini and refreshes the sliders

    LARGE_INTEGER qpcNow = {};
    QueryPerformanceCounter(&qpcNow);
    const float dtSec = float(double(qpcNow.QuadPart - qpcPrev.QuadPart) / double(qpcFreq.QuadPart));
    qpcPrev = qpcNow;

    fpsSmoothed = SmoothTowards(fpsSmoothed, dtSec > 0.0f ? 1.0f / dtSec : 0.0f, dtSec, 0.25f);
    frameMsSmoothed = SmoothTowards(frameMsSmoothed, dtSec * 1000.0f, dtSec, 0.25f);
    ++frameCount;

    if (g_gfx.m_widthPx == 0 || g_gfx.m_heightPx == 0)
    {
      continue;
    }

    g_gfx.BeginFrame(Rgba{g_tuning.skyColourR, g_tuning.skyColourG, g_tuning.skyColourB, 1.0f});
    g_scene.Render(g_gfx);

    const float hudScale = float(GetDpiForWindow(hwnd)) / 96.0f;
    char hud[256] = {};
    std::snprintf(hud, sizeof(hud), "fps      %6.1f\nframe    %6.2f ms\nviewport %u x %u\nships    %zu\nframes   %llu",
                  double(fpsSmoothed), double(frameMsSmoothed), g_gfx.m_widthPx, g_gfx.m_heightPx, g_scene.m_ships.size(),
                  static_cast<unsigned long long>(frameCount));
    g_gfx.DrawTextLine(12.0f * hudScale, 10.0f * hudScale, hudScale, Rgba{0.78f, 0.87f, 0.96f, 1.0f}, hud);

    g_gfx.EndFrame();
  }

  TuningShutdown();
  g_gfx.Shutdown();
  return 0;
}
