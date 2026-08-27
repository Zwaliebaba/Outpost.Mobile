#include "pch.h"

#include "Gfx.h"
#include "Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
Gfx g_gfx;
Scene g_scene;
bool g_running = true;
uint64_t g_simTick = 0;
bool g_sawPointerWheel = false; // once the pointer API delivers the wheel, ignore the legacy message

float g_timeScale = 1.0f;

int64_t NowQpc()
{
  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

// WM_POINTER covers mouse, pen and touch with one path, which is the whole reason for using it:
// the same build works on a desktop and on a tablet with no second code path.
bool DecodePointer(HWND _hwnd, WPARAM _wparam, PointerEvent& _event)
{
  const UINT32 pointerId = GET_POINTERID_WPARAM(_wparam);
  POINTER_INFO info = {};
  if (!GetPointerInfo(pointerId, &info))
    return false;
  POINT point = info.ptPixelLocation;
  ScreenToClient(_hwnd, &point);

  _event.pointerId = pointerId;
  _event.xPx = static_cast<float>(point.x);
  _event.yPx = static_cast<float>(point.y);
  _event.isTouch = info.pointerType == PT_TOUCH || info.pointerType == PT_PEN;
  _event.buttons = 0;
  _event.buttons |= (info.pointerFlags & POINTER_FLAG_FIRSTBUTTON) ? 1u : 0u;
  _event.buttons |= (info.pointerFlags & POINTER_FLAG_SECONDBUTTON) ? 2u : 0u;
  _event.buttons |= (info.pointerFlags & POINTER_FLAG_THIRDBUTTON) ? 4u : 0u;
  _event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  // PerformanceCount is when the hardware reported the contact; it is not always populated, so
  // fall back to now.
  _event.timestampQpc = info.PerformanceCount != 0 ? static_cast<int64_t>(info.PerformanceCount) : NowQpc();
  return true;
}

// Framerate-independent easing, used for everything that eases, HUD readouts included.
float SmoothTowards(float _current, float _target, float _dtSec, float _halfLifeSec)
{
  if (_halfLifeSec <= 0.0f)
    return _target;
  const float t = 1.0f - std::exp2(-_dtSec / _halfLifeSec);
  return _current + (_target - _current) * t;
}

LRESULT CALLBACK WndProc(HWND _hwnd, UINT _msg, WPARAM _wparam, LPARAM _lparam)
{
  switch (_msg)
  {
  case WM_SIZE:
    if (_wparam != SIZE_MINIMIZED)
      g_gfx.Resize(LOWORD(_lparam), HIWORD(_lparam));
    return 0;

  case WM_POINTERDOWN:
  case WM_POINTERUPDATE:
  case WM_POINTERUP:
  case WM_POINTERCAPTURECHANGED:
  {
    PointerEvent event;
    if (!DecodePointer(_hwnd, _wparam, event))
      return 0;
    if (_msg == WM_POINTERDOWN)
    {
      event.kind = PointerEvent::Kind::Down;
      if (!event.isTouch)
        SetCapture(_hwnd); // so a mouse drag that leaves the window still reports
    }
    else if (_msg == WM_POINTERUPDATE)
      event.kind = PointerEvent::Kind::Update;
    else
    {
      event.kind = PointerEvent::Kind::Up;
      if (!event.isTouch) // touch contacts are captured implicitly, per contact
        ReleaseCapture();
    }
    g_scene.QueuePointerEvent(event);
    return 0;
  }

  case WM_POINTERWHEEL:
  {
    PointerEvent event;
    if (!DecodePointer(_hwnd, _wparam, event))
      return 0;
    g_sawPointerWheel = true;
    event.kind = PointerEvent::Kind::Wheel;
    event.wheelNotches = GET_WHEEL_DELTA_WPARAM(_wparam) / WHEEL_DELTA;
    g_scene.QueuePointerEvent(event);
    return 0;
  }

  case WM_MOUSEWHEEL: // only reached where WM_POINTERWHEEL is not delivered
  {
    if (g_sawPointerWheel)
      return 0;
    PointerEvent event;
    event.kind = PointerEvent::Kind::Wheel;
    event.wheelNotches = GET_WHEEL_DELTA_WPARAM(_wparam) / WHEEL_DELTA;
    event.timestampQpc = NowQpc();
    g_scene.QueuePointerEvent(event);
    return 0;
  }

  case WM_POINTERLEAVE:
    g_scene.m_hoverShip = -1;
    return 0;

  case WM_DPICHANGED:
  {
    auto suggested = reinterpret_cast<const RECT*>(_lparam);
    SetWindowPos(_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }

  case WM_KEYDOWN:
    if (_wparam == VK_ESCAPE)
    {
      // Drops the selection first; only quits once nothing is selected.
      if (g_scene.SelectedCount() > 0)
        g_scene.ClearSelection();
      else
        PostMessageW(_hwnd, WM_CLOSE, 0, 0);
    }
    else if (_wparam == VK_F3)
      g_scene.TriggerCameraShake(); // the debug hook, so the shake curve can be tuned on demand
    else if (_wparam == '1')
      g_timeScale = 0.25f;
    else if (_wparam == '2')
      g_timeScale = 1.0f;
    else if (_wparam == '3')
      g_timeScale = 4.0f;
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

  wchar_t filename[MAX_PATH];
  GetModuleFileNameW(nullptr, filename, MAX_PATH);
  auto path = std::wstring(filename);
  path = path.substr(0, path.find_last_of('\\'));

  FileSys::SetHomeDirectory(path);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = _instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"ShipFeelWindow";
  RegisterClassExW(&wc);

  constexpr DWORD style = WS_OVERLAPPEDWINDOW;
  RECT rect = {0, 0, 1600, 900};
  AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());

  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ShipFeel", style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                              rect.bottom - rect.top, nullptr, nullptr, _instance, nullptr);
  if (!hwnd)
    FatalHr("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));

  // Mouse arrives as WM_POINTER too, so one input path covers mouse and touch.
  EnableMouseInPointer(TRUE);

  g_gfx.Init(hwnd);
  g_scene.Init(g_gfx);
  ShowWindow(hwnd, SW_SHOW);

  LARGE_INTEGER qpcFreq = {};
  QueryPerformanceFrequency(&qpcFreq);
  LARGE_INTEGER qpcPrev = {};
  QueryPerformanceCounter(&qpcPrev);

  float fpsSmoothed = 0.0f;
  float frameMsSmoothed = 0.0f;
  float simAccumulator = 0.0f;

  while (g_running)
  {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_QUIT)
        g_running = false;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!g_running)
      break;

    LARGE_INTEGER qpcNow = {};
    QueryPerformanceCounter(&qpcNow);
    const float dtSec = static_cast<float>(double(qpcNow.QuadPart - qpcPrev.QuadPart) / double(qpcFreq.QuadPart));
    qpcPrev = qpcNow;

    fpsSmoothed = SmoothTowards(fpsSmoothed, dtSec > 0.0f ? 1.0f / dtSec : 0.0f, dtSec, 0.25f);
    frameMsSmoothed = SmoothTowards(frameMsSmoothed, dtSec * 1000.0f, dtSec, 0.25f);

    if (g_gfx.m_widthPx == 0 || g_gfx.m_heightPx == 0)
      continue;

    g_scene.Update(g_gfx.m_widthPx, g_gfx.m_heightPx);

    // Fixed 60 Hz simulation with an accumulator; the leftover fraction interpolates the render.
    // Capped so a stall (a dragged window, a breakpoint) cannot spiral into a burst of ticks.
    simAccumulator = std::min(simAccumulator + dtSec * g_timeScale, 0.25f);
    while (simAccumulator >= SIM_DT)
    {
      g_scene.Step();
      simAccumulator -= SIM_DT;
      ++g_simTick;
    }

    const float simAlpha = simAccumulator / SIM_DT;

    // Rings, banking, thrusters, markers and the camera all ease on real time rather than sim
    // time, so they stay smooth however far the swapchain runs ahead of 60 Hz.
    g_scene.UpdateFeedback(dtSec);

    g_gfx.BeginFrame(g_scene.SkyColour());
    g_scene.Render(g_gfx, simAlpha);

    const float hudScale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    char hud[512] = {};
    std::snprintf(hud, sizeof(hud),
                  "fps       %6.1f\nframe     %6.2f ms\nsim tick  %6llu\nselected  %6d of %zu\ntime      %5.2fx",
                  static_cast<double>(fpsSmoothed), static_cast<double>(frameMsSmoothed), static_cast<unsigned long long>(g_simTick),
                  g_scene.SelectedCount(), g_scene.m_ships.size(), static_cast<double>(g_timeScale));
    g_gfx.DrawTextLine(12.0f * hudScale, 10.0f * hudScale, hudScale, Rgba{0.78f, 0.87f, 0.96f, 1.0f}, hud);

    g_gfx.EndFrame();
  }

  g_gfx.Shutdown();
  return 0;
}
