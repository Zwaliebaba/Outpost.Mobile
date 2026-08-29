#pragma once

#include "BodyCatalogue.h"
#include "EventLog.h"
#include "Hud.h"
#include "WorldSimulation.h"
#include "WorldView.h"

#include "LoopbackTransport.h"
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
  // Opens the QUIC link both halves will talk over: library, listener, client, and a bounded wait for
  // the handshake. False means the game runs on the loopback instead, and the reason is already in
  // the event log by the time it returns.
  [[nodiscard]] bool OpenQuicLink();

  void LoadHullMeshes();
  void SpawnStartingFleet();

  // Generates, uploads and places the starting bodies from one seed. Called at boot and again on
  // every F5; the caller brackets it with BeginUploads and ExecuteAndWait, because every body's
  // vertex copy belongs in one submission.
  void SpawnStartingBodies(std::uint64_t _seed);
  // The sky, unlike the bodies, *does* release its buffer when it is rebuilt: there is exactly one
  // of them and UploadField replaces it. It is cheap enough that nothing keeps its timing -- the
  // trace at the call site is where a regression in it would show.
  void BuildSky(std::uint64_t _seed);
  void ReseedBodies();
  void SpawnHostileBase();
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
  // is one executable and stays one for every phase of Design/Collision.md -- what the transport
  // changes is the code boundary, not the process boundary (Design/Collision.md 2).
  //
  // That transport is QUIC across 127.0.0.1 when it can be and the loopback when it cannot, and
  // neither half can tell which it got: the seam is four virtual functions and the choice is made
  // here and nowhere else (Design/QuicTransport.md 6). Booting on the real stack is the point --
  // a path nobody runs is a path nobody notices breaking -- and the fallback is what keeps a taken
  // port or a locked-down key store from being the reason the game did not start.
  Game::World m_world;
  WorldSimulation m_simulation{m_world};

  // Which faction this client is. Session identity, which nothing below the composition root can
  // know: it decides what the overview colors green, what may be selected, and what counts as a
  // contact. The day a login exists it arrives with the session and only this line changes.
  Game::FactionId m_ownFaction = Game::FACTION_PLAYER;

  Neuron::ServerHost m_host;

  // The fallback, and the instrument: tick-counted latency is the only reproducible kind, so the
  // measurements Design/Collision.md 18 wants are still taken here whether or not QUIC is carrying
  // the game (Design/Archive/Collision-slice-2b.md 2.1). Constructed either way; two unused rings
  // cost 288 KB each and keeping them means the knob is still one line.
  Neuron::LoopbackTransport m_serverLink;
  Neuron::LoopbackTransport m_clientLink;

  // Declared after the loopback pair and in this order on purpose: members are destroyed in reverse,
  // so the client end goes before the listener and the listener before the library, which is the one
  // order MsQuic accepts (a registration cannot close over a live connection).
  Neuron::QuicApi m_quic;
  Neuron::QuicListener m_listener;
  Neuron::QuicTransport m_clientQuic;
  Neuron::QuicTransport* m_serverQuic = nullptr; // the listener's, once accepted; null on the fallback
  bool m_linkIsQuic = false;

  // Presentation.
  WorldView m_view;
  EventLog m_log;
  Hud m_hud;
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
  // something to consume, and **F5 reseeds every body and the sky with them**. F5 does not release the buffers the last
  // scene's bodies are in -- BodyRenderer keeps every handle for the run -- so each press costs the
  // memory of the scene it replaced. That is acceptable for a tuning key and is not acceptable for
  // anything a player does; a ReleaseBody is a slice of its own the day a body has to go away.
  float m_timeScale = 1.0f;
  bool m_showDebug = false;
  std::uint32_t m_bodyRerollCount = 0;
  float m_bodyGenerationMs = 0.0f;
};
} // namespace Outpost
