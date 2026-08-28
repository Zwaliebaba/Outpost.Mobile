# Collision and avoidance

**Status: proposal, for review. Nothing here is implemented.**

This document proposes how ships stop passing through each other and each other's structures, and
how they give way while under way. It is written against the MMO target — many players connected in
parallel, one authoritative server — because the structure that serves collision is the same
structure that serves interest management, and choosing it twice would mean choosing it wrong once.

It does not propose pathfinding (§12).

---

## 1. This is not a collision system

The thing being built is a **spatial index owned by `World`**. Collision is its first customer.

Its second customer is interest management: deciding which entities each connected player's snapshot
contains. That is the hardest problem in the MMO target, because snapshot cost is O(N²) in connected
players without it and O(N · k) with it. Weapons range, sensor range, blast radius and region
ownership are customers three through six.

So the design question is not "which collision algorithm". It is **what query does `World` expose,
is it deterministic, and does it shard**. Get that right and the rest is replaceable.

Everything below follows from that. Where a cheaper choice would have worked for collision alone and
a more general one is proposed instead, the reason is always the second customer.

---

## 2. What the tree already guarantees

These are constraints, not preferences. A design that breaks one of them is wrong regardless of how
well it collides.

| Constraint | Where it comes from |
|---|---|
| `GameLogic` depends on `NeuronCore` and nothing else | AGENTS.md §2 |
| No wall clock, no OS entropy, no pointers as keys, no iteration order that is not dense-array order | AGENTS.md §5 |
| `XM*Est` banned in `GameLogic`; `/fp:precise`; no `/arch` | AGENTS.md §5 |
| Presentation state does not live in the simulation | AGENTS.md §5 |
| Two identical runs are bit-identical, tick for tick | `GameLogicTests::TheSameOrderProducesTheSameRun` |
| Ships are a dense `std::vector<ShipState>`; `ShipId` is the index | `GameLogic/World.h` |

### The determinism this design assumes, and the determinism it does not

The tree is **server-authoritative** — `ServerHost`, `Simulation`, `Transport` snapshots, and
AGENTS.md §5's rule that a value which would go over a wire to a spectator belongs to `World`. That
is the right model for an MMO and this design assumes it.

It has one large consequence, and it should be recorded here because it saves a great deal of work:
**cross-machine bit-exact determinism is not required.** Only the server simulates. Clients predict
and interpolate; a mismatch is corrected by the next snapshot rather than desynchronising a match.

Therefore:

- `float` stays. **Do not migrate to fixed-point.** That migration is only justified by lockstep,
  and lockstep is not the model here.
- The determinism that *is* required is same-binary, same-input, same-output — for replay, for the
  test gate, and for server-side rollback. That is what `/fp:precise` buys and it is enough.
- x64-versus-ARM64 divergence is a non-issue under server authority. Under lockstep it would be
  fatal. Worth stating, since the tree has just grown an ARM64 configuration.

---

## 3. The world this has to survive — measured

The current tree is three same-sized hulls on a plane at one speed. That is not what this must be
designed for. Bounding radii below are measured from the shipped meshes (`Outpost/Assets/Meshes`),
half the larger of the X and Z extents:

| Mesh | Width (X) | Length (Z) | Bounding radius | Mobile |
|---|---|---|---|---|
| Interceptor | 2.23 | 7.01 | **3.51** | yes |
| Bomber | 17.41 | 17.01 | 8.71 | yes |
| Corvette | 17.19 | 26.22 | 13.11 | yes |
| Miner | 22.80 | 34.40 | 17.20 | yes |
| Frigate | 20.81 | 44.63 | 22.31 | yes |
| Hauler | 46.21 | 57.42 | 28.71 | yes |
| Battleship | 43.28 | 80.75 | 40.37 | yes |
| Carrier | 79.34 | 215.08 | 107.54 | yes |
| Stargate | 263.22 | 242.53 | 131.61 | no |
| Structure | 502.36 | 503.54 | **251.77** | no |

Three facts drop out of that table, and each of them kills a design that would have been fine for
the current tree:

1. **The size ratio is 72:1**, and 31:1 among mobile hulls alone. A single uniform grid cannot serve
   both ends. Sized for the Interceptor, a Structure occupies roughly four thousand cells; sized for
   the Structure, a two-hundred-ship fighter engagement lands in one cell and every query degenerates
   to a linear scan.
2. **The largest things do not move.** Structure and Stargate are static. That is the cheapest
   available split and it removes the worst case from the per-tick path entirely.
3. **Hulls are elongated, and the small ones most of all.** The Interceptor is 2.23 × 7.01 — a
   bounding circle is three times too fat across the beam. The Carrier is 79 × 215. Circles would
   make a formation of Interceptors behave as though each were a Bomber.

Speed spread is not yet in the tree — `SimTuning.h` has one `MAX_SPEED = 34.0f` for every hull — but
the request is explicitly for slow and fast ships. §11 treats what that breaks.

---

## 4. Ship shape: capsules, not circles

**Every collidable is a capsule**: a line segment of half-length `L` along the hull's forward axis,
inflated by radius `r`. A circle is the degenerate case `L = 0`, so this is one shape, not two.

Fitted to the meshes, taking `r` as half the beam and `L` as half the length beyond that:

| Hull | r | L | Bounding radius (`L + r`) |
|---|---|---|---|
| Interceptor | 1.12 | 2.39 | 3.51 |
| Corvette | 8.60 | 4.52 | 13.11 |
| Frigate | 10.41 | 11.91 | 22.31 |
| Battleship | 21.64 | 18.74 | 40.37 |
| Carrier | 39.67 | 67.87 | 107.54 |

Why a capsule and not an oriented box:

- Closest distance between two capsules is closest distance between two segments — one short,
  branch-light, closed-form function that also yields the contact normal directly. A box needs SAT,
  and SAT's contact normal needs a separate face/edge decision that is easy to get subtly wrong.
- The gameplay difference between a capsule and a box, at these sizes and from an RTS camera, does
  not pay for that code.
- One shape means one narrow-phase function, and one function is one place for the determinism to
  be right.

Two notes for the implementation:

- **The broad phase always works in bounding circles** (`L + r`). Shape fidelity is a narrow-phase
  concern only. This is what keeps the index simple no matter what shapes arrive later.
- `L == 0` on both sides is worth a fast path — circle-versus-circle is a handful of operations
  against roughly thirty for segment-versus-segment, and it is the overwhelmingly common pair.

Authored, not derived: collision capsules should be tuned values in the hull table, generally sitting
slightly *inside* the visual hull. Deriving them from mesh bounds at load time would put them in the
renderer's reach, would drag antennae and wingtips into the collision hull, and would make the
simulation depend on content it must be able to run without (§5).

---

## 5. `HullSpec`: the data that has to exist first

There is no size information in the simulation today. The only extent anywhere is
`WorldView::ShipView::halfExtents` (`Outpost/WorldView.h:35`), taken from mesh bounds — presentation
data, in the client half, absent on a headless server.

Proposed: a `HullSpec` table in `GameLogic`, indexed by the existing `ShipState::hullId`.

```cpp
struct HullSpec
{
  float capsuleRadiusMetres = 1.0f;
  float capsuleHalfLengthMetres = 0.0f;   // 0 == a circle

  float maxSpeedMetresPerSec = 34.0f;
  float accelerationMetresPerSec2 = 26.0f;
  float decelerationMetresPerSec2 = 34.0f;
  float turnRateRadPerSec = 0.0f;

  float avoidanceAuthority = 1.0f;        // see §9
  bool  immovable = false;                // structures

  [[nodiscard]] float BoundingRadiusMetres() const noexcept
  {
    return capsuleHalfLengthMetres + capsuleRadiusMetres;
  }
};
```

`hullId → mesh` resolution stays exactly where it is, in the view. The simulation gets its own table
and never learns what a mesh is.

This is worth doing on its own merits, independently of collision. `SimTuning.h` currently gives an
Interceptor and a Carrier identical acceleration and identical turn rate, which is a 30:1 mass ratio
handled as though it were 1:1. The collision work is simply the moment that stops being deferrable,
because collision needs a per-hull number from the same table.

These are `constexpr` today, as `SimTuning.h` is. AGENTS.md §5 already anticipates the shape they
take when they become loaded data: a struct `World` owns, read once at boot by the composition root.
Nothing in this design blocks that move, and nothing in it requires it yet.

---

## 6. The tick: five passes and one invariant

### The invariant: order-independence

This is the single property that will silently break the MMO path if it is got wrong now.

`World::Step()` today is `for (ShipState& ship : m_ships) StepShip(ship);` — order-independent by
accident, because `StepShip` reads nothing but its own ship. The moment avoidance reads neighbours
out of a live, in-place-mutating array, ship 0 sees start-of-tick positions and ship 500 sees
half-updated ones, and **the result depends on array order**.

Array order stops being stable the moment any of the following arrives, all of which are on the
roadmap: despawn with swap-and-pop, splitting the ship array across worker threads, or handing
entities between region servers. At that point the simulation silently produces different answers
for the same inputs, replays stop reproducing, and the cause is a pass written years earlier.

The fix is nearly free, and the buffer for it already exists. `Movement.cpp:16-17` writes
`prevPos`/`prevHeading` at the top of `StepShip`. **Hoist those two lines into a pass of their own at
the top of `World::Step()`.** `prevPos` then becomes an authoritative start-of-tick snapshot that
every neighbour query reads, and the entire tick becomes a Jacobi solve — order-independent,
thread-splittable, shard-splittable — for the cost of moving two lines.

### The passes

```
0.  Snapshot     prevPos, prevHeading := current           every read below is start-of-tick
1.  Broad phase  rebuild the dynamic index from prevPos    static index only when dirty
2.  Sense        gather <= K neighbours per ship           sorted by (distance^2, ShipId)
3.  Intent       SolveOrder -> AvoidNeighbours             desired heading and speed
4.  Integrate    the existing turn-rate and accel limiter  unchanged
5.  Resolve      separation deltas -> scratch, then apply  never read-modify-write in place
```

Pass 5 accumulates corrections into a scratch buffer and applies them after the loop has finished.
That makes it a Jacobi rather than a Gauss–Seidel solve. It is marginally softer per iteration, and
that is the trade being taken deliberately: Gauss–Seidel converges faster and is order-dependent,
which is precisely the property this design is spending effort to avoid.

Keeping these as separate whole-array passes rather than one fused per-ship loop is also what makes
the arrays vectorisable and lets pass 3 later run across worker threads. On an MMO server that is not
a micro-optimisation; it is the difference between one region per core and one region per machine.

---

## 7. The spatial index

### Three stores behind one query

`QueryCircle(centreX, centreZ, radiusMetres, out)` — not `NeighboursOf(shipId)`. The interest
management query, the weapons-range query and the blast-radius query are all already expressible in
that signature, and none of them has a ship at the centre.

Behind it, three stores, walked in fixed order:

| Store | Contents | Rebuild | Why separate |
|---|---|---|---|
| `m_dynamicFine` | hulls with bounding radius ≤ ½ cell — Interceptor through Hauler | every tick | The hot path. O(N), cache-linear. |
| `m_dynamicCoarse` | large mobile hulls — Battleship, Carrier | every tick | Few, and each would otherwise stamp into ~270 fine cells per tick. |
| `m_static` | Structure, Stargate, and anything else that never moves | on spawn/despawn only | A Structure spans ~4000 fine cells. Stamping it once is free; stamping it 60 times a second is not. |

The static/dynamic split is the larger of the two wins and the simpler one, because the biggest
objects in the table happen to be the ones that never move (§3, fact 2).

The two dynamic levels generalise: cell sizes are powers of two from a base, a hull goes into the
level where cell size ≥ 2 × bounding radius, and the query walks every level with a ring sized for
that level. Three levels are enough for the table in §3 and a fourth costs nothing until something
needs it. Do not special-case "capital ship" — special-casing a class is how a fourth class becomes a
rewrite.

### Dense counting sort, never a hash map

`std::unordered_map<cellCoord, std::vector<ShipId>>` is the obvious implementation and it is
**forbidden here**. Iteration order depends on hashing and on allocation addresses, which is exactly
the "iteration order that is not dense-array order" AGENTS.md §5 bans, and it would take the replay
gate red in a way that reproduces on one machine and not another.

Use the dense layout instead: count occupants per cell, prefix-sum into cell-start offsets,
counting-sort entity indices into one flat array. Rebuild from scratch each tick — O(N), no
incremental state that can drift, no tombstones, and trivially correct. Two `std::vector<uint32_t>`
and no allocation after the first tick.

### Cell size must not be able to change the answer

Design this in deliberately, because it is easy to lose and expensive to recover.

Gather **all** candidates from the covering cell ring, then sort by `(distanceSquared, ShipId)`, and
only then truncate to K. `ShipId` is the tie-break and is what makes the sort total.

Truncating cell-by-cell instead would make cell size part of the replay contract, and it could then
never be retuned — not per region, not per server, not after profiling. With sort-then-truncate, cell
size and level count are pure performance knobs, testable against a brute-force scan, and free to
differ between a dense-battle region and an empty one.

**K itself is in the contract**, because truncation genuinely changes the answer. Set it generously
(16 is ample for avoidance) and record it in `SimTuning.h` alongside the other contract values.

### Naming

`SpatialIndex`, not `ShipGrid`. Structures go in it, and so eventually do projectiles, mines, wrecks
and sensor contacts. A name that says "ship" is a name that gets worked around.

---

## 8. Narrow phase

For each candidate pair surviving the broad phase, closest distance between the two capsule segments;
overlap is `(rA + rB) - distance`; the contact normal is the vector between the closest points,
normalised. Degenerate case — closest points coincident, i.e. concentric hulls — falls back to a
deterministic fixed normal chosen from the two `ShipId`s, never to a random or an
uninitialised-memory direction.

That is the whole of it. No mass, no momentum, no restitution, no impulses. This is an RTS: units
nudge each other apart and give way. Rigid-body response would make a fleet behave like a break shot
and would put an integrator's stability into the replay contract.

---

## 9. Separation: authority, not symmetry

With a 31:1 mobile size ratio, symmetric separation is wrong, and it looks wrong. A Carrier shouldering
aside for an Interceptor reads as a bug to anyone watching.

Each hull carries an `avoidanceAuthority`. For a contacting pair, the share of the correction each
side takes is:

```
shareA = authorityB / (authorityA + authorityB)
shareB = authorityA / (authorityA + authorityB)     shareA + shareB == 1
```

Both parties compute the same split from the same two numbers, so the pass stays order-independent
and no arbitration step is needed.

This one mechanism covers every case that would otherwise be a separate code path:

- **Two Interceptors** — equal authority, 50/50, both ease aside.
- **Interceptor and Carrier** — the Interceptor does substantially all of the work.
- **Anything and a Structure** — `immovable` means share 0, so the ship is projected fully out along
  the contact normal and the Structure does not move. Hard blocking against structures and soft
  push between ships fall out of the same three lines.
- **A parked formation under traffic** — give `Idle` ships elevated authority and a fleet sitting on
  station stops being shoved off it by passers-by. Formation drift is not a separate problem; it is
  this one with a different number.

Authority should be an authored per-hull value rather than derived from radius or mass, so that a
hull which is small but immovable in intent — a picket, a deployed turret — is expressible without a
special case.

### The symmetry that authority does not break

Two identical hulls meeting head-on have equal authority and mirror each other's correction exactly.
They deadlock, or oscillate — the pedestrian sidewalk dance, and at fleet scale it is very visible.

Break it with a rule, not with a tie-break on `ShipId`: **give way to starboard.** When a pair is
near-symmetric and closing head-on, both bias their avoidance to their own right. It is
deterministic, needs no shared state, breaks the symmetry without either party knowing the other's
id, and reads on screen as intentional seamanship rather than as jitter. It is also what actual
vessels do, which is not an accident — the problem is the same one.

---

## 10. Avoidance: time, not distance

### Why distance-based scoring fails once speeds differ

A ship 200 m away closing at 68 m/s matters more than one 40 m away closing at 2 m/s. Any scoring
function built on separation distance gets that backwards, and gets it more wrong the wider the speed
spread becomes.

Score on **time to closest approach**. For relative position `p` and relative velocity `v`:

```
tca         = max(0, -dot(p, v) / dot(v, v))
missDistance = length(p + v * tca)
```

A neighbour is a threat when `missDistance < rA + rB + margin` and `tca < horizon`. Everything else
is ignored no matter how close it currently is — which is what allows ships to pass each other
closely and calmly when they are not actually converging.

### Context steering, not ORCA

The instinct is RVO2/ORCA, and it is the quality standard for crowd avoidance. It is the wrong fit
here for a specific reason: **ORCA assumes holonomic velocity control** — agents that may select any
velocity vector each step. These ships have a hard turn-rate limit and cannot strafe
(`Movement.cpp:44-50`). Adopting ORCA means either replacing a motion model that already works and
is tested, or spending every tick fighting the limiter that clamps ORCA's chosen velocity into
something the hull can actually do.

Context steering fits the code that exists. Sample N candidate headings around the ship; score each
for interest (does it close on the order point) and danger (what is the worst time-to-closest-approach
along it); take the best. It handles the turn-rate constraint natively, because only reachable
headings are scored in the first place. It is cheap, it is trivially deterministic, and it produces
the arcing give-way motion that looks right.

It drops into the existing code with minimal disturbance. Split `Movement.cpp` into three pieces:

```cpp
MotionIntent SolveOrder(const ShipState& _ship, const HullSpec& _hull) noexcept;

MotionIntent AvoidNeighbours(const ShipState& _ship, const HullSpec& _hull,
                             MotionIntent _intent, std::span<const Neighbour> _neighbours) noexcept;

void IntegrateShip(ShipState& _ship, const HullSpec& _hull, MotionIntent _intent) noexcept;
```

`SolveOrder` is the first half of today's `StepShip`; `IntegrateShip` is the second half, unchanged.
Each is unit-testable alone, which is the reason `Movement.h` exists as its own file. And the middle
signature is an interface: if ORCA is ever wanted, it goes behind that signature and nothing above or
below it moves.

### The horizon is per-hull and derived

A Carrier turning at a few degrees per second needs to begin avoiding far earlier than an Interceptor
turning at two hundred. A single global look-ahead constant would be either uselessly short for the
Carrier or absurdly long for the Interceptor.

Derive it: `horizonSec ≈ π / turnRateRadPerSec + maxSpeed / deceleration` — the time to reverse
course plus the time to stop. That makes the horizon a consequence of the hull's own agility rather
than a magic number needing separate tuning per hull.

The query radius follows from the horizon and is therefore per-ship and per-tick:

```
queryRadius = (ownSpeed + fastestNeighbourSpeed) * horizonSec + ownBoundingRadius + largestNeighbourRadius
```

`QueryCircle` taking an explicit radius is what makes this work — a fast ship naturally sweeps a
wider cell ring than a slow one, and cell size stays a performance knob (§7).

---

## 11. Tunnelling: an invariant to check, not to assume

Discrete overlap tests miss a contact entirely when the pair passes through each other inside a
single tick. The condition for safety is that relative displacement per tick stays below the pair's
combined radii; the conservative and easily checked form is:

```
maxSpeedMetresPerSec * TICK_DT  <  smallest capsuleRadiusMetres in the table
```

**Today this holds, with less headroom than the bounding radii suggest.** At `MAX_SPEED = 34` and
60 Hz a ship covers 0.567 m per tick, 1.13 m for a head-on pair, against a combined Interceptor
capsule radius of 2.24 m. That is a factor of two — not the factor of six the 3.51 m bounding radii
would imply. Fitting capsules to elongated hulls improves shape fidelity and *narrows* the tunnelling
margin, and that trade should be made with the number in view.

**It stops holding at roughly 67 m/s** — `1.12 m ÷ (1/60 s)` — for the current smallest hull. That is
not a distant limit. "Fast ship" in any ordinary reading of the request lands near or past it, and a
missile or projectile is far past it.

Therefore: **a test walks the whole `HullSpec` table and asserts the inequality for every hull.** The
day someone adds a 90 m/s Interceptor or a 300 m/s missile, the suite goes red naming the hull,
rather than the hull going through another hull in a live match six months later. This is the entire
value of the design point — not the inequality, which is arithmetic, but the fact that it is enforced
by a gate instead of remembered.

The escape hatch, named now and built when the test first goes red: hulls flagged fast get a swept
narrow phase — segment-versus-capsule along the tick's travel — applied to that class only, so the
cost lands on the few hulls that need it and not on the fleet.

---

## 12. Where this stops: pathfinding

**Local avoidance is not pathfinding, and large static structures are what makes the difference
matter.** A ship steering locally around a 503 m Structure will hug it, and can be trapped in a
concave pocket or orbit it indefinitely. No amount of tuning in §10 fixes that, because the
information needed — that the way around is left, not right — is not available locally.

This design does not solve it, and should not: pathfinding is a larger piece of work with its own
data structures, and bolting an approximation of it into the avoidance scorer produces something that
is neither.

What this design does is **name the seam**, in the same spirit as `NeuronCore/Transport.h`. The
intent layer steers toward a single point. Today that point is `ShipState::orderPos`. When
pathfinding lands, it becomes the current waypoint from a path follower, and `SolveOrder`,
`AvoidNeighbours` and `IntegrateShip` are all unchanged. Renaming the field `steerTargetPos` when
this work lands costs nothing now and makes the seam visible to whoever arrives next.

Until then, structures are avoidable by local steering only, which is adequate while they are sparse
and convex, and stops being adequate the day a station has an interior.

---

## 13. Consequences for code that already exists

Three things in the tree become wrong the moment hulls have real sizes, and none of them is
discovered by the collision code itself.

**`FORMATION_SPACING` cannot stay a constant.** It is 34 m. A Carrier's bounding radius is 107 m, so
a formation of Carriers is born with every hull deeply inside its neighbours, and the separation pass
would spend the rest of the match pushing them apart while `IssueMoveOrder` pushes them back
together. Spacing has to be derived from the largest hull in the ordered group — roughly
`2 × maxBoundingRadius × margin` — which makes it a property of the order, not of the game.

**`ARRIVAL_RADIUS` cannot stay a constant either.** It is 3.5 m, which is a sensible tolerance for a
3.5 m Interceptor and an unreachable one for a 107 m Carrier: the hull is thirty times larger than
the tolerance it is asked to stop within. It should scale with hull size.

**`Movement.h:8` and AGENTS.md's "deliberately not here yet" list both say there is no avoidance.**
AGENTS.md §"What is actually here" requires that a change making one of its sentences false updates
that sentence in the same commit. Both are part of phase 4's diff, not a follow-up.

---

## 14. What is in the replay contract

`SimTuning.h`'s header states the rule: a value on the simulation side is part of the replay contract
and cannot change without invalidating recorded games. This design adds values on both sides of that
line, so the split is worth writing down explicitly.

**In the contract** — changing any of these changes the outcome of a recorded match:

- Every field of `HullSpec`: radii, half-lengths, speeds, accelerations, turn rates, authorities.
- `TICK_HZ`, as today.
- The avoidance horizon coefficients, the danger margin, the separation stiffness.
- **`K`**, the neighbour cap, because truncation changes which neighbours are seen (§7).

**Not in the contract** — free to retune at any time, including per region on a live server:

- Base cell size and level count in `SpatialIndex`, *provided* the sort-then-truncate ordering of §7
  is preserved. That proviso is the whole reason that section is written the way it is.
- Any scratch buffer capacity, reserve size or allocation strategy.
- Whether the fine level is rebuilt on one thread or eight.

---

## 15. Phasing

Each phase is shippable, each is independently useful, and each is testable before the next begins.

| Phase | What lands | Visible change |
|---|---|---|
| 1 | `HullSpec` table, capsule shapes, the tunnelling test (§11) | None. Per-hull motion becomes possible. |
| 2 | `SpatialIndex` — static store, dynamic levels, `QueryCircle` | None. Ships still pass through each other. |
| 3 | Pass 0 hoist, pass 5 separation with authority | Ships stop overlapping. Structures block. |
| 4 | `SolveOrder` / `AvoidNeighbours` / `IntegrateShip` split, context steering, starboard rule | Ships give way while under way. |
| 5 | Formation and arrival scaling (§13) | Capital formations stop being born in collision. |
| 6 | Interest management reuses `QueryCircle` when `Transport` lands | No new spatial code at all. |

Phases 1 and 2 change no behaviour, which makes them safe to land and review on their own — the
replay gate must be green across both without a single tuning value moving. That is a genuine
property worth having, and it is the argument for this ordering over the tempting one where
separation lands first and the index is retrofitted under it.

Phase 6 is the payoff, and the reason §1 is written the way it is.

---

## 16. Tests

The suite is the gate, so these are part of the design rather than a follow-up.

**Determinism and structure**

- *Permutation invariance.* Spawn the same fleet in two different array orders, run N ticks, assert
  matching positions. This is the test that protects the MMO property. It is about twenty lines and
  it is the one that will actually catch the day someone writes a pass that mutates in place.
- *Replay equality with a mixed fleet* — extend `TheSameOrderProducesTheSameRun` to a fleet spanning
  Interceptor through Carrier with a Structure present.

**Index**

- *Brute-force agreement.* Random configurations spanning the full size range; `QueryCircle` returns
  the same set as an O(N²) scan. Catches every cell-ring off-by-one, and proves the §7 claim that
  cell size cannot change the answer — run it twice at different cell sizes and compare.

**Separation and avoidance**

- *No overlap after a head-on pass*, at every pairing of small and large hull.
- *A capital holds course* while a fighter crossing its bow yields (§9).
- *An equal head-on pair both break starboard*, clear each other, and do not oscillate.
- *A ship never ends inside a Structure, and the Structure never moves.*
- *A parked formation does not drift* while traffic passes through it.
- *A hundred-ship dense spawn separates* with bounded energy rather than exploding.

**Invariants**

- *Tunnelling* (§11) — `maxSpeed × TICK_DT < minCapsuleRadius` across the whole table.
- *Formation slots are collision-free at issue* — extend `AFormationOrderSpreadsShipsOut` to assert
  slot separation against the group's actual hull radii rather than against a bare 1.0 m.

---

## 17. Alternatives considered and rejected

| Alternative | Why not |
|---|---|
| One uniform grid | 72:1 size ratio (§3). Fine cells make a Structure span ~4000 of them; coarse cells make a fighter engagement a linear scan. |
| Quadtree or BVH for the dynamic set | Rebuild or refit cost per tick, and a traversal order that is harder to keep deterministic, in exchange for handling a size distribution the static/dynamic split has already removed. |
| `unordered_map` spatial hash | Iteration order depends on hashing and allocation. Banned by AGENTS.md §5 and would fail the replay gate non-reproducibly. |
| ORCA / RVO2 | Assumes holonomic velocity control; these hulls are turn-rate limited (§10). Retained as a possible future implementation behind `AvoidNeighbours`. |
| Boids-style separation forces alone | Oscillates, does not read as giving way, and has no principled answer to mixed speeds. |
| Oriented boxes | SAT plus a face/edge normal decision, for a difference not visible from an RTS camera (§4). |
| Rigid-body response with mass and impulses | Wrong feel for an RTS, and puts integrator stability into the replay contract (§8). |
| Continuous collision everywhere | Unnecessary at current speeds and a cost on every pair. Kept as a per-class escape hatch behind a gate that fires when it becomes necessary (§11). |
| Fixed-point maths | Only justified by lockstep. The tree is server-authoritative (§2). |
| Sharding by entity | The index is a spatial structure; regions are spatial. Shard by space. |

---

## 18. Open questions for review

1. **Is server-authoritative settled?** §2 assumes it and a great deal follows. If lockstep is
   genuinely on the table, fixed-point has to be decided before phase 1, not after phase 6.
2. **Does the Stargate collide at all?** It is 263 × 243 and is presumably flown *through*. A capsule
   is the wrong shape for a ring. Options: no collision at all plus a trigger volume, an annulus as a
   bespoke shape, or four capsules approximating the ring. This is the one entry in §3 the shape
   model in §4 does not cover.
3. **Do friendly hulls block each other, or only push?** This document assumes soft push between
   ships and hard blocking against structures. Under latency, hard blocking between player ships
   enables body-griefing and worsens client prediction error. Worth deciding before phase 3, since it
   is a property of the authority numbers rather than of the code.
4. **How many neighbours is K?** 16 is proposed. It is in the replay contract (§14), so it is
   cheaper to set generously now than to raise later.
5. **Does `Design/` become the home for documents of this kind?** `NeuronCore/Transport.h:8` already
   cites a `Design.md` that does not exist in the tree. If yes, that dangling reference should be
   pointed at a real file, and AGENTS.md §2's repository map should gain a row.
6. **File naming.** AGENTS.md §1 R7 governs `.h`/`.cpp` and is silent on Markdown; `AGENTS.md`
   itself is upper-case. This file is `collision.md` as requested. If the convention should be
   `Collision.md`, it is a rename now and a habit later.
