#include "pch.h"
#include "AssemblyScreen.h"

#include "ViewTuning.h"

using namespace Neuron;

namespace Outpost
{
namespace
{
// The atlas holds Latin-1 from 192 up, so the multiplication sign is there. Its own copy rather than
// a shared one: both screens keep theirs in an anonymous namespace, which is what this tree does
// instead of a UI-support header nobody owns.
constexpr char TIMES[] = "\xD7";

[[nodiscard]] Rgba WithAlpha(Rgba _colour, float _alpha) noexcept
{
  return Rgba{_colour.r, _colour.g, _colour.b, _colour.a * _alpha};
}

void DrawPanel(TextRenderer& _text, float _x0, float _y0, float _x1, float _y1, Rgba _fill, Rgba _outline, float _scale)
{
  const float line = std::max(1.0f, std::floor(_scale));
  _text.DrawScreenRect(_x0, _y0, _x1, _y1, _fill);
  _text.DrawScreenRect(_x0, _y0, _x1, _y0 + line, _outline);
  _text.DrawScreenRect(_x0, _y1 - line, _x1, _y1, _outline);
  _text.DrawScreenRect(_x0, _y0, _x0 + line, _y1, _outline);
  _text.DrawScreenRect(_x1 - line, _y0, _x1, _y1, _outline);
}
} // namespace

void AssemblyScreen::Open(const Game::LedgerReply& _reply, const UniverseView& _view)
{
  m_ledger = _reply;
  for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    m_draft[hull] = 0;

  // The first free slot, so the common case -- one fleet at a time, into whichever button is empty
  // -- costs no press at all. None free leaves it at -1, which is what greys LAUNCH.
  m_slot = -1;
  for (int slot = 0; slot < UniverseView::FLEET_SLOTS && m_slot < 0; ++slot)
  {
    if (!_view.IsFleetHeld(slot))
      m_slot = slot;
  }
  m_open = true;
}

int AssemblyScreen::HullOfRow(int _row) const noexcept
{
  // Hull-id order, which is also the order the manifest launches in, so the screen lists a fleet in
  // the order the player will watch it come out (Universe::ComposeFleet).
  int seen = 0;
  for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
  {
    // The reply's count, not the count less the draft: a row is listed while the station HAD any,
    // so a hull drafted down to nothing keeps its place. A row that vanished under the player's
    // finger would move every row below it in the middle of a press.
    if (m_ledger.hullCounts[hull] == 0)
      continue;
    if (seen == _row)
      return static_cast<int>(hull);
    ++seen;
  }
  return -1;
}

std::uint32_t AssemblyScreen::DraftTotal() const noexcept
{
  std::uint32_t total = 0;
  for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    total += m_draft[hull];
  return total;
}

bool AssemblyScreen::CanLaunch(const UniverseView& _view) const noexcept
{
  return DraftTotal() > 0 && m_slot >= 0 && !_view.IsFleetHeld(m_slot);
}

AssemblyScreen::Layout AssemblyScreen::ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept
{
  Layout layout;
  const float s = std::max(0.5f, _dpiScale);
  layout.scale = s;

  int rows = 0;
  for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    rows += (m_ledger.hullCounts[hull] > 0) ? 1 : 0;
  layout.rowCount = rows;

  // The right column needs three things in it, so the panel is never shorter than they are however
  // few hulls the station holds -- an empty ledger still has to draw its slots and its LAUNCH.
  const float rowsPx = static_cast<float>(std::max(rows, 3)) * HUD_ASSEMBLY_ROW_PX * s;
  // Never wider than the window less its margins, and never degenerate: a window narrow enough to
  // make this negative is one the frame loop is already skipping, and a rectangle that folded
  // inside out would still have to hit-test sanely if one ever got here.
  const float w = std::max(1.0f, std::min(HUD_ASSEMBLY_WIDTH_PX * s, static_cast<float>(_widthPx) - 2.0f * HUD_MARGIN_PX * s));
  const float h = HUD_ASSEMBLY_HEADER_PX * s + rowsPx + HUD_ASSEMBLY_FOOTER_PX * s;
  const float x0 = (static_cast<float>(_widthPx) - w) * 0.5f;
  const float y0 = (static_cast<float>(_heightPx) - h) * 0.5f;
  layout.panel = {x0, y0, x0 + w, y0 + h};

  const float pad = HUD_ASSEMBLY_PAD_PX * s;
  const float split = x0 + w * HUD_ASSEMBLY_LEFT_FRACTION;
  const float rowH = HUD_ASSEMBLY_ROW_PX * s;
  const float step = HUD_ASSEMBLY_STEP_BUTTON_PX * s;

  float y = y0 + HUD_ASSEMBLY_HEADER_PX * s;
  for (int row = 0; row < rows && row < static_cast<int>(Game::HULL_COUNT); ++row)
  {
    layout.rows[row] = {x0 + pad, y, split - pad, y + rowH};
    // Right-aligned inside the left column, so a long hull name never pushes the buttons off it.
    layout.minus[row] = {split - pad - step, y + (rowH - step) * 0.5f, split - pad, y + (rowH + step) * 0.5f};
    layout.plus[row] = {layout.minus[row].x0 - step - 4.0f * s, layout.minus[row].y0, layout.minus[row].x0 - 4.0f * s,
                        layout.minus[row].y1};
    y += rowH;
  }

  // The right column: draft on the first line, the five slots on the second, LAUNCH on the third.
  const float slotPx = HUD_ASSEMBLY_SLOT_BUTTON_PX * s;
  const float slotsY = y0 + HUD_ASSEMBLY_HEADER_PX * s + rowH;
  float slotX = split + pad;
  for (int slot = 0; slot < UniverseView::FLEET_SLOTS; ++slot)
  {
    layout.slots[slot] = {slotX, slotsY, slotX + slotPx, slotsY + slotPx};
    slotX += slotPx + 4.0f * s;
  }

  const float launchY = slotsY + slotPx + rowH * 0.4f;
  layout.launch = {split + pad, launchY, layout.panel.x1 - pad, launchY + HUD_ASSEMBLY_LAUNCH_H_PX * s};
  return layout;
}

void AssemblyScreen::Draw(TextRenderer& _text, const UniverseView& _view, std::span<const char* const> _hullNames,
                          std::span<const char* const> _factionNames, float _dpiScale, std::uint32_t _widthPx,
                          std::uint32_t _heightPx) const
{
  if (!m_open)
    return;

  const Layout layout = ComputeLayout(_dpiScale, _widthPx, _heightPx);
  const float s = layout.scale;
  const float textPx = _text.AdvancePx(FontId::Ui, HUD_TEXT_SCALE * s);
  const float labelPx = _text.AdvancePx(FontId::Ui, HUD_LABEL_SCALE * s);
  const float pad = HUD_ASSEMBLY_PAD_PX * s;

  // The scrim first: this is modal, and a panel over an unchanged universe reads as a widget.
  _text.DrawScreenRect(0.0f, 0.0f, static_cast<float>(_widthPx), static_cast<float>(_heightPx), HUD_ASSEMBLY_SCRIM);
  DrawPanel(_text, layout.panel.x0, layout.panel.y0, layout.panel.x1, layout.panel.y1, HUD_PANEL_FILL, HUD_PANEL_OUTLINE, s);

  // --- header: whose station, and that this client is docked in it -------------------------------
  {
    // The station's own record says whose it is. The reply does not carry an owner and does not
    // need to: the station is on screen -- a long press is how this opened -- so it is in the
    // interest set, and the client already knows every record's faction.
    const Game::FactionId owner = _view.FactionOfEntity(m_ledger.station);
    const char* name = (owner < _factionNames.size()) ? _factionNames[owner] : "UNKNOWN";
    char title[64] = {};
    std::snprintf(title, sizeof(title), "%s STATION", name);
    _text.DrawTextLine(FontId::Ui, layout.panel.x0 + pad, layout.panel.y0 + pad, HUD_TEXT_SCALE * s, HUD_ACCENT_GREEN, title);

    const char* state = "DOCKED";
    const float width = labelPx * static_cast<float>(std::strlen(state));
    _text.DrawTextLine(FontId::Ui, layout.panel.x1 - pad - width, layout.panel.y0 + pad, HUD_LABEL_SCALE * s, HUD_LABEL_COLOUR, state);
  }

  // --- the ledger rows ---------------------------------------------------------------------------
  for (int row = 0; row < layout.rowCount; ++row)
  {
    const int hull = HullOfRow(row);
    if (hull < 0)
      break;
    const std::uint32_t left = m_ledger.hullCounts[hull] - m_draft[hull];
    const Rect& rect = layout.rows[row];

    const bool named = static_cast<std::size_t>(hull) < _hullNames.size();
    char line[64] = {};
    std::snprintf(line, sizeof(line), "%s", named ? _hullNames[static_cast<std::size_t>(hull)] : "HULL");
    _text.DrawTextLine(FontId::Ui, rect.x0, rect.y0 + (rect.y1 - rect.y0 - textPx) * 0.5f, HUD_TEXT_SCALE * s,
                       (left > 0) ? HUD_COLOUR : HUD_LABEL_COLOUR, line);

    // What is still in the ledger, then what the draft has taken -- two numbers of one total, which
    // is why the reply itself is never decremented.
    //
    // Right-aligned against the + button rather than placed at a column, because the hull names are
    // authored content: "INTERCEPTOR" is eleven cells and a fixed column that cleared it would
    // waste half the panel on every other row, while one that did not would have the name growing
    // through the count.
    char counts[32] = {};
    if (m_draft[hull] > 0)
      std::snprintf(counts, sizeof(counts), "%s%u  (%s%u)", TIMES, left, TIMES, m_draft[hull]);
    else
      std::snprintf(counts, sizeof(counts), "%s%u", TIMES, left);
    const float countsWidth = textPx * static_cast<float>(std::strlen(counts));
    _text.DrawTextLine(FontId::Ui, layout.plus[row].x0 - 8.0f * s - countsWidth, rect.y0 + (rect.y1 - rect.y0 - textPx) * 0.5f,
                       HUD_TEXT_SCALE * s, (m_draft[hull] > 0) ? HUD_ACCENT_GREEN : HUD_COLOUR, counts);

    const bool canAdd = left > 0 && DraftTotal() < Game::MAX_FLEET_SHIPS;
    const bool canTake = m_draft[hull] > 0;
    const auto stepButton = [&](const Rect& _at, const char* _glyph, bool _enabled)
    {
      const Rgba tint = _enabled ? HUD_ACCENT_GREEN : WithAlpha(HUD_LABEL_COLOUR, HUD_ASSEMBLY_DISABLED_ALPHA);
      DrawPanel(_text, _at.x0, _at.y0, _at.x1, _at.y1, HUD_PANEL_FILL, WithAlpha(tint, HUD_ACTIVE_OUTLINE_ALPHA), s);
      const float glyphWidth = labelPx * static_cast<float>(std::strlen(_glyph));
      _text.DrawTextLine(FontId::Ui, (_at.x0 + _at.x1 - glyphWidth) * 0.5f, (_at.y0 + _at.y1 - labelPx) * 0.5f, HUD_LABEL_SCALE * s, tint,
                         _glyph);
    };
    stepButton(layout.plus[row], "+", canAdd);
    stepButton(layout.minus[row], "-", canTake);
  }

  if (layout.rowCount == 0)
  {
    _text.DrawTextLine(FontId::Ui, layout.panel.x0 + pad, layout.panel.y0 + HUD_ASSEMBLY_HEADER_PX * s, HUD_TEXT_SCALE * s,
                       HUD_LABEL_COLOUR, "NOTHING DOCKED HERE");
  }

  // --- the right column: draft, slots, launch ----------------------------------------------------
  {
    const float split = layout.slots[0].x0;
    char draft[48] = {};
    std::snprintf(draft, sizeof(draft), "DRAFT  %s%u OF %u", TIMES, DraftTotal(), Game::MAX_FLEET_SHIPS);
    _text.DrawTextLine(FontId::Ui, split, layout.panel.y0 + HUD_ASSEMBLY_HEADER_PX * s, HUD_TEXT_SCALE * s,
                       (DraftTotal() > 0) ? HUD_ACCENT_GREEN : HUD_LABEL_COLOUR, draft);

    for (int slot = 0; slot < UniverseView::FLEET_SLOTS; ++slot)
    {
      const Rect& at = layout.slots[slot];
      const bool taken = _view.IsFleetHeld(slot);
      const bool chosen = (slot == m_slot);
      // A taken slot is drawn as a dot rather than a digit, and is inert. The affordance says which
      // slots exist before the wire is touched; the gate refuses a raced one behind it (ADR 0014).
      const Rgba tint = taken ? WithAlpha(HUD_LABEL_COLOUR, HUD_ASSEMBLY_DISABLED_ALPHA) : (chosen ? HUD_ACCENT_GREEN : HUD_COLOUR);
      DrawPanel(_text, at.x0, at.y0, at.x1, at.y1, HUD_PANEL_FILL, WithAlpha(tint, HUD_ACTIVE_OUTLINE_ALPHA), s);
      if (chosen)
        _text.DrawScreenRect(at.x0, at.y0, at.x1, at.y1, WithAlpha(HUD_ACCENT_GREEN, HUD_FLEET_ACTIVE_FILL_ALPHA));

      char glyph[4] = {};
      std::snprintf(glyph, sizeof(glyph), taken ? "." : "%d", slot + 1);
      const float glyphWidth = labelPx * static_cast<float>(std::strlen(glyph));
      _text.DrawTextLine(FontId::Ui, (at.x0 + at.x1 - glyphWidth) * 0.5f, (at.y0 + at.y1 - labelPx) * 0.5f, HUD_LABEL_SCALE * s, tint,
                         glyph);
    }

    const bool ready = CanLaunch(_view);
    const Rgba tint = ready ? HUD_ACCENT_GREEN : WithAlpha(HUD_LABEL_COLOUR, HUD_ASSEMBLY_DISABLED_ALPHA);
    DrawPanel(_text, layout.launch.x0, layout.launch.y0, layout.launch.x1, layout.launch.y1, HUD_PANEL_FILL,
              WithAlpha(tint, HUD_ACTIVE_OUTLINE_ALPHA), s);
    if (ready)
      _text.DrawScreenRect(layout.launch.x0, layout.launch.y0, layout.launch.x1, layout.launch.y1,
                           WithAlpha(HUD_ACCENT_GREEN, HUD_FLEET_ACTIVE_FILL_ALPHA));
    const char* label = "LAUNCH";
    const float width = textPx * static_cast<float>(std::strlen(label));
    _text.DrawTextLine(FontId::Ui, (layout.launch.x0 + layout.launch.x1 - width) * 0.5f,
                       (layout.launch.y0 + layout.launch.y1 - textPx) * 0.5f, HUD_TEXT_SCALE * s, tint, label);
  }

  if constexpr (HUD_SCANLINES)
  {
    const float step = std::max(2.0f, HUD_SCANLINE_STEP_PX * s);
    const float line = std::max(1.0f, std::floor(s));
    const Rgba dark{0.0f, 0.0f, 0.0f, HUD_SCANLINE_ALPHA};
    for (float y = layout.panel.y0 + step - line; y < layout.panel.y1; y += step)
      _text.DrawScreenRect(layout.panel.x0, y, layout.panel.x1, std::min(y + line, layout.panel.y1), dark);
  }
}

bool AssemblyScreen::HandlePointer(const PointerEvent& _event, UniverseView& _view, float _dpiScale, std::uint32_t _widthPx,
                                   std::uint32_t _heightPx)
{
  if (!m_open)
    return false;

  // Modal: every event is consumed while this is up, whatever it is and wherever it landed. Only
  // the release below does anything.
  if (_event.kind != PointerEvent::Kind::Up)
    return true;

  const Layout layout = ComputeLayout(_dpiScale, _widthPx, _heightPx);

  // Outside the panel closes it. A modal with no way out but a button is a modal a player gets
  // stuck in, and tapping past it is what every player tries first.
  if (!layout.panel.Contains(_event.xPx, _event.yPx))
  {
    Close();
    return true;
  }

  for (int row = 0; row < layout.rowCount; ++row)
  {
    const int hull = HullOfRow(row);
    if (hull < 0)
      break;
    const std::uint32_t left = m_ledger.hullCounts[hull] - m_draft[hull];

    // Both bounds are affordances telling the truth first: the ledger cannot give what it does not
    // hold, and a fleet cannot exceed MAX_FLEET_SHIPS. ComposeFleet refuses independently on both
    // counts, and neither is the other's substitute (ADR 0014).
    if (layout.plus[row].Contains(_event.xPx, _event.yPx) && left > 0 && DraftTotal() < Game::MAX_FLEET_SHIPS)
      ++m_draft[hull];
    else if (layout.minus[row].Contains(_event.xPx, _event.yPx) && m_draft[hull] > 0)
      --m_draft[hull];
  }

  for (int slot = 0; slot < UniverseView::FLEET_SLOTS; ++slot)
  {
    if (layout.slots[slot].Contains(_event.xPx, _event.yPx) && !_view.IsFleetHeld(slot))
      m_slot = slot;
  }

  if (layout.launch.Contains(_event.xPx, _event.yPx) && CanLaunch(_view))
  {
    _view.SendComposeOrder(m_ledger.station, static_cast<std::uint8_t>(m_slot), m_draft);
    Close();
  }
  return true;
}
} // namespace Outpost
