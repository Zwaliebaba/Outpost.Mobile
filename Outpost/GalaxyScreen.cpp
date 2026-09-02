#include "pch.h"
#include "GalaxyScreen.h"

#include "ViewTuning.h"

using namespace Neuron;

namespace Outpost
{
namespace
{
void DrawPanel(TextRenderer& _text, float _x0, float _y0, float _x1, float _y1, Rgba _fill, Rgba _outline, float _scale)
{
  const float line = std::max(1.0f, std::floor(_scale));
  _text.DrawScreenRect(_x0, _y0, _x1, _y1, _fill);
  _text.DrawScreenRect(_x0, _y0, _x1, _y0 + line, _outline);
  _text.DrawScreenRect(_x0, _y1 - line, _x1, _y1, _outline);
  _text.DrawScreenRect(_x0, _y0, _x0 + line, _y1, _outline);
  _text.DrawScreenRect(_x1 - line, _y0, _x1, _y1, _outline);
}

// A hollow square, which is what marks the system the camera is in. Four rects rather than a ring,
// because the overlay draws rectangles and lines and a circle would be a new primitive for one mark.
void DrawMark(TextRenderer& _text, float _xPx, float _yPx, float _halfPx, float _linePx, Rgba _colour)
{
  const float x0 = _xPx - _halfPx;
  const float y0 = _yPx - _halfPx;
  const float x1 = _xPx + _halfPx;
  const float y1 = _yPx + _halfPx;
  _text.DrawScreenRect(x0, y0, x1, y0 + _linePx, _colour);
  _text.DrawScreenRect(x0, y1 - _linePx, x1, y1, _colour);
  _text.DrawScreenRect(x0, y0, x0 + _linePx, y1, _colour);
  _text.DrawScreenRect(x1 - _linePx, y0, x1, y1, _colour);
}
} // namespace

GalaxyScreen::Layout GalaxyScreen::ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept
{
  Layout layout;
  const float s = std::max(0.5f, _dpiScale);
  layout.scale = s;

  // Near-full-screen, unlike the assembly panel, because it is a map: the whole galaxy in a
  // fixed-width box would be a picture of a galaxy rather than something to read. Clamped at zero
  // width so a window mid-resize gives a degenerate rectangle rather than one folded inside out --
  // the fit below then produces a non-positive scale and the screen draws no graph.
  const float margin = HUD_MAP_MARGIN_PX * s;
  const float x1 = std::max(margin, static_cast<float>(_widthPx) - margin);
  const float y1 = std::max(margin, static_cast<float>(_heightPx) - margin);
  layout.panel = {margin, margin, x1, y1};

  const float inset = HUD_MAP_INSET_PX * s;
  const float pad = HUD_MAP_PAD_PX * s;
  const float plotY0 = layout.panel.y0 + HUD_MAP_HEADER_PX * s;
  layout.plot = {layout.panel.x0 + pad + inset, plotY0 + inset, std::max(layout.panel.x0, layout.panel.x1 - pad - inset),
                 std::max(plotY0, layout.panel.y1 - pad - inset)};
  return layout;
}

BoxFit GalaxyScreen::Projection(const Game::GalaxyLayout& _galaxy, const Layout& _layout) noexcept
{
  // Metres from the galaxy's own origin, through UniversePos's offsets rather than by touching the
  // sector fields -- the discipline that type exists to impose. The galaxy is a megametre across and
  // a float carries that to well under a metre, which is four orders of magnitude finer than a pixel
  // of this map.
  float x0 = 0.0f;
  float z0 = 0.0f;
  float x1 = 0.0f;
  float z1 = 0.0f;
  bool any = false;
  for (const Game::SystemSite& site : _galaxy.systems)
  {
    const float x = Game::OffsetX(_galaxy.origin, site.starPos);
    const float z = Game::OffsetZ(_galaxy.origin, site.starPos);
    if (!any)
    {
      x0 = x1 = x;
      z0 = z1 = z;
      any = true;
      continue;
    }
    x0 = std::min(x0, x);
    x1 = std::max(x1, x);
    z0 = std::min(z0, z);
    z1 = std::max(z1, z);
  }

  // +Z is north on the minimap and on the map, which is the same orientation the camera looks down
  // and the same one the sector readout counts in. A map whose north disagreed with the minimap's
  // would be worse than no map.
  return FitBoxIsotropic(x0, z0, x1, z1, _layout.plot.x0, _layout.plot.y0, _layout.plot.x1, _layout.plot.y1);
}

void GalaxyScreen::Draw(TextRenderer& _text, const Game::GalaxyLayout& _galaxy, const UniverseView& _view, std::uint32_t _here,
                        float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const
{
  if (!m_open)
    return;

  const Layout layout = ComputeLayout(_dpiScale, _widthPx, _heightPx);
  const float s = layout.scale;
  const float pad = HUD_MAP_PAD_PX * s;
  const float textPx = _text.AdvancePx(FontId::Ui, HUD_TEXT_SCALE * s);
  const float labelPx = _text.AdvancePx(FontId::Ui, HUD_LABEL_SCALE * s);

  _text.DrawScreenRect(0.0f, 0.0f, static_cast<float>(_widthPx), static_cast<float>(_heightPx), HUD_MAP_SCRIM);
  DrawPanel(_text, layout.panel.x0, layout.panel.y0, layout.panel.x1, layout.panel.y1, HUD_PANEL_FILL, HUD_PANEL_OUTLINE, s);

  _text.DrawTextLine(FontId::Ui, layout.panel.x0 + pad, layout.panel.y0 + pad, HUD_TEXT_SCALE * s, HUD_ACCENT_GREEN, "GALAXY");
  {
    const char* hint = "ESC TO CLOSE";
    const float width = labelPx * static_cast<float>(std::strlen(hint));
    _text.DrawTextLine(FontId::Ui, layout.panel.x1 - pad - width, layout.panel.y0 + pad, HUD_LABEL_SCALE * s, HUD_LABEL_COLOUR, hint);
  }

  const BoxFit fit = Projection(_galaxy, layout);
  if (fit.scale <= 0.0f || _galaxy.systems.empty())
    return; // a window with no room for a plot, or a layout that was never laid

  // The gates first, so the nodes sit on top of them rather than being crossed out by them. Every
  // link once: GateLink is stored with systemA < systemB and the list holds each pair one time.
  for (const Game::GateLink& link : _galaxy.links)
  {
    if (link.systemA >= _galaxy.systems.size() || link.systemB >= _galaxy.systems.size())
      continue;
    const Game::SystemSite& a = _galaxy.systems[link.systemA];
    const Game::SystemSite& b = _galaxy.systems[link.systemB];
    _text.DrawScreenLine(fit.XPx(Game::OffsetX(_galaxy.origin, a.starPos)), fit.YPx(Game::OffsetZ(_galaxy.origin, a.starPos)),
                         fit.XPx(Game::OffsetX(_galaxy.origin, b.starPos)), fit.YPx(Game::OffsetZ(_galaxy.origin, b.starPos)),
                         std::max(1.0f, HUD_MAP_EDGE_PX * s), HUD_MAP_EDGE_COLOUR);
  }

  const float node = std::max(1.0f, HUD_MAP_NODE_PX * s);
  for (std::size_t at = 0; at < _galaxy.systems.size(); ++at)
  {
    const Game::SystemSite& site = _galaxy.systems[at];
    const float xPx = fit.XPx(Game::OffsetX(_galaxy.origin, site.starPos));
    const float yPx = fit.YPx(Game::OffsetZ(_galaxy.origin, site.starPos));
    _text.DrawScreenRect(xPx - node * 0.5f, yPx - node * 0.5f, xPx + node * 0.5f, yPx + node * 0.5f, HUD_MAP_NODE_COLOUR);
    if (static_cast<std::uint32_t>(at) == _here)
      DrawMark(_text, xPx, yPx, HUD_MAP_HERE_PX * s * 0.5f, std::max(1.0f, std::floor(s)), HUD_ACCENT_GREEN);
  }

  // The fleets, last, because a digit is the thing a player is looking for. Every held slot, from the
  // status block in the snapshot header -- four of five are routinely outside the interest set and
  // are still drawn, which is the decision the fleet bar took and this screen inherits.
  //
  // Which system a fleet is IN is Game::SystemAt's, and that is a nearest rather than a containment:
  // it always has an answer, so a fleet mid-crossing draws on the system it is closest to instead of
  // vanishing. A slot the server does not hold draws nothing at all, which is the only "no digit"
  // case there is.
  for (int slot = 0; slot < UniverseView::FLEET_SLOTS; ++slot)
  {
    if (!_view.IsFleetHeld(slot))
      continue;
    const std::uint32_t in = Game::SystemAt(_galaxy.systems, _view.FleetPosition(slot));
    if (in >= _galaxy.systems.size())
      continue;
    const Game::SystemSite& site = _galaxy.systems[in];
    const float xPx = fit.XPx(Game::OffsetX(_galaxy.origin, site.starPos));
    const float yPx = fit.YPx(Game::OffsetZ(_galaxy.origin, site.starPos));

    // The bar's digit and the minimap's, so a player reads one number everywhere: the slot, one
    // based. Offset up and right of the node so it never sits on the dot it belongs to.
    char digit[2] = {static_cast<char>('1' + slot), '\0'};
    const Rgba colour = _view.IsFleetSelected(slot) ? HUD_ACCENT_GREEN : HUD_COLOUR;
    _text.DrawTextLine(FontId::Ui, xPx + node, yPx - node - textPx, HUD_TEXT_SCALE * s, colour, digit);
  }
}
} // namespace Outpost
