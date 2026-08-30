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

// The port the in-process server listens on and the in-process client dials, on 127.0.0.1 only.
// Arbitrary and unregistered. Here rather than in a configuration file because there is no
// configuration file (AGENTS.md 5). Since the fallback went (ADR 0027) this number can be the reason
// the game does not start, which is why the failure below names it rather than reporting "no link".
constexpr std::uint16_t OUTPOST_QUIC_PORT = 30081;

// For the one log line that has to say what a connection was doing when it ran out of time.
[[nodiscard]] const char* LinkStateName(Neuron::ConnectionState _state) noexcept
{
  switch (_state)
  {
  case Neuron::ConnectionState::Disconnected:
    return "DISCONNECTED";
  case Neuron::ConnectionState::Connecting:
    return "CONNECTING";
  case Neuron::ConnectionState::Connected:
    return "CONNECTED";
  case Neuron::ConnectionState::Draining:
    return "DRAINING";
  case Neuron::ConnectionState::Closed:
    return "CLOSED";
  }
  return "?";
}

// The one error path, taken where the wire cannot be opened (AGENTS.md 5). It throws rather than
// returning, because with no fallback there is no second thing for the caller to try.
//
// hresult_error rather than Fatal(): wWinMain catches it first and shows message() verbatim, so the
// player reads which stage refused and why. Neuron::Fatal drops its formatted arguments on the floor
// and puts "Fatal Error" in the box, which for a taken port is a support ticket rather than a
// diagnostic. The reason strings come from QuicApi, QuicListener and QuicTransport, which is why
// each of them keeps one.
[[noreturn]] void ThrowLinkFailure(const char* _stage, const char* _reason)
{
  const std::string message = std::format("Outpost could not open its link: {} ({}).", _stage, _reason);
  throw winrt::hresult_error(E_FAIL, winrt::to_hstring(message));
}

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

  // The wire the two halves meet on: QUIC across 127.0.0.1, and nothing else. Every frame of every
  // run crosses the real stack, because there is no second path for it to cross instead -- and a
  // boot that cannot open the wire fails here rather than quietly running on something else
  // (ADR 0027, Design/QuicTransport.md 6).
  OpenQuicLink();
  m_simulation.Connect(*m_serverQuic);

  m_view.Init(m_clientQuic, m_camera, m_meshes, m_sceneRenderer.UnitQuad());
  m_view.SetTracker(m_pointers);
  m_view.SetEventLog(m_log);
  m_view.SetFxRenderer(m_fxRenderer);
  // Who this client is. One subscriber today, so it is the player's faction at both sites; the day
  // a login exists it arrives with the session and only this line changes.
  m_view.SetOwnFaction(m_ownFaction);
  m_view.SetBodyRenderer(m_bodyRenderer);

  // Where the sphere sits, how bright it is and how hard a star scintillates are the game's choices,
  // so they arrive from ViewTuning rather than being the engine's defaults. The view fills in the
  // matrices and the clock every frame.
  SkyRenderer::Frame skyTuning;
  skyTuning.radiusMetres = SKY_RADIUS_METRES;
  skyTuning.intensity = SKY_INTENSITY;
  skyTuning.twinkleMaxRateRadPerSec = SKY_TWINKLE_MAX_RATE_RAD_PER_SEC;
  m_view.SetSkyRenderer(m_skyRenderer, skyTuning);
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

  // The sky's three textures, named here for the same reason: the engine knows there is a nebula
  // layer, a star layer and a flare layer, and which file fills each is content.
  SkyRenderer::Desc skyDesc;
  skyDesc.nebulaTexture = TEXTURE_DIR + L"CloudyGlow.dds";
  skyDesc.starTexture = TEXTURE_DIR + L"Glow.dds";
  skyDesc.burstTexture = TEXTURE_DIR + L"Starburst.dds";

  m_gpu.BeginUploads();
  m_bodyRenderer.Init(m_gpu, bodyDesc);
  m_skyRenderer.Init(m_gpu, skyDesc);
  m_ramps.resize(BODY_CLASS_COUNT);
  for (std::uint32_t i = 0; i < BODY_CLASS_COUNT; ++i)
  {
    const wchar_t* const ramp = BODY_CLASSES[i].ramp;
    if (ramp != nullptr && !ColourRamp::Load(TERRAIN_DIR + ramp, m_ramps[i]))
      DebugTrace(L"body ramp {} did not load; that class draws in the fallback grey\n", ramp);
  }
  SpawnStartingBodies(BODY_START_SEED);
  BuildSky(SKY_SEED);
  m_gpu.ExecuteAndWait();
  m_bodyRenderer.DiscardStaging();
  m_skyRenderer.DiscardStaging();
  m_log.PushFormat(EventLog::Severity::Friendly, 0.0f, "FLEET ONLINE | %u SHIPS", OwnShipCount());

  m_window.Show();
}

void OutpostApp::OpenQuicLink()
{
  // The development placeholder, and the only site in the tree allowed to set it: the client is told
  // to accept whatever certificate the server presents, because that certificate is one this process
  // generated moments ago and there is no trust store to check it against. The connection is
  // encrypted and it is not authenticated (Design/Decisions/0023).
  QuicApi::Desc quicDesc;
  quicDesc.allowUnvalidatedPeer = true;
  if (!m_quic.Open(quicDesc))
    ThrowLinkFailure("the QUIC library would not open", m_quic.Reason());

  if (!m_listener.Start(m_quic, OUTPOST_QUIC_PORT, {}))
  {
    const std::string reason = m_listener.Reason();
    m_quic.Close();
    ThrowLinkFailure("the port was refused", reason.c_str());
  }

  if (!m_clientQuic.Connect(m_quic, {"127.0.0.1", OUTPOST_QUIC_PORT}, {}))
  {
    const std::string reason = m_clientQuic.Reason();
    m_listener.Stop();
    m_quic.Close();
    ThrowLinkFailure("the dial was refused", reason.c_str());
  }

  // Boot is the one place in this program where waiting is acceptable, and it is bounded: on the way
  // out it either logs the link or throws naming what both ends were still doing.
  // Both ends have to reach Connected -- which means the handshake completed AND the peer will take
  // a datagram of MAX_DATAGRAM_BYTES, since a wire that would truncate one is worse than no wire.
  // On localhost this costs a few milliseconds; anything near the timeout is a finding, not a pass.
  const std::int64_t startQpc = m_clock.Now();
  for (;;)
  {
    m_listener.Poll();
    m_clientQuic.Poll();
    const std::span<Neuron::QuicTransport* const> accepted = m_listener.Accepted();
    if (!accepted.empty())
      accepted[0]->Poll();

    const float elapsedMs = m_clock.ElapsedMs(startQpc, m_clock.Now());
    if (!accepted.empty() && m_clientQuic.State() == ConnectionState::Connected && accepted[0]->State() == ConnectionState::Connected)
    {
      m_serverQuic = accepted[0];
      m_linkOpen = true;
      m_log.PushFormat(EventLog::Severity::Friendly, 0.0f, "LINK | QUIC | 127.0.0.1:%u | %.1f MS", static_cast<unsigned>(OUTPOST_QUIC_PORT),
                       elapsedMs);
      return;
    }

    if (elapsedMs >= static_cast<float>(QUIC_HANDSHAKE_TIMEOUT_MS))
    {
      const std::string states = std::format("client {}, server {}", LinkStateName(m_clientQuic.State()),
                                             accepted.empty() ? "UNACCEPTED" : LinkStateName(accepted[0]->State()));
      m_clientQuic.Close();
      m_listener.Stop();
      m_quic.Close();
      ThrowLinkFailure("the handshake timed out", states.c_str());
    }

    // A millisecond back to the scheduler rather than a spin: MsQuic is doing the handshake on its
    // own threads and this one has nothing to do until it has finished.
    Sleep(1);
  }
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
  // _centreY is the height of the body's centre, so a caller says where a world sits rather than
  // deriving it from a radius the seed chose. A rock passes its own radius and so rests on the
  // fleet's plane; a world passes a depth well below it (ViewTuning.h).
  const auto place = [&](BodyClass bodyClass, float _radiusMetres, float _bearingRad, float _distanceMetres, float _centreY, bool _asteroid)
  {
    const std::uint64_t bodySeed = (static_cast<std::uint64_t>(rng.Next()) << 32) | rng.Next();
    const BodyDesc desc = RandomBody(bodySeed, bodyClass, _radiusMetres);
    const ColourRamp& ramp = m_ramps[static_cast<std::size_t>(bodyClass)];

    const BodyClassSpec& spec = BodyClassOf(bodyClass);
    std::vector<MeshVertex> ocean;
    BodyBuildStats stats;
    const XMFLOAT3 oceanColour(spec.oceanColour.r, spec.oceanColour.g, spec.oceanColour.b);

    BodyHandle handle = INVALID_BODY;
    if constexpr (BODY_BAKE_ON_GPU)
    {
      // The kernel writes the vertices; this side draws the random numbers into the block and builds
      // the water, which needs no height. No BodyField is constructed at all -- its two measurement
      // passes over the grid are exactly what the first two dispatches replace.
      const BodyParams params = BodyField::ParamsFor(desc);
      BodyMeshBuilder::BuildOcean(params, oceanColour, ocean);
      handle = m_bodyRenderer.BakeBody(m_gpu, params, ramp);

      // A baked body writes a degenerate where the builder culls, so its triangle count is every
      // cell and the sea floor is in it. There is no CPU-side cull count on this path and the
      // readout says so rather than reporting a zero that would read as "nothing was culled".
      const std::uint32_t cells = (1u << static_cast<std::uint32_t>(params.outsideMaxHeightGrid.z));
      stats.trianglesEmitted = CUBE_FACE_COUNT * cells * cells * 2u;
    }
    else
    {
      std::vector<FxVertex> terrain;
      BodyMeshBuilder::Build(BodyField(desc), ramp.Loaded() ? &ramp : nullptr, terrain, ocean, oceanColour, stats);
      handle = m_bodyRenderer.UploadBody(m_gpu, terrain);
    }

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
    view.centreY = _centreY;
    // The sphere the frustum test uses. Every length in a BodyDesc but radiusMetres is a fraction of
    // it, so the furthest the body can reach is its radius stretched by its widest ellipsoid axis and
    // raised by its tallest continent. Read off the description, so it is right on either bake path
    // -- BodyBuildStats::maxHeightMetres is only filled on the CPU one. Not desc.maxHeight, which
    // scales colour and is usually zero; a tile's own desiredHeight and lift are the geometry.
    const float widestAxis = std::max({desc.ellipsoid.x, desc.ellipsoid.y, desc.ellipsoid.z});
    float tallestTile = 0.0f;
    for (const BodyTile& tile : desc.tiles)
      tallestTile = std::max(tallestTile, tile.desiredHeight + tile.posY);
    view.boundingRadiusMetres = _radiusMetres * (widestAxis + std::max(0.0f, tallestTile));
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
  place(BodyClass::Terran, planetRadius, XMConvertToRadians(BODY_START_PLANET_BEARING_DEG), BODY_START_PLANET_DISTANCE_METRES,
        -BODY_START_PLANET_DEPTH_METRES, false);
  place(BodyClass::Barren, planetRadius * BODY_START_MOON_RADIUS_FRACTION, XMConvertToRadians(BODY_START_MOON_BEARING_DEG),
        BODY_START_MOON_DISTANCE_METRES, -BODY_START_MOON_DEPTH_METRES, false);

  for (int i = 0; i < BODY_START_ASTEROIDS; ++i)
  {
    const float radius =
      BODY_ASTEROID_RADIUS_MIN_METRES + rng.Float01() * (BODY_ASTEROID_RADIUS_MAX_METRES - BODY_ASTEROID_RADIUS_MIN_METRES);
    const float bearing = rng.Float01() * XM_2PI;
    const float distance =
      BODY_START_ASTEROID_RING_MIN_METRES + rng.Float01() * (BODY_START_ASTEROID_RING_MAX_METRES - BODY_START_ASTEROID_RING_MIN_METRES);
    place(BodyClass::Asteroid, radius, bearing, distance, radius, true);
  }

  m_bodyGenerationMs = m_clock.ElapsedMs(startQpc, m_clock.Now());
  DebugTrace("bodies: {} generated in {} ms\n", m_view.BodyCount(), m_bodyGenerationMs);
}

// The sky the outpost is under. One call, because the whole of it follows from one number: the
// generator is device-free and the renderer takes the result as a single static buffer.
void OutpostApp::BuildSky(std::uint64_t _seed)
{
  const std::int64_t startQpc = m_clock.Now();

  SkyField::Desc desc;
  desc.seed = _seed;
  desc.starCount = SKY_STAR_COUNT;
  desc.brightStarCount = SKY_BRIGHT_STAR_COUNT;
  desc.nebulaCount = SKY_NEBULA_COUNT;

  SkyMesh sky;
  SkyField::Build(desc, sky);
  m_skyRenderer.UploadField(m_gpu, sky);

  DebugTrace("sky: {} billboards generated in {} ms\n", sky.verts.size() / 6, m_clock.ElapsedMs(startQpc, m_clock.Now()));
}

// F5. A different scene each press, and the same different scene after a restart: what the seed is
// offset by is the number of presses, not a clock.
//
// The sky is reseeded with the bodies rather than separately, because what F5 rerolls is the
// neighborhood and the sky is the far half of it. A second key for it would be a second thing to
// remember for no second question it answers.
void OutpostApp::ReseedBodies()
{
  m_view.ClearBodies();
  ++m_bodyRerollCount;

  m_gpu.BeginUploads();
  SpawnStartingBodies(BODY_START_SEED + m_bodyRerollCount);
  BuildSky(SKY_SEED + m_bodyRerollCount);
  m_gpu.ExecuteAndWait();
  m_bodyRenderer.DiscardStaging();
  m_skyRenderer.DiscardStaging();
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
  // Last frame's, because Render has not run for this one yet. That is the right reading anyway: it
  // is the frame whose cost the numbers beside it describe.
  frame.stats.submittedCount = m_view.SubmittedCount();
  frame.stats.culledCount = m_view.CulledCount();
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
    // the simulation only, so the display stays at the refresh rate.
    //
    // Nothing advances a link clock here any more. Tick-counted latency was the loopback's, and the
    // loopback is no longer in this program (ADR 0027); QUIC's delay is the wire's own and arrives
    // whenever MsQuic's workers deliver it.
    const int steps = m_host.Advance(dtSec * m_timeScale);
    for (int step = 0; step < steps; ++step)
    {
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
  // In this order, and before the device goes away: a registration cannot close while a connection
  // on it lives, so the client end closes first, then the listener with everything it accepted, then
  // the library (Design/QuicTransport.md 6). Nothing is logged here -- a shutdown path does nothing
  // rather than report (AGENTS.md 5).
  if (m_linkOpen)
  {
    m_clientQuic.Close();
    m_listener.Stop();
    m_serverQuic = nullptr; // the listener owned it, and Stop has just closed it
    m_quic.Close();
  }

  m_gpu.Shutdown();
}
} // namespace Outpost
