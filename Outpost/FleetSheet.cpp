#include "pch.h"
#include "FleetSheet.h"

#include "ViewTuning.h"

using namespace Neuron;

namespace Outpost
{
namespace
{
constexpr char TIMES[] = "\xD7";

// In the order the design lists them, and MINE is not among them (Design/Fleets.md 9.3, 6.6).
constexpr const char* COMMAND_LABELS[] = {"MOVE", "ATTACK", "DOCK", "STOP"};
constexpr WorldView::ArmedOrder COMMAND_ARMS[] = {WorldView::ArmedOrder::Move, WorldView::ArmedOrder::Attack, WorldView::ArmedOrder::Dock,
                                                  WorldView::ArmedOrder::None};
static_assert(std::size(COMMAND_LABELS) == std::size(COMMAND_ARMS), "a command lost its meaning");

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

void FleetSheet::Update(const WorldView& _view) noexcept
{
  if (m_slot < 0)
    return;

  // A fleet can dock or lose its last ship while its sheet is open. Closing is the honest answer:
  // a panel describing a slot nobody holds would keep offering commands nothing could carry out.
  //
  // And a sheet whose fleet is no longer selected closes too. Opening one selects its fleet, so the
  // only way to lose the selection is to take hold of another -- and a sheet that then kept naming
  // the old fleet while its commands went to the new one is the one way this panel could lie.
  if (!_view.IsFleetHeld(m_slot) || !_view.IsFleetSelected(m_slot))
    Close();
}

FleetSheet::Layout FleetSheet::ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept
{
  Layout layout;
  const float s = std::max(0.5f, _dpiScale);
  layout.scale = s;

  // Above the bar and hard against the left margin, over the buttons it was opened from -- so the
  // panel and the button it belongs to are the same place on screen.
  const float w = std::max(1.0f, std::min(HUD_SHEET_WIDTH_PX * s, static_cast<float>(_widthPx) - 2.0f * HUD_MARGIN_PX * s));
  const float h = HUD_SHEET_HEIGHT_PX * s;
  const float x0 = HUD_MARGIN_PX * s;
  const float y1 = static_cast<float>(_heightPx) - HUD_BAR_HEIGHT_PX * s - HUD_MARGIN_PX * 0.5f * s;
  layout.panel = {x0, y1 - h, x0 + w, y1};

  const float pad = HUD_SHEET_PAD_PX * s;
  const float gap = HUD_SHEET_COMMAND_GAP_PX * s;
  const float buttonH = HUD_SHEET_COMMAND_H_PX * s;
  const float buttonW = (w - 2.0f * pad - gap * static_cast<float>(COMMANDS - 1)) / static_cast<float>(COMMANDS);
  const float buttonY = layout.panel.y1 - pad - buttonH;
  for (int at = 0; at < COMMANDS; ++at)
  {
    const float bx = x0 + pad + static_cast<float>(at) * (buttonW + gap);
    layout.commands[at] = {bx, buttonY, bx + buttonW, buttonY + buttonH};
  }
  return layout;
}

void FleetSheet::Draw(TextRenderer& _text, const WorldView& _view, std::span<const char* const> _hullNames, float _dpiScale,
                      std::uint32_t _widthPx, std::uint32_t _heightPx) const
{
  const float s = std::max(0.5f, _dpiScale);
  const float textPx = _text.AdvancePx(FontId::Ui, HUD_TEXT_SCALE * s);
  const float labelPx = _text.AdvancePx(FontId::Ui, HUD_LABEL_SCALE * s);

  // The armed prompt stands in the sheet's own place once a command has closed it, so the state is
  // visible while it is live rather than only in the log line that announced it.
  if (_view.Armed() != WorldView::ArmedOrder::None)
  {
    const Layout layout = ComputeLayout(_dpiScale, _widthPx, _heightPx);
    const float h = HUD_SHEET_PROMPT_H_PX * s;
    const float y0 = layout.panel.y1 - h;
    DrawPanel(_text, layout.panel.x0, y0, layout.panel.x1, layout.panel.y1, HUD_PANEL_FILL,
              WithAlpha(HUD_ACCENT_AMBER, HUD_ACTIVE_OUTLINE_ALPHA), s);

    const WorldView::ArmedOrder armed = _view.Armed();
    const char* verb = (armed == WorldView::ArmedOrder::Move) ? "MOVE" : ((armed == WorldView::ArmedOrder::Attack) ? "ATTACK" : "DOCK");
    char line[64] = {};
    std::snprintf(line, sizeof(line), "%s | TAP A TARGET", verb);
    _text.DrawTextLine(FontId::Ui, layout.panel.x0 + HUD_SHEET_PAD_PX * s, y0 + (h - textPx) * 0.5f, HUD_TEXT_SCALE * s, HUD_ACCENT_AMBER,
                       line);
    return;
  }

  if (m_slot < 0)
    return;

  static_assert(std::size(COMMAND_LABELS) == static_cast<std::size_t>(COMMANDS), "the command row and its labels disagree");

  const Layout layout = ComputeLayout(_dpiScale, _widthPx, _heightPx);
  const float pad = HUD_SHEET_PAD_PX * s;
  DrawPanel(_text, layout.panel.x0, layout.panel.y0, layout.panel.x1, layout.panel.y1, HUD_PANEL_FILL, HUD_PANEL_OUTLINE, s);

  float y = layout.panel.y0 + pad;

  // --- FLEET 3                          x 6 -----------------------------------------------------
  {
    char title[32] = {};
    std::snprintf(title, sizeof(title), "FLEET %d", m_slot + 1);
    const bool alert = _view.IsFleetUnderAttack(m_slot);
    _text.DrawTextLine(FontId::Ui, layout.panel.x0 + pad, y, HUD_TEXT_SCALE * s, alert ? HUD_ALERT_RED : HUD_ACCENT_GREEN, title);

    char count[16] = {};
    std::snprintf(count, sizeof(count), "%s%d", TIMES, _view.FleetCount(m_slot));
    const float width = textPx * static_cast<float>(std::strlen(count));
    _text.DrawTextLine(FontId::Ui, layout.panel.x1 - pad - width, y, HUD_TEXT_SCALE * s, HUD_COLOUR, count);
    y += textPx + pad * 0.4f;
  }

  // --- ENGAGED - DEFENDING, or LAUNCHING 4 OF 8 --------------------------------------------------
  {
    const std::uint8_t bits = _view.FleetStatusBits(m_slot);
    const int out = static_cast<int>(_view.RosterOf(m_slot).size());
    char status[64] = {};
    if ((bits & Game::FLEET_STATUS_KIND_MASK) == Game::FLEET_STATUS_LAUNCHING)
    {
      // The roster's size against the status block's count -- the two numbers Design/Fleets.md 8.2's
      // amendment named this line as the use for.
      std::snprintf(status, sizeof(status), "LAUNCHING %d OF %d", out, _view.FleetCount(m_slot));
    }
    else if ((bits & Game::FLEET_STATUS_ENGAGED) != 0)
    {
      // A bar rather than the design's em dash: the UI atlas holds Latin-1 from 192 up, and an em
      // dash is not in it. The same stand-in the HUD's own field separator makes.
      std::snprintf(status, sizeof(status), "ENGAGED | DEFENDING");
    }
    else
    {
      std::snprintf(status, sizeof(status), "%s", _view.FleetActivity(m_slot));
    }
    const bool engaged = (bits & Game::FLEET_STATUS_ENGAGED) != 0;
    _text.DrawTextLine(FontId::Ui, layout.panel.x0 + pad, y, HUD_LABEL_SCALE * s, engaged ? HUD_ALERT_RED : HUD_LABEL_COLOUR, status);
    y += labelPx + pad * 0.6f;
  }

  // --- CORVETTE x 2   MINER x 3   HAULER x 1 -----------------------------------------------------
  {
    // Counted by hull id, so the row reads in the order the manifest launches in. A member this
    // half has never held a record for counts as an unnamed hull rather than as a guessed one --
    // which is what a fleet composed while the camera was elsewhere looks like from here.
    int counts[Game::HULL_COUNT] = {};
    int unknown = 0;
    for (const Game::EntityId member : _view.RosterOf(m_slot))
    {
      const std::uint32_t hull = _view.HullOfMember(member);
      if (hull < Game::HULL_COUNT)
        ++counts[hull];
      else
        ++unknown;
    }

    // snprintf returns what it WOULD have written, so the running offset is clamped after each
    // one: unclamped, a truncation would step the next write past the end of the buffer.
    char line[192] = {};
    std::size_t written = 0;
    const auto append = [&](const char* _name, int _count)
    {
      if (written >= sizeof(line) - 1)
        return;
      const int wrote =
        std::snprintf(line + written, sizeof(line) - written, "%s%s %s%d", (written > 0) ? "   " : "", _name, TIMES, _count);
      written = (wrote < 0) ? written : std::min(written + static_cast<std::size_t>(wrote), sizeof(line) - 1);
    };

    for (std::uint32_t hull = 0; hull < Game::HULL_COUNT; ++hull)
    {
      if (counts[hull] == 0)
        continue;
      const bool named = static_cast<std::size_t>(hull) < _hullNames.size();
      append(named ? _hullNames[static_cast<std::size_t>(hull)] : "HULL", counts[hull]);
    }
    if (unknown > 0)
      append("UNKNOWN", unknown);
    if (written == 0)
      (void)std::snprintf(line, sizeof(line), "NOBODY IN SPACE YET");

    _text.DrawTextLine(FontId::Ui, layout.panel.x0 + pad, y, HUD_TEXT_SCALE * s, HUD_COLOUR, line);
  }

  // --- [ MOVE ] [ ATTACK ] [ DOCK ] [ STOP ] -----------------------------------------------------
  for (int at = 0; at < COMMANDS; ++at)
  {
    const Rect& button = layout.commands[at];
    DrawPanel(_text, button.x0, button.y0, button.x1, button.y1, HUD_PANEL_FILL, WithAlpha(HUD_ACCENT_GREEN, HUD_ACTIVE_OUTLINE_ALPHA), s);
    const float width = labelPx * static_cast<float>(std::strlen(COMMAND_LABELS[at]));
    _text.DrawTextLine(FontId::Ui, (button.x0 + button.x1 - width) * 0.5f, (button.y0 + button.y1 - labelPx) * 0.5f, HUD_LABEL_SCALE * s,
                       HUD_ACCENT_GREEN, COMMAND_LABELS[at]);
  }

  if constexpr (HUD_SCANLINES)
  {
    const float step = std::max(2.0f, HUD_SCANLINE_STEP_PX * s);
    const float line = std::max(1.0f, std::floor(s));
    const Rgba dark{0.0f, 0.0f, 0.0f, HUD_SCANLINE_ALPHA};
    for (float at = layout.panel.y0 + step - line; at < layout.panel.y1; at += step)
      _text.DrawScreenRect(layout.panel.x0, at, layout.panel.x1, std::min(at + line, layout.panel.y1), dark);
  }
}

bool FleetSheet::HandlePointer(const PointerEvent& _event, WorldView& _view, float _dpiScale, std::uint32_t _widthPx,
                               std::uint32_t _heightPx)
{
  if (m_slot < 0)
  {
    m_captured = false;
    return false;
  }

  const Layout layout = ComputeLayout(_dpiScale, _widthPx, _heightPx);

  if (_event.kind == PointerEvent::Kind::Down)
  {
    if (m_captured || !layout.panel.Contains(_event.xPx, _event.yPx))
      return false; // not modal: a press past the panel is a press on the world, and stays one
    m_captured = true;
    m_capturedPointer = _event.pointerId;
    return true;
  }

  // Only what this class captured. A contact that began on the world and drifted over the panel is
  // still the world's, and it is still the world that will be told it lifted.
  if (!m_captured || _event.pointerId != m_capturedPointer)
    return false;
  if (_event.kind == PointerEvent::Kind::Update)
    return true;

  m_captured = false;
  for (int at = 0; at < COMMANDS; ++at)
  {
    // A button only fires if the contact lifts inside it, which is what lets a press be cancelled
    // by sliding off -- Hud's rule, kept.
    if (!layout.commands[at].Contains(_event.xPx, _event.yPx))
      continue;

    // STOP needs no target, so it is the one command that sends immediately. The other three arm
    // the next world tap and close, which is what turns a named verb into an order
    // (Design/Fleets.md 9.3).
    if (COMMAND_ARMS[at] == WorldView::ArmedOrder::None)
      _view.IssueStopOrder();
    else
      _view.ArmFleetOrder(COMMAND_ARMS[at]);
    Close();
    return true;
  }
  return true;
}
} // namespace Outpost
