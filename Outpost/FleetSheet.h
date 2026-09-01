#pragma once

#include "UniverseView.h"

#include "PointerEvent.h"
#include "TextRenderer.h"

#include <cstdint>
#include <span>

namespace Outpost
{
// One fleet, read: what it is, what is in it, and what it can be told to do (Design/Archive/Fleets.md 9.3).
//
// A panel over the bar rather than a modal screen, which is the whole difference between this and
// AssemblyScreen: the universe keeps working behind it, and it consumes only what lands on itself. A
// player can watch the fleet the sheet describes while the sheet is open, which is most of why it
// is worth opening.
//
// It exists because the commands have no names anywhere else. A fleet can be moved by tapping the
// ground and attacked with by tapping a hostile, and until this panel nothing on screen ever said
// either was possible -- which is what a first session needs and a tap cannot teach.
class FleetSheet
{
public:
  void Open(int _slot) noexcept
  {
    m_slot = _slot;
  }

  void Close() noexcept
  {
    m_slot = -1;
  }

  [[nodiscard]] bool IsOpen() const noexcept
  {
    return m_slot >= 0;
  }

  [[nodiscard]] int Slot() const noexcept
  {
    return m_slot;
  }

  // Closes itself when the fleet it describes stops being held -- docked, or lost its last ship.
  // Called once a frame, because a slot clearing is an update's news and not a gesture's.
  void Update(const UniverseView& _view) noexcept;

  // Draws the panel, or -- when a command has armed a target tap and closed it -- the one-line
  // prompt in its place, so the armed state is visible while it is live rather than only in the log.
  void Draw(Neuron::TextRenderer& _text, const UniverseView& _view, std::span<const char* const> _hullNames, float _dpiScale,
            std::uint32_t _widthPx, std::uint32_t _heightPx) const;

  // True only for events over the panel itself. Not modal: a tap past it is a tap on the universe,
  // which is the opposite of AssemblyScreen's rule and is what "a panel over the bar" means.
  [[nodiscard]] bool HandlePointer(const Neuron::PointerEvent& _event, UniverseView& _view, float _dpiScale, std::uint32_t _widthPx,
                                   std::uint32_t _heightPx);

private:
  // The four the design names. MINE is absent until the mining design lands (Design/Archive/Fleets.md 6.6),
  // and it is absent from this array rather than drawn disabled: a button that has never worked is
  // a promise, and the design would rather make none.
  // MOVE, ATTACK, DOCK, JUMP, STOP. STOP stays last because it is the one that needs no target and
  // therefore arms nothing; JUMP joins beside DOCK, which is the command it is shaped like.
  static constexpr int COMMANDS = 5;

  struct Rect
  {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

    [[nodiscard]] bool Contains(float _xPx, float _yPx) const noexcept
    {
      return _xPx >= x0 && _xPx < x1 && _yPx >= y0 && _yPx < y1;
    }
  };

  struct Layout
  {
    float scale = 1.0f;
    Rect panel;
    Rect commands[COMMANDS];
  };

  [[nodiscard]] Layout ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept;

  int m_slot = -1;

  // A contact is captured on the press and only fires on the release, and only if the release is
  // inside the same button -- Hud's idiom, and its reason twice over. It lets a press be cancelled
  // by sliding off, and it means this class consumes exactly the events it captured, so a contact
  // that began on the universe and ended over the panel is still the universe's to release.
  bool m_captured = false;
  std::uint32_t m_capturedPointer = 0;
};
} // namespace Outpost
