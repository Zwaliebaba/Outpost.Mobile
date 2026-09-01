#include "pch.h"
#include "Hud.h"

#include "ViewTuning.h"

using namespace DirectX;
using namespace Neuron;

namespace Outpost
{
namespace
{
// The atlas holds Latin-1 from 192 up, so the multiplication sign on a group count is there; the
// middle dot the mock separates fields with is not, and a bar stands in for it.
constexpr char SEPARATOR[] = " | ";
constexpr char TIMES[] = "\xD7";

constexpr const char* RAIL_LABELS[Hud::RAIL_BUTTONS] = {"RSRCH", "WALLET", "STORE", "UNIVRS"};
constexpr const char* ORDER_NAMES[] = {"IDLE", "MOVING", "ALIGNING"}; // by Game::OrderState

// Counted per hullId in the selection summary. Taken from the hull table rather than guessed, so a
// hull added there cannot quietly start being counted as a different one.
constexpr int MAX_HULL_KINDS = static_cast<int>(Game::HULL_COUNT);

Rgba WithAlpha(Rgba _colour, float _alpha) noexcept
{
  return Rgba{_colour.r, _colour.g, _colour.b, _alpha};
}

// The overview column of Design/Archive/Stations.md 9.3: what a faction is to the viewer, as a map colour.
// The mask row outranks the Vanguard row for the reason UniverseView::LiveryOf gives -- turn criminal
// and the law turns red, dots and hulls together. A faction with no row is red, because a stranger
// drawn as a friend is the one mistake this table must not make.
Rgba OverviewColourOf(Game::FactionId _faction, bool _own, bool _hostileToMe) noexcept
{
  if (_own)
    return HUD_ACCENT_GREEN;
  if (_hostileToMe)
    return HUD_ALERT_RED;
  if (_faction == Game::FACTION_VANGUARD)
    return HUD_VANGUARD_BLUE;
  return HUD_ALERT_RED;
}

// "12,480": thousands separated, the way the mock writes a resource total.
void FormatThousands(char* _out, size_t _size, int _value) noexcept
{
  char digits[16] = {};
  std::snprintf(digits, sizeof(digits), "%d", std::abs(_value));
  const size_t count = std::strlen(digits);

  size_t written = 0;
  if (_value < 0 && written + 1 < _size)
    _out[written++] = '-';
  for (size_t i = 0; i < count && written + 1 < _size; ++i)
  {
    if (i > 0 && (count - i) % 3 == 0 && written + 1 < _size)
      _out[written++] = ',';
    _out[written++] = digits[i];
  }
  _out[written] = '\0';
}

// Liang-Barsky: the part of a segment inside a rectangle, or false when none of it is. The minimap
// draws the camera frustum this way rather than clamping its corners, because clamping folds a
// trapezoid that runs off the map into a line along the edge.
bool ClipToRect(float _x0, float _y0, float _x1, float _y1, float& _outX0, float& _outY0, float& _outX1, float& _outY1, float _left,
                float _top, float _right, float _bottom) noexcept
{
  const float dx = _x1 - _x0;
  const float dy = _y1 - _y0;
  float tEnter = 0.0f;
  float tLeave = 1.0f;
  const float p[4] = {-dx, dx, -dy, dy};
  const float q[4] = {_x0 - _left, _right - _x0, _y0 - _top, _bottom - _y0};
  for (int edge = 0; edge < 4; ++edge)
  {
    if (p[edge] == 0.0f)
    {
      if (q[edge] < 0.0f)
        return false;
      continue;
    }
    const float t = q[edge] / p[edge];
    if (p[edge] < 0.0f)
      tEnter = std::max(tEnter, t);
    else
      tLeave = std::min(tLeave, t);
  }
  if (tEnter > tLeave)
    return false;
  _outX0 = _x0 + dx * tEnter;
  _outY0 = _y0 + dy * tEnter;
  _outX1 = _x0 + dx * tLeave;
  _outY1 = _y0 + dy * tLeave;
  return true;
}
} // namespace

// ------------------------------------------------------------------------------------------------
// Layout. Everything is a rectangle in window pixels, derived once per frame from the tuning
// constants, the DPI and the window size. Panels that hold text size themselves from the atlas
// cell, which is why the cell is an input here.

Hud::Layout Hud::ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept
{
  Layout layout;
  const float s = _dpiScale;
  const float w = static_cast<float>(_widthPx);
  const float h = static_cast<float>(_heightPx);
  layout.scale = s;

  const float labelPx = m_cellPx * HUD_LABEL_SCALE;
  const float valuePx = m_cellPx * HUD_VALUE_SCALE;

  // Resources: label, value, income, each its own run of fixed-pitch cells.
  {
    constexpr int LABEL_CHARS[2] = {2, 5}; // CR, ALLOY
    constexpr int VALUE_CHARS = 6;         // "12,480"
    constexpr int INCOME_CHARS = 5;        // "+42/m"
    float x = HUD_MARGIN_PX * s;
    const float y = HUD_TOP_PX * s;
    const float height = (HUD_PANEL_PAD_Y_PX * 2.0f) * s + valuePx * s;
    for (int i = 0; i < 2; ++i)
    {
      const float width = (HUD_PANEL_PAD_X_PX * 2.0f + HUD_PANEL_GAP_PX * 2.0f) * s +
                          (static_cast<float>(LABEL_CHARS[i] + INCOME_CHARS) * labelPx + static_cast<float>(VALUE_CHARS) * valuePx) * s;
      layout.resources[i] = {x, y, x + width, y + height};
      x += width + HUD_PANEL_GAP_PX * s;
    }
  }

  // Minimap, top right.
  {
    const float x1 = w - HUD_MARGIN_PX * s;
    const float x0 = x1 - HUD_MINIMAP_WIDTH_PX * s;
    const float y0 = HUD_TOP_PX * s;
    const float headerY1 = y0 + HUD_MINIMAP_HEADER_PX * s;
    layout.minimap = {x0, y0, x1, headerY1 + HUD_MINIMAP_MAP_PX * s};
    layout.minimapMap = {x0, headerY1, x1, layout.minimap.y1};
  }

  // Function rail, left edge, centred vertically.
  {
    const float button = HUD_RAIL_BUTTON_PX * s;
    const float gap = HUD_RAIL_GAP_PX * s;
    const float total = button * static_cast<float>(RAIL_BUTTONS) + gap * static_cast<float>(RAIL_BUTTONS - 1);
    const float x0 = HUD_MARGIN_PX * s;
    float y = (h - total) * 0.5f;
    for (int i = 0; i < RAIL_BUTTONS; ++i)
    {
      layout.rail[i] = {x0, y, x0 + button, y + button};
      y += button + gap;
    }
  }

  // Bottom bar, full width.
  layout.bar = {0.0f, h - HUD_BAR_HEIGHT_PX * s, w, h};
  {
    const float bw = HUD_FLEET_BUTTON_W_PX * s;
    const float bh = HUD_FLEET_BUTTON_H_PX * s;
    const float y0 = layout.bar.y0 + (layout.bar.Height() - bh) * 0.5f;
    float x = HUD_MARGIN_PX * s;
    for (int i = 0; i < UniverseView::FLEET_SLOTS; ++i)
    {
      layout.fleets[i] = {x, y0, x + bw, y0 + bh};
      x += bw + HUD_FLEET_GAP_PX * s;
    }
  }

  // Event log, bottom left, above the bar. Not a panel: rows sit straight on the scene.
  {
    const float rows = static_cast<float>(HUD_LOG_ROWS) * HUD_LOG_ROW_PX * s;
    const float y1 = layout.bar.y0 - HUD_MARGIN_PX * s;
    layout.log = {HUD_MARGIN_PX * s, y1 - rows, HUD_MARGIN_PX * s + HUD_SUMMARY_WIDTH_PX * s, y1};
  }

  return layout;
}

bool Hud::OverAnyPanel(const Layout& _layout, float _xPx, float _yPx) const noexcept
{
  for (const Rect& rect : _layout.resources)
  {
    if (rect.Contains(_xPx, _yPx))
      return true;
  }
  for (const Rect& rect : _layout.rail)
  {
    if (rect.Contains(_xPx, _yPx))
      return true;
  }
  return _layout.minimap.Contains(_xPx, _yPx) || _layout.bar.Contains(_xPx, _yPx);
}

// ------------------------------------------------------------------------------------------------
// Drawing primitives. A panel is a fill and a one-pixel outline; there are no corners to round.

void Hud::DrawPanel(TextRenderer& _text, const Rect& _rect, Rgba _fill, Rgba _outline, float _scale) const
{
  const float line = std::max(1.0f, std::floor(_scale));
  _text.DrawScreenRect(_rect.x0, _rect.y0, _rect.x1, _rect.y1, _fill);
  _text.DrawScreenRect(_rect.x0, _rect.y0, _rect.x1, _rect.y0 + line, _outline);
  _text.DrawScreenRect(_rect.x0, _rect.y1 - line, _rect.x1, _rect.y1, _outline);
  _text.DrawScreenRect(_rect.x0, _rect.y0, _rect.x0 + line, _rect.y1, _outline);
  _text.DrawScreenRect(_rect.x1 - line, _rect.y0, _rect.x1, _rect.y1, _outline);
}

void Hud::DrawScanlines(TextRenderer& _text, const Rect& _rect, float _scale) const
{
  const float step = std::max(2.0f, HUD_SCANLINE_STEP_PX * _scale);
  const float line = std::max(1.0f, std::floor(_scale));
  const Rgba dark{0.0f, 0.0f, 0.0f, HUD_SCANLINE_ALPHA};
  for (float y = _rect.y0 + step - line; y < _rect.y1; y += step)
    _text.DrawScreenRect(_rect.x0, y, _rect.x1, std::min(y + line, _rect.y1), dark);
}

// ------------------------------------------------------------------------------------------------

void Hud::DrawResources(TextRenderer& _text, const Layout& _layout, const Frame& _frame) const
{
  const float s = _layout.scale;
  const float labelPx = _text.AdvancePx(FontId::Ui, HUD_LABEL_SCALE * s);
  const float valuePx = _text.AdvancePx(FontId::Ui, HUD_VALUE_SCALE * s);

  const char* labels[2] = {"CR", "ALLOY"};
  const int values[2] = {_frame.credits, _frame.alloy};
  const int incomes[2] = {_frame.creditsPerMin, _frame.alloyPerMin};

  for (int i = 0; i < 2; ++i)
  {
    const Rect& panel = _layout.resources[i];
    DrawPanel(_text, panel, HUD_PANEL_FILL, HUD_PANEL_OUTLINE, s);

    // Everything sits on the value's baseline: the smaller runs are pushed down by the difference
    // in cell height so their bottoms line up.
    const float valueY = panel.y0 + HUD_PANEL_PAD_Y_PX * s;
    const float smallY = valueY + (valuePx - labelPx);
    float x = panel.x0 + HUD_PANEL_PAD_X_PX * s;

    _text.DrawTextLine(FontId::Ui, x, smallY, HUD_LABEL_SCALE * s, HUD_LABEL_COLOUR, labels[i]);
    x += labelPx * static_cast<float>(std::strlen(labels[i])) + HUD_PANEL_GAP_PX * s;

    char value[32] = {};
    FormatThousands(value, sizeof(value), values[i]);
    _text.DrawTextLine(FontId::Ui, x, valueY, HUD_VALUE_SCALE * s, HUD_COLOUR, value);
    x += valuePx * static_cast<float>(std::strlen(value)) + HUD_PANEL_GAP_PX * s;

    char income[32] = {};
    std::snprintf(income, sizeof(income), "%+d/m", incomes[i]);
    _text.DrawTextLine(FontId::Ui, x, smallY, HUD_LABEL_SCALE * s, incomes[i] >= 0 ? HUD_ACCENT_GREEN : HUD_ALERT_RED, income);
  }
}

void Hud::DrawDebug(TextRenderer& _text, const Layout& _layout, const Frame& _frame, std::uint32_t _widthPx) const
{
  const float s = _layout.scale;
  char line[512] = {};
  std::snprintf(line, sizeof(line), "TICK %llu%sTIME %.2fx%s%.0f FPS%s%.2f MS", static_cast<unsigned long long>(_frame.stats.tick),
                SEPARATOR, static_cast<double>(_frame.stats.timeScale), SEPARATOR, static_cast<double>(_frame.stats.fps), SEPARATOR,
                static_cast<double>(_frame.stats.frameMs));
  const float advance = _text.AdvancePx(FontId::Ui, HUD_TEXT_SCALE * s);
  const float top = _layout.resources[0].y0 + HUD_PANEL_PAD_Y_PX * s;
  float width = advance * static_cast<float>(std::strlen(line));
  _text.DrawTextLine(FontId::Ui, (static_cast<float>(_widthPx) - width) * 0.5f, top, HUD_TEXT_SCALE * s, HUD_LABEL_COLOUR, line);

  // The explosion effect. Two of these four are the numbers that go wrong quietly -- a pool that
  // refused a particle and a vertex ring that ran out -- and neither is visible in what is drawn.
  std::snprintf(line, sizeof(line), "FX %d%sPARTICLES %u%sDROPPED %u%sVERTS LOST %u%sDRAWN %u%sCULLED %u", _frame.stats.explosionCount,
                SEPARATOR, _frame.stats.particleCount, SEPARATOR, _frame.stats.particlesDropped, SEPARATOR, _frame.stats.fxVertsDropped,
                SEPARATOR, _frame.stats.submittedCount, SEPARATOR, _frame.stats.culledCount);
  const float lineHeight = _text.LineHeightPx(FontId::Ui, HUD_TEXT_SCALE * s);
  width = advance * static_cast<float>(std::strlen(line));
  _text.DrawTextLine(FontId::Ui, (static_cast<float>(_widthPx) - width) * 0.5f, top + lineHeight, HUD_TEXT_SCALE * s, HUD_LABEL_COLOUR,
                     line);

  // The bodies. Generation time is the number that decides a later slice: the design will move the
  // whole build onto the GPU when boot or a reseed stops feeling instant (Design/Archive/PlanetRenderer.md 17).
  std::snprintf(line, sizeof(line), "BODIES %u%sBODY TRIS %u%sGEN %.1f MS", static_cast<unsigned>(_frame.stats.bodyCount), SEPARATOR,
                _frame.stats.bodyTriangles, SEPARATOR, static_cast<double>(_frame.stats.bodyGenerationMs));
  width = advance * static_cast<float>(std::strlen(line));
  _text.DrawTextLine(FontId::Ui, (static_cast<float>(_widthPx) - width) * 0.5f, top + lineHeight * 2.0f, HUD_TEXT_SCALE * s,
                     HUD_LABEL_COLOUR, line);

  // The router. DECLINED is the one number here that is not a curiosity: an island that refused to
  // build routes nothing and looks like open space, so this is where it says so.
  std::snprintf(line, sizeof(line), "ISLANDS %u%sDECLINED %u", _frame.stats.pathIslandCount, SEPARATOR, _frame.stats.pathIslandsDeclined);
  width = advance * static_cast<float>(std::strlen(line));
  _text.DrawTextLine(FontId::Ui, (static_cast<float>(_widthPx) - width) * 0.5f, top + lineHeight * 3.0f, HUD_TEXT_SCALE * s,
                     HUD_LABEL_COLOUR, line);
}

float Hud::AlertPulse() const noexcept
{
  // A cosine rather than a square wave: the alert holds for ten seconds (Design/Archive/Fleets.md 7.3), so
  // a blink would be a metronome for the whole of a fight. It never reaches zero -- a button that
  // vanishes half the time is a button the eye stops finding.
  const float turns = m_alertPhaseSec / HUD_FLEET_ALERT_PERIOD_SEC;
  const float wave = 0.5f + 0.5f * std::cos(turns * DirectX::XM_2PI);
  return HUD_FLEET_ALERT_MIN_ALPHA + (1.0f - HUD_FLEET_ALERT_MIN_ALPHA) * wave;
}

void Hud::UpdatePulse(float _dtSec)
{
  // Real time and wrapped at the period, for the reason UniverseView::m_navTimeSec is wrapped: a float
  // second counter left running loses enough precision after a few hours that consecutive frames
  // land on the same argument and the pulse freezes.
  m_alertPhaseSec = std::fmod(m_alertPhaseSec + _dtSec, HUD_FLEET_ALERT_PERIOD_SEC);
}

void Hud::DrawMinimap(TextRenderer& _text, const Layout& _layout, std::span<const Game::ShipSnapshot> _ships, const UniverseView& _view,
                      const Camera& _camera, const Frame& _frame, std::uint32_t _widthPx, std::uint32_t _heightPx) const
{
  const float s = _layout.scale;
  const Rect& panel = _layout.minimap;
  const Rect& map = _layout.minimapMap;
  DrawPanel(_text, panel, HUD_PANEL_FILL, HUD_PANEL_OUTLINE, s);

  // Header: sector on the left, contacts on the right, a rule beneath.
  {
    const float textPx = _text.AdvancePx(FontId::Ui, HUD_SMALL_SCALE * s);
    const float y = panel.y0 + (HUD_MINIMAP_HEADER_PX * s - textPx) * 0.5f;
    char line[64] = {};
    std::snprintf(line, sizeof(line), "SECTOR %lld,%lld", static_cast<long long>(_frame.sector.sectorX),
                  static_cast<long long>(_frame.sector.sectorZ));
    _text.DrawTextLine(FontId::Ui, panel.x0 + HUD_PANEL_PAD_Y_PX * s, y, HUD_SMALL_SCALE * s, HUD_COLOUR, line);

    std::snprintf(line, sizeof(line), "CONTACTS %d", _frame.contacts);
    const float width = textPx * static_cast<float>(std::strlen(line));
    _text.DrawTextLine(FontId::Ui, panel.x1 - HUD_PANEL_PAD_Y_PX * s - width, y, HUD_SMALL_SCALE * s,
                       _frame.contacts > 0 ? HUD_ALERT_RED : HUD_ACCENT_AMBER, line);

    const float line1 = std::max(1.0f, std::floor(s));
    _text.DrawScreenRect(panel.x0, map.y0 - line1, panel.x1, map.y0, HUD_PANEL_OUTLINE);
  }

  // A faint grid, in from the outline so the two never sit on the same pixel.
  {
    const Rgba faint = WithAlpha(HUD_PANEL_OUTLINE, HUD_PANEL_OUTLINE.a * 0.5f);
    const float line1 = std::max(1.0f, std::floor(s));
    const float step = HUD_MINIMAP_GRID_PX * s;
    for (float x = map.x0 + step; x < map.x1 - line1; x += step)
      _text.DrawScreenRect(x, map.y0, x + line1, map.y1, faint);
    for (float y = map.y0 + step; y < map.y1 - line1; y += step)
      _text.DrawScreenRect(map.x0, y, map.x1, y + line1, faint);
  }

  // Universe to map: centred on the camera target, north up, east right.
  const XMFLOAT3& centre = _camera.Target();
  const float pxPerMetre = map.Width() / (2.0f * HUD_MINIMAP_HALF_RANGE);
  const float mapCx = (map.x0 + map.x1) * 0.5f;
  const float mapCy = (map.y0 + map.y1) * 0.5f;
  const auto toMapX = [&](float _viewX) { return mapCx + (_viewX - centre.x) * pxPerMetre; };
  const auto toMapY = [&](float _viewZ) { return mapCy - (_viewZ - centre.z) * pxPerMetre; };

  // Sector boundaries, so the header's sector pair has an edge to read against. View metres are
  // universe metres (UniverseView::ViewX), so a boundary sits at a whole multiple of the sector size.
  {
    const float line1 = std::max(1.0f, std::floor(s));
    const Rgba boundary = WithAlpha(HUD_ACCENT_AMBER, 0.45f);
    const float size = Game::SECTOR_SIZE_METRES;
    const auto firstAfter = [](float _metres, float _size) { return std::ceil(_metres / _size) * _size; };
    for (float x = firstAfter(centre.x - HUD_MINIMAP_HALF_RANGE, size); x <= centre.x + HUD_MINIMAP_HALF_RANGE; x += size)
    {
      const float px = std::floor(toMapX(x));
      if (px > map.x0 && px < map.x1 - line1)
        _text.DrawScreenRect(px, map.y0, px + line1, map.y1, boundary);
    }
    for (float z = firstAfter(centre.z - HUD_MINIMAP_HALF_RANGE, size); z <= centre.z + HUD_MINIMAP_HALF_RANGE; z += size)
    {
      const float py = std::floor(toMapY(z));
      if (py > map.y0 && py < map.y1 - line1)
        _text.DrawScreenRect(map.x0, py, map.x1, py + line1, boundary);
    }
  }

  // The camera's view, as its four corners dropped onto the ground. A corner that looks at the sky
  // has no ground point, so that edge is simply not drawn rather than guessed.
  {
    const float w = static_cast<float>(_widthPx);
    const float h = static_cast<float>(_heightPx);
    const float cornersPx[4][2] = {{0.0f, 0.0f}, {w, 0.0f}, {w, h}, {0.0f, h}};
    XMFLOAT3 ground[4];
    bool onGround[4];
    for (int i = 0; i < 4; ++i)
      onGround[i] = _camera.RayToGround(cornersPx[i][0], cornersPx[i][1], ground[i]);

    const Rgba frustum = WithAlpha(HUD_COLOUR, 0.55f);
    const float line1 = std::max(1.0f, std::floor(s));
    for (int i = 0; i < 4; ++i)
    {
      const int j = (i + 1) % 4;
      if (!onGround[i] || !onGround[j])
        continue;
      float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
      if (ClipToRect(toMapX(ground[i].x), toMapY(ground[i].z), toMapX(ground[j].x), toMapY(ground[j].z), x0, y0, x1, y1, map.x0, map.y0,
                     map.x1, map.y1))
        _text.DrawScreenLine(x0, y0, x1, y1, line1, frustum);
    }
  }

  // Marks: every station of the layout, as a hollow diamond in its owner's colour, from static
  // content rather than from a record -- so it is there from the first frame however far off the
  // station is. A second draw path beside the dots and deliberately not a parameter on it: a dot
  // past the edge is clipped, a mark is clamped to the edge at reduced alpha, direction honest and
  // distance saturated, because a mark's whole job is to say which way to fly. Drawn before the
  // dots, so a live record at the same spot draws its filled dot over the hollow mark
  // (Design/Archive/Stations.md 9.3).
  {
    const float half = HUD_MINIMAP_MARK_PX * 0.5f * s;
    const float line1 = std::max(1.0f, std::floor(s));
    for (const UniverseView::StationMark& mark : _view.StationMarks())
    {
      const float rawX = toMapX(_view.ViewX(mark.posUniverse));
      const float rawY = toMapY(_view.ViewZ(mark.posUniverse));
      const float x = std::clamp(rawX, map.x0 + half, map.x1 - half);
      const float y = std::clamp(rawY, map.y0 + half, map.y1 - half);
      const bool clamped = x != rawX || y != rawY;

      const bool own = mark.faction == _frame.ownFaction;
      Rgba colour = OverviewColourOf(mark.faction, own, _view.IsHostileToMe(mark.faction));
      if (clamped)
        colour = WithAlpha(colour, colour.a * HUD_MINIMAP_MARK_CLAMPED_ALPHA);
      _text.DrawScreenLine(x, y - half, x + half, y, line1, colour);
      _text.DrawScreenLine(x + half, y, x, y + half, line1, colour);
      _text.DrawScreenLine(x, y + half, x - half, y, line1, colour);
      _text.DrawScreenLine(x - half, y, x, y - half, line1, colour);
    }
  }

  // The fleets, as their slot digits at the position the status block states. Between the marks and
  // the dots on purpose: a fleet's digit sits over a station's diamond and under its own hulls.
  //
  // The marks' treatment exactly -- inside the map at the position, past it clamped to the edge and
  // dimmed, direction honest and distance saturated -- because a fleet's whole point is that it can
  // be somewhere else, and a digit clipped at the edge would say nothing about which way that is.
  // This is where "spread over the universe" becomes a thing the player can see (Design/Archive/Fleets.md 9.5).
  {
    const float half = HUD_MINIMAP_MARK_PX * 0.5f * s;
    const float digitPx = _text.AdvancePx(FontId::Ui, HUD_LABEL_SCALE * s);
    for (int slot = 0; slot < UniverseView::FLEET_SLOTS; ++slot)
    {
      if (!_view.IsFleetHeld(slot))
        continue;
      const Game::UniversePos at = _view.FleetPosition(slot);
      const float rawX = toMapX(_view.ViewX(at));
      const float rawY = toMapY(_view.ViewZ(at));
      const float x = std::clamp(rawX, map.x0 + half, map.x1 - half);
      const float y = std::clamp(rawY, map.y0 + half, map.y1 - half);
      const bool clamped = x != rawX || y != rawY;

      const bool alert = _view.IsFleetUnderAttack(slot);
      Rgba colour = alert ? WithAlpha(HUD_ALERT_RED, AlertPulse()) : (_view.IsFleetSelected(slot) ? HUD_ACCENT_GREEN : HUD_COLOUR);
      if (clamped)
        colour = WithAlpha(colour, colour.a * HUD_MINIMAP_MARK_CLAMPED_ALPHA);

      char digit[4] = {};
      std::snprintf(digit, sizeof(digit), "%d", slot + 1);
      _text.DrawTextLine(FontId::Ui, x - digitPx * 0.5f, y - digitPx * 0.5f, HUD_LABEL_SCALE * s, colour, digit);
    }
  }

  // Blips, colored by allegiance. The server states whose a ship is and this is where the client
  // turns that identity into a relation, through the update header's mask rather than by inference
  // (OverviewColourOf). A station draws at the larger dot so a base reads bigger than a fighter
  // without pretending to scale (Design/Archive/Hostiles.md 7) -- by the record's own flag, because
  // "immovable hull of another faction" is the client working out server state, which is the thing
  // the flag exists to stop (Design/Archive/Stations.md 6.2).
  {
    const std::span<const Game::ShipSnapshot>& ships = _ships;
    for (size_t i = 0; i < ships.size(); ++i)
    {
      const bool own = ships[i].factionId == _frame.ownFaction;
      const bool station = (ships[i].flags & Game::SHIP_FLAG_STATION) != 0;
      const float dot = station ? HUD_MINIMAP_STRUCTURE_DOT_PX * s : HUD_MINIMAP_DOT_PX * s;
      const float x = toMapX(_view.ViewX(ships[i].posUniverse));
      const float y = toMapY(_view.ViewZ(ships[i].posUniverse));
      // Clipped against the dot's own size, so the bigger square does not hang over the edge.
      if (x < map.x0 + dot || x > map.x1 - dot || y < map.y0 + dot || y > map.y1 - dot)
        continue;

      Rgba colour = OverviewColourOf(ships[i].factionId, own, _view.IsHostileToMe(ships[i].factionId));
      if (own && !_view.IsSelected(i))
        colour = WithAlpha(colour, 0.7f);
      _text.DrawScreenRect(x - dot * 0.5f, y - dot * 0.5f, x + dot * 0.5f, y + dot * 0.5f, colour);
    }
  }
}

void Hud::DrawRail(TextRenderer& _text, const Layout& _layout) const
{
  const float s = _layout.scale;
  const float labelPx = _text.AdvancePx(FontId::Ui, HUD_LABEL_SCALE * s);
  const float icon = HUD_RAIL_ICON_PX * s;

  for (int i = 0; i < RAIL_BUTTONS; ++i)
  {
    const Rect& button = _layout.rail[i];
    const bool lit = (i == m_activeRail) || (i == m_pressedRail);
    const Rgba fill = lit ? WithAlpha(HUD_ACCENT_GREEN, HUD_ACTIVE_FILL_ALPHA) : HUD_PANEL_FILL;
    const Rgba outline = lit ? WithAlpha(HUD_ACCENT_GREEN, HUD_ACTIVE_OUTLINE_ALPHA) : HUD_PANEL_OUTLINE;
    DrawPanel(_text, button, HUD_PANEL_FILL, outline, s);
    if (lit)
      _text.DrawScreenRect(button.x0, button.y0, button.x1, button.y1, fill);

    // Icon above, label beneath, the pair centred in the button.
    const float labelWidth = labelPx * static_cast<float>(std::strlen(RAIL_LABELS[i]));
    const float stack = icon + HUD_PANEL_GAP_PX * 0.5f * s + labelPx;
    const float top = button.y0 + (button.Height() - stack) * 0.5f;
    const float cx = (button.x0 + button.x1) * 0.5f;
    _text.DrawScreenImage(static_cast<ImageId>(i), cx - icon * 0.5f, top, cx + icon * 0.5f, top + icon,
                          lit ? HUD_ACCENT_GREEN : HUD_COLOUR);
    _text.DrawTextLine(FontId::Ui, cx - labelWidth * 0.5f, top + icon + HUD_PANEL_GAP_PX * 0.5f * s, HUD_LABEL_SCALE * s,
                       lit ? HUD_ACCENT_GREEN : HUD_LABEL_COLOUR, RAIL_LABELS[i]);
  }
}

void Hud::DrawEventLog(TextRenderer& _text, const Layout& _layout, const EventLog& _log) const
{
  const float s = _layout.scale;
  const float rowPx = HUD_LOG_ROW_PX * s;
  const float textPx = _text.AdvancePx(FontId::Ui, HUD_TEXT_SCALE * s);
  const float rule = HUD_LOG_RULE_PX * s;
  const int rows = std::min(_log.Count(), HUD_LOG_ROWS);

  for (int row = 0; row < rows; ++row)
  {
    const EventLog::Entry& entry = _log.Newest(row);
    const float y0 = _layout.log.y0 + static_cast<float>(row) * rowPx;
    const float textY = y0 + (rowPx - textPx) * 0.5f;

    Rgba severity = HUD_INFO_GREY;
    if (entry.severity == EventLog::Severity::Alert)
      severity = HUD_ACCENT_AMBER;
    else if (entry.severity == EventLog::Severity::Friendly)
      severity = HUD_ACCENT_GREEN;
    _text.DrawScreenRect(_layout.log.x0, y0 + 2.0f * s, _layout.log.x0 + rule, y0 + rowPx - 2.0f * s, severity);

    const int totalSec = std::max(0, static_cast<int>(entry.timeSec));
    char stamp[16] = {};
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d", (totalSec / 60) % 100, totalSec % 60);
    float x = _layout.log.x0 + rule + HUD_PANEL_GAP_PX * s;
    _text.DrawTextLine(FontId::Ui, x, textY, HUD_TEXT_SCALE * s, HUD_LABEL_COLOUR, stamp);
    x += textPx * 6.0f;
    _text.DrawTextLine(FontId::Ui, x, textY, HUD_TEXT_SCALE * s, HUD_COLOUR, entry.text);
  }
}

void Hud::DrawBottomBar(TextRenderer& _text, const Layout& _layout, std::span<const Game::ShipSnapshot> _ships, const UniverseView& _view,
                        const Frame& _frame) const
{
  const float s = _layout.scale;
  const Rect& bar = _layout.bar;
  const float line1 = std::max(1.0f, std::floor(s));
  const float textPx = _text.AdvancePx(FontId::Ui, HUD_TEXT_SCALE * s);
  const float labelPx = _text.AdvancePx(FontId::Ui, HUD_LABEL_SCALE * s);

  _text.DrawScreenRect(bar.x0, bar.y0, bar.x1, bar.y1, HUD_PANEL_FILL);
  _text.DrawScreenRect(bar.x0, bar.y0, bar.x1, bar.y0 + line1, HUD_PANEL_OUTLINE);

  const auto separator = [&](float _x)
  { _text.DrawScreenRect(_x, bar.y0 + HUD_MARGIN_PX * s, _x + line1, bar.y1 - HUD_MARGIN_PX * s, HUD_PANEL_OUTLINE); };

  // --- the five fleet slots ---------------------------------------------------------------------
  // Occupancy is the status block's mask and never an empty roster: a composed fleet holds its slot
  // with nobody in space yet, and the mask rides every update where a roster is stated once
  // (Design/Archive/Fleets.md 8.1's amendment).
  for (int i = 0; i < UniverseView::FLEET_SLOTS; ++i)
  {
    const Rect& button = _layout.fleets[i];
    const bool held = _view.IsFleetHeld(i);
    const bool lit = held && (_view.IsFleetSelected(i) || i == m_pressedFleet);
    const bool alert = _view.IsFleetUnderAttack(i);

    // The pulse rides on top of whatever the button would otherwise be, so a selected fleet under
    // attack still reads as selected. It is the alert that changes colour, not the state.
    const Rgba accent = alert ? WithAlpha(HUD_ALERT_RED, AlertPulse()) : HUD_ACCENT_GREEN;
    const Rgba outline = (lit || alert) ? WithAlpha(accent, HUD_ACTIVE_OUTLINE_ALPHA * (alert ? AlertPulse() : 1.0f)) : HUD_PANEL_OUTLINE;
    DrawPanel(_text, button, HUD_PANEL_FILL, outline, s);
    if (lit || alert)
      _text.DrawScreenRect(button.x0, button.y0, button.x1, button.y1, WithAlpha(accent, HUD_FLEET_ACTIVE_FILL_ALPHA));

    char number[4] = {};
    std::snprintf(number, sizeof(number), "%d", i + 1);
    const Rgba digit = alert ? accent : (lit ? HUD_ACCENT_GREEN : (held ? HUD_COLOUR : HUD_LABEL_COLOUR));
    _text.DrawTextLine(FontId::Ui, button.x0 + HUD_PANEL_GAP_PX * 0.6f * s, button.y0 + HUD_PANEL_GAP_PX * 0.6f * s, HUD_LABEL_SCALE * s,
                       digit, number);

    // The composed size, so the button says eight from the moment the fleet exists rather than
    // climbing as the hulls launch. How many are actually out is the roster's, and the sheet is
    // where the difference is spelled (Design/Archive/Fleets.md 8.2).
    char count[16] = {};
    const int size = _view.FleetCount(i);
    if (held)
      std::snprintf(count, sizeof(count), "%s%d", TIMES, size);
    else
      std::snprintf(count, sizeof(count), "-");
    const float width = textPx * static_cast<float>(std::strlen(count));
    _text.DrawTextLine(FontId::Ui, (button.x0 + button.x1 - width) * 0.5f, button.y1 - HUD_PANEL_GAP_PX * 0.8f * s - textPx,
                       HUD_TEXT_SCALE * s, held ? HUD_COLOUR : HUD_LABEL_COLOUR, count);
  }

  float x = _layout.fleets[UniverseView::FLEET_SLOTS - 1].x1 + HUD_PANEL_GAP_PX * s;
  separator(x);
  x += HUD_PANEL_GAP_PX * s + line1;

  // --- selection summary ------------------------------------------------------------------------
  {
    const std::span<const Game::ShipSnapshot>& ships = _ships;
    int hullCounts[MAX_HULL_KINDS] = {};
    for (size_t i = 0; i < ships.size(); ++i)
    {
      if (_view.IsSelected(i))
        ++hullCounts[std::min<std::uint32_t>(ships[i].hullId, MAX_HULL_KINDS - 1)];
    }

    char title[32] = {};
    const int fleets = _view.SelectedFleetCount();
    if (fleets == 1)
      std::snprintf(title, sizeof(title), "FLEET %d", _view.FirstSelectedFleet() + 1);
    else if (fleets > 1)
      std::snprintf(title, sizeof(title), "%d FLEETS", fleets);
    else
      std::snprintf(title, sizeof(title), "NO SELECTION");
    const float titleY = bar.y0 + HUD_MARGIN_PX * 1.25f * s;
    _text.DrawTextLine(FontId::Ui, x, titleY, HUD_TEXT_SCALE * s, fleets > 0 ? HUD_ACCENT_GREEN : HUD_LABEL_COLOUR, title);

    // How many ships are COMMANDED, from the status blocks, and then the hulls among them this
    // client can currently see. The two are different numbers now and the bar has to say the first:
    // a fleet ordered across the map is selected with none of its hulls in the interest set, and a
    // count of visible records would read "0 SELECTED" under a title naming a live fleet.
    int commanded = 0;
    for (int slot = 0; slot < UniverseView::FLEET_SLOTS; ++slot)
      commanded += _view.IsFleetSelected(slot) ? _view.FleetCount(slot) : 0;

    char line[256] = {};
    int written = std::snprintf(line, sizeof(line), "%d SHIPS", commanded);
    for (int hull = 0; hull < MAX_HULL_KINDS && written > 0 && written < static_cast<int>(sizeof(line)); ++hull)
    {
      if (hullCounts[hull] == 0)
        continue;
      const bool named = static_cast<size_t>(hull) < _frame.hullNames.size();
      written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written), "%s%d %s", SEPARATOR, hullCounts[hull],
                               named ? _frame.hullNames[static_cast<size_t>(hull)] : "HULL");
    }
    _text.DrawTextLine(FontId::Ui, x, titleY + textPx + HUD_PANEL_GAP_PX * 0.6f * s, HUD_TEXT_SCALE * s, HUD_COLOUR, line);

    x += HUD_SUMMARY_WIDTH_PX * s;
    separator(x);
    x += HUD_PANEL_GAP_PX * s + line1;
  }

  // --- selection stats --------------------------------------------------------------------------
  // Anchored to the right edge where the window is wide enough, so the bar reads the same at any
  // width; on a narrow one it flows on after the summary instead.
  x = std::max(x, bar.x1 - (HUD_MARGIN_PX + HUD_STATS_WIDTH_PX) * s);
  {
    const std::span<const Game::ShipSnapshot>& ships = _ships;
    int selected = 0;
    float speedSum = 0.0f;
    bool anyMoving = false;
    bool anyAligning = false;
    for (size_t i = 0; i < ships.size(); ++i)
    {
      if (!_view.IsSelected(i))
        continue;
      ++selected;
      speedSum += ships[i].speed;
      anyMoving = anyMoving || ships[i].order == Game::OrderState::Moving;
      anyAligning = anyAligning || ships[i].order == Game::OrderState::Aligning;
    }

    const float row1 = bar.y0 + HUD_MARGIN_PX * 1.25f * s;
    const float row2 = row1 + textPx + HUD_PANEL_GAP_PX * 0.6f * s;
    const float barWidth = HUD_STAT_BAR_WIDTH_PX * s;
    const float barPx = HUD_STAT_BAR_PX * s;
    const float labelWidth = labelPx * 7.0f; // "SHIELD" and a space

    const auto statBar = [&](float _y, const char* _label, float _fraction, Rgba _fill)
    {
      _text.DrawTextLine(FontId::Ui, x, _y + (textPx - labelPx), HUD_LABEL_SCALE * s, HUD_LABEL_COLOUR, _label);
      const float bx = x + labelWidth;
      const float by = _y + (textPx - barPx) * 0.5f;
      const float filled = std::clamp(_fraction, 0.0f, 1.0f);
      _text.DrawScreenRect(bx, by, bx + barWidth, by + barPx, HUD_BAR_TRACK);
      if (selected > 0 && filled > 0.0f)
        _text.DrawScreenRect(bx, by, bx + barWidth * filled, by + barPx, _fill);
      char percent[16] = {};
      if (selected > 0)
        std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(std::lround(filled * 100.0f)));
      else
        std::snprintf(percent, sizeof(percent), "-");
      _text.DrawTextLine(FontId::Ui, bx + barWidth + HUD_PANEL_GAP_PX * s, _y, HUD_TEXT_SCALE * s, HUD_COLOUR, percent);
    };
    statBar(row1, "HULL", _frame.hullFraction, SELECTABLE_LIVERIES[PLAYER_LIVERY_INDEX]);
    statBar(row2, "SHIELD", _frame.shieldFraction, HUD_ACCENT_GREEN);

    const float column = x + HUD_STAT_COLUMN_PX * s;
    const float valueX = column + labelPx * 6.0f;
    char speed[32] = {};
    if (selected > 0)
      std::snprintf(speed, sizeof(speed), "%d m/s", static_cast<int>(std::lround(speedSum / static_cast<float>(selected))));
    else
      std::snprintf(speed, sizeof(speed), "-");
    _text.DrawTextLine(FontId::Ui, column, row1 + (textPx - labelPx), HUD_LABEL_SCALE * s, HUD_LABEL_COLOUR, "SPEED");
    _text.DrawTextLine(FontId::Ui, valueX, row1, HUD_TEXT_SCALE * s, HUD_COLOUR, speed);

    const char* order = "-";
    if (selected > 0)
      order = anyMoving ? ORDER_NAMES[1] : (anyAligning ? ORDER_NAMES[2] : ORDER_NAMES[0]);
    _text.DrawTextLine(FontId::Ui, column, row2 + (textPx - labelPx), HUD_LABEL_SCALE * s, HUD_LABEL_COLOUR, "ORDER");
    _text.DrawTextLine(FontId::Ui, valueX, row2, HUD_TEXT_SCALE * s, selected > 0 ? HUD_ACCENT_AMBER : HUD_COLOUR, order);
  }
}

// ------------------------------------------------------------------------------------------------

void Hud::Draw(TextRenderer& _text, std::span<const Game::ShipSnapshot> _ships, const UniverseView& _view, const Camera& _camera,
               const EventLog& _log, const Frame& _frame, float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx)
{
  m_cellPx = std::max(1.0f, _text.AdvancePx(FontId::Ui, 1.0f));
  const Layout layout = ComputeLayout(_dpiScale, _widthPx, _heightPx);

  DrawResources(_text, layout, _frame);
  if (_frame.showDebug)
    DrawDebug(_text, layout, _frame, _widthPx);
  DrawMinimap(_text, layout, _ships, _view, _camera, _frame, _widthPx, _heightPx);
  DrawRail(_text, layout);
  DrawEventLog(_text, layout, _log);
  DrawBottomBar(_text, layout, _ships, _view, _frame);

  // Scanlines go over the panels and only the panels: the scene underneath is not a CRT.
  if constexpr (HUD_SCANLINES)
  {
    for (const Rect& rect : layout.resources)
      DrawScanlines(_text, rect, layout.scale);
    DrawScanlines(_text, layout.minimap, layout.scale);
    for (const Rect& rect : layout.rail)
      DrawScanlines(_text, rect, layout.scale);
    DrawScanlines(_text, layout.bar, layout.scale);
  }
}

// ------------------------------------------------------------------------------------------------
// Input. A press that starts on a panel belongs to the HUD until it lifts; nothing about it reaches
// the tracker, so it can neither pick a hull nor lay down an order through the bar.

bool Hud::HandlePointer(const PointerEvent& _event, UniverseView& _view, int& _outOpenSheet, float _dpiScale, std::uint32_t _widthPx,
                        std::uint32_t _heightPx)
{
  const Layout layout = ComputeLayout(_dpiScale, _widthPx, _heightPx);

  if (_event.kind == PointerEvent::Kind::Wheel)
    return OverAnyPanel(layout, _event.xPx, _event.yPx); // no zooming the universe through the minimap

  if (_event.kind == PointerEvent::Kind::Down)
  {
    if (m_captured || !OverAnyPanel(layout, _event.xPx, _event.yPx))
      return false;

    m_captured = true;
    m_capturedPointer = _event.pointerId;
    m_downQpc = _event.timestampQpc;
    m_pressedRail = -1;
    m_pressedFleet = -1;
    for (int i = 0; i < RAIL_BUTTONS; ++i)
    {
      if (layout.rail[i].Contains(_event.xPx, _event.yPx))
        m_pressedRail = i;
    }
    for (int i = 0; i < UniverseView::FLEET_SLOTS; ++i)
    {
      if (layout.fleets[i].Contains(_event.xPx, _event.yPx))
        m_pressedFleet = i;
    }
    return true;
  }

  if (!m_captured || _event.pointerId != m_capturedPointer)
    return false;

  if (_event.kind == PointerEvent::Kind::Update)
    return true;

  // Release. A button only fires if the contact lifts inside it, which is what lets a press be
  // cancelled by sliding off.
  if (m_pressedRail >= 0 && layout.rail[m_pressedRail].Contains(_event.xPx, _event.yPx))
    m_activeRail = (m_activeRail == m_pressedRail) ? -1 : m_pressedRail;

  // What a press MEANS is the view's, not the HUD's: this class knows where a contact landed and
  // UniverseView knows what a fleet slot is. The same division the selection calls already keep -- and
  // opening a panel is a third thing, which belongs to neither, so it is reported upward.
  if (m_pressedFleet >= 0 && layout.fleets[m_pressedFleet].Contains(_event.xPx, _event.yPx))
  {
    const bool held = m_clock.ElapsedMs(m_downQpc, _event.timestampQpc) >= HUD_LONG_PRESS_MS;
    if (_view.PressFleetButton(m_pressedFleet, held) == UniverseView::ButtonPress::OpenSheet)
      _outOpenSheet = m_pressedFleet;
  }

  m_captured = false;
  m_pressedRail = -1;
  m_pressedFleet = -1;
  return true;
}
} // namespace Outpost
