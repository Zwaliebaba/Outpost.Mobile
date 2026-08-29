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
const std::wstring TERRAIN_DIR = L"Terrain\\";

// Mesh and hull are paired here rather than left to line up by index. The simulation's hull id is
// a row in Game::HULL_SPECS and the view's mesh is a file; an index that happened to serve as both
// gave the Bomber an Interceptor's turn rate the moment that table arrived.
//
// Which meshes load and which ships spawn used to be one array, which worked only while the two were
// the same list. They stopped being the same list the moment somebody else's ships existed: the
// hostile base flies Interceptors and stands on a Structure, and the player's fleet is still three
// hulls. So this is the mesh table, and the two spawn functions below are the content.
struct HullMesh
{
  std::wstring mesh;
  Game::HullId hull;
};
const HullMesh HULL_MESHES[] = {{L"Bomber", Game::HullId::Bomber},
                                {L"Corvette", Game::HullId::Corvette},
                                {L"Frigate", Game::HullId::Frigate},
                                {L"Interceptor", Game::HullId::Interceptor},
                                {L"Structure", Game::HullId::Structure}};

const Game::HullId STARTING_FLEET[] = {Game::HullId::Bomber, Game::HullId::Corvette, Game::HullId::Frigate};

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
  // Who this client is. One subscriber today, so it is the player's faction at both sites; the day
  // a login exists it arrives with the session and only this line changes.
  m_view.SetOwnFaction(m_ownFaction);
  m_view.SetBodyRenderer(m_bodyRenderer);
  LoadHullMeshes();
  SpawnStartingFleet();
  SpawnHostileBase();

  // The bodies, last, and all in one bracket. The outline texture and every body's vertices are
  // copies recorded into one command list and carried by one submission, which is what
  // BodyRenderer.h asks for -- and the bracket is kept tight, with nothing between its two ends but
  // the work it is for. Opened at the top of Init instead it would still work today and would break
  // silently the day somebody adds a second uploader in the middle of it.
  //
  // The ramps are content, named here for the same reason the fonts are; one that fails to load
  // leaves its class drawing the builder's fallback grey.
  BodyRenderer::Desc bodyDesc;
  bodyDesc.outlineTexture = TEXTURE_DIR + L"TriangleOutline.dds";

  m_gpu.BeginUploads();
  m_bodyRenderer.Init(m_gpu, bodyDesc);
  m_ramps.resize(BODY_CLASS_COUNT);
  for (std::uint32_t i = 0; i < BODY_CLASS_COUNT; ++i)
  {
    const wchar_t* const ramp = BODY_CLASSES[i].ramp;
    if (ramp != nullptr && !ColourRamp::Load(TERRAIN_DIR + ramp, m_ramps[i]))
      DebugTrace(L"body ramp {} did not load; that class draws in the fallback grey\n", ramp);
  }
  SpawnStartingBodies(BODY_START_SEED);
  m_gpu.ExecuteAndWait();
  m_bodyRenderer.DiscardStaging();
  m_log.PushFormat(EventLog::Severity::Friendly, 0.0f, "FLEET ONLINE | %u SHIPS", OwnShipCount());

  m_window.Show();
}

void OutpostApp::LoadHullMeshes()
{
  for (const HullMesh& row : HULL_MESHES)
  {
    const MeshHandle mesh = m_meshes.Load(m_gpu, m_sceneRenderer, MESH_DIR, row.mesh);
    if (mesh == INVALID_MESH)
      continue; // a missing hull is a content diagnostic, logged by the library; a ship still simulates

    // The view is told which mesh a hull uses, not which mesh this ship uses: it learns that a ship
    // exists from a snapshot, which carries a hullId and knows nothing about meshes.
    m_view.RegisterHullMesh(row.hull, mesh);
  }
}

// The neighbourhood: one terran world on the north-east horizon, a barren moon north-west, and six
// rocks among the fleet. Everything about it comes out of _seed, so the pull request's screenshot
// reproduces and F5 is a different scene rather than an unrepeatable one.
void OutpostApp::SpawnStartingBodies(std::uint64_t _seed)
{
  const std::int64_t startQpc = m_clock.Now();
  Pcg32 rng(_seed);

  // A body of a class, at a bearing and a distance. The seed each body is generated from is drawn
  // from this scene's generator, so the whole scene follows from one number and no body has to be
  // told where in the stream it sits.
  const auto place = [&](BodyClass bodyClass, float _radiusMetres, float _bearingRad, float _distanceMetres, bool _asteroid)
  {
    const std::uint64_t bodySeed = (static_cast<std::uint64_t>(rng.Next()) << 32) | rng.Next();
    const BodyDesc desc = RandomBody(bodySeed, bodyClass, _radiusMetres);
    const ColourRamp& ramp = m_ramps[static_cast<std::size_t>(bodyClass)];

    const BodyClassSpec& spec = BodyClassOf(bodyClass);
    std::vector<FxVertex> terrain;
    std::vector<MeshVertex> ocean;
    BodyBuildStats stats;
    const XMFLOAT3 oceanColour(spec.oceanColour.r, spec.oceanColour.g, spec.oceanColour.b);
    BodyMeshBuilder::Build(BodyField(desc), ramp.Loaded() ? &ramp : nullptr, terrain, ocean, oceanColour, stats);

    const BodyHandle handle = m_bodyRenderer.UploadBody(m_gpu, terrain);
    if (handle == INVALID_BODY)
      return;

    WorldView::BodyView view;
    view.terrain = handle;
    // The ocean goes through the scene renderer's own mesh path, so it is an upload-heap mesh like a
    // hull rather than one of the body renderer's staged buffers. Three thousand triangles is what
    // that path's comment argues for; seven megabytes of terrain is what it argues against.
    if (!ocean.empty())
    {
      view.ocean = m_sceneRenderer.UploadMesh(m_gpu, ocean);
      view.oceanColour = spec.oceanColour;
    }
    view.centre = Game::LocalPos(std::sin(_bearingRad) * _distanceMetres, std::cos(_bearingRad) * _distanceMetres);
    // A planet floats clear of the ground quad; a rock rests on it, the way a Structure does.
    view.centreY = _asteroid ? _radiusMetres : _radiusMetres * BODY_PLANET_LIFT;
    view.spinAxis = desc.spinAxis;
    view.triangleCount = stats.trianglesEmitted;
    if (_asteroid)
    {
      // Two axes, so the rock turns end over end rather than about one pole. Named draws, not
      // arguments: the order a compiler evaluates arguments in is unspecified and this is seeded.
      const float tumbleX = rng.Signed(BODY_ASTEROID_TUMBLE_MAX_RAD_PER_SEC);
      const float tumbleY = rng.Signed(BODY_ASTEROID_TUMBLE_MAX_RAD_PER_SEC);
      view.tumbleRadPerSec = XMFLOAT3(tumbleX, tumbleY, 0.0f);
    }
    else
    {
      view.spinRadPerSec = XM_2PI / BODY_PLANET_SPIN_SEC;
    }

    m_view.AddBody(view);
    DebugTrace("body: {} triangles ({} culled at sea level), {} ocean triangles, maximum height {} m, radius {} m\n",
               stats.trianglesEmitted, stats.trianglesCulled, ocean.size() / 3, stats.maxHeightMetres, _radiusMetres);
  };

  const float planetRadius =
    BODY_PLANET_RADIUS_MIN_METRES + rng.Float01() * (BODY_PLANET_RADIUS_MAX_METRES - BODY_PLANET_RADIUS_MIN_METRES);
  place(BodyClass::Terran, planetRadius, XM_PIDIV4, BODY_START_PLANET_DISTANCE_METRES, false);
  place(BodyClass::Barren, planetRadius * BODY_START_MOON_RADIUS_FRACTION, -XM_PIDIV4, BODY_START_MOON_DISTANCE_METRES, false);

  for (int i = 0; i < BODY_START_ASTEROIDS; ++i)
  {
    const float radius =
      BODY_ASTEROID_RADIUS_MIN_METRES + rng.Float01() * (BODY_ASTEROID_RADIUS_MAX_METRES - BODY_ASTEROID_RADIUS_MIN_METRES);
    const float bearing = rng.Float01() * XM_2PI;
    const float distance =
      BODY_START_ASTEROID_RING_MIN_METRES + rng.Float01() * (BODY_START_ASTEROID_RING_MAX_METRES - BODY_START_ASTEROID_RING_MIN_METRES);
    place(BodyClass::Asteroid, radius, bearing, distance, true);
  }

  m_bodyGenerationMs = m_clock.ElapsedMs(startQpc, m_clock.Now());
  DebugTrace("bodies: {} generated in {} ms\n", m_view.BodyCount(), m_bodyGenerationMs);
}

// F5. A different scene each press, and the same different scene after a restart: what the seed is
// offset by is the number of presses, not a clock.
void OutpostApp::ReseedBodies()
{
  m_view.ClearBodies();
  ++m_bodyRerollCount;

  m_gpu.BeginUploads();
  SpawnStartingBodies(BODY_START_SEED + m_bodyRerollCount);
  m_gpu.ExecuteAndWait();
  m_bodyRenderer.DiscardStaging();
}

void OutpostApp::SpawnStartingFleet()
{
  constexpr int hullCount = static_cast<int>(std::size(STARTING_FLEET));
  for (int i = 0; i < hullCount; ++i)
  {
    const float x = (static_cast<float>(i) - static_cast<float>(hullCount - 1) * 0.5f) * START_SPACING;
    m_world.SpawnShip(Game::LocalPos(x, 0.0f), 0.0f, static_cast<std::uint32_t>(STARTING_FLEET[i]), Game::FACTION_PLAYER);
  }
}

// Somebody else lives here: a station northeast of the fleet, and three Interceptors walking a ring
// around it at a third of their top speed. They do nothing else -- no combat, no reaction to the
// player -- and the ring is a metronome by the owner's brief (Design/Hostiles.md 6).
void OutpostApp::SpawnHostileBase()
{
  const Game::ShipId station = m_world.SpawnShip(Game::LocalPos(HOSTILE_BASE_EAST_METRES, HOSTILE_BASE_NORTH_METRES), 0.0f,
                                                 static_cast<std::uint32_t>(Game::HullId::Structure), Game::FACTION_HOSTILE);
  const Game::WorldPos anchor = Game::LocalPos(HOSTILE_BASE_EAST_METRES, HOSTILE_BASE_NORTH_METRES);

  for (int at = 0; at < HOSTILE_PATROL_COUNT; ++at)
  {
    // Spread evenly over the ring -- 0, 4, 8 of twelve -- and headed along it, so the first leg does
    // not begin with a turn. The geometry is Patrol.h's, because the world steers by the same
    // function and a root doing its own arithmetic would put them somewhere it then walks away from.
    const std::uint32_t index =
      static_cast<std::uint32_t>(at) * Game::PATROL_RING_WAYPOINTS / static_cast<std::uint32_t>(HOSTILE_PATROL_COUNT);
    const Game::ShipId ship =
      m_world.SpawnShip(Game::PatrolRingPoint(anchor, index, HOSTILE_PATROL_RING_METRES), Game::PatrolRingHeadingRad(index),
                        static_cast<std::uint32_t>(Game::HullId::Interceptor), Game::FACTION_HOSTILE);
    m_world.AssignPatrol(ship, station, HOSTILE_PATROL_RING_METRES, HOSTILE_PATROL_CRUISE_MPS);
  }
}

// The player's own ships, not every ship in the world. Without this the game would greet the player
// with FLEET ONLINE | 7 SHIPS, four of them the enemy's.
std::uint32_t OutpostApp::OwnShipCount() const noexcept
{
  std::uint32_t count = 0;
  for (const Game::ShipState& ship : m_world.Ships())
    count += (ship.factionId == m_ownFaction) ? 1u : 0u;
  return count;
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
  case VK_F5:
    // Reseed every body. A tuning key: what it costs is the memory of the scene it replaces, since
    // BodyRenderer keeps every handle for the run (OutpostApp.h says so beside the key list).
    ReseedBodies();
    break;
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
  frame.stats.bodyCount = m_view.BodyCount();
  frame.stats.bodyTriangles = m_view.BodyTriangleCount();
  frame.stats.bodyGenerationMs = m_bodyGenerationMs;
  frame.showDebug = m_showDebug;
  frame.sector = m_view.WorldPosAt(m_camera.Target().x, m_camera.Target().z);
  frame.hullNames = HULL_NAMES;
  frame.ownFaction = m_ownFaction;
  // What this client currently knows about, counted off the snapshot rather than off the world: a
  // contact is a hostile *record*, which is the only reading that stays honest over a real wire. The
  // station counts, so the base reads as four.
  frame.contacts = 0;
  for (const Game::ShipSnapshot& ship : m_view.Ships())
    frame.contacts += (ship.factionId != m_ownFaction) ? 1 : 0;
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
