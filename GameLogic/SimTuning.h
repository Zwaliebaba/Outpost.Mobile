#pragma once

#include <cstdint>

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
// the day it moves, the suite goes red naming the hulls (Design/Archive/Collision.md 11).
inline constexpr float TICK_HZ = 60.0f;
inline constexpr float TICK_DT = 1.0f / TICK_HZ;

// --- the universe ------------------------------------------------------------------------------
// The edge length of one sector, the unit every stored position is denominated in. In the contract
// and near the top of it: change this and every recorded position means a different place.
//
// A power of two, and that is a correctness requirement rather than a preference. WorldPos::Translate
// carries whole sectors out of the local offset by dividing by this value, and at 2^13 that division
// is an exponent adjustment -- exact, on every machine, under /fp:precise. At 10000 it would round,
// and two ships that reached the same point by different routes could end up in different sectors,
// which is a replay divergence with no visible cause. The static_assert below is load-bearing.
//
// 8192 m gives 8192 / 2^24 = 0.49 mm of local precision, uniform in every sector rather than
// decaying with distance from an origin, across a universe of +/-10^19 m (Design/Archive/Collision.md 3).
inline constexpr float SECTOR_SIZE_METRES = 8192.0f;

// Halving a power of two is exact, so a value is one exactly when repeated halving lands on 1.
[[nodiscard]] constexpr bool IsPowerOfTwoMetres(float _metres) noexcept
{
  if (!(_metres > 0.0f))
    return false;
  float value = _metres;
  while (value > 1.0f)
    value *= 0.5f;
  return value == 1.0f;
}

static_assert(IsPowerOfTwoMetres(SECTOR_SIZE_METRES), "SECTOR_SIZE_METRES must be a power of two so the sector carry is exact");

// Cell sizes are powers of two no larger than a sector, so a cell never straddles a sector boundary
// and its index stays a function of the position. PATH_CELL_SIZE_METRES is checked where it is
// defined; the index's cell sizes are runtime knobs, so SpatialIndex::Configure checks those.
[[nodiscard]] constexpr bool IsSectorAlignedCellSize(float _cellSizeMetres) noexcept
{
  return IsPowerOfTwoMetres(_cellSizeMetres) && _cellSizeMetres <= SECTOR_SIZE_METRES;
}

// --- motion ----------------------------------------------------------------------------------
// Arrival tolerance as a fraction of the hull's own bounding radius, with a floor for the smallest
// hulls. It cannot be one constant: 3.5 m is a sensible tolerance for a 3.5 m Interceptor and an
// unreachable one for a 107 m Carrier, which would be asked to stop within a thirtieth of its own
// length (Design/Archive/Collision.md 13).
inline constexpr float ARRIVAL_RADIUS_FRACTION = 0.35f;
inline constexpr float ARRIVAL_RADIUS_MIN_METRES = 1.5f;
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
// kilometres. At 8 s the widest query in the table is the Carrier's 655 m (Collision.md 10).
inline constexpr float AVOID_HORIZON_MAX_SEC = 8.0f;

// How far outside the touching distance a closing neighbour still counts as a threat. Everything
// converging inside this is steered around; everything else is ignored no matter how close it
// currently is, which is what lets ships pass each other closely and calmly.
inline constexpr float AVOID_MARGIN_METRES = 8.0f;

// How many headings are scored, and how far ahead the fan reaches: the arc a hull can turn through
// in this many seconds. Only reachable headings are sampled, which is what makes this fit a
// turn-rate-limited motion model.
inline constexpr int AVOID_CANDIDATE_COUNT = 11;
inline constexpr float AVOID_STEER_REACH_SEC = 1.0f;

// How heavily danger counts against interest, against an interest term that spans [-1, 1].
//
// Swept against a fighter crossing a Carrier's bow at point-blank range, which is the hardest case
// the table can pose: at 2.5 the fighter buried itself 6.8 m into the capital, at 5 it grazes by
// 8 cm, and at 8 the *Carrier* starts swerving more than the fighter does -- which reads as a bug
// whatever the numbers say.
inline constexpr float AVOID_DANGER_WEIGHT = 5.0f;

// The bonus last tick's chosen heading carries. Without it a plain argmax flips left, right, left
// between candidates that score within noise of each other, and the ship shivers down the middle
// instead of committing to its turn. In the contract: it changes which heading wins a near-tie.
inline constexpr float AVOID_CONTINUITY_BONUS = 0.30f;

// The starboard rule. Two identical hulls meeting head-on have equal authority and mirror each
// other exactly, so they deadlock; both biasing to their own right breaks the symmetry with no
// shared state and reads as seamanship rather than as jitter.
inline constexpr float AVOID_STARBOARD_BIAS = 0.35f;
inline constexpr float AVOID_HEAD_ON_CONE_RAD = 0.6f; // about 34 degrees either side of the bow

// How fast the starboard bias ramps in with the threat, and where it saturates -- here, once the
// threat passes 1/16. Both ends of this were measured rather than guessed, and each is a real
// failure:
//
//   Scaled linearly by the threat, the bias arrives too late to be a manoeuvre. A pair of
//   Interceptors deviated 0.5 m each and squeaked past on the danger term alone -- which resolves
//   a symmetric pair by luck of the tie-break, which is the deadlock the rule exists to prevent.
//
//   Applied at full strength as a predicate -- inside the cone and closing -- it is a step of a
//   third of the interest range appearing and vanishing between ticks as the pair weaves, and a
//   Frigate pair measured fourteen heading reversals per second on it.
//
// Saturating early and continuously gives both: an 8.5 m break for that Interceptor pair, and at
// most four reversals per second across the whole table.
inline constexpr float AVOID_HEAD_ON_GAIN = 16.0f;

// What a hull turn-limited past the point where any reachable heading is safe sheds instead.
inline constexpr float AVOID_SPEED_SHED = 0.6f;

// How much earlier than the naive crossing time a hull has to begin its turn.
//
// A ship cannot translate sideways. Clearing a neighbour by its own radius plus the other's takes
// the *turn* that generates the lateral component, and turning costs forward progress, so the time
// needed is longer than the distance divided by the speed -- by roughly the reciprocal of the sine
// of the deviation the ship is willing to make. At the naive time an Interceptor begins avoiding a
// Carrier 119 m out and would need a 90-degree break to clear it, which the interest term rightly
// refuses; measured, it flew into the capital's flank and buried itself 40 m deep. Doubling the
// lead turns that into a 30-degree deviation begun 238 m out.
inline constexpr float AVOID_CLEARANCE_LEAD = 2.0f;

// Slack on the separation half of the query radius, so a contact is found on the tick before it
// happens rather than on the tick it does.
inline constexpr float SEPARATION_QUERY_MARGIN_METRES = 4.0f;

// --- separation --------------------------------------------------------------------------------
// The fraction of a contact's overlap a pair resolves in one tick. A Jacobi solve applies every
// correction at once, so anything approaching 1 overshoots and rings.
//
// Jacobi is the deliberate trade (Design/Archive/Collision.md 6): Gauss-Seidel converges faster and is
// order-dependent, which is precisely the property this design spends effort to avoid.
//
// The cost lands on one shape, and it is worth stating exactly rather than as "slow", because the
// exactness is what tells the next reader not to try to tune it away. A compressed pack of
// identically oriented hulls collapses into lines, and the interior of a line is
// translation-invariant: every ship sees the same neighbourhood, so any solver that is local,
// order-independent and translation-equivariant must give every interior ship the same answer --
// and the same answer everywhere is a translation, which lengthens nothing. Expansion can only be
// sourced at the two ends, and it reaches the middle by diffusion, so the relaxation front advances
// as the square root of time and the whole thing costs O(N^2). Measured on a chain of Interceptors
// compressed to half their spacing: 3.2 * N^2 ticks, from 100 at five hulls to 20,250 at eighty,
// with the front creeping in from both ends and visibly decelerating.
//
// Parallel is the case that matters, because a fleet in formation is parallel by construction. The
// same pack with varied headings relaxes an order of magnitude faster -- 275 ticks against 2,625 at
// N = 40 -- because the closest-approach normals stop being collinear and the pack can spread in
// two dimensions instead of one.
//
// Three things were measured and do NOT help, so that nobody spends the afternoon again: the
// per-tick clamp is not the limit (raising it eightfold changed the chain by nothing), the pair cap
// is not the limit (four settings, no material change), and softening the cap into a smooth
// saturation to preserve its gradient made matters worse, because the magnitude it gives up costs
// more than the gradient it buys. The one lever is running the solve more than once per tick, and
// SEPARATION_ITERATIONS below is that lever.
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
// its own top speed, which is the most that can be called a budget (Design/Archive/Collision.md 9, 10).
inline constexpr float SEPARATION_CLAMP_FRACTION = 0.5f;

// How many times the separation solve runs per tick, and the largest correction that still counts
// as settled. Both are in the contract: the first changes how far a jam unwinds in a tick, and the
// second changes which tick it stops on.
//
// Each step carries the ends' information one ship further inward, so k of them divide the
// quadratic above. Measured on a compressed pack of parallel Interceptors, eight steps against one:
// 8 hulls 2.9 s -> 0.4 s, 16 hulls 10.4 s -> 2.5 s, 24 hulls 21.7 s -> 6.2 s, 40 hulls 45.4 s ->
// 13.3 s. It stays quadratic -- that is the theorem above, not a tuning failure -- but the constant
// is three to seven times better.
//
// It is close to free when there is nothing to solve. A pack that is not overlapping produces no
// corrections, so the first step's largest correction is zero and the loop leaves after one: the
// common case pays one comparison. The cost is spent only where there is a jam, which is exactly
// where it is worth spending.
//
// The clamp is applied to the tick's running total rather than to each step, so extra steps buy
// convergence and never extra displacement -- the prediction error budget is the same number it was
// with one step, and the dense-spawn test measures it at exactly the clamp either way.
inline constexpr std::uint32_t SEPARATION_ITERATIONS = 8;
inline constexpr float SEPARATION_SETTLE_METRES = 0.001f;

// A parked ship holds its station harder than one under way. Formation drift under traffic is not
// a separate problem; it is this one with a different number.
inline constexpr float IDLE_AVOIDANCE_AUTHORITY_SCALE = 4.0f;

// --- pathfinding -------------------------------------------------------------------------------
// All three of these change which path is found, which changes recorded outcomes, so all three are
// in the contract. With the smallest obstacle in the table 250 m across, 32 m cells lose nothing
// that matters.
inline constexpr float PATH_CELL_SIZE_METRES = 32.0f;
static_assert(IsSectorAlignedCellSize(PATH_CELL_SIZE_METRES),
              "a path cell must not straddle a sector boundary (Design/Archive/Collision-slice-8.md 2.2)");
inline constexpr float PATH_CLEARANCE_MARGIN_METRES = 8.0f;

// How far off its planned leg a follower may drift before the route is re-planned. Never per tick:
// a plan is a pure function of the static set and the two endpoints, and re-running it every tick
// would cost everything and change nothing.
inline constexpr float PATH_REPLAN_DEVIATION_METRES = 64.0f;

// How much open ground the grid keeps around the architecture in it, so a route always has room to
// go round the outside, and the ceiling on how many cells that may come to. Past the ceiling the
// grid declines to build and steering falls back to what it did before pathfinding existed --
// coarsening the cells instead would change recorded outcomes as a side effect of where someone
// put a building.
inline constexpr float PATH_GRID_MARGIN_METRES = 512.0f;
inline constexpr int PATH_GRID_MAX_CELLS_PER_AXIS = 512;

// The most waypoints one route may carry. A string-pulled route through sparse convex architecture
// is two or three; a route that needs more is re-planned from its last waypoint rather than
// truncated into a shortcut through a wall.
inline constexpr std::uint32_t MAX_PATH_WAYPOINTS = 16;

// --- test thresholds ---------------------------------------------------------------------------
// Not in the replay contract: nothing reads this at run time, and tightening it changes no
// recorded game. A bare `<` on the tunnelling inequality passes at a margin of 1.001 and calls
// that safe; this is what makes the gate fire before the real limit rather than at it. The live
// ratio today is 0.51, so 0.6 is tight enough to be worth having and loose enough to be green.
inline constexpr float TUNNEL_HEADROOM = 0.6f;

// --- formation -------------------------------------------------------------------------------
// Slot spacing is derived from the largest hull in the ordered group, not fixed. At the old 34 m a
// formation of Carriers was born with every hull deeply inside its neighbours, and separation would
// have spent the rest of the match pushing them apart while the order pushed them back together.
// That makes spacing a property of the order rather than of the game.
//
// The margin has to hold the two scalings apart. Arrival radius and slot spacing both grow with the
// hull, so if the margin were too tight a Carrier's arrival radius could reach past its own slot
// and into the next one, and ships would "arrive" in each other's positions -- a formation that
// assembles into the wrong shape and never corrects, because every ship believes it is done. At
// 1.15 the worst case in the table is 0.30 of the half-spacing, and the suite asserts it.
inline constexpr float FORMATION_SPACING_MARGIN = 1.15f;
inline constexpr int FORMATION_SHAPE = 1; // FormationShape::Wedge

// --- patrol ------------------------------------------------------------------------------------
// How many waypoints a patrol ring has. In the contract: it changes which points a patrolling ship
// steers at, and therefore the whole shape of the run (Design/Archive/Hostiles.md 5.2).
//
// The ring radius and the cruise speed are deliberately not here. They are inputs to AssignPatrol,
// passed by whoever assigns the patrol, the way a spawn position is passed to SpawnShip -- content,
// not contract.
inline constexpr std::uint32_t PATROL_RING_WAYPOINTS = 12;

// --- interest management -----------------------------------------------------------------------
// Not in the replay contract, and that is worth saying because everything around it is: these change
// what is *sent*, never what is *simulated*. A recording made at one radius replays identically at
// another, and a server may tune them per region while a match is running.
//
// The update rate is counted in ticks rather than hertz for the reason latency is: a wall clock
// makes the result depend on how fast the machine ran, and 10 Hz is then a thing a test can assert
// rather than approximate. Six ticks against a 60 Hz tick is the middle of the 5-20 Hz band
// Design/Archive/Collision.md 1 asks for.
inline constexpr std::uint32_t INTEREST_UPDATE_EVERY_TICKS = 6;
inline constexpr float INTEREST_RADIUS_METRES = 2000.0f;

// How much less often the furthest subscribed entity refreshes than the nearest. At 0.125 a ship at
// the edge of the radius is sent once for every eight updates a ship at the centre gets, which is
// the whole of what "priority" buys: the near world stays smooth while the far world stays cheap.
inline constexpr float INTEREST_MIN_WEIGHT = 0.125f;
} // namespace Game
