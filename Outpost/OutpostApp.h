#pragma once

#include "Hud.h"
#include "WorldSimulation.h"
#include "WorldView.h"

#include "AppWindow.h"
#include "Camera.h"
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
  void SpawnStartingFleet();
  void Update();
  void Render();

  void OnResize(std::uint32_t _widthPx, std::uint32_t _heightPx);
  void OnKeyDown(std::uint32_t _virtualKey);

  // Window and device.
  Neuron::AppWindow m_window;
  Neuron::GpuDevice m_gpu;
  Neuron::SceneRenderer m_sceneRenderer;
  Neuron::TextRenderer m_textRenderer;
  Neuron::MeshLibrary m_meshes;

  // Input and framing.
  Neuron::Camera m_camera;
  Neuron::PointerTracker m_pointers;
  // Queued rather than acted on immediately: the frame loop drains them once the camera matrices
  // are current, so a pick tests against what was on screen when the contact happened.
  std::vector<Neuron::PointerEvent> m_pendingEvents;

  // Simulation, hosted.
  Game::World m_world;
  WorldSimulation m_simulation{m_world};
  Neuron::ServerHost m_host;

  // Presentation.
  WorldView m_view;
  Hud m_hud;
  Neuron::FrameClock m_clock;

  // Debug: 1, 2 and 3 slow, restore and speed up the simulation without touching the frame rate.
  float m_timeScale = 1.0f;
};
} // namespace Outpost
