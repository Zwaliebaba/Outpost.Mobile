#pragma once

namespace Game
{
// Simulation tuning. Everything that changes what the world *does* lives here; everything that
// changes how it *looks* lives in the view's own tuning header. The split is not cosmetic -- a
// value on this side is part of the replay contract and cannot be changed without invalidating
// recorded games, while a value on the other side can be tuned at any time.
//
// Per-hull values are not here. They are in HullSpec.h, which is the same contract by a different
// route: every field of every row in that table changes a recorded outcome.
//
// These are compile-time constants today. When they move into loaded data they become a struct
// World owns, read once at boot by the composition root -- never read from a file by this library.

// --- tick rate -------------------------------------------------------------------------------
// The simulation runs at a fixed rate and only at that rate. The render frame interpolates between
// the last two ticks, so the tick rate is a property of the game and not of the display.
//
// 60 Hz is inherited from a build that ran one world on one machine for one player, and an MMO
// server is unlikely to keep it -- tick rate multiplies against entity count and region count and
// server count. Lowering it is not a free knob: at 20 Hz an Interceptor covers 1.70 m per tick
// against a 1.115 m capsule radius and two of them pass straight through each other. The
// tunnelling test is parameterised on this value rather than on a baked 1/60 precisely so that
// the day it moves, the suite goes red naming the hulls (Design/Collision.md 11).
inline constexpr float TICK_HZ = 60.0f;
inline constexpr float TICK_DT = 1.0f / TICK_HZ;

// --- motion ----------------------------------------------------------------------------------
inline constexpr float ARRIVAL_RADIUS = 3.5f;          // metres; inside this the order is done
inline constexpr float STOP_DAMPING_HALF_LIFE = 0.16f; // seconds

// Below this a residual heading error or speed counts as zero, so a ship settles instead of
// jittering around its target for ever.
inline constexpr float ALIGNED_HEADING_RAD = 0.02f;
inline constexpr float ALIGNED_SPEED = 0.05f;
inline constexpr float STOPPED_SPEED = 0.01f;

// --- avoidance -------------------------------------------------------------------------------
// The cap on the derived per-hull avoidance horizon. In the contract: it changes which neighbours
// are considered at all. Uncapped, a Carrier's horizon is 39 s and its query circle 2.7 km, which
// makes the neighbour cap meaningless and sets a floor on region size measured in tens of
// kilometres. At 8 s the widest query in the table is the Carrier's 647 m (Collision.md 10).
inline constexpr float AVOID_HORIZON_MAX_SEC = 8.0f;

// Slack on the separation half of the query radius, so a contact is found on the tick before it
// happens rather than on the tick it does.
inline constexpr float SEPARATION_QUERY_MARGIN_METRES = 4.0f;

// --- test thresholds ---------------------------------------------------------------------------
// Not in the replay contract: nothing reads this at run time, and tightening it changes no
// recorded game. A bare `<` on the tunnelling inequality passes at a margin of 1.001 and calls
// that safe; this is what makes the gate fire before the real limit rather than at it. The live
// ratio today is 0.51, so 0.6 is tight enough to be worth having and loose enough to be green.
inline constexpr float TUNNEL_HEADROOM = 0.6f;

// --- formation -------------------------------------------------------------------------------
inline constexpr float FORMATION_SPACING = 34.0f; // metres between slots
inline constexpr int FORMATION_SHAPE = 1;         // FormationShape::Wedge
} // namespace Game
