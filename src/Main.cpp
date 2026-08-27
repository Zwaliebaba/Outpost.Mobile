#include "Gfx.h"
#include "Scene.h"
#include "Tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

Gfx g_gfx;
Scene g_scene;
bool g_running = true;
int64_t g_qpcFrequency = 1;
uint64_t g_simTick = 0;
bool g_sawPointerWheel = false; // once the pointer API delivers the wheel, ignore the legacy message

float g_timeScale = 1.0f;
bool g_paused = false;
bool g_stepOnce = false;
float g_latencyMs = 0.0f; // pointer event to present, measured right after Present returns

// Record and replay. Each event carries the sim tick it was applied on, and replay injects it on
// exactly that tick -- which is the whole reason the simulation is integer-tick-driven. The same
// recording under two tuning sets then produces two runs that can be compared honestly.
enum class ReplayMode : uint8_t
{
  Off,
  Recording,
  Playing
};

struct ReplayRecord
{
  uint64_t tick = 0;
  uint32_t pointerId = 0;
  float xPx = 0.0f;
  float yPx = 0.0f;
  int32_t wheelNotches = 0;
  uint8_t kind = 0;
  uint8_t buttons = 0;
  uint8_t flags = 0; // bit 0 touch, bit 1 shift
  uint8_t pad = 0;
};

ReplayMode g_replayMode = ReplayMode::Off;
std::vector<ReplayRecord> g_replay;
size_t g_replayCursor = 0;
uint64_t g_replayBaseTick = 0;
uint64_t g_replayLastTick = 0;

float ElapsedMs(int64_t _fromQpc, int64_t _toQpc)
{
  return float(double(_toQpc - _fromQpc) / double(g_qpcFrequency) * 1000.0);
}

std::wstring ReplayPath()
{
  return FindDataRoot() + L"last.replay";
}

int64_t NowQpc()
{
  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

// Every pointer event goes through here so recording and replay can sit on one seam.
void QueuePointer(const PointerEvent& _event)
{
  if (g_replayMode == ReplayMode::Playing)
  {
    return; // a replay is not a replay if live input can steer it
  }
  if (g_replayMode == ReplayMode::Recording)
  {
    ReplayRecord record;
    record.tick = g_simTick - g_replayBaseTick;
    record.pointerId = _event.pointerId;
    record.xPx = _event.xPx;
    record.yPx = _event.yPx;
    record.wheelNotches = _event.wheelNotches;
    record.kind = uint8_t(_event.kind);
    record.buttons = uint8_t(_event.buttons);
    record.flags = uint8_t((_event.isTouch ? 1u : 0u) | (_event.shift ? 2u : 0u));
    g_replay.push_back(record);
  }
  g_scene.QueuePointerEvent(_event);
}

void InjectReplayTick(uint64_t _relativeTick)
{
  while (g_replayCursor < g_replay.size() && g_replay[g_replayCursor].tick <= _relativeTick)
  {
    const ReplayRecord& record = g_replay[g_replayCursor];
    PointerEvent event;
    event.kind = PointerEvent::Kind(record.kind);
    event.pointerId = record.pointerId;
    event.xPx = record.xPx;
    event.yPx = record.yPx;
    event.buttons = record.buttons;
    event.isTouch = (record.flags & 1u) != 0;
    event.shift = (record.flags & 2u) != 0;
    event.wheelNotches = record.wheelNotches;
    event.timestampQpc = NowQpc();
    g_scene.QueuePointerEvent(event);
    ++g_replayCursor;
  }
}

void WriteReplayFile()
{
  const std::wstring path = ReplayPath();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    DebugPrintf("cannot write %S\n", path.c_str());
    return;
  }
  const uint32_t header[2] = {0x50524653u, uint32_t(g_replay.size())}; // 'SFRP'
  DWORD written = 0;
  WriteFile(file, header, sizeof(header), &written, nullptr);
  if (!g_replay.empty())
  {
    WriteFile(file, g_replay.data(), DWORD(g_replay.size() * sizeof(ReplayRecord)), &written, nullptr);
  }
  CloseHandle(file);
  DebugPrintf("wrote %S (%zu events over %llu ticks)\n", path.c_str(), g_replay.size(),
              static_cast<unsigned long long>(g_replayLastTick));
}

bool ReadReplayFile()
{
  const std::wstring path = ReplayPath();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    DebugPrintf("no %S to replay\n", path.c_str());
    return false;
  }
  uint32_t header[2] = {};
  DWORD read = 0;
  const bool headerOk = ReadFile(file, header, sizeof(header), &read, nullptr) && read == sizeof(header) && header[0] == 0x50524653u;
  if (!headerOk)
  {
    CloseHandle(file);
    DebugPrintf("%S is not a replay file\n", path.c_str());
    return false;
  }
  g_replay.assign(header[1], ReplayRecord{});
  if (header[1] != 0)
  {
    ReadFile(file, g_replay.data(), DWORD(g_replay.size() * sizeof(ReplayRecord)), &read, nullptr);
    g_replay.resize(read / sizeof(ReplayRecord));
  }
  CloseHandle(file);
  g_replayLastTick = g_replay.empty() ? 0 : g_replay.back().tick;
  DebugPrintf("loaded %zu events over %llu ticks\n", g_replay.size(), static_cast<unsigned long long>(g_replayLastTick));
  return !g_replay.empty();
}

// F12: the PNG and a copy of the tuning values that produced it, side by side, so any screenshot
// can be reproduced later.
void CaptureScreenshot()
{
  const std::wstring directory = FindDataRoot() + L"captures";
  CreateDirectoryW(directory.c_str(), nullptr);
  for (int index = 1; index < 10000; ++index)
  {
    wchar_t stem[MAX_PATH] = {};
    std::swprintf(stem, MAX_PATH, L"%s\\shipfeel_%04d", directory.c_str(), index);
    const std::wstring png = std::wstring(stem) + L".png";
    if (GetFileAttributesW(png.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
      continue;
    }
    TuningSaveTo((std::wstring(stem) + L".ini").c_str());
    g_gfx.RequestCapture(png);
    return;
  }
  DebugPrintf("captures folder is full\n");
}

// WM_POINTER covers mouse, pen and touch with one path, which is the whole reason for using it:
// the same build works on a desktop and on a tablet with no second code path.
bool DecodePointer(HWND _hwnd, WPARAM _wparam, PointerEvent& _event)
{
  const UINT32 pointerId = GET_POINTERID_WPARAM(_wparam);
  POINTER_INFO info = {};
  if (!GetPointerInfo(pointerId, &info))
  {
    return false;
  }
  POINT point = info.ptPixelLocation;
  ScreenToClient(_hwnd, &point);

  _event.pointerId = pointerId;
  _event.xPx = float(point.x);
  _event.yPx = float(point.y);
  _event.isTouch = info.pointerType == PT_TOUCH || info.pointerType == PT_PEN;
  _event.buttons = 0;
  _event.buttons |= (info.pointerFlags & POINTER_FLAG_FIRSTBUTTON) ? 1u : 0u;
  _event.buttons |= (info.pointerFlags & POINTER_FLAG_SECONDBUTTON) ? 2u : 0u;
  _event.buttons |= (info.pointerFlags & POINTER_FLAG_THIRDBUTTON) ? 4u : 0u;
  _event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  // PerformanceCount is when the hardware reported the contact, which is what the stage 5 latency
  // readout wants; it is not always populated, so fall back to now.
  _event.timestampQpc = info.PerformanceCount != 0 ? int64_t(info.PerformanceCount) : NowQpc();
  return true;
}

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

  case WM_POINTERDOWN:
  case WM_POINTERUPDATE:
  case WM_POINTERUP:
  case WM_POINTERCAPTURECHANGED:
  {
    PointerEvent event;
    if (!DecodePointer(_hwnd, _wparam, event))
    {
      return 0;
    }
    if (_msg == WM_POINTERDOWN)
    {
      event.kind = PointerEvent::Kind::Down;
      if (!event.isTouch)
      {
        SetCapture(_hwnd); // so a mouse drag that leaves the window still reports
      }
    }
    else if (_msg == WM_POINTERUPDATE)
    {
      event.kind = PointerEvent::Kind::Update;
    }
    else
    {
      event.kind = PointerEvent::Kind::Up;
      if (!event.isTouch) // touch contacts are captured implicitly, per contact
      {
        ReleaseCapture();
      }
    }
    QueuePointer(event);
    return 0;
  }

  case WM_POINTERWHEEL:
  {
    PointerEvent event;
    if (!DecodePointer(_hwnd, _wparam, event))
    {
      return 0;
    }
    g_sawPointerWheel = true;
    event.kind = PointerEvent::Kind::Wheel;
    event.wheelNotches = GET_WHEEL_DELTA_WPARAM(_wparam) / WHEEL_DELTA;
    QueuePointer(event);
    return 0;
  }

  case WM_MOUSEWHEEL: // only reached where WM_POINTERWHEEL is not delivered
  {
    if (g_sawPointerWheel)
    {
      return 0;
    }
    PointerEvent event;
    event.kind = PointerEvent::Kind::Wheel;
    event.wheelNotches = GET_WHEEL_DELTA_WPARAM(_wparam) / WHEEL_DELTA;
    event.timestampQpc = NowQpc();
    QueuePointer(event);
    return 0;
  }

  case WM_POINTERLEAVE:
    g_scene.m_hoverShip = -1;
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
      // Drops the selection first; only quits once nothing is selected.
      if (g_scene.SelectedCount() > 0)
      {
        g_scene.ClearSelection();
      }
      else
      {
        PostMessageW(_hwnd, WM_CLOSE, 0, 0);
      }
    }
    else if (_wparam == VK_F2)
    {
      TuningToggleWindow();
    }
    else if (_wparam == VK_F3)
    {
      g_scene.TriggerCameraShake(); // the debug hook, so the shake curve can be tuned on demand
    }
    else if (_wparam == VK_F5 || _wparam == VK_F6)
    {
      TuningStoreSlot(_wparam == VK_F5 ? 1 : 2);
    }
    else if (_wparam == VK_F7)
    {
      TuningToggleSlot();
    }
    else if (_wparam == VK_F9)
    {
      // Recording starts from a known world, or the replay has nothing to reproduce against.
      g_scene.ResetWorld();
      g_replay.clear();
      g_replayBaseTick = g_simTick;
      g_replayLastTick = 0;
      g_replayMode = ReplayMode::Recording;
      DebugPrintf("recording\n");
    }
    else if (_wparam == VK_F10)
    {
      if (g_replayMode == ReplayMode::Recording)
      {
        g_replayLastTick = g_replay.empty() ? 0 : g_replay.back().tick;
        g_replayMode = ReplayMode::Off;
        WriteReplayFile();
      }
    }
    else if (_wparam == VK_F11)
    {
      if (ReadReplayFile())
      {
        g_scene.ResetWorld();
        g_replayCursor = 0;
        g_replayBaseTick = g_simTick;
        g_replayMode = ReplayMode::Playing;
      }
    }
    else if (_wparam == VK_F12)
    {
      CaptureScreenshot();
    }
    else if (_wparam == '1')
    {
      g_timeScale = 0.25f;
      g_paused = false;
    }
    else if (_wparam == '2')
    {
      g_timeScale = 1.0f;
      g_paused = false;
    }
    else if (_wparam == '3')
    {
      g_timeScale = 4.0f;
      g_paused = false;
    }
    else if (_wparam == VK_SPACE)
    {
      g_paused = !g_paused;
    }
    else if (_wparam == VK_OEM_PERIOD)
    {
      g_paused = true;
      g_stepOnce = true; // one sim tick, then stop again
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
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // WIC, for F12

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
  g_qpcFrequency = qpcFreq.QuadPart;
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

    if (g_gfx.m_widthPx == 0 || g_gfx.m_heightPx == 0)
    {
      continue;
    }

    g_scene.Update(g_gfx.m_widthPx, g_gfx.m_heightPx);

    // Fixed 60 Hz simulation with an accumulator; the leftover fraction interpolates the render.
    // Capped so a stall (a dragged window, a breakpoint) cannot spiral into a burst of ticks.
    // A replay injects and drains its events inside this loop rather than once per frame, so each
    // one lands on exactly the tick it was recorded on however many ticks this frame happens to
    // cover.
    if (g_paused)
    {
      simAccumulator = 0.0f;
      if (g_stepOnce)
      {
        if (g_replayMode == ReplayMode::Playing)
        {
          InjectReplayTick(g_simTick - g_replayBaseTick);
          g_scene.Update(g_gfx.m_widthPx, g_gfx.m_heightPx);
        }
        g_scene.Step();
        ++g_simTick;
        g_stepOnce = false;
      }
    }
    else
    {
      simAccumulator = std::min(simAccumulator + dtSec * g_timeScale, 0.25f);
      while (simAccumulator >= SIM_DT)
      {
        if (g_replayMode == ReplayMode::Playing)
        {
          InjectReplayTick(g_simTick - g_replayBaseTick);
          g_scene.Update(g_gfx.m_widthPx, g_gfx.m_heightPx);
        }
        g_scene.Step();
        simAccumulator -= SIM_DT;
        ++g_simTick;
      }
    }

    // Let the last order play out before handing control back.
    if (g_replayMode == ReplayMode::Playing && g_replayCursor >= g_replay.size() &&
        (g_simTick - g_replayBaseTick) > g_replayLastTick + 120)
    {
      g_replayMode = ReplayMode::Off;
      DebugPrintf("replay finished\n");
    }

    const float simAlpha = simAccumulator / SIM_DT;

    // Rings, banking, thrusters, markers and the camera all ease on real time rather than sim
    // time, so they stay smooth however far the swapchain runs ahead of 60 Hz.
    g_scene.UpdateFeedback(dtSec);

    g_gfx.BeginFrame(Rgba{g_tuning.skyColourR, g_tuning.skyColourG, g_tuning.skyColourB, 1.0f});
    g_scene.Render(g_gfx, simAlpha);

    const float hudScale = float(GetDpiForWindow(hwnd)) / 96.0f;
    const char* transport = (g_replayMode == ReplayMode::Recording) ? "recording"
                            : (g_replayMode == ReplayMode::Playing) ? "replaying"
                            : g_paused                              ? "paused"
                                                                    : "live";
    char hud[512] = {};
    std::snprintf(hud, sizeof(hud),
                  "fps       %6.1f\nframe     %6.2f ms\nlatency   %6.2f ms\nsim tick  %6llu\nselected  %6d of %zu\n"
                  "slot      %6s\ntime      %5.2fx\n%-9s %6zu events",
                  double(fpsSmoothed), double(frameMsSmoothed), double(g_latencyMs), static_cast<unsigned long long>(g_simTick),
                  g_scene.SelectedCount(), g_scene.m_ships.size(), TuningActiveSlot(), double(g_paused ? 0.0f : g_timeScale), transport,
                  g_replay.size());
    g_gfx.DrawTextLine(12.0f * hudScale, 10.0f * hudScale, hudScale, Rgba{0.78f, 0.87f, 0.96f, 1.0f}, hud);

    g_gfx.EndFrame();

    // Pointer message in, Present out. Measured the moment Present returns, which is as close to
    // the frame leaving as the API lets us get.
    if (g_scene.m_lastPointerQpc != 0)
    {
      g_latencyMs = ElapsedMs(g_scene.m_lastPointerQpc, NowQpc());
      g_scene.m_lastPointerQpc = 0;
    }
  }

  TuningShutdown();
  g_gfx.Shutdown();
  CoUninitialize();
  return 0;
}
