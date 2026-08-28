#include "pch.h"
#include "AppWindow.h"

namespace Neuron
{
namespace
{
std::int64_t NowQpc() noexcept
{
  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}
} // namespace

void AppWindow::Create(const Desc& _desc, HINSTANCE _instance)
{
  // Before any window exists, or the first one is created at the wrong scale.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProcThunk;
  wc.hInstance = _instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = _desc.className.c_str();
  RegisterClassExW(&wc);

  constexpr DWORD style = WS_OVERLAPPEDWINDOW;
  RECT rect = {0, 0, _desc.widthPx, _desc.heightPx};
  AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem());

  m_hwnd = CreateWindowExW(0, _desc.className.c_str(), _desc.title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                           rect.bottom - rect.top, nullptr, nullptr, _instance, this);
  if (!m_hwnd)
    throw_last_error();

  // Mouse arrives as WM_POINTER too, so one input path covers mouse and touch.
  EnableMouseInPointer(TRUE);
}

void AppWindow::Show() noexcept
{
  ShowWindow(m_hwnd, SW_SHOW);
}

float AppWindow::DpiScale() const noexcept
{
  return static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f;
}

bool AppWindow::PumpMessages()
{
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
  {
    if (msg.message == WM_QUIT)
      m_running = false;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return m_running;
}

// The window carries its own instance pointer, set at WM_NCCREATE so that every message after it
// finds one. A static or global would work for exactly as long as there is one window.
LRESULT CALLBACK AppWindow::WndProcThunk(HWND _hwnd, UINT _msg, WPARAM _wparam, LPARAM _lparam)
{
  if (_msg == WM_NCCREATE)
  {
    auto create = reinterpret_cast<const CREATESTRUCTW*>(_lparam);
    auto window = static_cast<AppWindow*>(create->lpCreateParams);
    window->m_hwnd = _hwnd;
    SetWindowLongPtrW(_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
  }

  if (auto window = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(_hwnd, GWLP_USERDATA)))
    return window->WndProc(_msg, _wparam, _lparam);
  return DefWindowProcW(_hwnd, _msg, _wparam, _lparam);
}

bool AppWindow::DecodePointer(WPARAM _wparam, PointerEvent& _outEvent) const
{
  const UINT32 pointerId = GET_POINTERID_WPARAM(_wparam);
  POINTER_INFO info = {};
  if (!GetPointerInfo(pointerId, &info))
    return false;
  POINT point = info.ptPixelLocation;
  ScreenToClient(m_hwnd, &point);

  _outEvent.pointerId = pointerId;
  _outEvent.xPx = static_cast<float>(point.x);
  _outEvent.yPx = static_cast<float>(point.y);
  _outEvent.isTouch = info.pointerType == PT_TOUCH || info.pointerType == PT_PEN;
  _outEvent.buttons = 0;
  _outEvent.buttons |= (info.pointerFlags & POINTER_FLAG_FIRSTBUTTON) ? PointerEvent::BUTTON_FIRST : 0u;
  _outEvent.buttons |= (info.pointerFlags & POINTER_FLAG_SECONDBUTTON) ? PointerEvent::BUTTON_SECOND : 0u;
  _outEvent.buttons |= (info.pointerFlags & POINTER_FLAG_THIRDBUTTON) ? PointerEvent::BUTTON_THIRD : 0u;
  _outEvent.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  // PerformanceCount is when the hardware reported the contact; it is not always populated, so
  // fall back to now.
  _outEvent.timestampQpc = info.PerformanceCount != 0 ? static_cast<std::int64_t>(info.PerformanceCount) : NowQpc();
  return true;
}

LRESULT AppWindow::WndProc(UINT _msg, WPARAM _wparam, LPARAM _lparam)
{
  switch (_msg)
  {
  case WM_SIZE:
    if (_wparam != SIZE_MINIMIZED && onResize)
      onResize(LOWORD(_lparam), HIWORD(_lparam));
    return 0;

  case WM_POINTERDOWN:
  case WM_POINTERUPDATE:
  case WM_POINTERUP:
  case WM_POINTERCAPTURECHANGED:
  {
    PointerEvent event;
    if (!DecodePointer(_wparam, event))
      return 0;
    if (_msg == WM_POINTERDOWN)
    {
      event.kind = PointerEvent::Kind::Down;
      if (!event.isTouch)
        SetCapture(m_hwnd); // so a mouse drag that leaves the window still reports
    }
    else if (_msg == WM_POINTERUPDATE)
      event.kind = PointerEvent::Kind::Update;
    else
    {
      event.kind = PointerEvent::Kind::Up;
      if (!event.isTouch) // touch contacts are captured implicitly, per contact
        ReleaseCapture();
    }
    if (onPointer)
      onPointer(event);
    return 0;
  }

  case WM_POINTERWHEEL:
  {
    PointerEvent event;
    if (!DecodePointer(_wparam, event))
      return 0;
    m_sawPointerWheel = true;
    event.kind = PointerEvent::Kind::Wheel;
    event.wheelNotches = GET_WHEEL_DELTA_WPARAM(_wparam) / WHEEL_DELTA;
    if (onPointer)
      onPointer(event);
    return 0;
  }

  case WM_MOUSEWHEEL: // only reached where WM_POINTERWHEEL is not delivered
  {
    if (m_sawPointerWheel)
      return 0;
    PointerEvent event;
    event.kind = PointerEvent::Kind::Wheel;
    event.wheelNotches = GET_WHEEL_DELTA_WPARAM(_wparam) / WHEEL_DELTA;
    event.timestampQpc = NowQpc();
    if (onPointer)
      onPointer(event);
    return 0;
  }

  case WM_POINTERLEAVE:
    if (onPointerLeave)
      onPointerLeave();
    return 0;

  case WM_DPICHANGED:
  {
    auto suggested = reinterpret_cast<const RECT*>(_lparam);
    SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }

  case WM_KEYDOWN:
    if (onKeyDown)
      onKeyDown(static_cast<std::uint32_t>(_wparam));
    return 0;

  case WM_CLOSE:
    m_running = false;
    return 0;

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;

  default:
    break;
  }
  return DefWindowProcW(m_hwnd, _msg, _wparam, _lparam);
}
} // namespace Neuron
