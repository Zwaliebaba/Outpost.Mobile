#pragma once

#include "PointerEvent.h"

#include <functional>
#include <string>

namespace Neuron
{
// The Win32 window and its message pump. It decodes input into PointerEvents and hands everything
// else out through callbacks; it knows nothing about what is being drawn or what a click means.
//
// The callbacks are set by the composition root, which is the only place that knows how the window,
// the renderer and the game fit together.
class AppWindow
{
public:
  struct Desc
  {
    std::wstring title = L"Outpost";
    std::wstring className = L"OutpostWindow";
    int widthPx = 1600;
    int heightPx = 900;
  };

  // Throws on failure; there is no usable state without a window.
  void Create(const Desc& _desc, HINSTANCE _instance);
  void Show() noexcept;

  // Drains the queue. Returns false once the application should stop running.
  [[nodiscard]] bool PumpMessages();

  [[nodiscard]] HWND Handle() const noexcept { return m_hwnd; }
  // 1.0 at 96 DPI. What HUD text and hit targets scale by.
  [[nodiscard]] float DpiScale() const noexcept;

  void RequestClose() noexcept { m_running = false; }

  std::function<void(std::uint32_t _widthPx, std::uint32_t _heightPx)> onResize;
  std::function<void(const PointerEvent& _event)> onPointer;
  std::function<void(std::uint32_t _virtualKey)> onKeyDown;
  std::function<void()> onPointerLeave;

private:
  static LRESULT CALLBACK WndProcThunk(HWND _hwnd, UINT _msg, WPARAM _wparam, LPARAM _lparam);
  LRESULT WndProc(UINT _msg, WPARAM _wparam, LPARAM _lparam);
  [[nodiscard]] bool DecodePointer(WPARAM _wparam, PointerEvent& _outEvent) const;

  HWND m_hwnd = nullptr;
  bool m_running = true;
  // Once the pointer API delivers the wheel, ignore the legacy message rather than acting twice.
  bool m_sawPointerWheel = false;
};
} // namespace Neuron
