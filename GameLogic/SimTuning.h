#pragma once

namespace Game
{
// Simulation tuning. Everything that changes what the world *does* lives here; everything that
// changes how it *looks* lives in the view's own tuning header. The split is not cosmetic -- a
// value on this side is part of the replay contract and cannot be changed without invalidating
// recorded games, while a value on the other side can be tuned at any time.
//
// These are compile-time constants today. When they move into loaded data they become a struct
// World owns, read once at boot by the composition root -- never read from a file by this library.

// --- tick rate -------------------------------------------------------------------------------
// The simulation runs at a fixed rate and only at that rate. The render frame interpolates between
// the last two ticks, so the tick rate is a property of the game and not of the display.
inline constexpr float TICK_HZ = 60.0f;
inline constexpr float TICK_DT = 1.0f / TICK_HZ;

// --- motion ----------------------------------------------------------------------------------
inline constexpr float MAX_SPEED = 34.0f;    // metres per second
inline constexpr float ACCELERATION = 26.0f; // metres per second squared
inline constexpr float DECELERATION = 34.0f; // metres per second squared
inline constexpr float TURN_RATE_DEG_PER_SEC = 70.0f;
inline constexpr float TURN_ACCELERATION_DEG = 240.0f; // degrees per second squared
inline constexpr float ARRIVAL_RADIUS = 3.5f;          // metres; inside this the order is done
inline constexpr float STOP_DAMPING_HALF_LIFE = 0.16f; // seconds

// Below this a residual heading error or speed counts as zero, so a ship settles instead of
// jittering around its target for ever.
inline constexpr float ALIGNED_HEADING_RAD = 0.02f;
inline constexpr float ALIGNED_SPEED = 0.05f;
inline constexpr float STOPPED_SPEED = 0.01f;

// --- formation -------------------------------------------------------------------------------
inline constexpr float FORMATION_SPACING = 34.0f; // metres between slots
inline constexpr int FORMATION_SHAPE = 1;         // FormationShape::Wedge
} // namespace Game
