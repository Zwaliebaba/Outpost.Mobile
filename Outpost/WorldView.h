#pragma once

#include "EventLog.h"
#include "ShipExplosion.h"

#include "Formation.h"
#include "HullSpec.h"
#include "WorldSnapshot.h"

#include "Transport.h"

#include "BodyRenderer.h"
#include "Camera.h"
#include "FxRenderer.h"
#include "FxVertex.h"
#include "MeshLibrary.h"
#include "PointerTracker.h"
#include "SceneRenderer.h"
#include "SpriteParticles.h"
#include "TextRenderer.h"

#include "Pcg32.h"

#include <array>
#include <span>
#include <vector>

namespace Outpost
{
// Everything the player sees and does that the simulation does not know about: which ships are
// selected, which one is under the pointer, how far a selection ring has sprung open, where the
// order markers are in their life, and what all of that draws as.
//
// It reads the snapshots that arrive over a Transport and never writes to the simulation except by
// sending a move order back up the same wire. It cannot do otherwise: this header no longer
// includes World.h, so the seam is structural rather than a convention (slice-2b 2.6). That one-way
// dependency is the whole design: the view can be rewritten, doubled for a second local player, or
// replaced by a headless stub, and the simulation does not notice. This is the class that used to
// hold a World& and now holds a snapshot buffer; the day the transport becomes a socket, nothing
// here changes at all.
//
// It is also the PointerListener: the tracker knows how contacts behave, this knows what they mean.
class WorldView : public Neuron::PointerListener
{
public:
  // Where a ship was on one tick the wire told us about, and how it was moving then. Two of these
  // bracket the display time and the pose is read between them; velocity is per tick, derived from
  // the record's own prevPos so nothing has to be remembered across updates to get it.
  struct MotionSample
  {
    float tick = 0.0f;
    Game::WorldPos pos;
    float headingRad = 0.0f;
    float velX = 0.0f; // metres per tick
    float velZ = 0.0f;
    float turnRadPerTick = 0.0f;
  };

  // A position and heading as the player sees them: interpolated, not the latest record.
  struct DisplayPose
  {
    Game::WorldPos pos;
    float headingRad = 0.0f;
  };

  // One planet or asteroid on screen. It is presentation and nothing else: it holds no simulation
  // handle, appears in no snapshot, and a ship flies straight through it (Design/Decisions/0016).
  //
  // The centre is a Game::WorldPos and goes through ViewX/ViewZ like a ship's, so the day the camera
  // rebase lands (WorldView.h's m_viewOrigin comment) a body moves with everything else for free.
  struct BodyView
  {
    Neuron::BodyHandle terrain = Neuron::INVALID_BODY;
    Neuron::MeshHandle ocean = Neuron::INVALID_MESH; // slice 5 fills it
    Game::WorldPos centre;
    float centreY = 0.0f;
    DirectX::XMFLOAT3 spinAxis{0.0f, 1.0f, 0.0f};
    float spinRadPerSec = 0.0f;
    float spinRad = 0.0f;
    // An accumulated orientation rather than three angles, and stored as a matrix, exactly as the
    // explosion's Tumbler is: two angular velocities composed step by step do not decompose into
    // Euler angles that can be integrated separately.
    DirectX::XMFLOAT3X3 tumble{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 tumbleRadPerSec{0.0f, 0.0f, 0.0f};
    std::uint32_t triangleCount = 0; // for the F1 readout only
  };

  // One ship's presentation state, parallel to the latest snapshot's ships and indexed the same
  // way. ApplySnapshot is what keeps that true across a snapshot in which ships changed places.
  struct ShipView
  {
    Neuron::MeshHandle mesh = Neuron::INVALID_MESH;

    // The last two distinct records for this ship. Snapshots arrive at a fraction of the tick rate
    // and a record may be skipped by priority, so these are stamped with the tick they describe
    // and can be any number of ticks apart.
    MotionSample from;
    MotionSample to;
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

    // What Render last drew this hull with, and what it was carrying. The explosion needs both at
    // the moment the ship leaves the snapshot, when nothing else remembers where it was: by then
    // the new snapshot has no record for it and this ShipView is about to be discarded.
    DirectX::XMFLOAT4X4 lastWorld{};
    DirectX::XMFLOAT3 lastVelMetresPerSec{0.0f, 0.0f, 0.0f};
    bool drawn = false; // false until the first Render; a ship that vanishes before one does not explode

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

  void Init(Neuron::Transport& _transport, Neuron::Camera& _camera, const Neuron::MeshLibrary& _meshes, Neuron::MeshHandle _quadMesh);

  // Which mesh a hull is drawn with. The view no longer learns about a ship when the world spawns
  // one -- it learns from a snapshot, which carries a hullId and not a mesh -- so the composition
  // root registers the table up front and the view resolves against it as ships appear.
  void RegisterHullMesh(Game::HullId _hull, Neuron::MeshHandle _mesh);

  // Which faction this client is, which is session identity no library can know: the composition
  // root supplies it. What it decides here is what may be selected -- and therefore what may be
  // ordered, since an order carries the selection. The simulation refuses somebody else's ship
  // anyway (Design/Hostiles.md 4.3); this is the affordance telling the same truth.
  void SetOwnFaction(Game::FactionId _faction) noexcept
  {
    m_ownFaction = _faction;
  }

  // Drains the transport and applies whatever snapshots arrived. Called once per tick from the
  // composition root, before anything reads Ships().
  void PumpNetwork();

  // The world as this half is allowed to see it: the newest complete snapshot, or nothing before
  // the first arrives. Presentation state in m_ships is kept parallel to it.
  [[nodiscard]] std::span<const Game::ShipSnapshot> Ships() const noexcept;

  // The moment the view shows, in ticks of the host clock with the fraction of the current tick
  // included. The view draws INTERP_DELAY_TICKS behind it, so that the samples on both sides of
  // what it draws have normally arrived; set by the composition root before anything below reads
  // a pose, which is every frame and every tick.
  void SetDisplayTime(float _tickTime) noexcept;

  // Where ship i is drawn right now: read between the two samples that bracket the display time,
  // carried on past the newer one for a bounded while when the wire has not caught up.
  [[nodiscard]] DisplayPose DisplayedPose(std::size_t _index) const noexcept;

  // Called once per simulation tick, from the composition root. Trail sampling has to happen on the
  // tick rather than on the frame, or trail length would mean something different at every frame
  // rate -- but it needs mesh data the simulation must not see, so it lives here.
  void SampleTrails();

  // Rings, banking, thrusters, markers and camera framing. Real time, not sim time: none of it
  // feeds back into a tick, so it is free to run as fast as the swapchain does.
  void UpdateFeedback(float _dtSec);

  void Render(Neuron::SceneRenderer& _renderer, Neuron::GpuDevice& _gpu, Neuron::TextRenderer& _text);

  void ClearSelection() noexcept;
  void ClearHover() noexcept
  {
    m_hoverShip = -1;
  }
  void TriggerCameraShake() noexcept;
  [[nodiscard]] int SelectedCount() const noexcept;
  // Simulation positions reach render space through these two and never by reading localX, which is
  // an offset inside a sector and not a world coordinate. m_viewOrigin is the universe origin, so
  // the result is a true world metre for any sector within float range. Moving the origin to follow
  // the camera is what buys precision far from it; that is a rendering slice of its own and this is
  // the seam it will change (Design/Archive/Collision-slice-8.md 2.7, 3).
  [[nodiscard]] float ViewX(const Game::WorldPos& _pos) const noexcept
  {
    return Game::OffsetX(m_viewOrigin, _pos);
  }

  [[nodiscard]] float ViewZ(const Game::WorldPos& _pos) const noexcept
  {
    return Game::OffsetZ(m_viewOrigin, _pos);
  }

  // The inverse: a point picked in render space, back to the position the simulation will store.
  [[nodiscard]] Game::WorldPos WorldPosAt(float _viewX, float _viewZ) const noexcept
  {
    Game::WorldPos pos = m_viewOrigin;
    Game::Translate(pos, _viewX, _viewZ);
    return pos;
  }

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

  // The explosion's draw path. Optional, like the log: with no renderer the effect is simulated and
  // never drawn, which is what a boot with a missing texture leaves.
  void SetFxRenderer(Neuron::FxRenderer& _fx) noexcept
  {
    m_fx = &_fx;
  }

  // The bodies' draw path. Optional, like the log and the effect: with no renderer the scene has no
  // planets in it and everything else is unchanged.
  void SetBodyRenderer(Neuron::BodyRenderer& _bodies) noexcept
  {
    m_bodyRenderer = &_bodies;
  }

  // The composition root generates and uploads a body, then hands the view what it needs to place
  // and turn it. The tumble starts at identity here rather than being asked for: a caller supplies
  // a rate, not an orientation.
  void AddBody(const BodyView& _body);

  // F5. The GPU buffers are **not** released -- BodyRenderer keeps every handle for the run -- so a
  // reseed costs the memory of the bodies it replaces. Stated where it is done rather than left for
  // somebody to find in a memory graph; a ReleaseBody is a slice of its own the day it matters.
  void ClearBodies() noexcept;

  [[nodiscard]] std::size_t BodyCount() const noexcept
  {
    return m_bodies.size();
  }

  // For the F1 readout: what the bodies on screen cost.
  [[nodiscard]] std::uint32_t BodyTriangleCount() const noexcept
  {
    return m_bodyTriangles;
  }

  // For the debug readout: how much of the effect is live right now.
  [[nodiscard]] int ExplosionCount() const noexcept
  {
    return static_cast<int>(m_explosions.size());
  }
  [[nodiscard]] const Neuron::SpriteParticles& Particles() const noexcept
  {
    return m_particles;
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
  void DrawFeedback(Neuron::SceneRenderer& _renderer, Neuron::GpuDevice& _gpu);
  [[nodiscard]] int PickShip(float _xPx, float _yPx) const;

  // Whether record _index is one this client may take hold of. Every selection path goes through it:
  // PickShip for taps and hovers, OnBoxSelect for a band, and SelectGroup -- which needs it despite
  // only ever recalling what was selected, because a group holds indices and a later snapshot can
  // put a different ship at one.
  [[nodiscard]] bool IsOwn(std::size_t _index) const noexcept;
  [[nodiscard]] static MotionSample SampleOf(const Game::ShipSnapshot& _ship, std::uint64_t _tick) noexcept;
  void IssueMoveOrder(const DirectX::XMFLOAT3& _point, bool _hasFacing, float _facingRad);
  [[nodiscard]] float SimTimeSec() const noexcept;

  // Carries per-ship presentation state onto a new snapshot by handle, so a ship that changed array
  // index -- which despawn does, by swap-and-pop -- keeps its selection, its rings and its trails
  // instead of inheriting a stranger's (ADR 0005; Design/Archive/Collision-slice-2b.md 5.3).
  void ApplySnapshot();

  // The other half of the carry: whatever ApplySnapshot did not match is a ship that vanished, and
  // this is what it does about it.
  void ExplodeTheLost(std::uint64_t _tick);

  Game::WorldPos m_viewOrigin;
  Game::FactionId m_ownFaction = Game::FACTION_PLAYER;
  Neuron::Transport* m_transport = nullptr;
  Game::SnapshotReceiver m_receiver;
  float m_displayTick = 0.0f; // host tick time less INTERP_DELAY_TICKS

  // Parallel to the snapshot's ships, and to m_ships.
  std::vector<Game::ShipHandle> m_handles;
  std::vector<Game::WorldPos> m_orderPositions; // gathered for FormationHeading when an order is sent
  std::vector<ShipView> m_carryScratch;
  std::vector<Game::ShipHandle> m_carryHandles;

  // Indexed by Game::HullId. A hull with no mesh registered simply is not drawn, which is the same
  // diagnostic-not-a-crash treatment a missing mesh already got at boot.
  std::array<Neuron::MeshHandle, Game::HULL_COUNT> m_hullMeshes{};
  Neuron::Camera* m_camera = nullptr;
  const Neuron::MeshLibrary* m_meshes = nullptr;
  Neuron::PointerTracker* m_tracker = nullptr;
  Neuron::MeshHandle m_quadMesh = Neuron::INVALID_MESH;

  std::vector<ShipView> m_ships;
  std::vector<OrderMarker> m_markers;

  // The explosion. Its particles are one pool shared by every death on screen -- what overflows is
  // smoke, which is the right thing to lose -- and the shatters are per dead ship because each one
  // holds a hull's worth of fragments.
  Neuron::FxRenderer* m_fx = nullptr;
  std::vector<ShipExplosion> m_explosions;
  Neuron::SpriteParticles m_particles;
  std::vector<Neuron::FxVertex> m_fxFragmentVerts;
  std::vector<Neuron::FxVertex> m_fxSpriteVerts;
  // Smoke is shed once a frame rather than once per death, so it cannot use a per-explosion seed
  // and does not need one: what a replay wants reproducible is the shatter, and that is seeded from
  // the ship's handle and the tick it died on.
  Neuron::Pcg32 m_fxRng;

  // The bodies. Presentation only: nothing in GameLogic knows one exists and nothing on the wire
  // carries one (Design/Decisions/0016). m_bodyWorlds is a scratch vector rather than a local so
  // that the two draw passes over the same bodies allocate nothing per frame.
  Neuron::BodyRenderer* m_bodyRenderer = nullptr;
  std::vector<BodyView> m_bodies;
  std::vector<DirectX::XMFLOAT4X4> m_bodyWorlds;
  std::uint32_t m_bodyTriangles = 0;

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
