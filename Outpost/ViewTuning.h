#pragma once

#include "BodyRenderer.h"
#include "RenderTypes.h"

#include "HullSpec.h"
#include "SimTuning.h"
#include "UniverseLayout.h"

#include <cstddef>

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
// (Design/Archive/SpaceshipExplosion.md 3). "scaled" below means multiplied by that at spawn: speeds and
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
// no texture of its own (Design/Archive/ShockRing-work-order.md).
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
// The nozzle's size is content now -- an Exhaust marker's scale, in metres, authored per hull
// (Design/Archive/NmoFormat.md 5.10) -- so what stays tunable is how loudly it is drawn.
inline constexpr float THRUSTER_GLOW_SCALE = 6.0f;
inline constexpr float THRUSTER_GLOW_FALLOFF = 2.2f;
inline constexpr int TRAIL_SAMPLES = 32; // half a second of history at the tick rate

// --- navigation lights ---------------------------------------------------------------------------
// One glow pip per NavLight marker, coloured by the marker and blinking on its own period and phase
// (Design/Archive/NmoFormat.md 5.10). Port red and starboard green are a convention older than any faction
// here, so a nav light is never liveried -- see the marker's flags, and 5.10 for why.
// The light's size is content, exactly as the nozzle's is: a NavLight marker's scale, in metres
// (Design/Archive/NmoFormat.md 5.10), so this is how loudly it is drawn and not how big it is. It has to be
// the marker's -- a station authors approach rings at 3.5 m and berth pads at 4.5, and one flat
// radius would draw a 500 m structure's running lights at a corvette's size.
inline constexpr float NAV_LIGHT_GLOW_SCALE = 1.0f;
inline constexpr float NAV_LIGHT_INTENSITY = 0.85f; // alpha at full on
inline constexpr float NAV_LIGHT_DUTY = 0.35f;      // fraction of the period a blinking light is lit
inline constexpr float NAV_LIGHT_OFF_LEVEL = 0.12f; // alpha between blinks -- a beacon dims, it does not vanish
// The clamp on an authored period, and the whole multiple the nav clock wraps at, so the wrap is
// seamless for every period a marker may legally carry.
inline constexpr float NAV_LIGHT_MAX_PERIOD_SEC = 30.0f;

// --- frustum culling ----------------------------------------------------------------------------
// What is not on screen is not submitted (Design/Archive/MmoScalabilityReview.md G2). Both numbers below
// buy the same thing: a test that errs towards drawing, because a wasted draw costs a fraction of a
// millisecond and a wrongly culled hull is a ship that vanishes.
//
// The pad is added to every bounding sphere. A hull's sphere is its mesh bounds, which is tight, and
// a ship straddling the edge of the screen is exactly where a tight sphere shows: the thruster plume
// trails behind the hull and is not in its bounds at all, so the pad is a trail length with room
// over.
inline constexpr float CULL_RADIUS_PAD_METRES = 24.0f;
// A body's own relief and ellipsoid stretch are already in its bounding radius, so this only covers
// the outline pass sitting a little proud of the terrain.
inline constexpr float CULL_BODY_RADIUS_SCALE = 1.05f;

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

// --- the sky -----------------------------------------------------------------------------------
// A procedurally generated star field, drawn before anything else and never tested against depth
// (Design/Archive/Skybox.md). Everything about it follows from SKY_SEED, so a screenshot of one reproduces,
// and F5 reseeds it with the bodies -- the sky and the neighborhood are one scene.
//
// The counts are what fills a sky rather than what a machine can afford: 6 000 stars is 36 000
// vertices and one megabyte, uploaded once and drawn in three calls, and doubling it would still not
// be measurable. What decides them is that a naked-eye sky holds about 3 000 stars over a hemisphere
// and this one is a whole sphere, because the outpost can see below its own horizon.
inline constexpr std::uint64_t SKY_SEED = 0x5C1B0A7E5741B0A7ull;
inline constexpr std::uint32_t SKY_STAR_COUNT = 14000;
inline constexpr std::uint32_t SKY_BRIGHT_STAR_COUNT = 24;
inline constexpr std::uint32_t SKY_NEBULA_COUNT = 240;
// Where the sphere is put: it only has to sit between CAMERA_NEAR_PLANE and CAMERA_FAR_PLANE, since
// the pass writes no depth and nothing in the scene is ever tested against it.
inline constexpr float SKY_RADIUS_METRES = 5000.0f;
inline constexpr float SKY_INTENSITY = 1.0f;
// The fastest a star scintillates. A vertex carries its own rate as a fraction of this, so the
// number lives here and the shader is told it rather than agreeing with it (SkyRenderer::Frame).
inline constexpr float SKY_TWINKLE_MAX_RATE_RAD_PER_SEC = 4.5f;

// --- world look --------------------------------------------------------------------------------
// The background the sky is drawn onto. Nearly black on purpose now that there are stars in front of
// it: what is left is the faint wash a real dark sky has, and lifting it any further washes the
// faintest stars out. It was three times this before the sky existed, when it was the whole sky.
inline constexpr Neuron::Rgba SKY_COLOUR{0.014f, 0.017f, 0.024f, 1.0f};
// There is no ground. The outpost is in open space and the sky wraps all the way round it, so the
// scene pass draws no plane and has no grid to draw on one (Design/Decisions/0025). What is still
// flat is the *order* plane at y = 0: a move order lands on it, ships fly at SHIP_HOVER_HEIGHT above
// it, and every ring and marker is a decal on it. That plane is arithmetic, not geometry.
inline constexpr float LIGHT_DIR_X = -0.42f;
inline constexpr float LIGHT_DIR_Y = 0.78f;
inline constexpr float LIGHT_DIR_Z = -0.46f;
inline constexpr float AMBIENT_LEVEL = 0.3f;
// --- liveries -------------------------------------------------------------------------------------
// A faction's paint. One colour, where an enemy used to need three: the mix is gone with the lerp
// (Design/Decisions/0036), and the accent is gone because the shade ladder already provides it -- a
// thruster is this colour at 1.0 and a plate is it at 0.45, so a livery is authored at the
// brightness its nozzles should burn and the hull falls out of it (Design/Archive/NmoFormat.md 13.1).
//
// Which faction counts as "another" is the viewer's own, supplied by the composition root -- the
// server states identity and each client decides what it means
// (Design/Archive/Hostiles.md 4.1), so this is a mapping the client owns and not a fact about the
// ship.
//
// The first two cannot be chosen. Core Vanguard Command is the government and the Vandal Collective
// is what the government is for; a player wearing either would be lying about who they are, and the
// affordance that would let them is simply absent -- a chooser walks SELECTABLE_LIVERIES and
// structurally cannot reach these two.
inline constexpr Neuron::Rgba LIVERY_VANGUARD{0.24f, 0.52f, 0.95f, 1.0f}; // CVC azure -- reserved
inline constexpr Neuron::Rgba LIVERY_VANDAL{0.92f, 0.26f, 0.22f, 1.0f};   // Vandal red -- reserved

inline constexpr Neuron::Rgba SELECTABLE_LIVERIES[] = {
  {0.54f, 1.00f, 0.14f, 1.0f}, // green -- what every hull in the game wore before liveries existed
  {0.95f, 0.66f, 0.15f, 1.0f}, // amber
  {0.62f, 0.36f, 0.92f, 1.0f}, // violet
  {0.20f, 0.80f, 0.82f, 1.0f}, // cyan
  {0.95f, 0.45f, 0.16f, 1.0f}, // orange
  {0.86f, 0.88f, 0.92f, 1.0f}, // white
};

// The player's livery is presentation, which Server.cfg deliberately does not reach (ADR 0043), so
// it is a constant and the chooser is not this slice. This is the seam one will arrive at, and it is an index into
// the selectable array rather than a colour precisely so that whatever chooses later cannot choose
// a reserved one.
inline constexpr std::size_t PLAYER_LIVERY_INDEX = 0;

// Toward white, on the selected hull. Selection stops being a colour under liveries: a mint-green
// selected hull would read as a different faction, and the player's own livery might be mint.
inline constexpr float SELECTED_HIGHLIGHT_LIFT = 0.35f;

inline constexpr float SHIP_SCALE = 1.0f;
inline constexpr float SHIP_HOVER_HEIGHT = 4.0f;
inline constexpr float DECAL_LIFT_Y = 0.2f; // clear of the ground quad so the two cannot z-fight

// --- planets and asteroids -----------------------------------------------------------------------
// Bodies are presentation only, placed by the composition root, at metre scale inside the frustum
// the camera already has (Design/Archive/PlanetRenderer.md 3, 14; Design/Decisions/0016). **Every length is
// a fraction of the body's radius unless the name carries a unit**, which is what lets one class row
// describe a 400 m world and a 1 200 m one.
inline constexpr float BODY_PLANET_RADIUS_MIN_METRES = 400.0f;
inline constexpr float BODY_PLANET_RADIUS_MAX_METRES = 1200.0f;
// A fifth of the design's 15-120 m, on the owner's eye after the first scene was on screen: at the
// design's figures a rock at 150 m read as another planet rather than as something the fleet flies
// among. Design/Archive/PlanetRenderer.md 3 carries the same numbers and the same sentence.
inline constexpr float BODY_ASTEROID_RADIUS_MIN_METRES = 3.0f;
inline constexpr float BODY_ASTEROID_RADIUS_MAX_METRES = 24.0f;
inline constexpr std::uint32_t BODY_ASTEROID_GRID_POWER = 5; // 33 a side

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

// The height outside every tile. It was a pair -- below zero for an ocean world, above it for a dry
// one -- and the wet value went with the sea (Design/Decisions/0026).
inline constexpr float BODY_OUTSIDE_HEIGHT = 0.01f;
inline constexpr float BODY_FRACTAL_DIMENSION = 0.8f;
inline constexpr float BODY_LOWLAND_SMOOTHING = 1.2f;
inline constexpr Neuron::BodyOverlayParams BODY_OVERLAY{1.2f, 4.0f, 0.5f, 40.0f};

// Which producer makes a body's vertices. Both are alive and both make the same mesh; the CPU one is
// the reference the GPU one is verified against, and it is the one the test suite exercises
// (Design/Archive/PlanetRenderer.md 17).
//
// **True: the readback comparison has been run, and the two producers agree.** Measured on an
// RTX 3070 Ti Laptop, Debug|x64, over the eight starting bodies of BODY_START_SEED, with every
// baked vertex read back through BodyRenderer::ReadBackBody and compared with BodyMeshBuilder's:
//
//   - triangle counts equal on every body, the wet world's 3 166 included, so the sea-level cull
//     agrees on which cells it keeps;
//   - positions within 9e-6 of the radius, none over the work order's 1e-4;
//   - colours within one step of 255, and uvs bitwise equal;
//   - normals within 2.2e-4, of which up to 102 triangles on the smallest asteroids exceed 1e-4 --
//     a metre-scale triangle on a 12 m body turns a 5e-6 R position difference into a hundredth of
//     a degree, and the work order's 1e-4 was written with a planet's triangles in mind.
//
// Three things had to be fixed before that held, and each was silent: BodyField::ParamsFor left the
// octave amplitudes zero, because they were filled by MeasureTiles and the GPU path never runs it;
// BakeBody seeded its maxima through a helper that left the buffer in the wrong state; and the
// kernel reseeded its dither generator per triangle where the builder seeds it per cell.
// Decisions/0020 records all three.
inline constexpr bool BODY_BAKE_ON_GPU = true;

// The starting scene, from one seed, so the pull request's screenshot reproduces. F5 reseeds with
// BODY_START_SEED + the number of presses, which makes a scene reproducible by press count.
inline constexpr std::uint64_t BODY_START_SEED = 0x4F75747031ull; // "Outp1"

// **Where the two worlds sit is framing, and it is worked out against the camera the player starts
// with, not chosen as a round number.** That camera looks north and 52 degrees *down* from an eye at
// (0, 152.7, -117): 190 m of zoom at 52 degrees of pitch over a target 3 m up. With a 45 degree
// vertical field of view on 16:9 that leaves a cone spanning 29.5 to 74.5 degrees of depression and
// plus or minus 36.4 degrees of azimuth -- so anything level with the fleet or above it, or more
// than 36 degrees off north, is off the screen before it is drawn.
//
// Both worlds used to be 45 degrees off north and lifted 1.15 radii *above* the plane, which put
// them roughly 40 degrees above the top edge and 7 degrees past the right one. They were in the
// scene and nobody had ever seen them.
//
// So each is placed where it lands on the screen, and the numbers below are what that works out to
// as a bearing and a range from the fleet. Against BODY_START_SEED, whose planet comes out 864 m
// across and whose moon is half that:
//
//   planet  23 deg left of north, 43.7 deg down  -> 5.0 km of slant range, 19.6 deg across
//
// There was a moon 23 degrees the other way, and it is gone: it was switched off while the look was
// being settled and then had nothing to wear, because a world is a picture now and there is one
// picture. Bringing it back is a second map and one more call beside the one below.
//
// It stays wholly on screen at 16:9 and at 4:3, and with the camera pulled back to CAMERA_MAX_ZOOM.
// Its far limb is 5.9 km away at the worst pitch and yaw the camera allows, which leaves two
// kilometres of headroom under CAMERA_FAR_PLANE.
//
// A depth is metres, not radii: what is being aimed at is an angle on the screen, and the seeded
// radius must not be able to move it. Both sit high on the screen while being far below the fleet in
// the world, which is the whole point -- the outpost is flying over them.
inline constexpr float BODY_START_PLANET_BEARING_DEG = -23.0f;
inline constexpr float BODY_START_PLANET_DISTANCE_METRES = 3500.0f;
inline constexpr float BODY_START_PLANET_DEPTH_METRES = 3300.0f; // below the fleet's plane
inline constexpr int BODY_START_ASTEROIDS = 6;

// **Where the worlds sit is the layout's now, not this file's.** The two numbers above are still
// the framing they always were, and they reach the scene as the pin on the starting system's first
// planet: LayOutSystem draws the other sites, and planet 0 takes this bearing and orbit verbatim so
// the worked camera framing survives as content instead of being superseded by a die roll
// (Design/Archive/Stations.md 5.3). Every other SystemDesc field keeps its default on purpose -- the
// defaults are the shipped numbers, and GameLogicTests proves the grid-ceiling bound against them,
// so a root that overrode the orbit band would be running a system the test never saw
// (UniverseLayout.h). The star anchor is the universe origin; nothing draws a star.
//
// Its own seed beside BODY_START_SEED, because F5 must not reach it: a reroll changes what the
// worlds look like and never where they are, since a station stands on every site and a debug key
// that moved simulation content would be a debug key changing the world.
inline constexpr std::uint64_t UNIVERSE_LAYOUT_SEED = 0x53797331ull; // "Sys1"
inline constexpr Game::SystemDesc STARTING_SYSTEM{.pinFirstPlanet = true,
                                                  .firstPlanetBearingRad = BODY_START_PLANET_BEARING_DEG * (DirectX::XM_PI / 180.0f),
                                                  .firstPlanetOrbitMetres = BODY_START_PLANET_DISTANCE_METRES};

// **The world wears a picture rather than a generated surface.** BodyMeshBuilder::BuildSphere makes
// a smooth sphere and PlanetPS samples Terrain\Planet1.dds off the direction, so neither the height
// field, the colour ramp nor the wire-frame outline runs for it. Everything that decides what a
// *generated* body looks like still runs for the six asteroids; this is a second kind of body beside
// that one, not a replacement for it (Design/Decisions/0026).
//
// Left as a switch rather than hard-wired because the generated path is still there and still
// tested, and turning this off is how it gets looked at again.
inline constexpr bool BODY_PLANET_TEXTURED = true;
// Its tessellation. Sixty-four cells a side is 49 152 triangles, the same as a generated planet, and
// puts a silhouette segment at about five pixels with the world framed as it is above.
inline constexpr std::uint32_t BODY_PLANET_SPHERE_GRID_POWER = 6;

// Level of detail (Design/Archive/BodyLod-work-order.md). Every body is baked at three grid powers -- the
// level's power is the body's own minus its index, so a grid-6 world carries {6, 5, 4} and a
// grid-5 rock {5, 4, 3} -- and the level drawn is chosen per frame from the body's projected
// radius on screen. The thresholds sit where a silhouette segment is already subpixel, so the
// switch is not visible; they are presentation tuning and outside any contract. Below the cull
// threshold an *asteroid* is not drawn at all; a world is never distance-culled, because a world
// on the horizon is the framing.
inline constexpr std::uint32_t BODY_LOD_COUNT = 3;
inline constexpr float BODY_LOD1_BELOW_PX = 220.0f;
inline constexpr float BODY_LOD2_BELOW_PX = 90.0f;
inline constexpr float BODY_CULL_BELOW_PX = 1.5f;
inline constexpr float BODY_START_ASTEROID_RING_MIN_METRES = 150.0f;
inline constexpr float BODY_START_ASTEROID_RING_MAX_METRES = 400.0f;

// --- starting scene ----------------------------------------------------------------------------
inline constexpr float START_SPACING = 55.0f;

// The hostile base and its patrol (Design/Archive/Hostiles.md 6). The station sits 1,202 m out on the
// diagonal: inside the 2,000 m interest radius, so the base is subscribed from the first update and
// the overview shows red immediately, and well inside the minimap's half-range, so it has an edge
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

// The Vanguard's garrison: what every station of the starting system launches when it is attacked,
// and how (Design/Archive/Stations.md 8.2). Content, passed to MakeStation by the composition root the way
// the patrol numbers above are passed to AssignPatrol -- not a tuning constant, because what the
// root spawns is content. Nothing in the running game attacks a station until slice 6's debug key,
// so these are exercised by GameLogicTests and by nothing else yet.
inline constexpr Game::HullId VANGUARD_PROTECTOR_HULL = Game::HullId::Corvette;
inline constexpr std::uint32_t VANGUARD_PROTECTOR_COMPLEMENT = 3;
inline constexpr std::uint32_t VANGUARD_LAUNCH_EVERY_TICKS = 90;
inline constexpr std::uint32_t VANGUARD_TARGET_CAP = 4;

// --- fleet focus -------------------------------------------------------------------------------
// Tapping a fleet button flies the camera to that fleet: one gesture, because under the design's
// first decision selecting a fleet IS attending to it (Design/Fleets.md 9.1).
//
// Camera::SetGoal and Camera::Follow have existed since the camera did and nothing had ever called
// them; this is what they were for. The ease is the camera's own followHalfLife, so a fleet jump
// and any later camera-follow feel like the same instrument.
//
// The arrival radius is generous on purpose. It is compared against the camera's TARGET, which the
// half-life eases asymptotically towards the goal -- so a tight radius would hold the camera
// captive for whole seconds after the flight has visibly finished, and the player's next pan would
// be fighting it. 25 m is inside a hull length at the closest zoom.
inline constexpr float FLEET_FOCUS_ARRIVE_METRES = 25.0f;

// --- HUD ---------------------------------------------------------------------------------------
// Flat and square-cornered: no blur, no glow, no gradients. Sizes are in px at 96 DPI and scale
// with the window DPI; the layout is anchored to corners and edges, so no width is assumed.
inline constexpr Neuron::Rgba HUD_COLOUR{0.78f, 0.87f, 0.96f, 1.0f}; // values and body text
inline constexpr Neuron::Rgba HUD_PANEL_FILL{0.043f, 0.051f, 0.063f, 0.82f};
// The slate blue the ground grid used to be drawn in. It outlived the grid: the HUD had taken it as
// its rule colour, and a panel outline is what it is now for.
inline constexpr Neuron::Rgba HUD_PANEL_OUTLINE{0.28f, 0.36f, 0.44f, 0.45f};
inline constexpr Neuron::Rgba HUD_LABEL_COLOUR{0.373f, 0.455f, 0.533f, 1.0f};
inline constexpr Neuron::Rgba HUD_ACCENT_GREEN{SEL_RING_COLOUR.r, SEL_RING_COLOUR.g, SEL_RING_COLOUR.b, 1.0f}; // active, positive
inline constexpr Neuron::Rgba HUD_ACCENT_AMBER{MARKER_COLOUR.r, MARKER_COLOUR.g, MARKER_COLOUR.b, 1.0f};       // alerts, orders
// Derived, the way HUD_ACCENT_GREEN is derived from the ring: the red a hostile draws in on the map
// is the red the Vandal Collective flies in the scene, so retuning the faction retunes both.
//
// The HUD is a *relation* language where the scene is an identity one, and the two must not be
// confused: liveries say who a ship is, and the minimap says whether it is a friend. A player who
// picks a red-ish livery must not turn their own dots into the colour the HUD uses for hostiles, so
// nothing on the HUD reads SELECTABLE_LIVERIES except the player's own hull bar.
inline constexpr Neuron::Rgba HUD_ALERT_RED{LIVERY_VANDAL.r, LIVERY_VANDAL.g, LIVERY_VANDAL.b, 1.0f}; // hostile, and alerts
// Derived the same way, for the same reason: the blue a Vanguard record draws in on the map is the
// azure the Vanguard flies in the scene. Only while the mask says they are not hostile -- the day
// the law turns on the player, every Vanguard dot goes HUD_ALERT_RED with the hulls
// (Design/Archive/Stations.md 9.3).
inline constexpr Neuron::Rgba HUD_VANGUARD_BLUE{LIVERY_VANGUARD.r, LIVERY_VANGUARD.g, LIVERY_VANGUARD.b, 1.0f};
inline constexpr Neuron::Rgba HUD_INFO_GREY{0.45f, 0.50f, 0.56f, 1.0f};
inline constexpr Neuron::Rgba HUD_BAR_TRACK{1.0f, 1.0f, 1.0f, 0.07f};
inline constexpr float HUD_ACTIVE_OUTLINE_ALPHA = 0.7f; // a pressed or active button, in the accent
inline constexpr float HUD_ACTIVE_FILL_ALPHA = 0.08f;
inline constexpr float HUD_FLEET_ACTIVE_FILL_ALPHA = 0.12f;
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
// Metres from the camera target to the map's edge. It was 1 400 -- wider than CAMERA_MAX_ZOOM sees,
// and enough to hold the Vandal base -- until the Vanguard stations existed: the nearest stands at
// the pinned world's 3 500 m, and a mark that is clamped to the edge from the first frame says which
// way to fly but never how far. At 4 000 the nearest station is inside the map from boot, by the
// owner's ask, and the two farther ones (2 500-6 500 m from the star) are inside or clamped by
// where the layout put them. What it costs is resolution: 140 px over 8 km is 57 m per pixel, so the
// three starting hulls draw as one cluster and the patrol ring is a 7 px circle. The map is
// iconography, not cartography (HUD_MINIMAP_STRUCTURE_DOT_PX says the same), and a station's
// distance is the thing the map now answers that it could not before.
inline constexpr float HUD_MINIMAP_HALF_RANGE = 4000.0f;
// A station mark: a hollow diamond at a station of the layout, drawn from static content rather than
// from a record, so it is there from the first frame however far away the station is. One past the
// map's edge is clamped to the edge and dimmed rather than clipped like a dot -- direction honest,
// distance saturated (Design/Archive/Stations.md 9.3).
inline constexpr float HUD_MINIMAP_MARK_PX = 10.0f;
inline constexpr float HUD_MINIMAP_MARK_CLAMPED_ALPHA = 0.5f;

inline constexpr float HUD_RAIL_BUTTON_PX = 60.0f; // a hit target, so never below 44
inline constexpr float HUD_RAIL_GAP_PX = 10.0f;
inline constexpr float HUD_RAIL_ICON_PX = 22.0f;

inline constexpr int HUD_LOG_ROWS = 3;
inline constexpr float HUD_LOG_ROW_PX = 18.0f;
inline constexpr float HUD_LOG_RULE_PX = 2.0f;

inline constexpr float HUD_BAR_HEIGHT_PX = 96.0f;
inline constexpr float HUD_FLEET_BUTTON_W_PX = 48.0f;
inline constexpr float HUD_FLEET_BUTTON_H_PX = 56.0f;
inline constexpr float HUD_FLEET_GAP_PX = 6.0f;
// The under-attack pulse, on the button and on the minimap digit together. A period rather than a
// blink: the alert holds for ten seconds by Design/Fleets.md 7.3, so a flash would be a metronome
// and a steady red would be indistinguishable from a livery. Real time, like the sky's twinkle --
// a paused game whose alert stops pulsing reads as a hung one.
inline constexpr float HUD_FLEET_ALERT_PERIOD_SEC = 0.9f;
inline constexpr float HUD_FLEET_ALERT_MIN_ALPHA = 0.35f;
inline constexpr float HUD_SUMMARY_WIDTH_PX = 430.0f;
inline constexpr float HUD_STAT_BAR_WIDTH_PX = 90.0f;
inline constexpr float HUD_STAT_BAR_PX = 6.0f;
inline constexpr float HUD_STAT_COLUMN_PX = 250.0f;
inline constexpr float HUD_STATS_WIDTH_PX = 410.0f; // both columns, from HULL to the end of the order state

inline constexpr float HUD_LONG_PRESS_MS = 450.0f; // holding a fleet button this long opens its sheet
} // namespace Outpost
