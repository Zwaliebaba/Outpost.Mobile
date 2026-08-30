#pragma once

#include "EventLog.h"
#include "WorldView.h"

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
// It reads the snapshot the client half received and WorldView, and writes to neither except
// through WorldView's own selection calls, so nothing here can feed back into a tick. Everything is laid out from
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
    // Frustum culling (Design/MmoScalabilityReview.md G2). On the screen rather than inferred,
    // because a culler that is quietly rejecting everything and one that is quietly rejecting
    // nothing both look exactly like a working one until you count.
    std::uint32_t submittedCount = 0;
    std::uint32_t culledCount = 0;
    // The router (Design/RegionalPathfinding.md 3.3). An island that refused to build is the one
    // failure here that looks exactly like success: every route through it is a straight line and
    // every ship crossing it steers locally, which is what the world looked like before there was a
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

  // What the composition root knows and the HUD does not. The economy has no home in the world
  // yet, so its numbers arrive from here; the defaults are the mock's so the panel reads right
  // until it does. The same goes for hull and shield, which have no damage model to read.
  //
  // Three of these are real. The sector is the position under the camera target, of which the
  // minimap names the sector pair. contacts is the count of records this client holds that are not
  // its own -- the subscription, not the map rectangle, so a contact past the map edge is counted
  // and clipped. ownFaction is session identity: the HUD colors by allegiance and cannot know whose
  // side it is on without being told.
  struct Frame
  {
    Stats stats;
    bool showDebug = false;
    int credits = 12480;
    int creditsPerMin = 42;
    int alloy = 3215;
    int alloyPerMin = 8;
    Game::WorldPos sector;
    int contacts = 0;
    Game::FactionId ownFaction = Game::FACTION_PLAYER;
    float hullFraction = 1.0f;
    float shieldFraction = 1.0f;
    std::span<const char* const> hullNames; // indexed by ShipSnapshot::hullId; an id past the end is unnamed
  };

  void Draw(Neuron::TextRenderer& _text, std::span<const Game::ShipSnapshot> _ships, const WorldView& _view, const Neuron::Camera& _camera,
            const EventLog& _log, const Frame& _frame, float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx);

  // Reports true when the event landed on a panel and was used up, so a tap on the bottom bar can
  // never fall through to the tracker as a ground order. A contact that went down on a panel is
  // kept until it lifts, whatever it drifts over in between.
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
    Rect groups[WorldView::CONTROL_GROUPS];
  };

  [[nodiscard]] Layout ComputeLayout(float _dpiScale, std::uint32_t _widthPx, std::uint32_t _heightPx) const noexcept;
  [[nodiscard]] bool OverAnyPanel(const Layout& _layout, float _xPx, float _yPx) const noexcept;

  void DrawPanel(Neuron::TextRenderer& _text, const Rect& _rect, Neuron::Rgba _fill, Neuron::Rgba _outline, float _scale) const;
  void DrawScanlines(Neuron::TextRenderer& _text, const Rect& _rect, float _scale) const;
  void DrawResources(Neuron::TextRenderer& _text, const Layout& _layout, const Frame& _frame) const;
  void DrawDebug(Neuron::TextRenderer& _text, const Layout& _layout, const Frame& _frame, std::uint32_t _widthPx) const;
  void DrawMinimap(Neuron::TextRenderer& _text, const Layout& _layout, std::span<const Game::ShipSnapshot> _ships, const WorldView& _view,
                   const Neuron::Camera& _camera, const Frame& _frame, std::uint32_t _widthPx, std::uint32_t _heightPx) const;
  void DrawRail(Neuron::TextRenderer& _text, const Layout& _layout) const;
  void DrawEventLog(Neuron::TextRenderer& _text, const Layout& _layout, const EventLog& _log) const;
  void DrawBottomBar(Neuron::TextRenderer& _text, const Layout& _layout, std::span<const Game::ShipSnapshot> _ships, const WorldView& _view,
                     const Frame& _frame) const;

  float m_cellPx = 16.0f; // the UI atlas cell, remembered from the last draw so a hit test can size the panels the same way

  // Only for the gap between two hardware timestamps; the HUD never reads the clock itself.
  Neuron::FrameClock m_clock;
  bool m_captured = false;
  std::uint32_t m_capturedPointer = 0;
  std::int64_t m_downQpc = 0;
  int m_pressedRail = -1;
  int m_pressedGroup = -1;
  int m_activeRail = -1;
};
} // namespace Outpost
