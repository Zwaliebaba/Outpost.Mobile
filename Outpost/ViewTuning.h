#pragma once

#include "BodyRenderer.h"
#include "RenderTypes.h"

#include "SimTuning.h"

namespace Outpost
{
// Presentation tuning: everything that changes how the game looks and feels without changing what
// it does. The simulation's own numbers live in GameLogic/SimTuning.h and are a different kind of
// value -- one of these can be changed between two builds of the same match, one of those cannot.
//
// Nothing here is ever read by GameLogic. The starting-scene block below is the one part of this
// file a tick can be affected by, and only at one remove: it is boot content, which the composition
// root reads and passes into the world as spawn positions and patrol numbers. That is the only route
// any of this has into a tick, and a value on this side still changes no recorded game -- it changes
// which game gets recorded.

// --- camera ------------------------------------------------------------------------------------
inline constexpr float CAMERA_MIN_ZOOM = 40.0f;
inline constexpr float CAMERA_MAX_ZOOM = 900.0f;
inline constexpr float CAMERA_TARGET_HEIGHT = 3.0f;
inline constexpr float CAMERA_FOV_DEG = 45.0f;
inline constexpr float CAMERA_NEAR_PLANE = 0.5f;
inline constexpr float CAMERA_FAR_PLANE = 8000.0f;
inline constexpr float CAMERA_PAN_SPEED = 1.0f;
inline constexpr float CAMERA_FOLLOW_HALF_LIFE = 0.18f;
inline constexpr float CAMERA_LEAD_FACTOR = 0.35f;
inline constexpr float CAMERA_SHAKE_AMPLITUDE = 2.5f;
inline constexpr float CAMERA_SHAKE_DECAY_HALF_LIFE = 0.15f;
inline constexpr float CAMERA_SHAKE_FREQUENCY_HZ = 22.0f;
inline constexpr float CAMERA_ROTATE_SPEED_DEG_PER_PX = 0.35f;
inline constexpr float CAMERA_ZOOM_STEP_FACTOR = 1.12f;
inline constexpr float CAMERA_MIN_PITCH_DEG = 5.0f;
inline constexpr float CAMERA_MAX_PITCH_DEG = 89.0f;

// --- selection ---------------------------------------------------------------------------------
inline constexpr float SEL_RING_FADE_IN_MS = 140.0f;
inline constexpr float SEL_RING_FADE_OUT_MS = 180.0f;
inline constexpr float SEL_RING_SCALE_OVERSHOOT = 1.35f;
inline constexpr float SEL_OVERSHOOT_SETTLE_HALF_LIFE = 0.09f;
inline constexpr float SEL_HOVER_HIGHLIGHT_STRENGTH = 0.45f;
inline constexpr float SEL_HOVER_RING_ALPHA = 0.35f;
inline constexpr float SEL_HOVER_RESPONSE_HALF_LIFE = 0.06f;
inline constexpr float SEL_RING_THICKNESS = 0.16f;
inline constexpr float SEL_RING_RADIUS_SCALE = 1.25f;
inline constexpr Neuron::Rgba SEL_RING_COLOUR{0.35f, 0.95f, 0.55f, 0.9f};

// --- order markers -----------------------------------------------------------------------------
inline constexpr float MARKER_EXPAND_MS = 160.0f;
inline constexpr int MARKER_PULSE_COUNT = 3;
inline constexpr float MARKER_PULSE_PERIOD_MS = 320.0f;
inline constexpr float MARKER_LIFETIME_MS = 1400.0f;
inline constexpr float MARKER_FADE_OUT_MS = 260.0f;
inline constexpr float MARKER_RADIUS = 9.0f;
inline constexpr float MARKER_THICKNESS = 0.18f;
inline constexpr float MARKER_PULSE_SCALE = 0.18f;
inline constexpr Neuron::Rgba MARKER_COLOUR{0.95f, 0.78f, 0.28f, 0.9f};

// --- ship explosion ----------------------------------------------------------------------------
// The source values from Interstellar Outpost's Building::Destroy, read as metres and seconds 1:1
// and scaled per ship by max(halfExtents) / EXPLOSION_REFERENCE_HALF_SIZE
// (Design/SpaceshipExplosion.md 3). "scaled" below means multiplied by that at spawn: speeds and
// sizes scale, lifetimes, friction and counts do not.
//
// The values marked "was N" have been tuned away from the source, once the effect was first seen
// running. The source blows up a building standing on the ground, so its plume starts above the
// roof and climbs; a ship is a free body, and the same numbers put the whole effect in a column
// overhead with a fireball wide enough to hide the hull it came from. The two lifts are gone, the
// upward-only velocities now straddle zero, and the cores are half the size.
inline constexpr float EXPLOSION_REFERENCE_HALF_SIZE = 10.0f;
inline constexpr float EXPLOSION_INTENSITY = 100.0f; // Building::Destroy(_intensity)
inline constexpr int EXPLOSION_HULL_COPIES = 3;
inline constexpr float EXPLOSION_HULL_FRACTION = 1.0f;
inline constexpr float EXPLOSION_FRAGMENT_LIFETIME_SEC = 3.0f; // was 5: the shards hung about long after the fireball had gone
inline constexpr float EXPLOSION_FRAGMENT_RADIAL_SPEED = 3.0f;
inline constexpr float EXPLOSION_FRAGMENT_MAX_ANG_VEL = 4.0f;
inline constexpr float EXPLOSION_FRAGMENT_FRICTION = 0.05f;
inline constexpr float EXPLOSION_FRAGMENT_ROT_FRICTION = 0.2f;
inline constexpr float EXPLOSION_FRAGMENT_MIN_CIRCUMFERENCE = 6.0f; // scaled
inline constexpr int EXPLOSION_FRAGMENT_CAP = 500;
inline constexpr float EXPLOSION_CORE_SIZE_MIN = 60.0f;       // was 120; scaled; Bang's cores
inline constexpr float EXPLOSION_CORE_SIZE_RANGE = 60.0f;     // was 120; scaled
inline constexpr float EXPLOSION_CORE_SPEED_XZ = 30.0f;       // scaled; Signed(30) on X and Z
inline constexpr float EXPLOSION_CORE_SPEED_UP_MIN = -10.0f;  // was 10; scaled. Negative, so the cores no longer all climb
inline constexpr float EXPLOSION_CORE_SPEED_UP_RANGE = 20.0f; // was 10; scaled. With the min above, vertical speed is +-10
inline constexpr float EXPLOSION_CORE_LIFT = 0.0f;            // was 0.3 x range along up, which is 26 m over a Corvette
inline constexpr float EXPLOSION_EXTRA_CORE_SIZE = 50.0f;     // was 100; scaled; Destroy's own cores
inline constexpr float EXPLOSION_EXTRA_CORE_SPEED = 100.0f;   // scaled; Signed(100) on all axes
inline constexpr int EXPLOSION_DEBRIS_MIN = 2;
inline constexpr int EXPLOSION_DEBRIS_MAX = 30;
inline constexpr float EXPLOSION_DEBRIS_SPEED_MIN = 20.0f;   // scaled
inline constexpr float EXPLOSION_DEBRIS_SPEED_RANGE = 30.0f; // scaled
inline constexpr float EXPLOSION_DEBRIS_UP_MIN = -1.0f;      // was 2, which tilted the ring 63-76 degrees up into a cone
inline constexpr float EXPLOSION_DEBRIS_UP_RANGE = 2.0f;     // with the min above, the ring spreads +-45 degrees
inline constexpr float EXPLOSION_DEBRIS_SIZE_MIN = 20.0f;    // scaled
inline constexpr float EXPLOSION_DEBRIS_SIZE_RANGE = 20.0f;  // scaled
inline constexpr float EXPLOSION_DEBRIS_LIFT = 0.0f;         // was 0.2 x range along up, which is 17 m over a Corvette
inline constexpr bool EXPLOSION_PLUME_ALONG_UP = true;       // false: the ring is random on a sphere
inline constexpr std::uint32_t EXPLOSION_PARTICLE_CAPACITY = 4096;
// The shake is Camera::Shake(), which takes no amplitude: CAMERA_SHAKE_AMPLITUDE above is the only
// knob, so a Carrier and an Interceptor shake the camera by the same amount.

// --- shock ring --------------------------------------------------------------------------------
// The blast front a station leaves behind: one expanding ring on the ground plane, drawn through
// the same decal the selection rings and order markers use, so it costs no pipeline, no shader and
// no texture of its own (Design/ShockRing-work-order.md).
//
// Flat on purpose. A real front is a sphere; a flat ring is what every other ground marker in this
// game already is, and a sphere would want a pass of its own.
inline constexpr float SHOCK_RING_LIFETIME_SEC = 0.9f;
inline constexpr float SHOCK_RING_MAX_RADIUS = 90.0f;  // scaled by the dead hull's own scale, like the rest of the effect
inline constexpr float SHOCK_RING_WIDTH_METRES = 2.5f; // held in metres as the ring grows, so the front stays a front
inline constexpr Neuron::Rgba SHOCK_RING_COLOUR{1.0f, 0.86f, 0.62f, 0.6f};

// A station exists in the game now (the hostile base below), but nothing can destroy one -- there is
// no combat, and it cannot be selected, so F4 cannot reach it -- and so nothing sets
// ShipExplosion::Spawn::shockRing on its own account yet. Until something can kill a station, every
// death carries a ring, which is the only way the effect can be looked at. Whatever gives a station
// a lifecycle sets the flag from the station and deletes this.
inline constexpr bool SHOCK_RING_ON_EVERY_DEATH = true;

// --- banking and thrusters ---------------------------------------------------------------------
inline constexpr float BANK_MAX_ANGLE_DEG = 28.0f;
inline constexpr float BANK_RESPONSE_HALF_LIFE = 0.14f;
inline constexpr float BANK_RETURN_HALF_LIFE = 0.3f;
inline constexpr float THRUSTER_IDLE_INTENSITY = 0.12f;
inline constexpr float THRUSTER_MAX_INTENSITY = 1.0f;
inline constexpr float THRUSTER_RESPONSE_HALF_LIFE = 0.1f;
inline constexpr float THRUSTER_TRAIL_LENGTH = 18.0f;
inline constexpr float THRUSTER_TRAIL_FADE = 0.55f;
inline constexpr float THRUSTER_GLOW_RADIUS = 6.0f;
inline constexpr float THRUSTER_GLOW_FALLOFF = 2.2f;
inline constexpr int TRAIL_SAMPLES = 32; // half a second of history at the tick rate

// --- snapshot interpolation --------------------------------------------------------------------
// The server publishes at 1/INTEREST_UPDATE_EVERY_TICKS of the tick rate, so the view cannot draw
// the latest record and stay smooth: it draws the world as it stood INTERP_DELAY_TICKS ago, which
// is far enough back that the two samples either side of that moment have both arrived. One update
// interval is the minimum that holds for a ship refreshed on every update; a real wire adds its
// jitter to this.
//
// A ship the priority accumulator refreshes less often runs out of samples before the next one
// lands. It is carried on at its last velocity rather than frozen, for at most the longest gap the
// accumulator can leave a subscribed ship in -- past that it is held, since a straight line a
// second long is no longer a prediction.
inline constexpr float INTERP_DELAY_TICKS = static_cast<float>(Game::INTEREST_UPDATE_EVERY_TICKS);
inline constexpr float INTERP_MAX_EXTRAPOLATE_TICKS = static_cast<float>(Game::INTEREST_UPDATE_EVERY_TICKS) / Game::INTEREST_MIN_WEIGHT;

// --- input feel --------------------------------------------------------------------------------
inline constexpr float INPUT_DRAG_THRESHOLD_PX = 6.0f;
inline constexpr float INPUT_TAP_MAX_DURATION_MS = 320.0f;
inline constexpr float INPUT_DOUBLE_TAP_WINDOW_MS = 300.0f;
inline constexpr float INPUT_PICK_PADDING = 1.15f;

// --- world look --------------------------------------------------------------------------------
inline constexpr Neuron::Rgba SKY_COLOUR{0.043f, 0.051f, 0.063f, 1.0f};
inline constexpr Neuron::Rgba GROUND_COLOUR{0.075f, 0.082f, 0.094f, 1.0f};
inline constexpr Neuron::Rgba GRID_COLOUR{0.28f, 0.36f, 0.44f, 0.55f}; // a = strength
inline constexpr float GRID_SPACING = 20.0f;
inline constexpr float GRID_LINE_WIDTH_PX = 1.3f;
inline constexpr float GRID_FADE_DISTANCE = 900.0f;
inline constexpr float GROUND_SIZE = 4000.0f;
inline constexpr float LIGHT_DIR_X = -0.42f;
inline constexpr float LIGHT_DIR_Y = 0.78f;
inline constexpr float LIGHT_DIR_Z = -0.46f;
inline constexpr float AMBIENT_LEVEL = 0.3f;
inline constexpr Neuron::Rgba SHIP_COLOUR{0.55f, 0.6f, 0.66f, 1.0f};
inline constexpr Neuron::Rgba SELECTED_COLOUR{0.35f, 0.95f, 0.66f, 1.0f};
inline constexpr float SHIP_MATERIAL_MIX = 0.55f;

// What a ship of another faction is drawn in: the three knobs that make an enemy a color rather than
// a silhouette. Which faction counts as "another" is the viewer's own, supplied by the composition
// root -- the server states identity and each client decides what it means (Design/Hostiles.md 4.1),
// so this is a mapping the client owns and not a fact about the ship.
//
// The mix is the one that matters, and it is why there are three constants rather than one. The
// pixel shader takes albedo = lerp(tint, the mesh's own vertex color, mix), so SHIP_MATERIAL_MIX
// keeps 55 % of the paint the hull was authored with -- and these hulls are authored with bright
// green panels (Kd 0.50 0.93 0.13 on the Interceptor). Worked through: a friendly's panel comes out
// #86C75E, and the same panel under a red tint at the same mix comes out #B0A932 -- olive, with red
// barely ahead of green, which is not a red ship. At a fifth it is #D57540 against a #BD473A hull:
// enough paint left for the panels to go on shading the hull, not enough for them to argue about
// whose ship it is.
//
// The accent is where the eye actually catches a hostile under way: exhaust and trail, which every
// ship in the game drew in SELECTED_COLOUR until now, so an enemy was a green-flamed ship. One
// accent for the whole feature -- HUD_ALERT_RED below derives from it, so retuning the enemy moves
// the hulls, the plumes and the dots on the overview together and they cannot drift apart.
inline constexpr Neuron::Rgba HOSTILE_SHIP_COLOUR{0.92f, 0.34f, 0.28f, 1.0f};
inline constexpr float HOSTILE_SHIP_MATERIAL_MIX = 0.2f;
inline constexpr Neuron::Rgba HOSTILE_ACCENT_COLOUR{0.95f, 0.43f, 0.35f, 1.0f};

inline constexpr float SHIP_SCALE = 1.0f;
inline constexpr float SHIP_HOVER_HEIGHT = 4.0f;
inline constexpr float DECAL_LIFT_Y = 0.2f; // clear of the ground quad so the two cannot z-fight

// --- planets and asteroids -----------------------------------------------------------------------
// Bodies are presentation only, placed by the composition root, at metre scale inside the frustum
// the camera already has (Design/PlanetRenderer.md 3, 14; Design/Decisions/0016). **Every length is
// a fraction of the body's radius unless the name carries a unit**, which is what lets one class row
// describe a 400 m world and a 1 200 m one.
inline constexpr float BODY_PLANET_RADIUS_MIN_METRES = 400.0f;
inline constexpr float BODY_PLANET_RADIUS_MAX_METRES = 1200.0f;
// A fifth of the design's 15-120 m, on the owner's eye after the first scene was on screen: at the
// design's figures a rock at 150 m read as another planet rather than as something the fleet flies
// among. Design/PlanetRenderer.md 3 carries the same numbers and the same sentence.
inline constexpr float BODY_ASTEROID_RADIUS_MIN_METRES = 3.0f;
inline constexpr float BODY_ASTEROID_RADIUS_MAX_METRES = 24.0f;
inline constexpr std::uint32_t BODY_PLANET_GRID_POWER = 6;   // 65 samples a side, 49 152 triangles
inline constexpr std::uint32_t BODY_ASTEROID_GRID_POWER = 5; // 33 a side

// The centre's height above the ground plane, as a multiple of the radius. At or above one, always:
// below it the ground quad -- 4 km across and following the camera -- slices the body in half.
inline constexpr float BODY_PLANET_LIFT = 1.15f;
inline constexpr float BODY_PLANET_SPIN_SEC = 240.0f; // one turn
inline constexpr float BODY_PLANET_TILT_MAX_DEG = 30.0f;
inline constexpr float BODY_ASTEROID_TUMBLE_MAX_RAD_PER_SEC = 0.15f;

inline constexpr float BODY_ASTEROID_ELLIPSOID_MIN = 0.55f;
inline constexpr float BODY_ASTEROID_LUMPINESS = 0.25f;
inline constexpr float BODY_ASTEROID_HEIGHT_SCALE_MIN = 0.15f;
inline constexpr float BODY_ASTEROID_HEIGHT_SCALE_MAX = 0.40f;
// Over pi, so one tile covers the whole rock and there is no cap rim anywhere on it -- which is why
// its edge fraction is zero: a fade band would put a soft dimple on the far side.
inline constexpr float BODY_ASTEROID_TILE_HALF_WIDTH_RAD = 3.2f;
inline constexpr float BODY_ASTEROID_TILE_EDGE_FRACTION = 0.0f;
inline constexpr int BODY_ASTEROID_CRATERS_MIN = 6;
inline constexpr int BODY_ASTEROID_CRATERS_MAX = 20;
inline constexpr float BODY_ASTEROID_CRATER_HALF_WIDTH_MIN_RAD = 0.07f;
inline constexpr float BODY_ASTEROID_CRATER_HALF_WIDTH_MAX_RAD = 0.26f;
// How deep a bowl digs at its centre. The work order's list did not carry it and a crater needs one;
// a tenth of the radius on a 60 m rock is a six-metre bowl, which reads as a crater at hull scale.
inline constexpr float BODY_ASTEROID_CRATER_DEPTH_MIN = 0.04f;
inline constexpr float BODY_ASTEROID_CRATER_DEPTH_MAX = 0.12f;

inline constexpr float BODY_PLANET_HEIGHT_SCALE_MIN = 0.03f;
inline constexpr float BODY_PLANET_HEIGHT_SCALE_MAX = 0.12f;
inline constexpr int BODY_PLANET_TILES_MIN = 1;
inline constexpr int BODY_PLANET_TILES_MAX = 4;
inline constexpr float BODY_PLANET_TILE_HALF_WIDTH_MIN_RAD = 0.44f; // 25 degrees
inline constexpr float BODY_PLANET_TILE_HALF_WIDTH_MAX_RAD = 1.22f; // 70 degrees
inline constexpr float BODY_PLANET_TILE_EDGE_FRACTION = 0.25f;
inline constexpr float BODY_PLANET_OUTSIDE_HEIGHT_WET = -0.02f;
inline constexpr float BODY_PLANET_OUTSIDE_HEIGHT_DRY = 0.01f;
inline constexpr float BODY_FRACTAL_DIMENSION = 0.8f;
inline constexpr float BODY_LOWLAND_SMOOTHING = 1.2f;
inline constexpr float BODY_CAP_NOISE = 0.1f;
inline constexpr float BODY_POLAR_GEOMETRY = 0.15f;
inline constexpr Neuron::BodyOverlayParams BODY_OVERLAY{1.2f, 4.0f, 0.5f, 40.0f};

// The starting scene, from one seed, so the pull request's screenshot reproduces. F5 reseeds with
// BODY_START_SEED + the number of presses, which makes a scene reproducible by press count.
inline constexpr std::uint64_t BODY_START_SEED = 0x4F75747031ull; // "Outp1"
inline constexpr float BODY_START_PLANET_DISTANCE_METRES = 4000.0f;
inline constexpr float BODY_START_MOON_DISTANCE_METRES = 3000.0f;
inline constexpr float BODY_START_MOON_RADIUS_FRACTION = 0.5f; // of the planet's
inline constexpr int BODY_START_ASTEROIDS = 6;
inline constexpr float BODY_START_ASTEROID_RING_MIN_METRES = 150.0f;
inline constexpr float BODY_START_ASTEROID_RING_MAX_METRES = 400.0f;

// --- starting scene ----------------------------------------------------------------------------
inline constexpr float START_SPACING = 55.0f;

// The hostile base and its patrol (Design/Hostiles.md 6). The station sits 1,202 m out on the
// diagonal: inside the 2,000 m interest radius, so the base is subscribed from the first update and
// the overview shows red immediately, and inside the minimap's 1,400 m half-range, so it has an edge
// to be seen against. The farthest patrol point is 1,602 m out, still inside both.
//
// The ring clears the station's 251.77 m skin by 148 m, and its chords clear the station's center by
// 386 m against the 263 m an Interceptor needs -- so the legs plan straight and the station never
// even scores as a threat. PatrolTests spells these same five numbers, and the two must agree.
inline constexpr float HOSTILE_BASE_EAST_METRES = 850.0f;
inline constexpr float HOSTILE_BASE_NORTH_METRES = 850.0f;
inline constexpr float HOSTILE_PATROL_RING_METRES = 400.0f;
inline constexpr float HOSTILE_PATROL_CRUISE_MPS = 10.0f; // 29 % of an Interceptor's maximum: a lap in about 4.2 minutes
inline constexpr int HOSTILE_PATROL_COUNT = 3;

// --- HUD ---------------------------------------------------------------------------------------
// Flat and square-cornered: no blur, no glow, no gradients. Sizes are in px at 96 DPI and scale
// with the window DPI; the layout is anchored to corners and edges, so no width is assumed.
inline constexpr Neuron::Rgba HUD_COLOUR{0.78f, 0.87f, 0.96f, 1.0f}; // values and body text
inline constexpr Neuron::Rgba HUD_PANEL_FILL{0.043f, 0.051f, 0.063f, 0.82f};
inline constexpr Neuron::Rgba HUD_PANEL_OUTLINE{GRID_COLOUR.r, GRID_COLOUR.g, GRID_COLOUR.b, 0.45f}; // the grid, as a rule
inline constexpr Neuron::Rgba HUD_LABEL_COLOUR{0.373f, 0.455f, 0.533f, 1.0f};
inline constexpr Neuron::Rgba HUD_ACCENT_GREEN{SEL_RING_COLOUR.r, SEL_RING_COLOUR.g, SEL_RING_COLOUR.b, 1.0f}; // active, positive
inline constexpr Neuron::Rgba HUD_ACCENT_AMBER{MARKER_COLOUR.r, MARKER_COLOUR.g, MARKER_COLOUR.b, 1.0f};       // alerts, orders
// Derived, the way HUD_ACCENT_GREEN is derived from the ring: the red a hostile draws in on the map
// is the red it draws in on the ground, so retuning the enemy retunes both (world look, above).
inline constexpr Neuron::Rgba HUD_ALERT_RED{HOSTILE_ACCENT_COLOUR.r, HOSTILE_ACCENT_COLOUR.g, HOSTILE_ACCENT_COLOUR.b,
                                            1.0f}; // hostile, and alerts
inline constexpr Neuron::Rgba HUD_INFO_GREY{0.45f, 0.50f, 0.56f, 1.0f};
inline constexpr Neuron::Rgba HUD_BAR_TRACK{1.0f, 1.0f, 1.0f, 0.07f};
inline constexpr float HUD_ACTIVE_OUTLINE_ALPHA = 0.7f; // a pressed or active button, in the accent
inline constexpr float HUD_ACTIVE_FILL_ALPHA = 0.08f;
inline constexpr float HUD_GROUP_ACTIVE_FILL_ALPHA = 0.12f;
inline constexpr bool HUD_SCANLINES = true; // CRT lines over the panels only
inline constexpr float HUD_SCANLINE_STEP_PX = 3.0f;
inline constexpr float HUD_SCANLINE_ALPHA = 0.16f;

inline constexpr float HUD_MARGIN_PX = 16.0f;
inline constexpr float HUD_TOP_PX = 14.0f;
inline constexpr float HUD_PANEL_GAP_PX = 10.0f;
inline constexpr float HUD_PANEL_PAD_X_PX = 16.0f;
inline constexpr float HUD_PANEL_PAD_Y_PX = 10.0f;
inline constexpr float HUD_SMALL_SCALE = 0.5f;   // of the UI atlas cell; the minimap header
inline constexpr float HUD_LABEL_SCALE = 0.625f; // small caps for labels
inline constexpr float HUD_TEXT_SCALE = 0.75f;
inline constexpr float HUD_VALUE_SCALE = 1.0f;

inline constexpr float HUD_MINIMAP_WIDTH_PX = 212.0f;
inline constexpr float HUD_MINIMAP_HEADER_PX = 30.0f;
inline constexpr float HUD_MINIMAP_MAP_PX = 140.0f;
inline constexpr float HUD_MINIMAP_GRID_PX = 28.0f;
inline constexpr float HUD_MINIMAP_DOT_PX = 4.0f;
// A structure reads bigger than a fighter without pretending to scale: to scale, a 500 m station is
// 25 px, a quarter of the map for one base. Iconography beats cartography at 0.05 px per metre.
inline constexpr float HUD_MINIMAP_STRUCTURE_DOT_PX = 8.0f;
inline constexpr float HUD_MINIMAP_HALF_RANGE = 1400.0f; // metres from the camera target to the map's edge; wider than CAMERA_MAX_ZOOM sees

inline constexpr float HUD_RAIL_BUTTON_PX = 60.0f; // a hit target, so never below 44
inline constexpr float HUD_RAIL_GAP_PX = 10.0f;
inline constexpr float HUD_RAIL_ICON_PX = 22.0f;

inline constexpr int HUD_LOG_ROWS = 3;
inline constexpr float HUD_LOG_ROW_PX = 18.0f;
inline constexpr float HUD_LOG_RULE_PX = 2.0f;

inline constexpr float HUD_BAR_HEIGHT_PX = 96.0f;
inline constexpr float HUD_GROUP_BUTTON_W_PX = 48.0f;
inline constexpr float HUD_GROUP_BUTTON_H_PX = 56.0f;
inline constexpr float HUD_GROUP_GAP_PX = 6.0f;
inline constexpr float HUD_SUMMARY_WIDTH_PX = 430.0f;
inline constexpr float HUD_STAT_BAR_WIDTH_PX = 90.0f;
inline constexpr float HUD_STAT_BAR_PX = 6.0f;
inline constexpr float HUD_STAT_COLUMN_PX = 250.0f;
inline constexpr float HUD_STATS_WIDTH_PX = 410.0f; // both columns, from HULL to the end of the order state

inline constexpr float HUD_LONG_PRESS_MS = 450.0f; // holding a group tab this long assigns it
} // namespace Outpost
