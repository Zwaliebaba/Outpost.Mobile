#pragma once

#include "BodyCatalogue.h"
#include "EventLog.h"
#include "AssemblyScreen.h"
#include "FleetSheet.h"
#include "Hud.h"
#include "ServerConfig.h"
#include "UniverseSimulation.h"
#include "UniverseView.h"

#include "UniverseLayout.h"

#include "QuicApi.h"
#include "QuicListener.h"
#include "QuicTransport.h"

#include "AppWindow.h"
#include "BodyRenderer.h"
#include "Camera.h"
#include "ColourRamp.h"
#include "FxRenderer.h"
#include "GpuDevice.h"
#include "MeshLibrary.h"
#include "PointerTracker.h"
#include "SceneRenderer.h"
#include "ServerHost.h"
#include "TextRenderer.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Outpost
{
// The composition root. It is the only place in the tree that knows every layer exists, and the
// only place allowed to: it constructs them, wires them together, and owns the boot and shutdown
// order. Nothing below it reaches sideways -- the client half never names a game type, the
// simulation never names a renderer, and neither of them names the other.
//
// It is also where configuration is read (there is no argv and no environment: libraries are handed
// plain structs) and where the one exception handler lives.
class OutpostApp
{
public:
  void Init(HINSTANCE _instance);
  void Run();
  void Shutdown();

private:
  // Reads Assets/Server.cfg into m_config, or leaves it at the values this executable compiles in.
  //
  // A missing file is not a failure -- the defaults are what the game booted on before there was a
  // file at all. A malformed one is: the parser applies nothing and names the line, and this root
  // logs that and boots on the defaults, because a typo in a tuning file should not be a black
  // screen. A headless root would print the same message and exit non-zero, which is why the
  // parser decides nothing and this function does (ADR 0043).
  void LoadServerConfig();

  // Opens the QUIC link both halves will talk over: library, listener, client, and a bounded wait for
  // the handshake. False means the game runs on the loopback instead, and the reason is already in
  // the event log by the time it returns.
  // Opens the one wire the halves meet on, or throws naming what refused (ADR 0028). Not
  // [[nodiscard]] because it no longer answers a question: it either returns having connected both
  // ends, or it does not return.
  void OpenQuicLink();

  void LoadHullMeshes();

  // Generates, uploads and places the starting bodies from one seed. Called at boot and again on
  // every F5; the caller brackets it with BeginUploads and ExecuteAndWait, because every body's
  // vertex copy belongs in one submission.
  void SpawnStartingBodies(std::uint64_t _seed);
  // The sky, unlike the bodies, *does* release its buffer when it is rebuilt: there is exactly one
  // of them and UploadField replaces it. It is cheap enough that nothing keeps its timing -- the
  // trace at the call site is where a regression in it would show.
  void BuildSky(std::uint64_t _seed);
  void ReseedBodies();

  // Which system the camera is standing in. Two lines: where the camera is looking, which only the
  // view can say, and what is there, which is Game::SystemAt's.
  [[nodiscard]] std::uint32_t SystemAtCamera() const noexcept;

  // Re-lays the local system and rebuilds everything drawn from it: the worlds, the rocks and the
  // minimap's station marks.
  //
  // The bodies go through the same free-and-upload bracket F5 uses, for its reason: a body's
  // vertices are a copy recorded into one command list, and the scene being replaced has to be
  // released or every crossing leaks the system it left (ADR 0044).
  void RebuildLocalSystemScenery();

  // What the boot found where the universe should be.
  //
  // Three answers and not two, because "there is no file" and "there is a file I cannot read" must
  // lead to opposite places: the first is a first boot and the second stops the program. Collapsing
  // them is the one mistake this file must not make -- a refused save quietly replaced by a fresh
  // universe is a player's game deleted by a bug in reading it (Design/Universe.md 8, ADR 0057).
  enum class RestoreResult
  {
    Absent,   // no file: this is a first boot and genesis runs
    Restored, // the universe came out of the file
    Refused,  // there is a file and it is not one this build can use
  };

  // Reads Universe.sav into m_universe, and the galaxy seed it was laid out from into m_galaxySeed.
  // Touches neither on anything but Restored.
  [[nodiscard]] RestoreResult RestoreUniverse();

  // Writes the universe to Universe.sav, atomically. Logs and carries on if the disk refuses: a
  // running game should not end because a save did, and the previous save is still there.
  //
  // Only ever called between ticks. The state codec's contract is a universe at rest, and a save
  // taken mid-Step would be a universe that never existed (Design/Universe.md 8).
  void SaveUniverse();

  // The local system's planets, as minimap marks. The stations they stand for are in the save file;
  // the marks are not, because a mark is a picture rather than a record. Rebuilt at boot and again
  // whenever the camera changes systems (Design/Universe-slice-4b.md 4).
  void MarkLocalStations();

  [[nodiscard]] std::uint32_t OwnShipCount() const noexcept;
  void Update();
  void Render();

  void OnResize(std::uint32_t _widthPx, std::uint32_t _heightPx);
  void OnKeyDown(std::uint32_t _virtualKey);

  // Window and device.
  Neuron::AppWindow m_window;
  Neuron::GpuDevice m_gpu;
  Neuron::SceneRenderer m_sceneRenderer;
  Neuron::TextRenderer m_textRenderer;
  Neuron::FxRenderer m_fxRenderer;
  Neuron::BodyRenderer m_bodyRenderer;
  Neuron::SkyRenderer m_skyRenderer;
  Neuron::MeshLibrary m_meshes;

  // Input and framing.
  Neuron::Camera m_camera;
  Neuron::PointerTracker m_pointers;
  // Queued rather than acted on immediately: the frame loop drains them once the camera matrices
  // are current, so a pick tests against what was on screen when the contact happened.
  std::vector<Neuron::PointerEvent> m_pendingEvents;

  // Simulation, hosted. The two halves meet only at the transport: the simulation publishes a
  // snapshot on each tick and reads the orders that arrived, and the view reads the snapshot. This
  // is one executable and stays one for every phase of Design/Archive/Collision.md -- what the transport
  // changes is the code boundary, not the process boundary (Design/Archive/Collision.md 2).
  //
  // That transport is QUIC across 127.0.0.1 when it can be and the loopback when it cannot, and
  // neither half can tell which it got: the seam is four virtual functions and the choice is made
  // here and nowhere else (Design/Archive/QuicTransport.md 6). Booting on the real stack is the point --
  // a path nobody runs is a path nobody notices breaking -- and the fallback is what keeps a taken
  // port or a locked-down key store from being the reason the game did not start.
  // What this root was told to be, read once at boot and a value from then on. It is the only thing
  // in the executable that came out of a file rather than out of a header, which is AGENTS.md 5's
  // rule about where configuration is allowed to enter (ADR 0043).
  ServerConfig m_config;

  Game::Universe m_universe;
  UniverseSimulation m_simulation{m_universe};

  // The starting system, laid out once at boot and read by three consumers: the station spawns,
  // the body placements and the minimap's marks. Static content, not simulation state -- the universe
  // is handed the sites as spawn positions and never sees the generator (Design/Archive/Stations.md 5, 10).
  // F5 rebuilds the bodies from it and never re-rolls it, so a debug key cannot move a station.
  Game::SystemLayout m_layout;

  // The galaxy, laid out once at boot beside the starting system. Static content on the same terms
  // (ADR 0037, ADR 0055): the universe is handed positions and seeds as spawn input and never sees
  // the generator, and F5 does not reach it.
  Game::GalaxyLayout m_galaxy;

  // Which system the camera is in. Home at boot, and nothing moves it yet -- the client half of
  // crossing a gate is slice 4's. It is here now because the station marks and the bodies both ask
  // it which system they are placing, and answering "home" in two places would be two places to
  // change (Design/Universe.md 9).
  std::uint32_t m_localSystem = 0;

  // The seed the galaxy on screen was laid out from, taken from the save header at boot. The
  // initialiser is what a default-constructed app would use and is never what runs: RestoreUniverse
  // overwrites it before anything reads it, and a boot that could not read a file does not get this
  // far. A binary whose own idea of the seed had moved on still draws the galaxy its file holds.
  std::uint64_t m_galaxySeed = Game::STARTING_GALAXY_SEED;

  // The tick the last save was taken at, so the cadence is a distance rather than a modulo -- see
  // the call site in Run.
  std::uint64_t m_lastSaveTick = 0;

  // Reused across saves rather than allocated per save: at 300 ships this is tens of kilobytes and
  // the save runs inside a frame.
  std::vector<std::uint8_t> m_saveScratch;

  // Which faction this client is. Session identity, which nothing below the composition root can
  // know: it decides what the overview colors green, what may be selected, and what counts as a
  // contact. The day a login exists it arrives with the session and only this line changes.
  Game::FactionId m_ownFaction = Game::FACTION_PLAYER;

  Neuron::ServerHost m_host;

  // Declared in this order on purpose: members are destroyed in reverse, so the client end goes
  // before the listener and the listener before the library, which is the one order MsQuic accepts
  // (a registration cannot close over a live connection).
  Neuron::QuicApi m_quic;
  Neuron::QuicListener m_listener;
  Neuron::QuicTransport m_clientQuic;
  Neuron::QuicTransport* m_serverQuic = nullptr; // the listener's, once accepted

  // Whether there is a link to close. Not "whether it is QUIC" -- there is only one wire now
  // (ADR 0028) -- but Shutdown still runs after a boot that threw partway, and must not close a
  // library that was never opened.
  bool m_linkOpen = false;

  // Presentation.
  UniverseView m_view;
  EventLog m_log;
  Hud m_hud;

  // The station's assembly view. Modal, so it sits ahead of the HUD in the pointer chain and is
  // drawn after it (Design/Archive/Fleets.md 9.4).
  AssemblyScreen m_assembly;

  // One fleet's sheet, over the bar. Not modal, so it takes only what lands on itself and sits
  // between the assembly screen and the HUD in the chain (Design/Archive/Fleets.md 9.3).
  FleetSheet m_sheet;
  Neuron::FrameClock m_clock;

  // One ramp per class, indexed by BodyClass. A ramp that fails to load leaves its class drawing the
  // builder's fallback grey, which is a diagnostic and not a crash.
  //
  // On the heap and not in a std::array, which is what this wants to be. A ColourRamp is a 64x64
  // table of floats -- 48 KB -- and six of them are nearly 300 KB; OutpostApp is a local of
  // wWinMain, and a default 1 MB stack reserve is not the place to put a third of a megabyte of
  // content. Sized once in Init and never resized, so it is a fixed table that happens to live
  // somewhere else.
  std::vector<Neuron::ColourRamp> m_ramps;

  // Debug: 1, 2 and 3 slow, restore and speed up the simulation without touching the frame rate;
  // F1 shows the readout, F3 shakes the camera, F4 despawns the selection so the explosion has
  // something to consume, and **F5 reseeds every body's look and the sky with them** -- never the
  // sites, which are the layout's (m_layout). F6 and F7 stood here and are gone: each declared an
  // act the simulation could not perform, and the fire pass performs both for real now
  // (OutpostApp.cpp's note where they were, Design/Combat.md 6, ADR 0052).
  //
  // F5 does not release the buffers the last scene's bodies are in -- BodyRenderer keeps every
  // handle for the run -- so each press costs the memory of the scene it replaced. That is acceptable
  // for a tuning key and is not acceptable for anything a player does; a ReleaseBody is a slice of
  // its own the day a body has to go away.
  float m_timeScale = 1.0f;
  bool m_showDebug = false;
  std::uint32_t m_bodyRerollCount = 0;
  float m_bodyGenerationMs = 0.0f;
};
} // namespace Outpost
