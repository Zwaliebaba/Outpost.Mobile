#pragma once

#include "EventLog.h"

#include "World.h"

#include "Camera.h"
#include "MeshLibrary.h"
#include "PointerTracker.h"
#include "SceneRenderer.h"
#include "TextRenderer.h"

#include <vector>

namespace Outpost
{
// Everything the player sees and does that the simulation does not know about: which ships are
// selected, which one is under the pointer, how far a selection ring has sprung open, where the
// order markers are in their life, and what all of that draws as.
//
// It reads Game::World and never writes to it except through World::IssueMoveOrder. That one-way
// dependency is the whole design: the view can be rewritten, doubled for a second local player, or
// replaced by a headless stub, and the simulation does not notice. When the halves separate, this
// is the class that stops holding a World& and starts holding a snapshot buffer.
//
// It is also the PointerListener: the tracker knows how contacts behave, this knows what they mean.
class WorldView : public Neuron::PointerListener
{
public:
  // One ship's presentation state, parallel to Game::World's ships and indexed the same way.
  struct ShipView
  {
    Neuron::MeshHandle mesh = Neuron::INVALID_MESH;
    float restY = 0.0f;                              // lifts the hull so its lowest vertex rests on the ground
    DirectX::XMFLOAT3 pickCentre{0.0f, 0.0f, 0.0f};  // mesh bounds centre, in local space
    DirectX::XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f}; // mesh half-size about that centre
    std::vector<DirectX::XMFLOAT3> thrusterLocals;   // one point per exhaust nozzle

    // One ring buffer per exhaust, nozzle-major: nozzle n owns [n * TRAIL_SAMPLES, (n + 1) *
    // TRAIL_SAMPLES), newest at trailHead. Every nozzle is sampled on the same tick, so the head
    // and the count are shared rather than stored per nozzle.
    std::vector<DirectX::XMFLOAT3> trail;
    int trailCount = 0;
    int trailHead = 0;

    bool selected = false;
    float ringFade = 0.0f;  // 0..1 alpha ramp on select and deselect
    float ringScale = 0.0f; // chases the selected state through a spring, so it overshoots
    float ringScaleVel = 0.0f;
    float hoverAmount = 0.0f;
    float bankRad = 0.0f;
    float thrusterIntensity = 0.0f;
  };

  // One per move order, at the point that was tapped rather than one per ship.
  struct OrderMarker
  {
    DirectX::XMFLOAT3 posWorld{0.0f, 0.0f, 0.0f};
    float facingRad = 0.0f;
    bool hasFacing = false;
    float ageSec = 0.0f;
  };

  void Init(Game::World& _world, Neuron::Camera& _camera, const Neuron::MeshLibrary& _meshes, Neuron::MeshHandle _quadMesh);

  // Adds the view state for a ship the world has just spawned. Indices stay in step with the
  // world's, which is what lets the two be read together with no lookup.
  void AddShip(Neuron::MeshHandle _mesh);

  // Called once per simulation tick, from the composition root. Trail sampling has to happen on the
  // tick rather than on the frame, or trail length would mean something different at every frame
  // rate -- but it needs mesh data the simulation must not see, so it lives here.
  void SampleTrails();

  // Rings, banking, thrusters, markers and camera framing. Real time, not sim time: none of it
  // feeds back into a tick, so it is free to run as fast as the swapchain does.
  void UpdateFeedback(float _dtSec);

  void Render(Neuron::SceneRenderer& _renderer, Neuron::GpuDevice& _gpu, Neuron::TextRenderer& _text, float _alpha);

  void ClearSelection() noexcept;
  void ClearHover() noexcept
  {
    m_hoverShip = -1;
  }
  void TriggerCameraShake() noexcept;
  [[nodiscard]] int SelectedCount() const noexcept;
  [[nodiscard]] bool IsSelected(size_t _index) const noexcept
  {
    return _index < m_ships.size() && m_ships[_index].selected;
  }

  // Control groups: a remembered selection under a number. Assigning with nothing selected clears
  // the group. The active group is the one the current selection was last taken from, and it stops
  // being active the moment the selection is changed by any other means.
  static constexpr int CONTROL_GROUPS = 5;
  void AssignGroup(int _group);
  void SelectGroup(int _group);
  [[nodiscard]] int GroupSize(int _group) const noexcept;
  [[nodiscard]] int ActiveGroup() const noexcept
  {
    return m_activeGroup;
  }

  // Where the view reports what the player did. Optional: with no log nothing is reported.
  void SetEventLog(EventLog& _log) noexcept
  {
    m_log = &_log;
  }

  // PointerListener.
  [[nodiscard]] bool WantsBoxSelect(bool _shiftHeld) override;
  void OnHover(float _xPx, float _yPx) override;
  void OnDragUpdate(bool _boxSelect, float _x0Px, float _y0Px, float _x1Px, float _y1Px) override;
  void OnDragCancelled() override;
  void OnBoxSelect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, bool _additive) override;
  void OnOrderDrag(float _x0Px, float _y0Px, float _x1Px, float _y1Px) override;
  void OnTap(float _xPx, float _yPx, bool _shiftHeld, bool _doubleTap) override;

  // The tracker needs telling when a tap hit a hull, so the next ground tap does not pair with it.
  void SetTracker(Neuron::PointerTracker& _tracker) noexcept
  {
    m_tracker = &_tracker;
  }

private:
  void DrawFeedback(Neuron::SceneRenderer& _renderer, Neuron::GpuDevice& _gpu, float _alpha);
  [[nodiscard]] int PickShip(float _xPx, float _yPx) const;
  void IssueMoveOrder(const DirectX::XMFLOAT3& _point, bool _hasFacing, float _facingRad);
  [[nodiscard]] float SimTimeSec() const noexcept;

  Game::World* m_world = nullptr;
  Neuron::Camera* m_camera = nullptr;
  const Neuron::MeshLibrary* m_meshes = nullptr;
  Neuron::PointerTracker* m_tracker = nullptr;
  Neuron::MeshHandle m_quadMesh = Neuron::INVALID_MESH;

  std::vector<ShipView> m_ships;
  std::vector<OrderMarker> m_markers;
  std::vector<Game::ShipId> m_groups[CONTROL_GROUPS];
  int m_activeGroup = -1;
  EventLog* m_log = nullptr;

  int m_hoverShip = -1;
  bool m_boxActive = false;
  float m_boxX0Px = 0.0f, m_boxY0Px = 0.0f, m_boxX1Px = 0.0f, m_boxY1Px = 0.0f;
  bool m_orderDragActive = false;
  float m_orderX0Px = 0.0f, m_orderY0Px = 0.0f, m_orderX1Px = 0.0f, m_orderY1Px = 0.0f;
};
} // namespace Outpost
