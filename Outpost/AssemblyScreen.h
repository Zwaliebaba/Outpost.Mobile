#pragma once

#include "WorldView.h"

#include "PointerEvent.h"
#include "TextRenderer.h"

#include <cstdint>
#include <span>

namespace Outpost
{
// The station's assembly view: what this client has docked here, a draft drawn out of it, and the
// slot the draft becomes a fleet in (Design/Archive/Fleets.md 9.4).
//
// Its own class rather than more of Hud, for three reasons that each stand alone. It holds state the
// HUD has no business in -- a draft, a chosen slot, the station it belongs to -- where the HUD holds
// almost none. It is MODAL where the HUD is an overlay, so its rule for a pointer event is the
// opposite one: everything is consumed, nothing falls through. And Hud.cpp is eight hundred lines,
// which is the point at which a second screen goes in a second file.
//
// It reads a ledger reply and WorldView, and writes only by asking WorldView to send -- the same
// one-way seam the HUD keeps, and for the same reason: nothing here can feed back into a tick.
//
// What it is NOT: the station management screen. Undocking, trade, repair, cargo and the docked-ship
// list are all still the next phase's (Design/Archive/Stations.md 14). This does one thing, which is
// the one thing the fleet design needs a station screen for.
class AssemblyScreen
{
public:
  // Opens over _reply's station, with the first free slot chosen. A second call replaces what was
  // open: a long press on another station while this one is up is the player changing their mind,
  // not an error.
  void Open(const Game::LedgerReply& _reply, const WorldView& _view);

  void Close() noexcept
  {
    m_open = false;
  }

  [[nodiscard]] bool IsOpen() const noexcept
  {
    return m_open;
  }

  void Draw(Neuron::TextRenderer& _text, const WorldView& _view, std::span<const char* const> _hullNames,
            std::span<const char* const> _factionNames, float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const;

  // Consumes EVERY pointer event while open, so nothing reaches the HUD or the world behind it.
  // That is what modal means, and stating it as the return value rather than as a rule the caller
  // has to remember is what keeps the caller's dispatch a two-line chain.
  [[nodiscard]] bool HandlePointer(const Neuron::PointerEvent& _event, WorldView& _view, float _dpiScale, std::uint32_t _widthPx,
                                   std::uint32_t _heightPx);

private:
  struct Rect
  {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

    [[nodiscard]] bool Contains(float _xPx, float _yPx) const noexcept
    {
      return _xPx >= x0 && _xPx < x1 && _yPx >= y0 && _yPx < y1;
    }
    [[nodiscard]] float Width() const noexcept
    {
      return x1 - x0;
    }
  };

  // One rectangle per thing that can be pressed, for one window size, so the draw and the hit test
  // cannot disagree -- Hud::Layout's shape and its reason.
  struct Layout
  {
    float scale = 1.0f;
    Rect panel;
    Rect rows[Game::HULL_COUNT]; // the whole row, for reading; the two buttons sit inside it
    Rect plus[Game::HULL_COUNT];
    Rect minus[Game::HULL_COUNT];
    Rect slots[WorldView::FLEET_SLOTS];
    Rect launch;
    int rowCount = 0;
  };

  [[nodiscard]] Layout ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept;

  // Which hull each drawn row is. Rebuilt with the layout rather than stored, so a ledger row that
  // is exhausted by the draft still holds its place on screen: a row that vanished under the
  // player's finger would move every row below it mid-press.
  [[nodiscard]] int HullOfRow(int _row) const noexcept;

  [[nodiscard]] std::uint32_t DraftTotal() const noexcept;

  // Whether LAUNCH would do anything: a non-empty draft, and a slot that is still free.
  [[nodiscard]] bool CanLaunch(const WorldView& _view) const noexcept;

  bool m_open = false;
  Game::LedgerReply m_ledger; // what was true when the reply was written; never refreshed

  // How many of each hull the draft has taken out of the ledger column. The two always sum to what
  // the reply carried, which is why the ledger is never decremented: one number, drawn twice.
  std::uint32_t m_draft[Game::HULL_COUNT]{};
  int m_slot = -1;
};
} // namespace Outpost
