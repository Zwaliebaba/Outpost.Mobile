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

// --- separation --------------------------------------------------------------------------------
// The fraction of a contact's overlap a pair resolves in one tick. A Jacobi solve applies every
// correction at once, so anything approaching 1 overshoots and rings.
//
// Jacobi is the deliberate trade (Design/Collision.md 6): Gauss-Seidel converges faster and is
// order-dependent, which is precisely the property this design spends effort to avoid. The cost is
// visible in one configuration and worth knowing about -- a chain of identically oriented hulls,
// each exactly balanced between the neighbour ahead and the neighbour behind, relaxes only from its
// ends, so a pathological pack of a hundred parallel hulls on one point unwinds over thousands of
// ticks rather than hundreds. It converges, it stays bounded, and it never explodes; it is simply
// slow, and no amount of neighbour cap fixes it because the cancellation is exact.
inline constexpr float SEPARATION_STIFFNESS = 0.5f;

// How much of one contact's overlap the pair may close in a tick, as a fraction of the *smaller*
// hull's capsule radius. Both sides derive it from the same two radii, so it is the same number on
// each, and the authority split therefore survives it: cap what the pair closes and then divide,
// and a Carrier still takes a fifteenth of what an Interceptor takes.
//
// This is a correction to the design, which clamps each ship's own displacement instead. That
// inverts the authority split exactly when it binds: an Interceptor deep inside a Carrier wants a
// 5 m correction and is cut to 0.28 m by its own small radius, while the Carrier's 0.34 m share
// passes unclamped, and the capital visibly drifts. Capping the pair rather than the ship keeps
// the ratio at 15:1 where clamping the ship gave 1.8:1.
inline constexpr float SEPARATION_PAIR_CLOSE_FRACTION = 0.25f;

// The backstop, for a ship in contact with many others at once: the ceiling on how far one tick
// may move it for a reason the client cannot predict, as a fraction of its own capsule radius so it
// scales with the hull. Without it, a hundred ships spawned on one point produce a correction as
// large as the overlap and the fleet explodes rather than unpacking.
//
// It buys a second thing that matters more: it *is* the prediction error budget. Avoidance is
// server-only and unpredicted, so the worst the client can be wrong by has to be a number rather
// than an unbounded one. At 0.5 an Interceptor may be displaced 0.56 m per tick -- 33 m/s, about
// its own top speed, which is the most that can be called a budget (Design/Collision.md 9, 10).
inline constexpr float SEPARATION_CLAMP_FRACTION = 0.5f;

// A parked ship holds its station harder than one under way. Formation drift under traffic is not
// a separate problem; it is this one with a different number.
inline constexpr float IDLE_AVOIDANCE_AUTHORITY_SCALE = 4.0f;

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
