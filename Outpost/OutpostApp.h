#pragma once

#include "EventLog.h"
#include "Hud.h"
#include "WorldSimulation.h"
#include "WorldView.h"

#include "LoopbackTransport.h"

#include "AppWindow.h"
#include "Camera.h"
#include "FxRenderer.h"
#include "GpuDevice.h"
#include "MeshLibrary.h"
#include "PointerTracker.h"
#include "SceneRenderer.h"
#include "ServerHost.h"
#include "TextRenderer.h"

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
  void LoadHullMeshes();
  void SpawnStartingFleet();
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
  Game::World m_world;
  WorldSimulation m_simulation{m_world};

  // Which faction this client is. Session identity, which nothing below the composition root can
  // know: it decides what the overview colors green, what may be selected, and what counts as a
  // contact. The day a login exists it arrives with the session and only this line changes.
  Game::FactionId m_ownFaction = Game::FACTION_PLAYER;

  Neuron::ServerHost m_host;
  Neuron::LoopbackTransport m_serverLink;
  Neuron::LoopbackTransport m_clientLink;

  // Presentation.
  WorldView m_view;
  EventLog m_log;
  Hud m_hud;
  Neuron::FrameClock m_clock;

  // Debug: 1, 2 and 3 slow, restore and speed up the simulation without touching the frame rate;
  // F1 shows the readout, F3 shakes the camera, F4 despawns the selection so the explosion has
  // something to consume.
  float m_timeScale = 1.0f;
  bool m_showDebug = false;
};
} // namespace Outpost
