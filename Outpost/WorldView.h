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
#include "GlowBillboards.h"
#include "MeshLibrary.h"
#include "PointerTracker.h"
#include "RenderTypes.h"
#include "SceneRenderer.h"
#include "SkyRenderer.h"
#include "SpriteParticles.h"
#include "TextRenderer.h"
#include "ViewCulling.h"

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
    // Three grids of the same body, finest first; the level drawn is chosen per frame from the
    // projected radius (Design/Archive/BodyLod-work-order.md 2.2). A level that failed to bake holds
    // INVALID_BODY and the finest one that exists is drawn instead.
    static constexpr std::uint32_t LOD_COUNT = 3;
    Neuron::BodyHandle terrainLod[LOD_COUNT] = {Neuron::INVALID_BODY, Neuron::INVALID_BODY, Neuron::INVALID_BODY};
    std::uint32_t triangleCountLod[LOD_COUNT] = {};
    // Drawn through BodyRenderer::DrawPlanet and given no outline pass: a smooth sphere wearing a
    // map rather than a generated height field (ViewTuning.h, BODY_PLANET_TEXTURED).
    bool textured = false;
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
    // The sphere the frustum test uses: the body's radius stretched by its longest ellipsoid axis
    // and raised by its tallest terrain. Both are fractions of the radius in BodyDesc, and both are
    // known on either bake path, so this is the extent rather than a guess at it.
    float boundingRadiusMetres = 0.0f;
  };

  // One authored exhaust nozzle: where it is on the hull, what colour it burns and how wide.
  struct ExhaustView
  {
    DirectX::XMFLOAT3 local{0.0f, 0.0f, 0.0f}; // nozzle position in mesh space
    Neuron::Rgba colour{1.0f, 1.0f, 1.0f, 1.0f};
    float radiusMetres = 0.0f; // the marker's scale, already in metres
    // The plume is livery where its author said so (Design/Archive/NmoFormat.md 5.10). Carried here so the
    // livery slice is one multiply rather than a second walk of the markers.
    bool raceTinted = false;
  };

  // One authored running light. periodSec of 0 is a steady light, which is what most of them are.
  struct NavLightView
  {
    DirectX::XMFLOAT3 local{0.0f, 0.0f, 0.0f};
    Neuron::Rgba colour{1.0f, 1.0f, 1.0f, 1.0f};
    float radiusMetres = 0.0f; // the marker's scale, already in metres
    float periodSec = 0.0f;    // the marker's param0, clamped at load
    float phase = 0.0f;        // the marker's param1, a fraction of the period
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
    // Whether the last Render submitted this hull. Set by the frustum test there and read by the
    // plume, so a trail is not built for a ship nobody can see.
    bool visible = true;
    // One entry per Exhaust marker on the hull, copied out of MeshData here rather than looked up
    // in MeshLibrary per frame: the draw loop runs per billboard, and restY, pickCentre and
    // halfExtents above are copied for exactly the same reason.
    std::vector<ExhaustView> exhausts;
    std::vector<NavLightView> navLights;

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
    Neuron::Rgba lastLivery{1.0f, 1.0f, 1.0f, 1.0f}; // so the debris wears the paint the hull did
    bool drawn = false;                              // false until the first Render; a ship that vanishes before one does not explode

    // Whose the record was, kept past the record: a hull that docked is counted for the log only
    // when it was the player's, and by then the snapshot no longer holds it.
    Game::FactionId faction = Game::FACTION_PLAYER;

    bool selected = false;
    float ringFade = 0.0f;  // 0..1 alpha ramp on select and deselect
    float ringScale = 0.0f; // chases the selected state through a spring, so it overshoots
    float ringScaleVel = 0.0f;
    float hoverAmount = 0.0f;
    float bankRad = 0.0f;
    float thrusterIntensity = 0.0f;
  };

  // A station of the layout, for the minimap: where it is and whose it is. Static content handed in
  // at boot the way body placements are, not a record -- it exists from the first frame however far
  // away the station is, which is what "static so can be marked" bought, and nothing on the wire
  // carries one (Design/Archive/Stations.md 9.3).
  struct StationMark
  {
    Game::WorldPos posWorld;
    Game::FactionId faction = Game::FACTION_VANGUARD;
  };

  // One per order, at the point that was tapped rather than one per ship. A move order's is the
  // marker colour; a dock order's flashes in the station's faction colour, so the tap visibly landed
  // on the thing and not the ground beside it (Design/Archive/Stations.md 9.2).
  struct OrderMarker
  {
    DirectX::XMFLOAT3 posWorld{0.0f, 0.0f, 0.0f};
    float facingRad = 0.0f;
    bool hasFacing = false;
    float ageSec = 0.0f;
    Neuron::Rgba colour{1.0f, 1.0f, 1.0f, 1.0f};
  };

  void Init(Neuron::Transport& _transport, Neuron::Camera& _camera, const Neuron::MeshLibrary& _meshes, Neuron::MeshHandle _quadMesh);

  // Which mesh a hull is drawn with. The view no longer learns about a ship when the world spawns
  // one -- it learns from a snapshot, which carries a hullId and not a mesh -- so the composition
  // root registers the table up front and the view resolves against it as ships appear.
  void RegisterHullMesh(Game::HullId _hull, Neuron::MeshHandle _mesh);

  // Which faction this client is, which is session identity no library can know: the composition
  // root supplies it. What it decides here is what may be selected -- and therefore what may be
  // ordered, since an order carries the selection. The simulation refuses somebody else's ship
  // anyway (Design/Archive/Hostiles.md 4.3); this is the affordance telling the same truth.
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

  // Fleets: the unit of command, and the only thing that can be selected (ADR 0049).
  //
  // A selection is a set of SLOTS and nothing else -- five bools, not a list of ships. That is what
  // replaced the control groups, and it is a smaller thing rather than a renamed one: a group was a
  // remembered list this half had to keep in step with a world it could not see, and a slot is a
  // number the server states the membership of on every change. Nothing here can hold a stale ship.
  //
  // Sub-fleet selection does not exist, on purpose: the day a ship must leave a fleet, the fleet
  // docks and the station screen is where it happens (Design/Fleets.md 15, decision 1).
  static constexpr int FLEET_SLOTS = static_cast<int>(Game::FLEET_SLOTS);

  // Selects one slot, or toggles it in or out when _additive. A slot the server does not hold is
  // ignored rather than selected empty.
  void SelectFleet(int _slot, bool _additive);

  [[nodiscard]] bool IsFleetSelected(int _slot) const noexcept
  {
    return _slot >= 0 && _slot < FLEET_SLOTS && m_fleetSelected[_slot];
  }

  // Whether the server says this slot is held, which is the status block's mask and NOT an empty
  // roster: a composed fleet has a live slot and nobody in space, and the mask rides every update
  // where a roster is stated once (Design/Fleets.md 8.1's amendment).
  [[nodiscard]] bool IsFleetHeld(int _slot) const noexcept
  {
    return _slot >= 0 && _slot < FLEET_SLOTS && (m_receiver.FleetMask() & (1u << _slot)) != 0;
  }

  // Members in space plus manifest -- the fleet's composed size, so a button says eight from the
  // moment the fleet exists rather than climbing as the hulls launch. How many are actually OUT is
  // the roster's own size, which the sheet will need for "LAUNCHING 4 OF 8" and nothing needs yet.
  [[nodiscard]] int FleetCount(int _slot) const noexcept
  {
    return IsFleetHeld(_slot) ? static_cast<int>(m_receiver.FleetStatusOf(static_cast<std::uint8_t>(_slot)).count) : 0;
  }

  // Bits 0-2 the kind shown, bit 6 engaged, bit 7 under attack (Design/Fleets.md 8.2).
  [[nodiscard]] std::uint8_t FleetStatusBits(int _slot) const noexcept
  {
    return IsFleetHeld(_slot) ? m_receiver.FleetStatusOf(static_cast<std::uint8_t>(_slot)).status : std::uint8_t{0};
  }

  [[nodiscard]] bool IsFleetUnderAttack(int _slot) const noexcept
  {
    return (FleetStatusBits(_slot) & Game::FLEET_STATUS_UNDER_ATTACK) != 0;
  }

  // Where the server says the fleet is: the centroid of its live members, or its launch station
  // while none is out. A readout, derived at publish time and simulated by nobody (ADR 0051's
  // neighbour argument, Design/Fleets.md 8.2).
  [[nodiscard]] Game::WorldPos FleetPosition(int _slot) const noexcept
  {
    return IsFleetHeld(_slot) ? m_receiver.FleetStatusOf(static_cast<std::uint8_t>(_slot)).position : m_viewOrigin;
  }

  [[nodiscard]] int SelectedFleetCount() const noexcept;

  // The lowest selected slot, or -1. What the bottom bar names when exactly one fleet is selected.
  [[nodiscard]] int FirstSelectedFleet() const noexcept;

  // Flies the camera to a slot's stated position, and keeps re-reading it while it flies: a fleet
  // is moving, and a goal fixed at the tap lands where it used to be. Ends on arrival, or the
  // moment the player takes the camera back (Design/Fleets.md 9.1).
  void FocusFleet(int _slot);
  void UpdateFocus(float _dtSec);

  // Gives the camera back. Called from the world's own gestures, and from the composition root when
  // it sees the player pan -- a pan reaches Camera directly and never touches this half, so the
  // root is the only thing that can notice one (OutpostApp::Update).
  void CancelFocus() noexcept
  {
    m_focusSlot = -1;
  }

  // What pressing one of the five buttons means. The HUD knows where a contact landed; this knows
  // what a fleet slot is, which is the division Hud.h's own comment already draws.
  //
  // A tap on a held slot selects it AND flies to it -- one gesture, because under decision 1
  // selecting a fleet is attending to it. A hold opens the sheet, which is slice 8; until then it
  // logs the line the sheet's header will carry, so the gesture is discoverable and says something
  // true. An empty slot is inert to a tap and answers a hold with the one thing there is to say
  // (Design/Fleets.md 9.1).
  void PressFleetButton(int _slot, bool _longPress);

  // What a fleet is doing, as the one word the sheet's header shows and the log stub prints.
  // Decoded from the status byte's low three bits (Design/Fleets.md 8.2, 9.3).
  [[nodiscard]] const char* FleetActivity(int _slot) const noexcept;

  // Where the view reports what the player did. Optional: with no log nothing is reported.
  void SetEventLog(EventLog& _log) noexcept
  {
    m_log = &_log;
  }

  // What a faction is called, indexed by FactionId, for the refusal line. Content, so the
  // composition root supplies it as it supplies the HUD's; a faction past the end is unnamed.
  void SetFactionNames(std::span<const char* const> _names) noexcept
  {
    m_factionNames = _names;
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

  // The sky's draw path. Optional, like the rest: with no renderer the scene is drawn onto the clear
  // colour and nothing else changes. The frame numbers are the composition root's, from ViewTuning,
  // because where the sphere sits and how hard a star scintillates are the game's choices and not the
  // engine's.
  void SetSkyRenderer(Neuron::SkyRenderer& _sky, const Neuron::SkyRenderer::Frame& _tuning) noexcept
  {
    m_sky = &_sky;
    m_skyTuning = _tuning;
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

  // The marks, from the composition root at boot. Nothing removes one: stations do not despawn this
  // phase, and a mark is content rather than state.
  void AddStationMark(const StationMark& _mark)
  {
    m_stationMarks.push_back(_mark);
  }

  [[nodiscard]] std::span<const StationMark> StationMarks() const noexcept
  {
    return m_stationMarks;
  }

  // Whether _faction holds this client hostile, as of the last update header that arrived. The one
  // reading of the mask in the executable: the livery table, the overview and the contact count all
  // ask this rather than inferring a relation from an identity (Design/Archive/Stations.md 4.3).
  [[nodiscard]] bool IsHostileToMe(Game::FactionId _faction) const noexcept
  {
    return m_receiver.IsHostileToMe(_faction);
  }

  // For the F1 readout: what the bodies on screen cost *last frame* -- the levels actually chosen
  // and drawn, not the sum of everything resident, which tripled when every body gained three
  // grids (Design/Archive/BodyLod-work-order.md 2.1).
  [[nodiscard]] std::uint32_t BodyTriangleCount() const noexcept
  {
    return m_bodyTriangles;
  }

  // For the debug readout: how much of the effect is live right now.
  // For the debug readout: what frustum culling kept and what it skipped, last frame.
  [[nodiscard]] std::uint32_t SubmittedCount() const noexcept
  {
    return m_submittedCount;
  }
  [[nodiscard]] std::uint32_t CulledCount() const noexcept
  {
    return m_culledCount;
  }

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
  // Takes the frame because the thruster plume it ends with goes through the effect pass, and
  // FxRenderer::Begin wants the lighting the scene was drawn under. The decals themselves do not
  // need it -- they are shaped entirely in the pixel shader from root constants.
  void DrawFeedback(Neuron::SceneRenderer& _renderer, Neuron::GpuDevice& _gpu, const Neuron::SceneFrame& _frame);

  // Where a screen ray meets record _index's hull, as a distance along the ray, or a negative
  // number for a miss. Against the oriented box of the hull as drawn. The two pickers below are two
  // filters over it and nothing else, so they cannot disagree about what a hull is.
  [[nodiscard]] float RayHitDistance(std::size_t _index, const DirectX::XMFLOAT3& _origin,
                                     const DirectX::XMFLOAT3& _direction) const noexcept;

  // Own hulls only: what may be selected, and therefore ordered.
  [[nodiscard]] int PickShip(float _xPx, float _yPx) const;

  // Records whose flag says station, of any faction. Consulted from exactly one place -- a tap with
  // a non-empty selection -- because a station is a place to send ships and not a thing to hold
  // (Design/Archive/Stations.md 9.1).
  [[nodiscard]] int PickStation(float _xPx, float _yPx) const;

  // Sends the selection to dock at record _station, or refuses before sending: the affordance
  // tells the truth first, and the simulation's gate stands behind it (Design/Archive/Stations.md 9.2).
  void IssueDockOrder(std::size_t _station);

  // Whether record _index is one this client may take hold of. Both pick paths go through it:
  // PickShip for taps and hovers, and OnBoxSelect for a band. What a pick then selects is the
  // record's FLEET, which FleetSlotOf answers.
  [[nodiscard]] bool IsOwn(std::size_t _index) const noexcept;

  // Whose paint a hull wears. Hostile outranks faction, which is the precedence Design/Archive/Stations.md
  // 9.3 sets and this slice does not get to re-litigate: a Vanguard ship whose faction holds this
  // client hostile paints the Vandals' red, because the law turning on you is the thing the player
  // must see.
  [[nodiscard]] static Neuron::Rgba LiveryOf(Game::FactionId _faction, bool _own, bool _hostileToMe) noexcept;

  // Which slot a record belongs to, by walking the five rosters, or -1.
  //
  // A record with no slot is not an error and must not be coded as one: a roster is stated on the
  // tick membership changed and a launched hull can reach this half one update before its roster
  // does, so "no fleet yet" is what a launch looks like from here.
  [[nodiscard]] int FleetSlotOf(Game::EntityId _entity) const noexcept;

  // Sets every record's `selected` flag from whether its entity is in a selected slot's roster.
  //
  // Derived rather than carried, and that is the whole shape of fleet-grain selection: the flag
  // stays exactly where the rings, the minimap, the bottom bar and the debug keys already read it,
  // and none of them learns what a fleet is. Carrying it instead would leave a hull launched into a
  // selected fleet unringed until something else touched the selection.
  void RefreshSelection() noexcept;
  [[nodiscard]] static MotionSample SampleOf(const Game::ShipSnapshot& _ship, std::uint64_t _tick) noexcept;

  // A point authored on the hull, in mesh space, to where it is drawn. The plume's trail sampler
  // and the navigation lights both need it and must not drift apart, so it is written once. Out of
  // line because it scales by ViewTuning.h's constants, and this header has no business seeing them.
  //
  // It takes the ShipView and not only the pose because the hull is drawn through more than a
  // heading: Render rolls it by bankRad about its own mid-height axis, and a point carried by the
  // heading alone stays where the unbanked hull would have been -- which is where the running lights
  // sat, a wing's width off the hull, through every turn. The one transform lives in HullMatrix.
  [[nodiscard]] DirectX::XMFLOAT3 HullPointToWorld(const ShipView& _view, const DisplayPose& _pose,
                                                   const DirectX::XMFLOAT3& _local) const noexcept;

  // The matrix a hull is drawn with, from its presentation state and its displayed pose. Written
  // once and used by Render and by HullPointToWorld, so the mesh and every point authored on it
  // cannot disagree about where the hull is.
  [[nodiscard]] DirectX::XMMATRIX HullMatrix(const ShipView& _view, const DisplayPose& _pose) const noexcept;
  void IssueMoveOrder(const DirectX::XMFLOAT3& _point, bool _hasFacing, float _facingRad);

  // Sends record _target's entity as an Attack to every selected fleet. The third tap meaning, and
  // the one slice 6 adds: ground moves, a station docks, a hostile record attacks
  // (Design/Fleets.md 9.3).
  void IssueAttackOrder(std::size_t _target);

  // One FleetOrder per selected slot, with everything but the kind's own fields already filled.
  // Five messages at the very most, each of fixed size, against an order budget of eight -- which
  // is what an order that names a fleet instead of its ships buys (ADR 0049).
  //
  // Returns how many were sent, so a caller can leave the marker and the log line to a send that
  // actually happened.
  std::uint32_t SendToSelectedFleets(const Game::FleetOrder& _order);

  // Records whose faction holds this client hostile. PickStation's shape and its reason: consulted
  // from one place only, a tap with a non-empty selection.
  [[nodiscard]] int PickHostile(float _xPx, float _yPx) const;
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

  // Parallel to the snapshot's ships, and to m_ships. Identities rather than handles since ADR 0047:
  // a handle is a reference into one World and this half has never held one.
  std::vector<Game::EntityId> m_entities;
  std::vector<Game::WorldPos> m_orderPositions; // gathered for FormationHeading when an order is sent
  std::vector<ShipView> m_carryScratch;

  // One reliable message's worth, kept so a pump allocates nothing. Sized on first use rather than
  // at Init, because MAX_RELIABLE_BYTES is eight kilobytes and a view that never sees a departure
  // should not carry it.
  std::vector<std::uint8_t> m_reliableScratch;
  std::vector<Game::EntityId> m_carryEntities;

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

  // The thruster plume, gathered before it is turned into quads. Two vectors rather than one
  // because what the view decides (where each glow is, how big, how bright) and how a quad faces
  // the camera are different jobs, and only the second is worth a test.
  std::vector<Neuron::GlowSample> m_glowSamples;
  std::vector<Neuron::FxVertex> m_fxGlowVerts;

  // What the last Render submitted and what it skipped, so the saving is read off the screen rather
  // than inferred.
  std::uint32_t m_submittedCount = 0;
  std::uint32_t m_culledCount = 0;

  // Decided once per body per frame and reused by the terrain and outline passes, so the outline
  // always cages the mesh that is actually drawn.
  std::vector<bool> m_bodyVisible;
  std::vector<std::uint8_t> m_bodyLod;

  // The visible hulls, grouped by the mesh they draw with, so a fleet sharing a hull is one draw
  // (Design/Archive/MmoScalabilityReview.md G2). A plain vector rather than a map: there are ten meshes in
  // the game and a linear scan over ten beats a hash on every ship, and it keeps the draw order
  // stable across frames, which a hash does not.
  struct MeshBucket
  {
    Neuron::MeshHandle mesh = Neuron::INVALID_MESH;
    std::vector<Neuron::MeshInstance> instances;
  };
  std::vector<MeshBucket> m_meshBuckets;

  // The bucket for a mesh, made on first sight. Buckets are kept across frames and only their
  // contents cleared, so a steady fleet allocates nothing after the first frame.
  [[nodiscard]] std::vector<Neuron::MeshInstance>& Bucket(Neuron::MeshHandle _mesh);

  // This frame's frustum, built at the top of Render. Held rather than passed because DrawFeedback
  // is a second pass over the same frame and rebuilding it there would be a second chance for the
  // two to disagree about what is on screen.
  DirectX::BoundingFrustum m_frustum;
  // Smoke is shed once a frame rather than once per death, so it cannot use a per-explosion seed
  // and does not need one: what a replay wants reproducible is the shatter, and that is seeded from
  // the ship's handle and the tick it died on.
  Neuron::Pcg32 m_fxRng;

  // The sky. Presentation only, like the bodies, and further out than either: it is generated once,
  // uploaded once and drawn before everything. m_skyTimeSec runs on real time so that pausing the
  // simulation does not stop the stars twinkling -- a paused game with a frozen sky reads as a
  // hung one.
  Neuron::SkyRenderer* m_sky = nullptr;
  Neuron::SkyRenderer::Frame m_skyTuning;
  float m_skyTimeSec = 0.0f;

  // The navigation lights' clock, wrapped for the reason m_skyTimeSec's comment gives: a float
  // second counter left running loses enough precision after a few hours that consecutive frames
  // land on the same argument and every beacon freezes. It wraps at the longest period a marker may
  // legally carry, so the number an author could break is a named constant; what that costs is one
  // mistimed beat per wrap on a light whose period does not divide it (UpdateFeedback says why that
  // is the right trade).
  float m_navTimeSec = 0.0f;

  // The bodies. Presentation only: nothing in GameLogic knows one exists and nothing on the wire
  // carries one (Design/Decisions/0016). m_bodyWorlds is a scratch vector rather than a local so
  // that the two draw passes over the same bodies allocate nothing per frame.
  Neuron::BodyRenderer* m_bodyRenderer = nullptr;
  std::vector<BodyView> m_bodies;
  std::vector<DirectX::XMFLOAT4X4> m_bodyWorlds;
  std::uint32_t m_bodyTriangles = 0;

  std::vector<StationMark> m_stationMarks;

  // Which slots are selected. The whole of the selection: five bools against the five vectors of
  // remembered ids the control groups needed, because the server states the membership now.
  bool m_fleetSelected[FLEET_SLOTS] = {};

  // The slot the camera is flying to, or -1. Its position is re-read every frame rather than
  // captured at the tap, because the fleet is moving.
  int m_focusSlot = -1;

  EventLog* m_log = nullptr;
  std::span<const char* const> m_factionNames;

  int m_hoverShip = -1;
  bool m_boxActive = false;
  float m_boxX0Px = 0.0f, m_boxY0Px = 0.0f, m_boxX1Px = 0.0f, m_boxY1Px = 0.0f;
  bool m_orderDragActive = false;
  float m_orderX0Px = 0.0f, m_orderY0Px = 0.0f, m_orderX1Px = 0.0f, m_orderY1Px = 0.0f;
};
} // namespace Outpost
