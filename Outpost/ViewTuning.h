#pragma once

#include "RenderTypes.h"

#include "SimTuning.h"

namespace Outpost
{
// Presentation tuning: everything that changes how the game looks and feels without changing what
// it does. The simulation's own numbers live in GameLogic/SimTuning.h and are a different kind of
// value -- one of these can be changed between two builds of the same match, one of those cannot.
//
// Nothing here is read by GameLogic, and nothing here feeds back into a tick.

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
inline constexpr float EXPLOSION_REFERENCE_HALF_SIZE = 10.0f;
inline constexpr float EXPLOSION_INTENSITY = 100.0f; // Building::Destroy(_intensity)
inline constexpr int EXPLOSION_HULL_COPIES = 3;
inline constexpr float EXPLOSION_HULL_FRACTION = 1.0f;
inline constexpr float EXPLOSION_FRAGMENT_LIFETIME_SEC = 5.0f;
inline constexpr float EXPLOSION_FRAGMENT_RADIAL_SPEED = 3.0f;
inline constexpr float EXPLOSION_FRAGMENT_MAX_ANG_VEL = 4.0f;
inline constexpr float EXPLOSION_FRAGMENT_FRICTION = 0.05f;
inline constexpr float EXPLOSION_FRAGMENT_ROT_FRICTION = 0.2f;
inline constexpr float EXPLOSION_FRAGMENT_MIN_CIRCUMFERENCE = 6.0f; // scaled
inline constexpr int EXPLOSION_FRAGMENT_CAP = 500;
inline constexpr float EXPLOSION_CORE_SIZE_MIN = 120.0f;      // scaled; Bang's cores
inline constexpr float EXPLOSION_CORE_SIZE_RANGE = 120.0f;    // scaled
inline constexpr float EXPLOSION_CORE_SPEED_XZ = 30.0f;       // scaled; Signed(30) on X and Z
inline constexpr float EXPLOSION_CORE_SPEED_UP_MIN = 10.0f;   // scaled
inline constexpr float EXPLOSION_CORE_SPEED_UP_RANGE = 10.0f; // scaled
inline constexpr float EXPLOSION_CORE_LIFT = 0.3f;            // x range, along up
inline constexpr float EXPLOSION_EXTRA_CORE_SIZE = 100.0f;    // scaled; Destroy's own cores
inline constexpr float EXPLOSION_EXTRA_CORE_SPEED = 100.0f;   // scaled; Signed(100) on all axes
inline constexpr int EXPLOSION_DEBRIS_MIN = 2;
inline constexpr int EXPLOSION_DEBRIS_MAX = 30;
inline constexpr float EXPLOSION_DEBRIS_SPEED_MIN = 20.0f;   // scaled
inline constexpr float EXPLOSION_DEBRIS_SPEED_RANGE = 30.0f; // scaled
inline constexpr float EXPLOSION_DEBRIS_UP_MIN = 2.0f;       // the ring's tilt, before normalising
inline constexpr float EXPLOSION_DEBRIS_UP_RANGE = 2.0f;
inline constexpr float EXPLOSION_DEBRIS_SIZE_MIN = 20.0f;   // scaled
inline constexpr float EXPLOSION_DEBRIS_SIZE_RANGE = 20.0f; // scaled
inline constexpr float EXPLOSION_DEBRIS_LIFT = 0.2f;        // x range, along up
inline constexpr bool EXPLOSION_PLUME_ALONG_UP = true;      // false: the ring is random on a sphere
inline constexpr std::uint32_t EXPLOSION_PARTICLE_CAPACITY = 4096;
// The shake is Camera::Shake(), which takes no amplitude: CAMERA_SHAKE_AMPLITUDE above is the only
// knob, so a Carrier and an Interceptor shake the camera by the same amount.

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
inline constexpr float SHIP_SCALE = 1.0f;
inline constexpr float SHIP_HOVER_HEIGHT = 4.0f;
inline constexpr float DECAL_LIFT_Y = 0.2f; // clear of the ground quad so the two cannot z-fight

// --- starting scene ----------------------------------------------------------------------------
inline constexpr float START_SPACING = 55.0f;

// --- HUD ---------------------------------------------------------------------------------------
// Flat and square-cornered: no blur, no glow, no gradients. Sizes are in px at 96 DPI and scale
// with the window DPI; the layout is anchored to corners and edges, so no width is assumed.
inline constexpr Neuron::Rgba HUD_COLOUR{0.78f, 0.87f, 0.96f, 1.0f}; // values and body text
inline constexpr Neuron::Rgba HUD_PANEL_FILL{0.043f, 0.051f, 0.063f, 0.82f};
inline constexpr Neuron::Rgba HUD_PANEL_OUTLINE{GRID_COLOUR.r, GRID_COLOUR.g, GRID_COLOUR.b, 0.45f}; // the grid, as a rule
inline constexpr Neuron::Rgba HUD_LABEL_COLOUR{0.373f, 0.455f, 0.533f, 1.0f};
inline constexpr Neuron::Rgba HUD_ACCENT_GREEN{SEL_RING_COLOUR.r, SEL_RING_COLOUR.g, SEL_RING_COLOUR.b, 1.0f}; // active, positive
inline constexpr Neuron::Rgba HUD_ACCENT_AMBER{MARKER_COLOUR.r, MARKER_COLOUR.g, MARKER_COLOUR.b, 1.0f};       // alerts, orders
inline constexpr Neuron::Rgba HUD_ALERT_RED{0.95f, 0.43f, 0.35f, 1.0f};                                        // hostile
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
