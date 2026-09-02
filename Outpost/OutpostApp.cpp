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

// The one error path, taken where the boot cannot have what it was told to have (AGENTS.md 5). It
// throws rather than returning, because with no fallback there is no second thing for the caller to
// try.
//
// It was ThrowLinkFailure and said "could not open its link", because the wire was the only thing
// that could refuse a boot. The saved universe is the second (ADR 0057) and it fails for the same
// reason -- there is no acceptable other thing to run instead -- so it is one helper and one
// sentence, with each stage naming itself.
//
// hresult_error rather than Fatal(): wWinMain catches it first and shows message() verbatim, so the
// player reads which stage refused and why. Neuron::Fatal drops its formatted arguments on the floor
// and puts "Fatal Error" in the box, which for a taken port is a support ticket rather than a
// diagnostic. The reason strings come from QuicApi, QuicListener and QuicTransport, which is why
// each of them keeps one.
[[noreturn]] void ThrowBootFailure(const char* _stage, const char* _reason)
{
  const std::string message = std::format("Outpost: Frontier could not start: {} ({}).", _stage, _reason);
  throw winrt::hresult_error(E_FAIL, winrt::to_hstring(message));
}

// What the HUD calls each hull, indexed by Game::HullId and covering the whole table rather than
// only the three the starting fleet uses -- the id is a row in that table now, not a position in
// the array above, so a name list of three would have called a Frigate a hull it is not.
const char* const HULL_NAMES[] = {"INTERCEPTOR", "BOMBER",     "CORVETTE", "MINER",    "FRIGATE",
                                  "HAULER",      "BATTLESHIP", "CARRIER",  "STARGATE", "STRUCTURE"};
static_assert(std::size(HULL_NAMES) == Game::HULL_COUNT, "the HUD's hull names have drifted from the hull table");

// What the HUD calls each faction, indexed by Game::FactionId. Design/Archive/Hostiles.md 12 deferred
// this table "until something displays it"; the docking refusal line is the something, and it
// arrives with slice 6 -- this slice plumbs the table to the HUD so that line has it to read. Sized
// to the factions that exist rather than to FACTION_LIMIT, because a name for a faction nobody has
// authored would be a lie the day one is.
const char* const FACTION_NAMES[] = {"PLAYER", "VANDAL", "VANGUARD"};
static_assert(std::size(FACTION_NAMES) == Game::FACTION_VANGUARD + 1, "the HUD's faction names have drifted from the faction ids");
static_assert(std::size(FACTION_NAMES) <= Game::FACTION_LIMIT, "more faction names than the standing table has rows");
} // namespace

void OutpostApp::Init(HINSTANCE _instance)
{
  AppWindow::Desc windowDesc;
  windowDesc.title = L"Outpost: Frontier";
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
  // slotted but drawn by nothing yet (Design/Archive/SpaceshipExplosion.md 12).
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
  pointerDesc.longPressMs = HUD_LONG_PRESS_MS;
  m_pointers.Init(pointerDesc);

  ServerHost::Desc hostDesc;
  hostDesc.tickHz = Game::TICK_HZ;
  m_host.Init(hostDesc, m_simulation);

  // Before the link opens and before the universe is configured, because both read out of it. Nothing
  // above this line is allowed to depend on a configured value, which is the whole reason it is one
  // call at one place rather than a read wherever a value is wanted.
  LoadServerConfig();

  // The wire the two halves meet on: QUIC across 127.0.0.1, and nothing else. Every frame of every
  // run crosses the real stack, because there is no second path for it to cross instead -- and a
  // boot that cannot open the wire fails here rather than quietly running on something else
  // (ADR 0028, Design/Archive/QuicTransport.md 6).
  OpenQuicLink();

  m_view.Init(m_clientQuic, m_camera, m_meshes, m_sceneRenderer.UnitQuad());
  m_view.SetTracker(m_pointers);
  m_view.SetEventLog(m_log);
  m_view.SetFxRenderer(m_fxRenderer);
  // Who this client is. One subscriber today, so it is the player's faction at both sites; the day
  // a login exists it arrives with the session and only this line changes.
  m_view.SetOwnFaction(m_ownFaction);
  m_view.SetFactionNames(FACTION_NAMES);
  m_view.SetBodyRenderer(m_bodyRenderer);

  // Where the sphere sits, how bright it is and how hard a star scintillates are the game's choices,
  // so they arrive from ViewTuning rather than being the engine's defaults. The view fills in the
  // matrices and the clock every frame.
  SkyRenderer::Frame skyTuning;
  skyTuning.radiusMetres = SKY_RADIUS_METRES;
  skyTuning.intensity = SKY_INTENSITY;
  skyTuning.twinkleMaxRateRadPerSec = SKY_TWINKLE_MAX_RATE_RAD_PER_SEC;
  m_view.SetSkyRenderer(m_skyRenderer, skyTuning);
  // Hull vertices go to default heaps (SceneRenderer::UploadMesh), so their copies are recorded
  // rather than written, and something has to run them. One bracket for all of them, not one each.
  m_gpu.BeginCopies();
  m_gpu.BeginUploads();
  LoadHullMeshes();
  m_gpu.SubmitCopies();
  m_gpu.ExecuteAndWait();
  m_sceneRenderer.DiscardStaging();

  // The universe comes out of the file, and out of nothing else.
  //
  // This program does not author a universe any more; UniverseGen does, and Outpost runs what it
  // wrote (ADR 0058). So BOTH of the ways this can fail stop the boot, and they say different
  // things because the reader has different work to do: a missing file means run the tool, and an
  // unreadable one means do not, because generating over it would destroy whatever it holds.
  //
  // It is the same shape as the link failing above -- a boot that cannot have what it was told to
  // have says so rather than quietly running on something else -- and the reason it matters more
  // here is that the something else would OVERWRITE.
  const RestoreResult restored = RestoreUniverse();
  if (restored == RestoreResult::Absent)
  {
    ThrowBootFailure("there is no universe to run", "Universe.sav was not found beside the executable; run UniverseGen to write one");
  }
  if (restored == RestoreResult::Refused)
  {
    const std::string reason = m_restoreRefusal + "; move it aside and run UniverseGen to write a new one";
    ThrowBootFailure("the saved universe was refused", reason.c_str());
  }

  // The places the file's contents stand in. Laid out rather than stored, because a galaxy is a pure
  // function of its seed and 54 systems of planet sites would be a second copy of something the file
  // already determines (ADR 0055). The body placements and the minimap's marks both read m_layout
  // and neither re-rolls it (Design/Archive/Stations.md 5.3).
  //
  // The galaxy first, then the system the player is standing in, taken out of it rather than laid
  // out beside it -- so home is the galaxy's home and not a second opinion about where it is.
  //
  // From m_galaxySeed, which came out of the save header, and NEVER from a compiled constant: the
  // ships in the file were spawned into the galaxy that seed lays out and no other, so a build whose
  // own idea of the seed had moved on would draw stations inside stars (Design/Archive/Universe.md 8).
  m_galaxy = Game::LayOutGalaxy(m_galaxySeed, Game::UniversePos{}, Game::STARTING_GALAXY, Game::GALAXY_PINS);
  for (std::size_t at = 0; at < m_galaxy.systems.size(); ++at)
  {
    if (m_galaxy.systems[at].pin != Game::INVALID_PIN_INDEX)
      m_localSystem = static_cast<std::uint32_t>(at);
  }
  m_layout = Game::LayOutGalaxySystem(m_galaxy.systems[m_localSystem], Game::STARTING_GALAXY, Game::GALAXY_PINS);

  // Every ship, station and gate is already in the file. What is NOT in the file is the marks: they
  // are drawn from the layout rather than from the universe, because they are a picture of where the
  // government is and not a record of anything. So they are built here, from the same layout the
  // stations in the file were spawned against -- the two agree because both derive from the seed the
  // file carries, which is the whole reason it carries one.
  MarkLocalStations();
  m_log.PushFormat(EventLog::Severity::Friendly, 0.0f, "UNIVERSE | TICK %llu | %u SHIPS | %u GATES",
                   static_cast<unsigned long long>(m_universe.Tick()), m_universe.ShipCount(), m_universe.GateCount());

  // After the universe exists, and that ordering is load-bearing. The subscriber opens its despawn
  // cursor at DespawnHead, so that a ship which died during boot is not replayed as news to a client
  // that never held it (ADR 0027) -- and on a restored boot the head is the SAVED one, tens of
  // thousands of deaths in. Connected before the restore, the cursor would open at zero and the
  // publisher would walk the whole despawn log on the first tick.
  m_simulation.Connect(*m_serverQuic, m_config);

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
  if constexpr (BODY_PLANET_TEXTURED)
    bodyDesc.planetTexture = TERRAIN_DIR + L"Planet1.dds"; // 2048x1024, equirectangular

  // The sky's three textures, named here for the same reason: the engine knows there is a nebula
  // layer, a star layer and a flare layer, and which file fills each is content.
  SkyRenderer::Desc skyDesc;
  skyDesc.nebulaTexture = TEXTURE_DIR + L"CloudyGlow.dds";
  skyDesc.starTexture = TEXTURE_DIR + L"Glow.dds";
  skyDesc.burstTexture = TEXTURE_DIR + L"Starburst.dds";

  m_gpu.BeginCopies();
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
  m_gpu.SubmitCopies();
  m_gpu.ExecuteAndWait();
  m_bodyRenderer.DiscardStaging();
  m_skyRenderer.DiscardStaging();
  m_log.PushFormat(EventLog::Severity::Friendly, 0.0f, "FLEET ONLINE | %u SHIPS", OwnShipCount());
  // Every station row, the Vandal base included: the line says the grid spawned what the layout
  // described, and the base is a row in the same table (Design/Archive/Stations.md 6.1, 9.4).
  m_log.PushFormat(EventLog::Severity::Info, 0.0f, "STATIONS ONLINE | %u", m_universe.StationCount());
  // What genesis actually built, so a boot that laid out a different galaxy says so rather than
  // being discovered later by a fleet that cannot get anywhere.
  m_log.PushFormat(EventLog::Severity::Info, 0.0f, "GALAXY | %u SYSTEMS | %u GATES", static_cast<unsigned>(m_galaxy.systems.size()),
                   m_universe.GateCount());

  m_window.Show();
}

void OutpostApp::LoadServerConfig()
{
  // Bare name, so FileSys::ResolvePath puts it under <exe>\Assets\ like every mesh and font. The
  // executable ships one stating exactly the defaults, so both paths through here are exercised by
  // somebody: the shipped file by every run, and the missing-file path by a checkout that has not
  // deployed assets.
  const std::string text = TextFile::ReadFileA(SERVER_CONFIG_FILE);
  if (text.empty())
    return; // no file, or an empty one: the defaults are what the game booted on before it existed

  std::string error;
  if (!ParseServerConfig(text, m_config, error))
  {
    // Reported and not obeyed. The parser applied nothing, so m_config is still the defaults -- and
    // this root logs rather than exits because there is a window and a person in front of it. A
    // headless root prints the same message and exits non-zero (ADR 0043).
    m_log.PushFormat(EventLog::Severity::Alert, 0.0f, "CONFIG REFUSED | %s", error.c_str());
    DebugTrace("Server.cfg refused: {}\n", error.c_str());
    return;
  }

  m_log.PushFormat(EventLog::Severity::Info, 0.0f, "CONFIG | PORT %u | BACKLOG %u", static_cast<unsigned>(m_config.port),
                   static_cast<unsigned>(m_config.backlog));
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
    ThrowBootFailure("the QUIC library would not open", m_quic.Reason());

  QuicListener::Desc listenerDesc;
  listenerDesc.backlog = m_config.backlog;
  if (!m_listener.Start(m_quic, m_config.port, listenerDesc))
  {
    const std::string reason = m_listener.Reason();
    m_quic.Close();
    ThrowBootFailure("the port was refused", reason.c_str());
  }

  if (!m_clientQuic.Connect(m_quic, {"127.0.0.1", m_config.port}, {}))
  {
    const std::string reason = m_clientQuic.Reason();
    m_listener.Stop();
    m_quic.Close();
    ThrowBootFailure("the dial was refused", reason.c_str());
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
      m_log.PushFormat(EventLog::Severity::Friendly, 0.0f, "LINK | QUIC | 127.0.0.1:%u | %.1f MS", static_cast<unsigned>(m_config.port),
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
      ThrowBootFailure("the handshake timed out", states.c_str());
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

// The neighbourhood: a world at every planet site of the starting system, and six rocks among the
// fleet. The worlds are the layout's -- their positions, radii and seeds come from m_layout, so they
// sit where the stations are and hold still under F5 -- and the rocks are _seed's, so a reroll is a
// different field of rocks rather than an unrepeatable one (Design/Archive/Stations.md 5.3).
void OutpostApp::SpawnStartingBodies(std::uint64_t _seed)
{
  const std::int64_t startQpc = m_clock.Now();
  Pcg32 rng(_seed);

  // A body of a class, at a bearing and a distance, from a seed. A rock's seed is drawn from this
  // scene's generator, so the field follows from one number and no rock has to be told where in the
  // stream it sits; a world's is the layout's, which is what keeps it the same world after F5.
  // _centreY is the height of the body's centre, so a caller says where a world sits rather than
  // deriving it from a radius the seed chose. A rock passes its own radius and so rests on the
  // fleet's plane; a world passes a depth well below it (ViewTuning.h).
  //
  // Everything in this scene is placed around the local system's star, not around the universe
  // origin. They were the same point until there was a second system -- home is pinned at lattice
  // cell (0, 0) and a pinned system takes no jitter, so its star IS the origin -- which is why this
  // used to be LocalPos of a bearing and a distance, and why a world in any other system would have
  // been drawn a whole system away from the one it belongs to (Design/Archive/Universe-slice-4b.md 4).
  //
  // The boot scene does not move: Translate on a default UniversePos carries exactly the two floats
  // LocalPos carries and stores the same two remainders, so while the anchor is the origin the
  // result is bit-identical.
  const Game::UniversePos anchor = m_layout.starPos;

  const auto place = [&](BodyClass bodyClass, std::uint64_t _bodySeed, float _radiusMetres, float _bearingRad, float _distanceMetres,
                         float _centreY, bool _asteroid, bool _textured)
  {
    const BodyDesc desc = RandomBody(_bodySeed, _radiusMetres);
    const ColourRamp& ramp = m_ramps[static_cast<std::size_t>(bodyClass)];

    // Three grids of the same body, finest first: the body's own power, then one and two below it,
    // clamped at the field's floor. The level drawn is the view's per-frame choice
    // (Design/Archive/BodyLod-work-order.md 2.1). Lowering gridPower re-samples the same seeded noise more
    // coarsely, which is the source design's own three-LOD scheme.
    UniverseView::BodyView view;
    BodyBuildStats stats;
    for (std::uint32_t level = 0; level < UniverseView::BodyView::LOD_COUNT; ++level)
    {
      BodyDesc levelDesc = desc;
      const std::uint32_t basePower = _textured ? BODY_PLANET_SPHERE_GRID_POWER : desc.gridPower;
      levelDesc.gridPower = std::max(BodyField::MIN_GRID_POWER, basePower - std::min(basePower, level));

      BodyBuildStats levelStats;
      BodyHandle handle = INVALID_BODY;
      if (_textured)
      {
        // A picture instead of a generated surface. None of the field, the ramp or the dither runs:
        // a sphere of this radius and a map sampled off the direction is the whole of it. The
        // description above is still drawn from the stream, because a body that skipped its draws
        // would move every body generated after it.
        std::vector<FxVertex> sphere;
        BodyMeshBuilder::BuildSphere(_radiusMetres, levelDesc.gridPower, sphere);
        handle = m_bodyRenderer.UploadBody(m_gpu, sphere);
        levelStats.trianglesEmitted = static_cast<std::uint32_t>(sphere.size() / 3u);
      }
      else if constexpr (BODY_BAKE_ON_GPU)
      {
        // The kernel writes the vertices; this side only draws the random numbers into the block.
        // No BodyField is constructed at all -- its two measurement passes over the grid are
        // exactly what the first two dispatches replace.
        const BodyParams params = BodyField::ParamsFor(levelDesc);
        handle = m_bodyRenderer.BakeBody(m_gpu, params, ramp);

        // Every cell, because a baked body writes a degenerate triangle where the builder would
        // have emitted nothing; there is no CPU-side count on this path to read instead.
        const std::uint32_t cells = (1u << static_cast<std::uint32_t>(params.outsideMaxHeightGrid.z));
        levelStats.trianglesEmitted = CUBE_FACE_COUNT * cells * cells * 2u;
      }
      else
      {
        std::vector<FxVertex> terrain;
        BodyMeshBuilder::Build(BodyField(levelDesc), ramp.Loaded() ? &ramp : nullptr, terrain, levelStats);
        handle = m_bodyRenderer.UploadBody(m_gpu, terrain);
      }
      view.terrainLod[level] = handle;
      view.triangleCountLod[level] = levelStats.trianglesEmitted;
      if (level == 0)
        stats = levelStats;
    }

    if (view.terrainLod[0] == INVALID_BODY)
      return;

    view.textured = _textured;
    view.centre = anchor;
    Game::Translate(view.centre, std::sin(_bearingRad) * _distanceMetres, std::cos(_bearingRad) * _distanceMetres);
    view.centreY = _centreY;
    // The sphere the frustum test uses. A textured body is the sphere BuildSphere emitted and nothing
    // more -- unit directions scaled by the radius -- so the radius is the whole of it. A generated
    // one reaches further: every length in a BodyDesc but radiusMetres is a fraction of the radius,
    // so the furthest it can go is that radius stretched by its widest ellipsoid axis and raised by
    // its tallest continent. Read off the description, so it is right on either bake path --
    // BodyBuildStats::maxHeightMetres is only filled on the CPU one. Not desc.maxHeight, which
    // scales colour and is usually zero; a tile's own desiredHeight and lift are the geometry.
    float reach = 1.0f;
    if (!_textured)
    {
      const float widestAxis = std::max({desc.ellipsoid.x, desc.ellipsoid.y, desc.ellipsoid.z});
      float tallestTile = 0.0f;
      for (const BodyTile& tile : desc.tiles)
        tallestTile = std::max(tallestTile, tile.desiredHeight + tile.posY);
      reach = widestAxis + std::max(0.0f, tallestTile);
    }
    view.boundingRadiusMetres = _radiusMetres * reach;
    view.spinAxis = desc.spinAxis;
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
    DebugTrace("body: {} triangles, maximum height {} m, radius {} m\n", stats.trianglesEmitted, stats.maxHeightMetres, _radiusMetres);
  };

  // The worlds, one per site, wearing the one picture there is (Design/Archive/Stations.md 14). The class
  // is the only one left and it is not read on this path anyway -- a textured body takes none of
  // the ramp, the tiles or the craters a class describes. The depth is still the framing device it
  // was: a world sits high on the screen by being far below the fleet in the universe.
  for (const Game::PlanetSite& site : m_layout.planets)
  {
    place(BodyClass::Asteroid, site.bodySeed, site.radiusMetres, site.bearingRad, Game::Distance(m_layout.starPos, site.posUniverse),
          -BODY_START_PLANET_DEPTH_METRES, false, BODY_PLANET_TEXTURED);
  }

  for (int i = 0; i < BODY_START_ASTEROIDS; ++i)
  {
    const float radius =
      BODY_ASTEROID_RADIUS_MIN_METRES + rng.Float01() * (BODY_ASTEROID_RADIUS_MAX_METRES - BODY_ASTEROID_RADIUS_MIN_METRES);
    const float bearing = rng.Float01() * XM_2PI;
    const float distance =
      BODY_START_ASTEROID_RING_MIN_METRES + rng.Float01() * (BODY_START_ASTEROID_RING_MAX_METRES - BODY_START_ASTEROID_RING_MIN_METRES);
    // Named draws, not arguments: the order a compiler evaluates arguments in is unspecified and
    // this is seeded.
    const std::uint64_t high = rng.Next();
    const std::uint64_t low = rng.Next();
    place(BodyClass::Asteroid, (high << 32) | low, radius, bearing, distance, radius, true, false);
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

std::uint32_t OutpostApp::SystemAtCamera() const noexcept
{
  // Where the camera is looking, as a universe position: the view owns that conversion, because it
  // owns the origin the render space is measured from (UniverseView::UniversePosAt). What that
  // point is IN is the galaxy's question and is answered in GameLogic, where a suite can reach it
  // (Game::SystemAt, ADR 0055).
  const Game::UniversePos eye = m_view.UniversePosAt(m_camera.Target().x, m_camera.Target().z);
  return Game::SystemAt(m_galaxy.systems, eye);
}

OutpostApp::RestoreResult OutpostApp::RestoreUniverse()
{
  // Asked before it is read, and that is the whole of failing closed here. ReadFile answers "no
  // such file" and "I could not read the file" with the same empty buffer -- right for a texture,
  // where both mean the same missing thing, and wrong for this, where one starts a new universe and
  // the other must not (Neuron::FileSys::Exists).
  if (!Neuron::FileSys::Exists(Game::UNIVERSE_SAVE_FILE))
    return RestoreResult::Absent;

  const Neuron::ByteBuffer file = Neuron::BinaryFile::ReadFile(Game::UNIVERSE_SAVE_FILE);
  if (file.empty())
  {
    m_restoreRefusal = "Universe.sav is present and could not be read, or is empty";
    return RestoreResult::Refused;
  }

  Game::SaveHeader header;
  if (!Game::ReadSaveFile(file, header, m_universe))
  {
    // The reader changed nothing, so it cannot say which format it refused; the bytes are peeked
    // for the sentence, because "not a universe this build can read" is a diagnosis only when it
    // names the byte (AGENTS.md 5).
    std::uint8_t fileFormat = 0;
    std::uint8_t stateFormat = 0;
    if (Game::PeekSaveFormats(file, fileFormat, stateFormat))
    {
      // Widened before formatting: a uint8_t is a char to std::format, and the sentence would carry
      // a control character where it should carry a seven.
      m_restoreRefusal =
        std::format("Universe.sav is file format {} and state format {}; this build reads file formats {} to {} and "
                    "state formats {} to {}",
                    static_cast<unsigned>(fileFormat), static_cast<unsigned>(stateFormat),
                    static_cast<unsigned>(Game::SAVE_FILE_FORMAT_OLDEST), static_cast<unsigned>(Game::SAVE_FILE_FORMAT),
                    static_cast<unsigned>(Game::UNIVERSE_STATE_FORMAT_OLDEST), static_cast<unsigned>(Game::UNIVERSE_STATE_FORMAT));
    }
    else
    {
      m_restoreRefusal = "Universe.sav is present and is not a universe: the magic is wrong or the file is torn";
    }
    return RestoreResult::Refused;
  }

  m_galaxySeed = header.galaxySeed;

  // Which format the universe came out of, on every restore, and when it was an older one, the fact
  // that this run migrated it -- the next save writes the current format over the file that was
  // read. The file read is kept beside the save under its format's name, once and never overwritten,
  // because a reader that misread an older field produces a universe that loads, saves thirty
  // seconds later, and has by then destroyed the only file that could show what the field was
  // (Design/Archive/SaveMigration-work-order.md 1.4). A copy of an ACCEPTED file decides nothing for the
  // player, which is what ADR 0057's refusal of moving a REFUSED one aside was about.
  m_log.PushFormat(EventLog::Severity::Info, 0.0f, "SAVE | FORMAT %u", static_cast<unsigned>(header.stateFormat));
  if (header.stateFormat < Game::UNIVERSE_STATE_FORMAT || header.fileFormat < Game::SAVE_FILE_FORMAT)
  {
    const std::wstring sidecar = Game::UniverseSaveSidecarName(header.stateFormat);
    if (Neuron::FileSys::Exists(sidecar))
    {
      m_log.PushFormat(EventLog::Severity::Info, 0.0f, "SAVE | MIGRATED %u -> %u | KEPT ALREADY", static_cast<unsigned>(header.stateFormat),
                       static_cast<unsigned>(Game::UNIVERSE_STATE_FORMAT));
    }
    else if (Neuron::BinaryFile::WriteFileAtomic(sidecar, file))
    {
      m_log.PushFormat(EventLog::Severity::Info, 0.0f, "SAVE | MIGRATED %u -> %u | KEPT Universe.sav.%u",
                       static_cast<unsigned>(header.stateFormat), static_cast<unsigned>(Game::UNIVERSE_STATE_FORMAT),
                       static_cast<unsigned>(header.stateFormat));
    }
    else
    {
      // Alert for SaveUniverse's reason: the one file that could show what a misread field was is
      // the one that did not get kept, and the player is the only one who can move it aside.
      m_log.PushFormat(EventLog::Severity::Alert, 0.0f, "SAVE | MIGRATED %u -> %u | KEEP REFUSED",
                       static_cast<unsigned>(header.stateFormat), static_cast<unsigned>(Game::UNIVERSE_STATE_FORMAT));
    }
  }

  // From the file's own tick, so the first periodic save falls a whole period after the restore
  // rather than immediately -- a game reloaded and quit again should not rewrite the file it just
  // read.
  m_lastSaveTick = m_universe.Tick();
  return RestoreResult::Restored;
}

void OutpostApp::WriteTickStats()
{
  // The levels are read here rather than accumulated per tick, because each is a level and not
  // something a tick adds to.
  TickStats& stats = m_simulation.Stats();
  stats.tick = m_universe.Tick();
  stats.shipCount = m_universe.ShipCount();
  stats.subscriberCount = m_simulation.SubscriberCount();

  // Stamped whether or not the write lands, on SaveUniverse's argument: a disk that refuses once
  // will refuse again, and retrying every tick would turn a full disk into a frame rate.
  m_lastStatsTick = m_universe.Tick();

  if (!WriteTickStatsFile(stats))
  {
    // Info and not Alert, which is where this differs from a refused save: nothing is lost that the
    // game needed, and a readout that cannot be written is not a reason to interrupt a player.
    m_log.PushFormat(EventLog::Severity::Info, 0.0f, "STATS REFUSED | TICK %llu", static_cast<unsigned long long>(m_universe.Tick()));
  }
  stats.ResetWindow();
}

void OutpostApp::SaveUniverse()
{
  Game::SaveHeader header;
  header.galaxySeed = m_galaxySeed;
  // From the universe rather than from a constant, so the header and the state cannot disagree --
  // which is the one thing ReadSaveFile refuses a file for.
  header.shard = m_universe.Shard();

  Game::WriteSaveFile(m_universe, header, m_saveScratch);

  // The tick is stamped whether or not the write lands, and deliberately: a disk that refuses once
  // will refuse again, and retrying every tick would turn a full disk into a frame rate. The log
  // line is what a player has to go on, so it says which tick was lost.
  m_lastSaveTick = m_universe.Tick();

  if (!Neuron::BinaryFile::WriteFileAtomic(Game::UNIVERSE_SAVE_FILE, m_saveScratch))
  {
    // Alert rather than Info: the enum has three levels and this is the one a player must actually
    // see, because everything since the last good save is what a crash now would cost.
    m_log.PushFormat(EventLog::Severity::Alert, 0.0f, "SAVE REFUSED | TICK %llu", static_cast<unsigned long long>(m_universe.Tick()));
    return;
  }

  m_log.PushFormat(EventLog::Severity::Info, 0.0f, "SAVED | TICK %llu | %u KB", static_cast<unsigned long long>(m_universe.Tick()),
                   static_cast<unsigned>(m_saveScratch.size() / 1024u));
}

void OutpostApp::RebuildLocalSystemScenery()
{
  m_layout = Game::LayOutGalaxySystem(m_galaxy.systems[m_localSystem], Game::STARTING_GALAXY, Game::GALAXY_PINS);

  // The marks are replaced, not added to: they belong to one system, and the minimap's half-range is
  // 4 km against a guaranteed 57 km between stars, so a mark left behind for the system the camera
  // came from draws pinned to the edge forever.
  m_view.ClearStationMarks();
  for (const Game::PlanetSite& site : m_layout.planets)
    m_view.AddStationMark({site.posUniverse, Game::FACTION_VANGUARD});

  // The scene is released before the next one is built, which is what stops a crossing leaking the
  // system it left -- F5's bracket exactly, copies first for its reason (ADR 0044).
  m_view.ClearBodies();
  m_bodyRenderer.FreeAllBodies();

  m_gpu.BeginCopies();
  m_gpu.BeginUploads();
  // The system's own seed, so a system looks the same every time it is entered. BODY_START_SEED is
  // home's and F5's; a system that borrowed it would wear home's rocks.
  //
  // The sky is NOT rebuilt with the bodies, which is where this parts company with F5. F5 rerolls
  // the neighborhood and the sky is the far half of it; a gate crossing moves the camera from one
  // system's gate ring to the other's -- 43 km at the guaranteed minimum, 117 km on the shipped
  // pitch -- and a background that visibly turned over at that range would be claiming the galaxy
  // is a few hundred kilometres across. The sky is the same sky from every system in it.
  SpawnStartingBodies(m_galaxy.systems[m_localSystem].systemSeed);
  m_gpu.SubmitCopies();
  m_gpu.ExecuteAndWait();
  m_bodyRenderer.DiscardStaging();

  m_log.PushFormat(EventLog::Severity::Info, 0.0f, "SYSTEM | %u | %u WORLDS", m_localSystem,
                   static_cast<unsigned>(m_layout.planets.size()));
}

// F5. A different scene each press, and the same different scene after a restart: what the seed is
// offset by is the number of presses, not a clock.
//
// The sky is reseeded with the bodies rather than separately, because what F5 rerolls is the
// neighborhood and the sky is the far half of it. A second key for it would be a second thing to
// remember for no second question it answers. What it does not reroll is the layout: the worlds
// are rebuilt from m_layout and stay where the stations are, because a debug key that moved
// simulation content would be a debug key changing the universe (Design/Archive/Stations.md 5.3).
void OutpostApp::ReseedBodies()
{
  m_view.ClearBodies();

  // The scene is now released rather than left on the GPU. What F5 cost before this was the memory
  // of every scene it had ever replaced -- BodyRenderer only ever appended, so a handle was an index
  // and nothing could say a body was finished with (Design/Archive/MmoScalabilityReview.md G3, ADR 0044).
  // The buffers themselves go at DiscardStaging below, which is the first point the GPU is known to
  // be done with them.
  m_bodyRenderer.FreeAllBodies();
  ++m_bodyRerollCount;

  m_gpu.BeginCopies();
  m_gpu.BeginUploads();
  SpawnStartingBodies(BODY_START_SEED + m_bodyRerollCount);
  BuildSky(SKY_SEED + m_bodyRerollCount);
  // Copies first, always: SubmitCopies enqueues the graphics queue's wait on the copy fence, so any
  // direct-queue work submitted after it is correctly ordered behind the copies and any submitted
  // before it is not (ADR 0044).
  m_gpu.SubmitCopies();
  m_gpu.ExecuteAndWait();
  m_bodyRenderer.DiscardStaging();
  m_skyRenderer.DiscardStaging();
}
void OutpostApp::MarkLocalStations()
{
  m_view.ClearStationMarks();
  for (const Game::PlanetSite& site : m_layout.planets)
    m_view.AddStationMark({site.posUniverse, Game::FACTION_VANGUARD});
}
// Somebody else lives here: a station northeast of the fleet, and three Interceptors walking a ring
// around it at a third of their top speed. Their guns work -- a mount acquires the nearest ship its
// faction already holds hostile, which at this range is anything of the player's that comes close --
// but their helms do not react to any of it: the ring stays a metronome by the owner's brief
// (Design/Archive/Hostiles.md 6), because in this game senses live in the mounts and not in the helm
// (Design/Archive/Combat.md 5.4).
//
// The base is a station row too, with no garrison: its patrol is not a garrison and does not
// change, and what the row buys is one answer path for "may I dock here" -- the player is refused
// by standing rather than by a special case (Design/Archive/Stations.md 6.1, 15 decision 4).
// The player's own ships, not every ship in the universe. Without this the game would greet the player
// with FLEET ONLINE | 7 SHIPS, four of them the enemy's.
std::uint32_t OutpostApp::OwnShipCount() const noexcept
{
  std::uint32_t count = 0;
  for (const Game::ShipState& ship : m_universe.Ships())
    count += (ship.factionId == m_ownFaction) ? 1u : 0u;
  return count;
}

void OutpostApp::FlyToSystem(std::uint32_t _system)
{
  if (_system >= m_galaxy.systems.size())
    return;

  // SnapGoal and not SetGoal. The map is a jump in attention rather than a pan, and the distances
  // are three orders of magnitude past what UniverseView::FollowFocusedFleet already calls "beyond
  // any distance worth watching" -- an eased flight across a galaxy is a minute of empty space.
  const Game::UniversePos star = m_galaxy.systems[_system].starPos;
  m_camera.SnapGoal(m_view.ViewX(star), m_view.ViewZ(star));

  // The focus goes with it. Flying somewhere is the player looking, and a fleet focus that survived
  // the flight would drag the camera back on the very next frame (Design/GalaxyMap-slice-2.md 4.4).
  m_view.CancelFocus();

  // Closed the one way this screen closes: the rail button is its state, so unlighting the button is
  // what closes it, and Escape runs these same two lines (OnKeyDown).
  m_map.Close();
  m_hud.ClearActiveRail();

  // And that is the whole flight. Nothing here rebuilds the scenery, because nothing has to: the
  // frame loop already asks SystemAtCamera once per frame, after the last thing that moves the
  // camera and before Render, and rebuilds when the answer changes. That check was written for a
  // fleet crossing a gate, and it covers this for free -- which is exactly the claim
  // Design/Archive/Universe-slice-4b.md made when it put the scenery on WHERE THE CAMERA IS rather
  // than on a jump event, and this is the first caller that tests it (Design/GalaxyMap-slice-2.md 1).
  // By value and at 0.0f, which is what every other line the composition root pushes does. The
  // pointer-and-SimTimeSec form belongs to UniverseView, which holds the log by pointer and is inside
  // the class whose sim clock that is.
  m_log.PushFormat(EventLog::Severity::Info, 0.0f, "MAP | SYSTEM %u", _system);
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
    // Three meanings, innermost first: close the modal screen, drop the selection, quit. A modal
    // that Escape does not close is a modal a player gets stuck in.
    if (m_assembly.IsOpen())
      m_assembly.Close();
    else if (m_map.IsOpen())
    {
      // The rail button is the map's switch, so closing the map by any other route has to unlight
      // it: a lit button over a closed screen would reopen the map on the next frame's sync below,
      // and Escape would look like it had done nothing.
      m_map.Close();
      m_hud.ClearActiveRail();
    }
    else if (m_sheet.IsOpen())
      m_sheet.Close();
    // Drops the selection next; only quits once nothing is selected. By fleets rather than by
    // records: a fleet can be selected with every one of its hulls outside the interest set, and
    // Escape must still drop it.
    else if (m_view.SelectedFleetCount() > 0)
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
    // composition root calls Universe directly. It is the one place allowed to, and this design must
    // not invent a despawn order on the wire for a tuning aid (Design/Archive/SpaceshipExplosion.md 9).
    //
    // The ids are collected before the first despawn: Ships() is a span over the last snapshot
    // rather than over the universe, so the walk itself is safe, and taking the ids first keeps it
    // that way if it ever stops being.
    //
    // The snapshot names entities and Universe despawns handles, so this is the one place in the
    // executable that crosses back the way the publisher crosses forward -- which the composition
    // root is entitled to do, being the only thing that holds both halves (ADR 0047).
    std::vector<Game::EntityId> doomed;
    const std::span<const Game::ShipSnapshot> ships = m_view.Ships();
    for (std::size_t i = 0; i < ships.size(); ++i)
    {
      if (m_view.IsSelected(i))
        doomed.push_back(ships[i].entity);
    }
    for (const Game::EntityId entity : doomed)
      m_universe.DespawnShip(m_universe.HandleOfEntity(entity));
    break;
  }
  case VK_F5:
    // Reseed every body. A tuning key: what it costs is the memory of the scene it replaces, since
    // BodyRenderer keeps every handle for the run (OutpostApp.h says so beside the key list).
    ReseedBodies();
    break;
  // F6 and F7 are gone, and this is where they were.
  //
  // Each stood in for an act nothing in the game could perform: F6 declared a selected ship an
  // aggressor against the nearest Vanguard station so the protector response could be watched, and
  // F7 declared the nearest hostile the attacker of the player's fleet so the defense could be. The
  // fire pass states both for itself now, off shots it observed (ADR 0052) -- an attack order on a
  // Vanguard asset IS F6, and any landed hit IS F7 -- so a hook that stated an act the simulation
  // never saw would be the one thing ADR 0041 forbids, wearing a keyboard shortcut.
  //
  // Universe::RecordAggression and Universe::RecordHostileAct stay exactly as they are. They are the
  // simulation's own entry points, they still have no client message and never will, and the tests
  // still drive them directly (Design/Archive/Combat-slice-4.md 2.6).
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

  // Where the camera was looking before this frame's input, so a pan can be told from a flight.
  const XMFLOAT3 wasLookingAt = m_camera.Target();

  for (const PointerEvent& event : m_pendingEvents)
  {
    // The modal screen first, then the HUD, then the universe. The assembly view consumes everything
    // while it is up, and the HUD consumes what lands on a panel -- so a tap on the bottom bar can
    // never reach the tracker as an order.
    if (m_assembly.HandlePointer(event, m_view, m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx()))
      continue;
    if (m_sheet.HandlePointer(event, m_view, m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx()))
      continue;

    // The HUD before the map, and narrowed to its rail while the map is up. That inversion is
    // deliberate and is the one place the map differs from the assembly screen: the rail's Universe
    // button is what opened the map, so it has to stay reachable to close it, while everything else
    // the HUD owns is behind the map's scrim and must not take a press through it. The map then
    // takes every event the rail did not, which is what modal means here.
    int openSheet = -1;
    const bool usedByHud =
      m_hud.HandlePointer(event, m_view, openSheet, m_map.IsOpen(), m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx());
    if (openSheet >= 0)
      m_sheet.Open(openSheet);
    if (usedByHud)
      continue;
    // The map, which consumes everything the rail did not and may name a system as it does.
    std::uint32_t tapped = static_cast<std::uint32_t>(m_galaxy.systems.size());
    const bool usedByMap = m_map.HandlePointer(event, m_galaxy, tapped, m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx());
    if (tapped < m_galaxy.systems.size())
      FlyToSystem(tapped);
    if (usedByMap)
      continue;
    m_pointers.Apply(event, m_camera, m_view);
  }
  m_pendingEvents.clear();
  m_camera.Update(); // input may have moved it again

  // The rail's Universe button IS the map's open state, followed rather than mirrored. The button is
  // a toggle the HUD already owned and lights itself; making the screen follow it keeps one truth
  // instead of two that can drift, and Escape unlights the button rather than closing the screen
  // behind its back (OnKeyDown).
  const bool wantsMap = m_hud.ActiveRail() == static_cast<int>(Hud::RailIcon::Universe);
  if (wantsMap && !m_map.IsOpen())
  {
    m_map.Open();
    // Modal from this frame on, so a contact the tracker is still holding would never be released to
    // it -- the assembly screen's reason, below. The HUD's capture is NOT dropped here and must not
    // be: this ran because a rail press completed, which released that capture itself, and cancelling
    // it again would be cancelling the next one if the sync ever stopped being immediate.
    m_pointers.CancelContacts();
    m_sheet.Close();
  }
  else if (!wantsMap && m_map.IsOpen())
    m_map.Close();

  // A pan gives the camera back to the player, and this is the only place that can see one: pan,
  // orbit and zoom reach Camera straight out of PointerTracker and never touch the listener, so
  // UniverseView cannot know a gesture happened. The composition root holds both and can compare.
  //
  // The TARGET, specifically, and not the whole camera: a pan is what moves it, and orbit and zoom
  // do not. That asymmetry is the right rule rather than an accident of what is easy to detect --
  // orbiting or zooming while the camera flies to a fleet is watching the flight, and cancelling it
  // for that would take the gesture away from the player to protect a gesture they still want.
  //
  // Compared exactly, because the question is whether anything moved it at all and the focus ease
  // that would move it by a hair has not run yet this frame (it runs in Run, after this returns).
  if (m_camera.Target().x != wasLookingAt.x || m_camera.Target().z != wasLookingAt.z)
    m_view.CancelFocus();

  // A ledger reply for the station the player long-pressed opens the assembly view over it. Taken
  // rather than read: one ask gets one screen, and a reply nobody is waiting for opens nothing
  // (UniverseView::TakeLedgerReply, ADR 0051).
  Game::LedgerReply ledger;
  if (m_view.TakeLedgerReply(ledger))
  {
    m_assembly.Open(ledger, m_view);

    // The screen is modal from this frame on, so any contact the tracker or the HUD is still
    // holding will never be released to them -- the assembly view swallows the lift. Dropping both
    // here is what stops a finger that went down in the frame before the reply arrived from
    // leaving a claimed slot, or a stuck capture, behind for good.
    m_pointers.CancelContacts();
    m_hud.CancelCapture();
    m_sheet.Close(); // one panel at a time; the modal one wins

    // And the map, whose switch is a rail button: closing it without unlighting the button would
    // have the sync above reopen it under the assembly screen on the next frame.
    m_map.Close();
    m_hud.ClearActiveRail();
  }

  // Where the player is looking becomes what the server sends. The composition root is the only
  // thing holding both halves, so it is the only thing that can say so; a dedicated server reads it
  // off the session instead (UniverseSimulation::SetViewCentre).
  m_simulation.SetViewCentre(m_view.UniversePosAt(m_camera.Target().x, m_camera.Target().z));
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
  frame.stats.shipCount = m_universe.ShipCount();
  frame.stats.timeScale = m_timeScale;
  // The last tick's two halves, and the worst of the window so far. Read straight off the block the
  // adapter fills, which is reset when the sidecar is written -- so WORST is "the worst since the
  // last file" rather than since boot, and it is meant to be (Outpost/TickStats.h).
  frame.stats.stepMs = m_simulation.Stats().stepLastMs;
  frame.stats.publishMs = m_simulation.Stats().publishLastMs;
  frame.stats.stepWorstMs = m_simulation.Stats().stepWorstMs;
  frame.stats.subscriberCount = m_simulation.SubscriberCount();
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
  frame.stats.pathIslandCount = static_cast<std::uint32_t>(m_universe.PathIslandCount());
  frame.stats.pathIslandsDeclined = static_cast<std::uint32_t>(m_universe.DeclinedPathIslandCount());
  frame.showDebug = m_showDebug;
  frame.sector = m_view.UniversePosAt(m_camera.Target().x, m_camera.Target().z);
  frame.hullNames = HULL_NAMES;
  frame.factionNames = FACTION_NAMES;
  frame.ownFaction = m_ownFaction;
  // What this client currently knows about, counted off the snapshot rather than off the universe: a
  // contact is a hostile *record*, which is the only reading that stays honest over a real wire. The
  // station counts, so the base reads as four. Hostile by the header's mask, not by "not mine": a
  // Vanguard station in view is not a contact until the law turns on the player, and then every
  // Vanguard record joins the count, which is the HUD saying what just happened without a new
  // widget (Design/Archive/Stations.md 9.4).
  frame.contacts = 0;
  for (const Game::ShipSnapshot& ship : m_view.Ships())
    frame.contacts += m_view.IsHostileToMe(ship.factionId) ? 1 : 0;
  m_hud.Draw(m_textRenderer, m_view.Ships(), m_view, m_camera, m_log, frame, m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx());

  // The sheet sits over the bar, then the map, then the assembly screen over everything: both
  // screens are modal and draw their own scrim, the bar and the sheet included. The assembly screen
  // is last because it is the one that can open while the map is up -- a ledger reply closes the map
  // below, and drawing it last means it is on top on the frame that happens.
  m_sheet.Draw(m_textRenderer, m_view, HULL_NAMES, m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx());
  // Guarded rather than left to Draw's own early return, because the argument is work: SystemAtCamera
  // walks all 54 stars, and a closed screen must cost nothing per frame (GalaxyMap-slice-1.md 4.5).
  if (m_map.IsOpen())
    m_map.Draw(m_textRenderer, m_galaxy, m_view, SystemAtCamera(), m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx());
  m_assembly.Draw(m_textRenderer, m_view, HULL_NAMES, FACTION_NAMES, m_window.DpiScale(), m_gpu.WidthPx(), m_gpu.HeightPx());

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
    // loopback is no longer in this program (ADR 0028); QUIC's delay is the wire's own and arrives
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
    // swapchain runs ahead of the tick rate. The fleet focus and the alert pulse are the same kind
    // of thing and ride the same clock.
    m_view.UpdateFeedback(dtSec);
    m_view.UpdateFocus(dtSec);
    m_hud.UpdatePulse(dtSec);
    m_sheet.Update(m_view);

    // The scenery follows the camera across a gate.
    //
    // Driven by where the camera IS rather than by the jump itself, and that is the load-bearing
    // choice: a jump is one way to arrive in another system, and the day there is a second -- a
    // galaxy map that flies you somewhere, a spectator following a fleet, a save reloaded
    // elsewhere -- this already covers it. It is also the only formulation that cannot get out of
    // step, because the question it asks is a fact about the camera rather than a memory of an
    // event.
    //
    // Here rather than in Update, and that placement is the whole of it: UpdateFocus is the last
    // thing in a frame that moves the camera, and it is what SNAPS across a crossing. Asked in
    // Update the answer would be one frame stale, and that frame renders the new system's ships with
    // the old system's worlds -- tens of kilometres behind the camera and so outside the frustum
    // entirely, which reads as the arrival flashing empty. Asked here it cannot: nothing moves the
    // camera between this line and Render.
    //
    // Once per frame and free at this scale: fifty-four squared distances, against a lattice whose
    // stars are 56 926 m apart, so the answer is never ambiguous anywhere a player can be
    // (Game::SystemAt, Design/Archive/Universe-slice-4b.md 4).
    const std::uint32_t here = SystemAtCamera();
    if (here != m_localSystem)
    {
      m_localSystem = here;
      RebuildLocalSystemScenery();
    }

    // The periodic save, here because here is between ticks: Advance ran every tick this frame was
    // owed and the next one cannot start until the next Advance, so the universe is at rest, which
    // is the codec's whole contract (Design/Archive/Universe.md 8).
    //
    // A distance since the last save, not a modulo of the tick. A frame that advances four ticks
    // steps straight over any single tick the modulo would have matched, so the save would be
    // skipped -- rarely, on a slow frame, and therefore in exactly the run that most wanted it.
    //
    // The universe's own tick, not the host's. They are the same number -- ServerHost::Tick forwards
    // to the simulation, which forwards to the universe -- and m_lastSaveTick is stamped from the
    // universe, so reading one object at both ends is what stops the subtraction underflowing into
    // a save every frame if that chain ever grows a counter of its own.
    if (m_config.saveEveryTicks != 0 && m_universe.Tick() - m_lastSaveTick >= m_config.saveEveryTicks)
      SaveUniverse();

    // Beside the save and for the save's reason: between ticks. It writes a window and resets, so
    // a file is what the last statsEveryTicks ticks cost rather than a mean since boot that stops
    // moving after ten minutes (Design/Archive/TickTelemetry-work-order.md).
    if (m_config.statsEveryTicks != 0 && m_universe.Tick() - m_lastStatsTick >= m_config.statsEveryTicks)
      WriteTickStats();

    Render();
  }

  // Once at clean shutdown, and "clean" means precisely this: the loop above returned rather than
  // threw. Putting it in Shutdown instead would save on the way out of a FAILED boot too -- wWinMain
  // calls Shutdown from both catch blocks -- which would write a half-built universe over a good
  // file, or, worse, over the very file that had just been refused (Design/Archive/Universe-slice-5.md 7).
  SaveUniverse();
}

void OutpostApp::Shutdown()
{
  // In this order, and before the device goes away: a registration cannot close while a connection
  // on it lives, so the client end closes first, then the listener with everything it accepted, then
  // the library (Design/Archive/QuicTransport.md 6). Nothing is logged here -- a shutdown path does nothing
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
