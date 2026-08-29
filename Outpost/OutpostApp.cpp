#include "pch.h"
#include "OutpostApp.h"

#include "ViewTuning.h"

using namespace DirectX;
using namespace Neuron;

namespace Outpost
{
namespace
{
const std::wstring MESH_DIR = L"Meshes\\";
const std::wstring FONT_DIR = L"Fonts\\";
const std::wstring TEXTURE_DIR = L"Textures\\";

// Mesh and hull are paired here rather than left to line up by index. The simulation's hull id is
// a row in Game::HULL_SPECS and the view's mesh is a file; an index that happened to serve as both
// gave the Bomber an Interceptor's turn rate the moment that table arrived.
struct StartingHull
{
  std::wstring mesh;
  Game::HullId hull;
};
const StartingHull STARTING_HULLS[] = {
  {L"Bomber", Game::HullId::Bomber}, {L"Corvette", Game::HullId::Corvette}, {L"Frigate", Game::HullId::Frigate}};

// What the HUD calls each hull, indexed by Game::HullId and covering the whole table rather than
// only the three the starting fleet uses -- the id is a row in that table now, not a position in
// the array above, so a name list of three would have called a Frigate a hull it is not.
const char* const HULL_NAMES[] = {"INTERCEPTOR", "BOMBER",     "CORVETTE", "MINER",    "FRIGATE",
                                  "HAULER",      "BATTLESHIP", "CARRIER",  "STARGATE", "STRUCTURE"};
static_assert(std::size(HULL_NAMES) == Game::HULL_COUNT, "the HUD's hull names have drifted from the hull table");
} // namespace

void OutpostApp::Init(HINSTANCE _instance)
{
  AppWindow::Desc windowDesc;
  windowDesc.title = L"Outpost";
  windowDesc.className = L"OutpostWindow";
  m_window.Create(windowDesc, _instance);

  m_window.onResize = [this](std::uint32_t _widthPx, std::uint32_t _heightPx) { OnResize(_widthPx, _heightPx); };
  m_window.onKeyDown = [this](std::uint32_t _virtualKey) { OnKeyDown(_virtualKey); };
  m_window.onPointerLeave = [this] { m_view.ClearHover(); };
  m_window.onPointer = [this](const PointerEvent& _event) { m_pendingEvents.push_back(_event); };

  m_gpu.Init(m_window.Handle());
  m_sceneRenderer.Init(m_gpu);
  // The engine names the two roles; which atlas fills each is content, and content is the
  // composition root's to know.
  TextRenderer::Desc textDesc;
  textDesc.uiFont = FONT_DIR + L"EditorFont.dds";
  textDesc.sceneFont = FONT_DIR + L"SpeccyFont.dds";
  // The rail's icons, in Hud::RailIcon order: that enum is the ImageId the HUD draws them by.
  textDesc.images = {TEXTURE_DIR + L"IconResearch.dds", TEXTURE_DIR + L"IconWallet.dds", TEXTURE_DIR + L"IconStorage.dds",
                     TEXTURE_DIR + L"IconUniverse.dds"};
  m_textRenderer.Init(m_gpu, textDesc); // records the atlas uploads into the command list and flushes them

  // The explosion's three textures, named here for the same reason the fonts are: a library that
  // spelled the name of a file would be a library with content in it. Starburst is loaded and
  // slotted but drawn by nothing yet (Design/SpaceshipExplosion.md 12).
  FxRenderer::Desc fxDesc;
  fxDesc.fragmentTexture = TEXTURE_DIR + L"ShapeWireframe.dds";
  fxDesc.spriteTexture = TEXTURE_DIR + L"Particle.dds";
  fxDesc.flashTexture = TEXTURE_DIR + L"Starburst.dds";
  m_fxRenderer.Init(m_gpu, fxDesc); // a second upload batch, bracketed the same way; order between the two is free

  Camera::Desc cameraDesc;
  cameraDesc.minZoom = CAMERA_MIN_ZOOM;
  cameraDesc.maxZoom = CAMERA_MAX_ZOOM;
  cameraDesc.targetHeight = CAMERA_TARGET_HEIGHT;
  cameraDesc.fovDeg = CAMERA_FOV_DEG;
  cameraDesc.nearPlane = CAMERA_NEAR_PLANE;
  cameraDesc.farPlane = CAMERA_FAR_PLANE;
  cameraDesc.minPitchDeg = CAMERA_MIN_PITCH_DEG;
  cameraDesc.maxPitchDeg = CAMERA_MAX_PITCH_DEG;
  cameraDesc.rotateSpeedDegPerPx = CAMERA_ROTATE_SPEED_DEG_PER_PX;
  cameraDesc.zoomStepFactor = CAMERA_ZOOM_STEP_FACTOR;
  cameraDesc.panSpeed = CAMERA_PAN_SPEED;
  cameraDesc.followHalfLife = CAMERA_FOLLOW_HALF_LIFE;
  cameraDesc.shakeAmplitude = CAMERA_SHAKE_AMPLITUDE;
  cameraDesc.shakeDecayHalfLife = CAMERA_SHAKE_DECAY_HALF_LIFE;
  cameraDesc.shakeFrequencyHz = CAMERA_SHAKE_FREQUENCY_HZ;
  m_camera.Init(cameraDesc);

  PointerTracker::Desc pointerDesc;
  pointerDesc.dragThresholdPx = INPUT_DRAG_THRESHOLD_PX;
  pointerDesc.tapMaxDurationMs = INPUT_TAP_MAX_DURATION_MS;
  pointerDesc.doubleTapWindowMs = INPUT_DOUBLE_TAP_WINDOW_MS;
  m_pointers.Init(pointerDesc);

  ServerHost::Desc hostDesc;
  hostDesc.tickHz = Game::TICK_HZ;
  m_host.Init(hostDesc, m_simulation);

  // Zero latency is the single-player default and has to mean genuinely zero: a snapshot published
  // this tick is readable this tick, or the game gains a frame of lag it never had. The knob is
  // here rather than in a config file because there is no config file (AGENTS.md 5); the
  // measurements Design/Collision.md 18 wants are taken by changing this line.
  LoopbackTransport::Desc linkDesc;
  linkDesc.latencyTicks = 0;
  LoopbackTransport::Connect(m_serverLink, m_clientLink, linkDesc);
  m_simulation.Connect(m_serverLink);

  m_view.Init(m_clientLink, m_camera, m_meshes, m_sceneRenderer.UnitQuad());
  m_view.SetTracker(m_pointers);
  m_view.SetEventLog(m_log);
  m_view.SetFxRenderer(m_fxRenderer);
  SpawnStartingFleet();
  m_log.PushFormat(EventLog::Severity::Friendly, 0.0f, "FLEET ONLINE | %u SHIPS", m_world.ShipCount());

  m_window.Show();
}

void OutpostApp::SpawnStartingFleet()
{
  constexpr int hullCount = static_cast<int>(std::size(STARTING_HULLS));
  for (int i = 0; i < hullCount; ++i)
  {
    const MeshHandle mesh = m_meshes.Load(m_gpu, m_sceneRenderer, MESH_DIR, STARTING_HULLS[i].mesh);
    if (mesh == INVALID_MESH)
      continue; // a missing hull is a content diagnostic, not a reason to fail boot

    const float x = (static_cast<float>(i) - static_cast<float>(hullCount - 1) * 0.5f) * START_SPACING;
    m_world.SpawnShip(Game::LocalPos(x, 0.0f), 0.0f, static_cast<std::uint32_t>(STARTING_HULLS[i].hull));

    // The view is told which mesh a hull uses, not which mesh this ship uses: it learns that a ship
    // exists from a snapshot, which carries a hullId and knows nothing about meshes.
    m_view.RegisterHullMesh(STARTING_HULLS[i].hull, mesh);
  }
}

void OutpostApp::OnResize(std::uint32_t _widthPx, std::uint32_t _heightPx)
{
  m_gpu.Resize(_widthPx, _heightPx);
}

void OutpostApp::OnKeyDown(std::uint32_t _virtualKey)
{
  switch (_virtualKey)
  {
  case VK_ESCAPE:
    // Drops the selection first; only quits once nothing is selected.
    if (m_view.SelectedCount() > 0)
      m_view.ClearSelection();
    else
      m_window.RequestClose();
    break;
  case VK_F1:
    m_showDebug = !m_showDebug;
    break;
  case VK_F3:
    m_view.TriggerCameraShake(); // the debug hook, so the shake curve can be tuned on demand
    break;
  case VK_F4:
  {
    // Debug hook, beside F3's. Nothing in the game can destroy a ship -- there is no health, no
    // damage and no order for it -- and the explosion needs something to consume, so the
    // composition root calls World directly. It is the one place allowed to, and this design must
    // not invent a despawn order on the wire for a tuning aid (Design/SpaceshipExplosion.md 9).
    //
    // The handles are collected before the first despawn: Ships() is a span over the last snapshot
    // rather than over the world, so the walk itself is safe, and taking the handles first keeps it
    // that way if it ever stops being.
    std::vector<Game::ShipHandle> doomed;
    const std::span<const Game::ShipSnapshot> ships = m_view.Ships();
    for (std::size_t i = 0; i < ships.size(); ++i)
    {
      if (m_view.IsSelected(i))
        doomed.push_back(ships[i].handle);
    }
    for (const Game::ShipHandle handle : doomed)
      m_world.DespawnShip(handle);
    break;
  }
  case '1':
    m_timeScale = 0.25f;
    break;
  case '2':
    m_timeScale = 1.0f;
    break;
  case '3':
    m_timeScale = 4.0f;
    break;
  default:
    break;
  }
}

void OutpostApp::Update()
{
  m_camera.SetViewport(m_gpu.WidthPx(), m_gpu.HeightPx());
  m_camera.Update(); // picking needs matrices that match what was on screen when the pointer moved
  for (const PointerEvent& event : m_pendingEvents)
  {
    // The HUD gets first refusal: a tap on the bottom bar must never reach the tracker as an order.
    if (m_hud.HandlePointer(event, m_view, m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx()))
      continue;
    m_pointers.Apply(event, m_camera, m_view);
  }
  m_pendingEvents.clear();
  m_camera.Update(); // input may have moved it again
}

void OutpostApp::Render()
{
  m_gpu.BeginFrame(SKY_COLOUR);
  m_textRenderer.BeginFrame();

  m_view.Render(m_sceneRenderer, m_gpu, m_textRenderer);

  Hud::Frame frame;
  frame.stats.fps = m_clock.Fps();
  frame.stats.frameMs = m_clock.FrameMs();
  frame.stats.tick = m_host.Tick();
  frame.stats.selectedCount = m_view.SelectedCount();
  frame.stats.shipCount = m_world.ShipCount();
  frame.stats.timeScale = m_timeScale;
  frame.stats.explosionCount = m_view.ExplosionCount();
  frame.stats.particleCount = m_view.Particles().Count();
  frame.stats.particlesDropped = m_view.Particles().Dropped();
  frame.stats.fxVertsDropped = m_fxRenderer.DroppedVerts();
  frame.showDebug = m_showDebug;
  frame.sector = m_view.WorldPosAt(m_camera.Target().x, m_camera.Target().z);
  frame.hullNames = HULL_NAMES;
  m_hud.Draw(m_textRenderer, m_view.Ships(), m_view, m_camera, m_log, frame, m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx());

  m_textRenderer.Flush(m_gpu); // the overlay goes on last, before the frame is presented
  m_gpu.EndFrame();
}

void OutpostApp::Run()
{
  while (m_window.PumpMessages())
  {
    const float dtSec = m_clock.Tick();

    // A minimised or zero-sized window has nothing to draw and no viewport to pick against, so the
    // frame is skipped whole rather than half-run.
    if (!m_gpu.Ready())
      continue;

    Update();

    // The simulation runs at its own fixed rate; the render frame interpolates between the last two
    // snapshots either side of a display time held a little behind the tick. Time scaling stretches
    // the simulation only, so the display stays at the refresh rate. Both ends stand on the same
    // tick, so a latency of N means N ticks either way. Advancing them before the host runs is what
    // lets an order sent this frame be drained by this frame's tick.
    m_serverLink.AdvanceTo(m_host.Tick());
    m_clientLink.AdvanceTo(m_host.Tick());

    const int steps = m_host.Advance(dtSec * m_timeScale);
    for (int step = 0; step < steps; ++step)
    {
      m_serverLink.AdvanceTo(m_host.Tick());
      m_clientLink.AdvanceTo(m_host.Tick());
      m_view.PumpNetwork();
      m_view.SetDisplayTime(static_cast<float>(m_host.Tick()));
      m_view.SampleTrails();
    }
    if (steps == 0)
      m_view.PumpNetwork(); // a frame with no tick still delivers what latency has made due

    // The frame falls part-way through a tick; the view reads its poses at that fraction.
    m_view.SetDisplayTime(static_cast<float>(m_host.Tick()) + m_host.InterpolationAlpha());

    // Feedback eases on real time rather than sim time, so it stays smooth however far the
    // swapchain runs ahead of the tick rate.
    m_view.UpdateFeedback(dtSec);

    Render();
  }
}

void OutpostApp::Shutdown()
{
  m_gpu.Shutdown();
}
} // namespace Outpost
