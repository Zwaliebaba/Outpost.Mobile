#pragma once

#include "EventLog.h"
#include "UniverseView.h"

#include "Camera.h"
#include "PointerEvent.h"
#include "TextRenderer.h"

#include "FrameClock.h"

#include <cstdint>
#include <span>

namespace Outpost
{
// The main game screen's overlay: resources, minimap, function rail, event log and the bottom bar,
// plus the debug readout it grew out of. Separate on purpose: it is the thing that grows every
// week, and it should grow somewhere that is not the frame loop.
//
// It reads the snapshot the client half received and UniverseView, and writes to neither except
// through UniverseView's own selection calls, so nothing here can feed back into a tick. Everything is laid out from
// ViewTuning.h constants, anchored to the corners and edges and scaled by DPI, and the draw path
// allocates nothing: every string goes through a fixed buffer.
class Hud
{
public:
  struct Stats
  {
    float fps = 0.0f;
    float frameMs = 0.0f;
    std::uint64_t tick = 0;
    int selectedCount = 0;
    std::uint32_t shipCount = 0;
    float timeScale = 1.0f;
    // What the tick itself costs, from the composition root's own clock (Outpost/TickStats.h). On
    // the screen for the reason every other number here is: the step and the publish scale with
    // different things, and a shard that is falling behind looks exactly like one that is not until
    // you read them (Design/TickTelemetry-work-order.md 1.4).
    double stepMs = 0.0;
    double publishMs = 0.0;
    double stepWorstMs = 0.0;
    std::uint32_t subscriberCount = 0;
    // The explosion effect, so an overflowing pool or a full vertex ring is read off the screen
    // rather than guessed at (Design/Archive/SpaceshipExplosion.md 6.2, 8.1).
    int explosionCount = 0;
    std::uint32_t particleCount = 0;
    std::uint32_t particlesDropped = 0;
    std::uint32_t fxVertsDropped = 0;
    // The bodies (Design/Archive/PlanetRenderer.md 9). Generation time is what decides whether the compute
    // bake of design 17 is due, so it is read off the screen rather than guessed at.
    std::size_t bodyCount = 0;
    std::uint32_t bodyTriangles = 0;
    float bodyGenerationMs = 0.0f;
    // Frustum culling (Design/Archive/MmoScalabilityReview.md G2). On the screen rather than inferred,
    // because a culler that is quietly rejecting everything and one that is quietly rejecting
    // nothing both look exactly like a working one until you count.
    std::uint32_t submittedCount = 0;
    std::uint32_t culledCount = 0;
    // The router (Design/Archive/RegionalPathfinding.md 3.3). An island that refused to build is the one
    // failure here that looks exactly like success: every route through it is a straight line and
    // every ship crossing it steers locally, which is what the universe looked like before there was a
    // planner at all. DECLINED above zero is the only thing that says so.
    std::uint32_t pathIslandCount = 0;
    std::uint32_t pathIslandsDeclined = 0;
  };

  // The rail's icons, which are the images the composition root lists in TextRenderer::Desc, in
  // this order. The screens they open are not built yet; a tap only toggles the button.
  enum class RailIcon : Neuron::ImageId
  {
    Research,
    Wallet,
    Storage,
    Universe
  };
  static constexpr int RAIL_BUTTONS = 4;

  // What the composition root knows and the HUD does not. The economy has no home in the universe
  // yet, so its numbers arrive from here; the defaults are the mock's so the panel reads right
  // until it does. The same goes for hull and shield, which have no damage model to read.
  //
  // Three of these are real. The sector is the position under the camera target, of which the
  // minimap names the sector pair. contacts is the count of records this client holds whose faction
  // holds it hostile, by the update header's mask -- the subscription, not the map rectangle, so a
  // contact past the map edge is counted and clipped, and not "not mine", so a Vanguard station in
  // view is not one (Design/Archive/Stations.md 9.4). ownFaction is session identity: the HUD colors by
  // allegiance and cannot know whose side it is on without being told.
  struct Frame
  {
    Stats stats;
    bool showDebug = false;
    int credits = 12480;
    int creditsPerMin = 42;
    int alloy = 3215;
    int alloyPerMin = 8;
    Game::UniversePos sector;
    int contacts = 0;
    Game::FactionId ownFaction = Game::FACTION_PLAYER;
    float hullFraction = 1.0f;
    float shieldFraction = 1.0f;
    std::span<const char* const> hullNames; // indexed by ShipSnapshot::hullId; an id past the end is unnamed
    // Indexed by FactionId, on the same terms. Plumbed here for the docking refusal line, which is
    // the first thing to print one and arrives with Stations slice 6; nothing reads it yet.
    std::span<const char* const> factionNames;
  };

  void Draw(Neuron::TextRenderer& _text, std::span<const Game::ShipSnapshot> _ships, const UniverseView& _view,
            const Neuron::Camera& _camera, const EventLog& _log, const Frame& _frame, float _dpiScale, std::uint32_t _widthPx,
            std::uint32_t _heightPx);

  // The alert pulse's clock. Called once a frame by the composition root, before Draw, because the
  // HUD never reads a clock itself and this is the one piece of it that has to advance whether or
  // not anything was redrawn.
  //
  // The log line the rising edge earns is UniverseView's now: two of Design/Archive/Fleets.md 9.6's other lines
  // turn on a departure's stated cause, which only that half sees, and splitting one section's log
  // across two files by which line happened to need what is the shape to avoid
  // (Design/Archive/Fleets-slice-8.md 2.1).
  void UpdatePulse(float _dtSec);

  // Reports true when the event landed on a panel and was used up, so a tap on the bottom bar can
  // never fall through to the tracker as a ground order. A contact that went down on a panel is
  // kept until it lifts, whatever it drifts over in between.
  // _outOpenSheet is set to the slot a long press asked to read, or left alone. Reported rather
  // than acted on, because a panel is the composition root's to own and this class's job ends at
  // saying where a contact landed.
  [[nodiscard]] bool HandlePointer(const Neuron::PointerEvent& _event, UniverseView& _view, int& _outOpenSheet, float _dpiScale,
                                   std::uint32_t _widthPx, std::uint32_t _heightPx);

  // Drops a capture this class is holding, for PointerTracker::CancelContacts's reason: a contact
  // that went down on a panel and lifts after something modal has taken the pointer away never
  // reaches the release below, so the capture would stand for ever -- and a stuck capture makes
  // every later press on the bar fall through to the universe as an order.
  void CancelCapture() noexcept
  {
    m_captured = false;
    m_pressedRail = -1;
    m_pressedFleet = -1;
  }

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
    [[nodiscard]] float Height() const noexcept
    {
      return y1 - y0;
    }
  };

  // Every panel's rectangle for one window size, so the draw and the hit test cannot disagree.
  struct Layout
  {
    float scale = 1.0f; // DPI
    Rect resources[2];
    Rect minimap; // header and map together
    Rect minimapMap;
    Rect rail[RAIL_BUTTONS];
    Rect log;
    Rect bar;
    Rect fleets[UniverseView::FLEET_SLOTS];
  };

  [[nodiscard]] Layout ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept;

  // The alert's brightness this frame, in [HUD_FLEET_ALERT_MIN_ALPHA, 1]. One definition, read by
  // the button and by the minimap digit, so the two cannot pulse out of step.
  [[nodiscard]] float AlertPulse() const noexcept;
  [[nodiscard]] bool OverAnyPanel(const Layout& _layout, float _xPx, float _yPx) const noexcept;

  void DrawPanel(Neuron::TextRenderer& _text, const Rect& _rect, Neuron::Rgba _fill, Neuron::Rgba _outline, float _scale) const;
  void DrawScanlines(Neuron::TextRenderer& _text, const Rect& _rect, float _scale) const;
  void DrawResources(Neuron::TextRenderer& _text, const Layout& _layout, const Frame& _frame) const;
  void DrawDebug(Neuron::TextRenderer& _text, const Layout& _layout, const Frame& _frame, std::uint32_t _widthPx) const;
  void DrawMinimap(Neuron::TextRenderer& _text, const Layout& _layout, std::span<const Game::ShipSnapshot> _ships,
                   const UniverseView& _view, const Neuron::Camera& _camera, const Frame& _frame, std::uint32_t _widthPx,
                   std::uint32_t _heightPx) const;
  void DrawRail(Neuron::TextRenderer& _text, const Layout& _layout) const;
  void DrawEventLog(Neuron::TextRenderer& _text, const Layout& _layout, const EventLog& _log) const;
  void DrawBottomBar(Neuron::TextRenderer& _text, const Layout& _layout, std::span<const Game::ShipSnapshot> _ships,
                     const UniverseView& _view, const Frame& _frame) const;

  float m_cellPx = 16.0f; // the UI atlas cell, remembered from the last draw so a hit test can size the panels the same way

  // Only for the gap between two hardware timestamps; the HUD never reads the clock itself.
  Neuron::FrameClock m_clock;
  bool m_captured = false;
  std::uint32_t m_capturedPointer = 0;
  std::int64_t m_downQpc = 0;
  int m_pressedRail = -1;
  int m_pressedFleet = -1;
  int m_activeRail = -1;

  // The under-attack pulse's clock, and the edge it is read against.
  //
  // Presentation state, and it belongs here rather than in the view for the reason the rest of this
  // class does: the alert bit itself is the simulation's (Design/Archive/Fleets.md 7.3), and how loudly a
  // button says so is the HUD's. Real time, so a paused game still pulses.
  float m_alertPhaseSec = 0.0f;
};
} // namespace Outpost
