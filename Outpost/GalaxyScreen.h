#pragma once

#include "UniverseView.h"

#include "GalaxyLayout.h"

#include "BoxFit.h"
#include "PointerEvent.h"
#include "TextRenderer.h"

#include <cstdint>

namespace Outpost
{
// The galaxy map: 54 systems, 68 gates, and where this client's fleets are (Design/GalaxyMap.md).
//
// AssemblyScreen's shape, and for the same three reasons. It holds state the HUD has no business in,
// it is MODAL where the HUD is an overlay, and Hud.cpp is eight hundred lines. A reader who knows one
// of these two screens knows the other: Open, Close, IsOpen, Draw, HandlePointer.
//
// It draws what the client already holds and nothing else. The layout comes from the seed in the save
// header, so it is the same graph the server spawned gates against; the fleet positions come from the
// snapshot header's status block, which carries all five whether or not they are in the interest set,
// and that is what makes a map of fleets possible at all (Design/Archive/FleetStatus-work-order.md).
//
// What it is NOT, in this slice: a thing a tap does anything with. Slice 2 flies the camera to a
// tapped system and slice 3 orders a fleet there. Consuming pointer events and doing nothing with
// them is the modal contract, not a placeholder for those.
class GalaxyScreen
{
public:
  void Open() noexcept
  {
    m_open = true;
  }

  void Close() noexcept
  {
    m_open = false;
    m_pressed = false; // a contact still down when the screen closes must not name a system later
  }

  [[nodiscard]] bool IsOpen() const noexcept
  {
    return m_open;
  }

  // _here is the system the camera is in, as Game::SystemAt answers it. That function is a NEAREST
  // and always has an answer, so there is no "between systems" case to draw: the mark is always on
  // exactly one node. An index past the end -- which only a caller holding an empty layout can
  // produce -- marks nothing.
  void Draw(Neuron::TextRenderer& _text, const Game::GalaxyLayout& _galaxy, const UniverseView& _view, std::uint32_t _here, float _dpiScale,
            std::uint32_t _widthPx, std::uint32_t _heightPx) const;

  // Consumes EVERY pointer event while open, so nothing reaches the sheet, the tracker or the
  // universe behind it. The rail runs ahead of this in the chain, which is what lets the button that
  // opened the map close it (Design/GalaxyMap-slice-1.md 7).
  //
  // _outSystem is set to the system a tap named, or left alone. Reported rather than acted on, for
  // Hud::HandlePointer's reason: this class knows where a contact landed, and what a *system* means
  // -- a camera flight now, a fleet order in slice 3 -- belongs to the composition root.
  //
  // A press that begins on a node and lifts elsewhere names nothing, which is the rule the HUD's
  // buttons already keep: a tap can be cancelled by sliding off.
  [[nodiscard]] bool HandlePointer(const Neuron::PointerEvent& _event, const Game::GalaxyLayout& _galaxy, std::uint32_t& _outSystem,
                                   float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx);

  // Which system is drawn nearest _xPx/_yPx, within HUD_MAP_TAP_RADIUS_PX, or the system count.
  //
  // Through the SAME layout and the same projection the draw used, which is the whole of why it is a
  // member function: a node drawn in one place and hit-tested in another is wrong in a way no test
  // catches and every player finds.
  [[nodiscard]] std::uint32_t SystemAtPointer(const Game::GalaxyLayout& _galaxy, float _xPx, float _yPx, float _dpiScale,
                                              std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept;

private:
  struct Rect
  {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
  };

  struct Layout
  {
    float scale = 1.0f;
    Rect panel;
    Rect plot; // where the graph is fitted, inset from the panel
  };

  [[nodiscard]] Layout ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept;

  // The projection, and it is the one piece of this screen with an argument in it: the galaxy's own
  // bounding box in metres, fitted into the plot ISOTROPICALLY (Design/GalaxyMap.md 3).
  //
  // From starPos and not from cellQ/cellR. The lattice is a clean hexagon and would draw a prettier
  // picture, but the jitter is what makes the map true -- a player reading distances off a map that
  // has quietly regularised them will misjudge which of two gates is the long one.
  [[nodiscard]] static Neuron::BoxFit Projection(const Game::GalaxyLayout& _galaxy, const Layout& _layout) noexcept;

  bool m_open = false;

  // The node a press went down on, and which contact is holding it. A press is only spent where it
  // began, so a finger that slid off a node between down and up has changed its mind.
  std::uint32_t m_pressedSystem = 0;
  std::uint32_t m_pressedPointer = 0;
  bool m_pressed = false;
};
} // namespace Outpost
