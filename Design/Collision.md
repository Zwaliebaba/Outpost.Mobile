# Collision and avoidance

**Status: phases 0, 1a, 1b, 2, 3, 4, 5 and 7 of §15 are implemented and under test.**
Phases 2b (loopback transport), 6 (interest management) and 8 (sectors) are not.
Decisions taken at review are recorded in §18. A second round the same day settled the process
model (§2), universe coordinates (§3), and brought pathfinding into scope (§12, phase 7).

Where building it proved a proposal here wrong, the code carries the correction and says so at the
site, and the commit that made it records the measurement. The substantive ones: the neighbour list
sorts by surface proximity rather than centre distance (§7), because a 72:1 size ratio makes those
different questions; the per-tick separation clamp caps what a *pair* closes before splitting it
rather than capping each ship afterwards (§9), because the latter inverts the authority split
whenever it binds; the avoidance horizon takes the longer of the hull's own agility and the time to
clear that particular neighbour (§10), because 1.8 s of look-ahead cannot clear a 107 m hull;
avoidance yields on the same authority split as separation (§9, §10); and `ShipHandle` carries a
stable slot rather than the ship's own index (§6), because the shorter form leaves a live handle
dangling whenever an unrelated ship despawns.

This document proposes how ships stop passing through each other and each other's structures, and
how they give way while under way. It is written against the MMO target — many players connected in
parallel, one authoritative server — because the structure that serves collision is the same
structure that serves interest management, and choosing it twice would mean choosing it wrong once.

Pathfinding, which the first draft excluded, is proposed in §12 and lands as phase 7.

---

## 1. This is not a collision system

The thing being built is a **spatial index owned by `World`**. Collision is its first customer.

Its second customer is interest management: deciding which entities each connected player's snapshot
contains. That is the hardest problem in the MMO target, because snapshot cost is O(N²) in connected
players without it and O(N · k) with it. Weapons range, sensor range, blast radius and region
ownership are customers three through six.

So the design question is not "which collision algorithm". It is **what query does `World` expose,
is it deterministic, and does it shard**. Get that right and the rest is replaceable.

### What the second customer needs that this does not give it

The index is the right *primitive* for interest management. It is not the whole of it, and an
earlier draft of this document implied that it was. The two queries differ in shape:

| | Collision | Interest management |
|---|---|---|
| Rate | Every tick | 5–20 Hz, decoupled from the tick |
| Result needed | The current set | The **delta** — who entered, who left since this player's last update |
| Precision | Exact | Conservative over-inclusion is fine; cell subscription is enough |
| Cost driver | O(N) rebuild | Per-player persistent state and send priority |

A `QueryCircle` returning a fresh sorted set every tick leaves interest management diffing sets per
player per tick to derive spawn and despawn messages — which is the O(N · k) cost this section
claims to avoid. Real interest management keeps a per-player **subscribed-cell set** and derives
enter and leave from changes to that set, with a priority accumulator so distant entities update at
a lower rate than near ones.

So the honest claim is narrower than "interest management is free": **`QueryCircle`, the cell
decomposition and the static/dynamic split are all reused unchanged; the subscription set, the
enter/leave delta and the priority accumulator are new code that sits on top of them.** §15 phases
it accordingly, and does not pretend it is a small phase.

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

### The condition that "float is fine" actually rests on

Float divergence is harmless here **because exactly one machine simulates any given entity**. That
is a stronger condition than "server-authoritative", and three things on the roadmap weaken it:

- **Region hand-off.** A ship crossing from one region server to another continues its motion on a
  different machine. If the two machines differ in architecture, the hand-off is a small position
  and heading discontinuity every time — not a desync, but visible jitter at region borders, which
  are exactly the places players congregate.
- **Replay verification off-box.** A recording made on the machine that produced it replays there.
  Replaying it elsewhere to diagnose a report is only meaningful if the arithmetic matches.
- **Client-side prediction.** The client re-simulates its own ships ahead of the snapshot. Divergence
  between an ARM64 client and an x64 server does not desync anything — the next snapshot corrects it
  — but it enlarges the correction, and correction size is what the player feels as rubber-banding.

None of these is fatal and none justifies fixed-point. What they justify is one rule, recorded here
so it is not discovered later:

> **Servers are x64 only. ARM64 is a client configuration.** All machines simulating the same world
> run the same binary for the same architecture.

Prediction error is then a design budget rather than an accident, and float divergence contributes
to it only on the client's own copy, where a snapshot is already arriving to correct it.

### One executable, for every phase in this document

Server-authoritative does not mean two processes, and this document does not schedule two
processes. **Every phase in §15 runs client and server together in one `Outpost.exe`** — which is
how the tree already runs (`NeuronCore/Transport.h`), and stays how it runs until well after
phase 6.

What changes at phase 2b is the *code* boundary, not the process boundary: from 2b onward the
client half touches the world only through a loopback `Transport` and the snapshots it carries,
with artificial latency available in the loopback for the measurements this document needs. The
split into a separate server executable is then a transport swap plus a composition root, not a
redesign — which is the entire point of paying for 2b early. Separation itself is a named later
step, deliberately outside this document's scope, taken when there is an operational reason — a
persistent world to host — rather than an architectural one.

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

### The universe is huge, and float cannot carry it

The stated ambition is **thousands of star systems**, with a small accessible area on day one. That
number belongs in this section because it kills a coordinate representation the same way the size
ratio killed the uniform grid.

A 32-bit float has a 24-bit mantissa, so absolute position resolution degrades with distance from
the origin: about 6 mm at 100 km out, about 1 m at 10,000 km, and visible jitter well before
either. `XMFLOAT3 posWorld` is therefore a day-one representation, not a universe one. The obvious
replacements are the wrong ones:

- **`uint64` coordinates.** Coordinate *differences* go negative constantly, and unsigned
  subtraction wraps silently. If a coordinate is an integer, it is `int64`.
- **A single global integer or fixed-point coordinate.** It forces integer arithmetic through every
  distance, dot product and trig call in the simulation. §2 rejected fixed-point math and those
  reasons stand — the problem here is float *range*, not float *arithmetic*.
- **`double` everywhere.** Buys range, halves SIMD width, doubles the cost of every position on the
  wire, and precision is still non-uniform across the map.

The representation that solves range, keeps every line of float math in this document unchanged,
and hands the MMO its sharding unit in the same move is **sector plus local offset**:

```cpp
struct WorldPos
{
  std::int64_t sectorX = 0;   // which sector
  std::int64_t sectorZ = 0;
  float localX = 0.0f;        // metres within it, [0, SECTOR_SIZE)
  float localZ = 0.0f;
};
```

With `SECTOR_SIZE = 8192.0f`, local precision is `8192 / 2^24 ≈ 0.5 mm` — **uniform in every
sector, everywhere in a universe of ±10¹⁹ m**, which is comfortably past thousands of star systems.
Everything in this document — the index, the narrow phase, avoidance, the pathfinding grid — runs
in small local floats within a sector's frame and does not change. A cross-sector relative vector
is `sectorDelta × SECTOR_SIZE + localDelta`, computed in float, and that is safe *because this
design already made every interaction local*: §10 caps the query radius near 700 m, an order of
magnitude under a sector. The renderer rebases camera-relative on its own side of the line, which
it wants at these scales regardless.

The same structure is the MMO's spatial currency. The sector is the natural unit of region sharding
(§17 — shard by space), the ghost zone is a ring of cells along a sector border, and a snapshot
position compresses to a sector id plus a quantised local offset.

None of the machinery is built now. **Phase 0 wraps `posWorld` in the `WorldPos` name while it is
still plain floats**, so position has one definition instead of being smeared across `ShipState`,
orders, formations, snapshots and the index. The sector fields land in phase 8 as a mechanical
change behind that name, and day-one content — near the origin, inside one sector — never notices.

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
  float maxTurnRateRadPerSec = 1.2217f;   // 70 deg/s -- today's global

  float avoidanceAuthority = 1.0f;        // see §9
  bool  immovable = false;                // structures

  [[nodiscard]] float BoundingRadiusMetres() const noexcept
  {
    return capsuleHalfLengthMetres + capsuleRadiusMetres;
  }
};
```

Two details in that struct are load-bearing and were wrong in the first draft:

- **`maxTurnRateRadPerSec`, not `turnRateRadPerSec`.** `ShipState` already has a
  `turnRateRadPerSec`, and it means live angular velocity, not a limit. Two fields with the same
  name and different meanings in the same simulation is a bug waiting for a tired afternoon.
- **Every default is today's `SimTuning.h` value, and none of them is zero.** §10 derives the
  avoidance horizon from `π / maxTurnRateRadPerSec`, so a default-constructed `HullSpec` with a zero
  turn rate divides by zero on first use. A default-constructed spec must be a working ship — the
  ship the tree already has — not an inert one.

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

### The other thing swap-and-pop breaks, and it is worse

Order-independence is only half of what despawn costs, and the cheaper half. The other half is
**identity**: `ShipId` *is* the array index (`GameLogic/ShipState.h`), and a swap-and-pop silently
retargets every stored copy of the moved ship's id to a different ship.

In the tree as it stands nothing stores a `ShipId` across ticks, so the hazard is invisible. Every
phase below adds a place that does:

| Stores a `ShipId` across ticks | Arrives in |
|---|---|
| Neighbour lists | Phase 2 |
| Separation scratch slots | Phase 3 |
| Weapon and order targets | Combat |
| Snapshot delta baselines, per player | The MMO target |

The last row is the one that hurts. A stale id in a delta baseline does not crash and does not
assert — the client smoothly interpolates one ship into a completely different ship, at a moment
when a player was watching, and the report that comes back says "ships teleport sometimes."

The fix is the standard one and it is small **now**: a generational handle.

```cpp
struct ShipHandle
{
  std::uint32_t index = 0;
  std::uint32_t generation = 0;   // bumped on despawn; 0 is never issued
};
```

The dense array does not change, iteration does not change, and the hot inner loops keep using the
raw index, so none of this design's performance argument is affected. The rule is simply: **anything
that stores a reference across a tick boundary, or sends one over a wire, stores a `ShipHandle`;
anything iterating within a tick uses the index.** Resolving a handle is an index plus a generation
compare, and a stale one resolves to nothing instead of to a stranger.

This is roughly sixty lines against today's tree, and it is a cross-cutting refactor of neighbour
buffers, scratch buffers, targeting and snapshot baselines once those exist. It is therefore
**phase 0** in §15 — not because collision needs it, but because it is the cheapest step available
that stops a later step from being a large one.

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

### Pass 5 gathers; it does not scatter

"Accumulates into a scratch buffer" admits two implementations, and only one of them survives
threading. This is normative, not an implementation note.

- **Scatter (forbidden).** Ship A visits the pair, computes the correction once, and writes `+δ` to
  A's slot and `−δ` to B's. Under threading that needs an atomic float add, and **float addition is
  not associative**: the sum depends on the order the threads happened to arrive. Determinism then
  depends on scheduling, which is the worst failure mode available — reproduces on one machine, not
  on another, not twice in a row under load.
- **Gather (required).** Each ship sums *its own* corrections by walking its own neighbour list and
  recomputing the pair term from its own side. No atomics, no shared writes, order-stable for free,
  and each ship's slot is written by exactly one thread.

Gather does the pair arithmetic twice, once from each side. That is the entire cost, and §9's
authority split is deliberately symmetric and stateless so that both sides derive the same numbers
without talking to each other. Paying two cheap closed-form evaluations to remove atomics and
scheduling from the replay contract is not a close call.

This is also what makes §6's claim of "thread-splittable" true rather than aspirational, and it is
why the same claim is repeated as a constraint in §14 rather than left as an aspiration.

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

**Build one dynamic level first.** The level-selection function lands in phase 2 with its signature
and its test, and returns level 0 for everything. The static store is the win that the §3 table
actually proves; the second dynamic level is a projection about Battleships and Carriers stamping
into fine cells, and it should be paid for by the phase 2 benchmark rather than by an argument.
Adding the second level once the structure exists is an afternoon; the structure is what has to be
right.

**The ring is derived, never a hardcoded 3×3.** With per-level cell sizes and a per-ship query
radius (§10), the number of cells a query must visit at level `l` is

```
ringCells = ceil((queryRadius + largestBoundingRadiusAtLevel(l)) / cellSize(l))
```

and it is routinely more than one. A hardcoded neighbourhood is the single most common way this
class of index goes subtly wrong, and it fails silently — a missed contact looks like a tuning
problem, not a bug. The brute-force agreement test in §16 is what catches it; this paragraph is what
tells the next reader that catching it was the point.

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

**K itself is in the contract**, because truncation genuinely changes the answer. Record it in
`SimTuning.h` alongside the other contract values.

**K cannot be chosen until §10's query radius is bounded.** "16 is ample" is only true against a
stated radius, and the first draft of §10 derived an unbounded one — at which point the K nearest
entities inside a two-kilometre circle are mostly irrelevant while the fighter three hundred metres
off the bow may not make the cut. §10 now caps the horizon; K is chosen against that cap, and the
choice is phase 4's, not phase 2's.

K also belongs in `HullSpec` rather than as one global. A Carrier legitimately tracks more
neighbours than an Interceptor, the storage is a scratch buffer sized to the table maximum either
way, and a per-hull value costs exactly the same in the replay contract as a global one.

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

The denominator is zero when both hulls are `immovable` — two structures authored overlapping, or a
deployed turret placed against a station. Neither can move, so the correct answer is to take no
correction at all; guard the divide and return zero shares rather than discovering the NaN when it
has already propagated through a position and into a snapshot.

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

### The correction is clamped, and the clamp is not optional

§16 asks that a hundred-ship dense spawn "separates with bounded energy rather than exploding."
Nothing above provides that. Overlap is unbounded — a fleet spawned on one point, or a ship squeezed
against a Structure by traffic behind it, produces a correction as large as the overlap, and a
Jacobi solve applying all of them at once overshoots and rings.

So: **the total correction applied to a ship in one tick is clamped**, expressed as a fraction of
its own capsule radius rather than as an absolute distance, so it scales with the hull the way
everything else in this design does. Deep overlap then resolves over several ticks — a fleet
unpacking itself — instead of in one frame as an explosion.

The clamp buys a second thing that matters more than the first. A per-tick bound on how far the
server may move a ship for reasons the client cannot predict **is** the prediction error budget
(§10). Without it, the worst case is unbounded and §18's question about hard blocking has no
answer; with it, the worst case is a number.

Two ordering rules fall out, and both are cheap:

- Corrections against `immovable` hulls are applied **after** ship-to-ship corrections and are
  **not** subject to the clamp, so traffic can never squeeze a ship through a Structure. Hard
  blocking has to be hard, or it is decoration.
- A ship's post-correction position is never inside an `immovable` hull, and that is the assertion
  the §16 structure test makes — on the final position, not on the correction.

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

`dot(v, v)` is zero whenever two ships have identical velocity, and that is not an edge case: it is
**a formation flying in company**, which is the single most common arrangement in the game. Guard it
— zero relative velocity means the separation never changes, so `tca` is 0 and `missDistance` is the
current distance. Getting this wrong produces a NaN in the most ordinary situation the game has,
which is the kind of bug that ships.

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

**A plain per-tick argmax chatters, and §16 has a test that will fail because of it.** Whenever two
candidate headings score within noise of each other — which is the *normal* condition for a
symmetric head-on pair, the exact case §9's starboard rule creates — the winner flips left, right,
left on successive ticks and the ship shivers down the middle. It is the best-known failure mode of
context steering and it is not fixed by tuning the danger weight.

Fix it with hysteresis: **score the heading chosen last tick with a small continuity bonus**, so
switching requires beating it by a margin rather than by an epsilon. One constant, in the replay
contract (§14), and it turns "does not oscillate" from an aspiration into a property. It also reads
better on screen: a ship that commits to its give-way turn looks decided, and a ship that re-decides
every 16 ms looks broken even when it eventually arrives.

That bonus is the one piece of per-ship state the avoidance pass needs to carry between ticks. It
lives in `ShipState` — one float — and it is simulation state, so it goes over the wire like
everything else there.

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

### The horizon is per-hull, derived — and capped

A Carrier turning at a few degrees per second needs to begin avoiding far earlier than an Interceptor
turning at two hundred. A single global look-ahead constant would be either uselessly short for the
Carrier or absurdly long for the Interceptor.

Derive it: `horizonSec ≈ π / maxTurnRateRadPerSec + maxSpeed / deceleration` — the time to reverse
course plus the time to stop. That makes the horizon a consequence of the hull's own agility rather
than a magic number needing separate tuning per hull.

The query radius follows from the horizon and is therefore per-ship and per-tick:

```
queryRadius = (ownSpeed + fastestNeighbourSpeed) * horizonSec + ownBoundingRadius + largestNeighbourRadius
```

`QueryCircle` taking an explicit radius is what makes this work — a fast ship naturally sweeps a
wider cell ring than a slow one, and cell size stays a performance knob (§7).

**Run the numbers before believing the formula.** They are alarming, and the first draft of this
document did not compute them. Taking a plausible per-hull turn rate and today's global speeds:

| Hull | Turn rate | Derived horizon | Query radius |
|---|---|---|---|
| Interceptor | 200 °/s | 0.9 s + 1.0 s = **1.9 s** | ~136 m |
| Corvette | 70 °/s | 2.6 s + 1.0 s = **3.6 s** | ~265 m |
| Frigate | 30 °/s | 6.0 s + 1.0 s = **7.0 s** | ~510 m |
| Battleship | 12 °/s | 15.0 s + 1.0 s = **16.0 s** | ~1,240 m |
| Carrier | 5 °/s | 36.0 s + 1.0 s = **37.0 s** | **~2,730 m** |

A 2.7 km circle query, per Carrier, sixty times a second. Three things break at once:

1. **K stops meaning anything.** The sixteen nearest entities inside 2.7 km are mostly irrelevant,
   while the Interceptor three hundred metres off the bow may not be among them in a busy region.
2. **It sets a floor on region size.** Sharding by space (§17) needs a ghost zone at least as wide
   as the largest query radius, or a ship near a boundary avoids a world that stops existing at the
   border. A 2.7 km ghost zone means regions measured in tens of kilometres. That is a world-layout
   constraint arriving out of a collision document, and it is far cheaper to know now than after a
   map exists.
3. **It is the wrong quantity anyway.** A Carrier does not need to *avoid* thirty-seven seconds out.
   It needs to *decelerate* thirty-seven seconds out. Those are different problems.

So split it, and cap the one that drives the query:

```
avoidHorizonSec = min(pi / maxTurnRateRadPerSec + maxSpeed / deceleration, AVOID_HORIZON_MAX_SEC)
brakeHorizonSec =     pi / maxTurnRateRadPerSec + maxSpeed / deceleration      // uncapped
```

`avoidHorizonSec` is what §10 scores against and what sizes `QueryCircle`. `brakeHorizonSec` is what
`SolveOrder` uses to decide when to start shedding speed for the order point — a purely local
calculation with no neighbour query behind it, so its size costs nothing.

`AVOID_HORIZON_MAX_SEC` is a contract value (§14). Somewhere around 8–10 s keeps the largest query
radius under about 700 m, which keeps ghost zones and region sizes sane. The consequence is honest
and should be stated rather than hidden: **a Carrier cannot fully avoid on local steering alone**,
because its turning circle is larger than its own look-ahead. That is not a failure of the cap; it
is the same statement §12 already makes about pathfinding, arriving one section early. A capital
ship needs a route planned for it, and until phase 7 lands (§12), capital ships manoeuvre in open
space.

### What the client can and cannot predict

`AvoidNeighbours` reads the K nearest neighbours. On a live server the client's entity set is
truncated by interest management, so if the client's area of interest is narrower than the server's
avoidance query radius — and at hundreds of metres it will be — then **client and server compute
different avoidance by construction, every tick, for every ship the player owns.** That is a
permanent divergence, not a latency artifact, and no amount of reconciliation removes it.

Three ways out; this design takes the second:

1. Make the area of interest at least as wide as the largest avoidance query radius. Correct, and it
   couples interest-management cost to hull agility, which is exactly the coupling §1 exists to
   avoid.
2. **Avoidance is server-only and is not predicted.** The client predicts `SolveOrder` and
   `IntegrateShip` — the two halves of today's `StepShip`, which it already effectively runs — and
   takes the avoidance component from the snapshot. Divergence is then bounded by §9's correction
   clamp, which is precisely why that clamp is not optional.
3. The client predicts avoidance with a deliberately shorter horizon and accepts known drift.
   Cheapest to write, hardest to reason about later.

Option 2 is the strongest argument for the three-function split below, and it costs nothing extra:
the seam it needs is the seam the split already creates.

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

**Assert with headroom, not at the line.** A bare `<` passes at a margin of 1.001 and calls that
safe. The test asserts

```
maxSpeedMetresPerSec * TICK_DT  <  TUNNEL_HEADROOM * smallest capsuleRadiusMetres
```

The ratio today is `0.567 / 1.12 = 0.51`. That is the same factor of two this section already
quotes, said the other way round, and it means the honest choice of `TUNNEL_HEADROOM` is **0.6** —
tight enough to fire before the real limit, loose enough that the current fleet is green on day one
at 0.51. Setting it at 0.5, the number that first suggests itself, takes the suite red immediately.

Recording the live ratio next to the threshold in the assertion message is worth the two lines. The
value of this gate is not that it eventually fails; it is that someone reading a passing run can see
how much room is left.

### The tick rate is a server cost, and 60 Hz is a single-player assumption

Everything above is computed against `TICK_HZ = 60`, which is inherited from a game that runs one
world on one machine in front of one player. MMO servers rarely simulate at 60 Hz, because tick rate
multiplies against entity count *and* region count *and* server count; 20–30 Hz with client-side
interpolation is the ordinary choice, and `NeuronServer/ServerHost.h` already
provides the interpolation half of that through `InterpolationAlpha`.

If load later forces 20 Hz, the ratio above goes from 0.51 to `34 × 0.05 / 1.12 = 1.52` — past the
headroom line and past the real limit, which is to say **Interceptors would tunnel through each
other on the first tick**. Every tuning value in §14 changes at the same moment. That is a large
step, arriving unplanned, which is the specific failure this document is phased to avoid.

Two consequences, both cheap now:

- **The tunnelling test is parameterised on `TICK_HZ`,** never on a baked 1/60. Lowering the tick
  rate then goes red loudly, naming the hulls, instead of quietly halving the margin.
- **Decide the target server tick rate before phase 1**, because `HullSpec`'s speeds are authored
  against it. Designing for 20–30 Hz simulation with 60 Hz client interpolation costs nothing today
  and is a rewrite of the tuning table later. §18 records this as a decision rather than leaving it
  to be discovered by a profiler.

---

## 12. Pathfinding

**Local avoidance is not pathfinding, and large static structures are what makes the difference
matter.** A ship steering locally around a 503 m Structure will hug it, and can be trapped in a
concave pocket or orbit it indefinitely. No amount of tuning in §10 fixes that, because the
information needed — that the way around is left, not right — is not available locally. §10's
capped horizon sharpens the need: a Carrier's look-ahead is deliberately shorter than its own
turning circle, so capital ships *require* a planned route anywhere obstacles are dense.

The first draft of this document named the seam and stopped there. This revision proposes the
structure behind it, because the input pathfinding needs is something this design already builds:
**the static store from phase 2 is the obstacle set.** Nothing mobile is ever an obstacle — ships
route around architecture and *avoid* each other, and keeping those two problems separate is what
keeps both of them small.

### The structure: a clearance grid over the static store

Three steps, each cheap at the cadence it runs:

1. **Rasterise** the static store into a coarse occupancy grid — on structure spawn and despawn
   only, the same cadence the static index already rebuilds on. With the smallest obstacle at
   250 m, 32 m cells lose nothing that matters.
2. **Distance-transform** the occupancy into a clearance field: each cell stores the distance to
   the nearest occupied cell. One pass, only when occupancy changed.
3. **A\*** over cells constrained to `clearance ≥ hullRadius + margin`, then string-pull the cell
   path into a short waypoint list.

One grid serves every hull: an Interceptor's query threads a 40 m gap that the same query, asked
with a Carrier's radius, routes around. A formation plans once, with the largest hull's radius, so
the group takes one route and stays together.

Why this and not the alternatives (§17 records them): a tangent-visibility graph over inflated
obstacle circles gives prettier paths for sparse convex obstacles and structurally cannot handle
the day a station has an interior — concave geometry breaks it, and interiors are on the horizon
this document is built against. Flow fields are the right tool when hundreds of ships share one
goal; they can land later *behind the same waypoint seam* for fleet orders, and are far too heavy
as the first step.

### Where it runs and what it touches

Planning is **server-side, at order time** — a pure function of the static set and the two
endpoints, so it is deterministic with nothing added, provided the A\* tie-break is total:
`(f, g, cellIndex)`, for the same reason every other ordering in this document is total. Re-plan
when the static set changes or when the follower has deviated past a threshold; never per tick. The
path itself stays on the server — clients see the resulting motion through snapshots, and a path is
never wire data.

The integration is the seam as originally named. The intent layer steers toward a single point;
today that is `ShipState::orderPos`, renamed `steerTargetPos` when this lands. The path follower
feeds it the current waypoint and advances on arrival, and `SolveOrder`, `AvoidNeighbours` and
`IntegrateShip` are all unchanged — the planner changes *which point* is steered at, never *how*.

Grid cell size, the clearance margin and the tie-break change which path is found, which changes
recorded outcomes: all three are in the replay contract (§14).

This lands as **phase 7** (§15). It depends on the static store (phase 2) and the steering seam
(phase 4), and on nothing else here. Until it lands, structures are avoidable by local steering
only, which is adequate while they are sparse and convex.

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

The two scale together, and that is a trap rather than a convenience: if arrival radius and slot
spacing both grow with the hull, a Carrier's arrival radius can reach past its own slot and into the
next one, and ships "arrive" in each other's positions — a formation that assembles into the wrong
shape and never corrects, because every ship believes it is done. Constrain it, and assert it where
the order is issued:

```
arrivalRadius < 0.5 * slotSpacing
```

**`Movement.h:8` and AGENTS.md's "deliberately not here yet" list both say there is no avoidance.**
AGENTS.md §"What is actually here" requires that a change making one of its sentences false updates
that sentence in the same commit. Both are part of phase 4's diff, not a follow-up.

---

## 14. What is in the replay contract

`SimTuning.h`'s header states the rule: a value on the simulation side is part of the replay contract
and cannot change without invalidating recorded games. This design adds values on both sides of that
line, so the split is worth writing down explicitly.

**In the contract** — changing any of these changes the outcome of a recorded match:

- Every field of `HullSpec`: radii, half-lengths, speeds, accelerations, turn rates, authorities,
  and the per-hull neighbour cap `K` (§7).
- `TICK_HZ`, as today — and see §11 on why its value is not yet settled.
- The avoidance horizon coefficients, the danger margin, the separation stiffness.
- **`AVOID_HORIZON_MAX_SEC`** (§10), because the cap changes which neighbours are considered at all.
- **The steering continuity bonus** (§10), because it changes which heading wins a near-tie.
- **The per-tick correction clamp** (§9), because it changes how deep overlap unwinds.
- **Pathfinding grid cell size, clearance margin and A\* tie-break** (§12), once phase 7 lands,
  because they change which path is found.
- **`SECTOR_SIZE`** (§3), once phase 8 lands, because every stored position is denominated in it.

**Not in the contract** — free to retune at any time, including per region on a live server:

- Base cell size and level count in `SpatialIndex`, *provided* the sort-then-truncate ordering of §7
  is preserved. That proviso is the whole reason that section is written the way it is.
- Any scratch buffer capacity, reserve size or allocation strategy.
- Whether the fine level is rebuilt on one thread or eight.
- `TUNNEL_HEADROOM` (§11). It is a test threshold, not a simulation input — nothing reads it at run
  time and tightening it changes no recorded game.

**A constraint, not a value.** Pass 5 gathers rather than scatters (§6). This is listed here because
it is the one thing on this page that a well-meaning optimisation would quietly reverse: replacing
the duplicated pair arithmetic with a single scattered write is an obvious win on a profile and it
silently makes determinism depend on thread scheduling. Anyone touching that loop should find the
reason here rather than in a commit message.

---

## 15. Phasing

Each phase is shippable, each is independently useful, and each is testable before the next begins.

| Phase | What lands | Visible change |
|---|---|---|
| **0** | Generational `ShipHandle`; spawn and despawn (§6); the `WorldPos` wrapper (§3) | None. Nothing after this stores a raw index across a tick, and position has one definition. |
| **1a** | `HullSpec` table — speeds, accelerations, turn rates | **Yes.** A Carrier finally turns like a Carrier. |
| **1b** | Capsule shapes; the tunnelling test and `TUNNEL_HEADROOM` (§11) | None. Shape data exists; nothing reads it yet. |
| **2** | `SpatialIndex` — static store, **one** dynamic level, `QueryCircle`, and a benchmark | None. Ships still pass through each other. |
| **2b** | Loopback `Transport`, full-fidelity snapshot, artificial latency | None in a LAN-less build; the client stops reading `World` directly. |
| **3** | Pass 0 hoist, pass 5 gather-separation with authority and clamp | Ships stop overlapping. Structures block. |
| **4** | `SolveOrder` / `AvoidNeighbours` / `IntegrateShip` split, context steering with continuity bias, starboard rule | Ships give way while under way. |
| **5** | Formation and arrival scaling (§13) | Capital formations stop being born in collision. |
| **6** | Interest management **on top of** `QueryCircle`: subscription sets, enter/leave deltas, send priority | Many players, one server. |
| **7** | Pathfinding — clearance grid over the static store, A\*, waypoint follower (§12) | Ships route around structures; capitals become navigable near architecture. |
| **8** | Sectors — `WorldPos` grows its sector fields (§3) | None near the origin. The universe stops having an edge. |

### Why this ordering and not the shorter one

The first draft of this table had six phases and put the network last. Four changes, each of which
exists to keep a later step from being a large one.

**Phase 0 is new, and it is first because it is cheapest first.** §6 explains the cost of deferring
it: every phase below adds another place that stores a `ShipId`, and the last of them is a snapshot
baseline, where a stale id is a silent visual bug rather than an assert.

**Phase 1 splits, because half of it is a feature.** The original table said phase 1 has "no visible
change", and that undersold it: `HullSpec` alone makes a Carrier accelerate and turn like a Carrier,
which is a visible improvement to the game that exists today and can be shipped, played and judged
with no collision code anywhere near it. Landing it separately means the per-hull motion tuning is
settled before capsules, separation and avoidance all start depending on the same numbers.

**Phase 2 lands one dynamic level and a benchmark, not three levels and a hope.** The benchmark
records tick cost at N = 100, 1,000 and 5,000 in the test output. Without a number, the second
dynamic level is guesswork, and the discovery that the index is too slow arrives at phase 6 — which
is precisely the shape of failure this phasing exists to prevent. With a number, the second level is
a decision with evidence behind it, taken in an afternoon, whenever the evidence appears.

**Phase 2b is the important one.** The original table scheduled the first contact with the network at
phase 6, which inverts the risk order: phases 1 through 5 are single-process work of a kind this tree
has already done well, while the replication pipeline is the thing that has not been done at all.
Putting the untried work last is how a project discovers at the end that the previous five phases
assumed something untrue.

A loopback `Transport` with a full-fidelity snapshot — every entity, no interest management, a
configurable artificial latency — is small. `NeuronCore/Transport.h` is already
declared and already documents this exact plan ("a loopback implementation lands first and a network
one after it"). It changes no gameplay. And it converts the hardest open questions in this document
from judgement into measurement:

- §18's question about hard blocking between player ships is *unanswerable* without latency in the
  loop, because the answer is entirely about how large the resulting correction feels.
- §10's claim that avoidance should be server-only and unpredicted is a prediction about divergence,
  and 2b is where that prediction becomes a number.
- §9's correction clamp is calibrated against prediction error, which does not exist until 2b.

It also means phases 3 to 5 are developed against a client that already reads a snapshot rather than
the world, so the day the transport becomes a real socket, the simulation does not notice.

**Phase 6 is re-scoped, not re-timed.** §1 now states plainly what carries over unchanged (the index,
the cells, the static split, `QueryCircle`) and what does not (subscription sets, enter/leave deltas,
priority). It remains the payoff and the reason §1 is written the way it is. It is no longer
described as free.

**Phases 7 and 8 are new in this revision, and neither blocks the others.** Pathfinding (§12)
depends only on the static store and the steering seam, so it can land any time after phase 4, and
"when capitals meet architecture" is its honest trigger. Sectors (§3) are deliberately last *as a
representation change*, while the type that makes them cheap — `WorldPos` — is deliberately first,
in phase 0. That pairing is this document's phasing principle in miniature: pay for the seam
immediately, pay for the machinery behind it when the game demands it.

**No phase in this table is "split the process."** Client and server stay in one `Outpost.exe`
throughout (§2). Phase 2b puts the transport between them, and the day a separate server executable
is wanted, the work is a socket implementation of `Transport` plus a composition root — scheduled
then, not here.

### The property worth protecting

Phases 0, 1b and 2 change no behaviour, and 2b changes no behaviour a single-player build can see.
Phase 8 is designed to join them: day-one content sits inside one sector, where sector-plus-local
arithmetic is bit-identical to plain float — and the replay gate is what proves the design achieved
it. The gate must be green across all five without a single tuning value moving. That is a real
property, it is what makes each of them safe to land and review alone, and it is the argument for
this ordering over the tempting one where separation lands first and everything else is retrofitted
underneath it.

Phases 1a, 3, 4, 5 and 7 each change recorded outcomes, and each should say so in its commit.
Knowing which five of the eleven invalidate replays — rather than discovering it when a replay
fails — is most of what this table is for.

---

## 16. Tests

The suite is the gate, so these are part of the design rather than a follow-up.

**Determinism and structure**

- *Permutation invariance.* Spawn the same fleet in two different array orders, run N ticks, assert
  matching positions. This is the test that protects the MMO property. It is about twenty lines and
  it is the one that will actually catch the day someone writes a pass that mutates in place.
- *Replay equality with a mixed fleet* — extend `TheSameOrderProducesTheSameRun` to a fleet spanning
  Interceptor through Carrier with a Structure present.
- *A stale handle resolves to nothing* (§6). Spawn three, despawn the middle one, and assert the old
  handle does not resolve to the ship that swap-and-pop moved into its slot. This is the test that
  makes phase 0 worth its own phase.
- *Thread-split equivalence.* Run N ticks single-threaded and N ticks with the ship array split
  across workers; assert bit-identical results. Green trivially while everything is single-threaded,
  and it is what fails the day pass 5 is "optimised" from a gather into a scatter (§14).

**Index**

- *Brute-force agreement.* Random configurations spanning the full size range; `QueryCircle` returns
  the same set as an O(N²) scan. Catches every cell-ring off-by-one, and proves the §7 claim that
  cell size cannot change the answer — run it twice at different cell sizes and compare.
- *Query radii larger than one cell.* The same test with query radii deliberately spanning several
  cells at every level, because a hardcoded 3×3 neighbourhood passes every small-radius case and
  fails silently on the large ones (§7).
- *Benchmark, recorded not asserted.* Tick cost of the rebuild and of a representative query sweep at
  N = 100, 1,000 and 5,000, printed in the test output. It gates nothing; it is what phase 2's
  decision about a second dynamic level is made from, and what makes a later regression visible as a
  number rather than as a feeling.

**Separation and avoidance**

- *No overlap after a head-on pass*, at every pairing of small and large hull.
- *A capital holds course* while a fighter crossing its bow yields (§9).
- *An equal head-on pair both break starboard*, clear each other, and do not oscillate.
- *A ship never ends inside a Structure, and the Structure never moves.*
- *A parked formation does not drift* while traffic passes through it.
- *A hundred-ship dense spawn separates* with bounded energy rather than exploding — and no ship
  moves more than the §9 clamp in any single tick, which is the mechanism that makes the first half
  of the sentence true.
- *Traffic cannot push a ship through a Structure.* A ship pinned against an `immovable` by a column
  of ships behind it ends outside it, every tick. This is the ordering rule in §9, and it is the one
  case where the clamp must not apply.
- *A give-way turn does not chatter.* A symmetric head-on pair changes its chosen heading a bounded
  number of times over the encounter — not merely "clears" (§10). Without the continuity bias this
  fails, which is the point of writing it as a test rather than as advice.

**Pathfinding** (phase 7)

- *Same static set, same endpoints, same path* — byte for byte, which is what the total A\*
  tie-break buys (§12).
- *A concave pocket is escaped.* A ship ordered out of a U-shaped arrangement of Structures leaves
  it — the case local steering provably cannot solve, and therefore the test that proves phase 7
  earned its place.
- *Clearance respects the hull.* A gap the Interceptor's path threads is one the Carrier's path
  routes around, from the same clearance field.
- *A Structure spawned across an active path forces a re-plan*, and the steered point moves without
  a discontinuity the integrator would turn into a swerve.

**Degenerate inputs** — each of these is a divide by zero in the obvious implementation, and each is
an ordinary situation rather than an exotic one:

- *Two ships in company at identical velocity* — `dot(v, v) == 0` in the `tca` formula (§10). This is
  what a formation does for its entire journey.
- *Two `immovable` hulls overlapping* — zero authority denominator (§9).
- *A default-constructed `HullSpec`* — must be a working ship, and must survive the horizon
  derivation `π / maxTurnRateRadPerSec` (§5).
- *Concentric hulls* — the coincident-closest-points fallback in §8 produces a deterministic normal,
  and the same pair produces the same normal on a rerun.

**Invariants**

- *Tunnelling* (§11) — `maxSpeed × TICK_DT < TUNNEL_HEADROOM × minCapsuleRadius` across the whole
  table, **parameterised on `TICK_HZ`** rather than on a baked 1/60, and reporting the live ratio
  alongside the threshold so a passing run still shows how much room is left.
- *Formation slots are collision-free at issue* — extend `AFormationOrderSpreadsShipsOut` to assert
  slot separation against the group's actual hull radii rather than against a bare 1.0 m.
- *Arrival radius fits inside its slot* — `arrivalRadius < 0.5 × slotSpacing` for every hull in the
  table (§13), so a Carrier cannot arrive in its neighbour's position.

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
| Scatter accumulation in pass 5 with atomic float adds | Halves the pair arithmetic and makes determinism depend on thread scheduling (§6). The worst trade available: reproduces on one machine and not another. |
| An uncapped derived avoidance horizon | Gives a Carrier a 2.7 km query radius at 60 Hz, makes K meaningless and forces ghost zones tens of kilometres wide (§10). |
| Predicting avoidance on the client | The client's neighbour set is truncated by interest management, so it is a different input to the same function. Divergence by construction, not by latency (§10). |
| `ShipId` as a bare index, kept | Free today, and a silent snapshot-baseline bug later (§6). Generational handles cost sixty lines now. |
| Tangent-visibility graph for pathfinding | Ideal for sparse convex obstacles; structurally cannot handle a concave station or an interior, which are on this document's horizon (§12). |
| Flow fields as the first pathfinder | The right tool when hundreds of ships share one goal, and far too heavy as a first step. Can land later behind the same waypoint seam (§12). |
| `uint64` global coordinates | Coordinate differences go negative; unsigned wrap is silent. And a global integer coordinate forces the fixed-point math already rejected above (§3). |
| `double` positions everywhere | Buys range, halves SIMD width, doubles every wire position, and precision is still non-uniform. Sector + local float is uniform everywhere (§3). |

---

## 18. Decisions taken at review

Reviewed 2026-08-28. The questions the first draft left open are answered here rather than in a pull
request thread, so that the reasoning survives with the design. Where an answer is provisional, it
says so and names the phase that settles it.

**1. Server authority is settled — and it is settled more narrowly than "server-authoritative".**
Lockstep is not on the table and fixed-point is rejected (§17). The condition the float decision
actually rests on is that exactly one machine simulates any given entity, so: **servers are x64
only, ARM64 is a client configuration**, and all machines simulating one world run the same binary
(§2). Region hand-off across differing architectures would be a visible discontinuity at exactly the
borders where players gather.

**2. The Stargate does not collide.** No hull, plus a trigger volume for the transit itself. Four
capsules approximating a ring is real narrow-phase cost on every pair, every tick, for an object
players fly through on purpose; an annulus is a bespoke second shape and breaks the one-shape,
one-function property §4 is built on. If a Stargate later needs to *obstruct* something, that is a
new requirement and gets its own decision.

**3. Soft push between ships; hard blocking only against `immovable` hulls.** Hard blocking between
player ships enables body-griefing, and under latency it produces the largest possible prediction
error at the moment of contact. Structures block absolutely (§9), and the ordering rule there
guarantees traffic cannot squeeze a ship through one. **Provisional until phase 2b** — it is the
right default, and 2b is the first point at which the alternative can be judged against a measured
correction rather than an intuition.

**4. K is not chosen yet, and choosing it now would be choosing it wrong.** "16 is ample" was stated
against an unbounded query radius (§10). K moves into `HullSpec` as a per-hull value, and is set in
**phase 4** against the capped horizon, with the phase 2 benchmark showing what it costs. It is in
the replay contract either way, so the thing to get right is the number, not the timing.

**5. Yes — `Design/` is the home for documents of this kind.** `NeuronCore/Transport.h:8` cites a
`Design.md` that does not exist; point it at a real file and add the row to AGENTS.md §2's
repository map. Both are part of the same commit as the first phase that lands, per AGENTS.md's rule
that a change making one of its sentences false updates the sentence.

**6. File naming: `Collision.md`.** `AGENTS.md` is upper-case and every other authored file in the
tree is `PascalCase`; a lower-case exception here becomes the habit that makes the next one
arguable. Done — the file was renamed when phase 0 landed.

**7. The universe is sector + local float, committed now, built later.** The stated ambition is
thousands of star systems with a small accessible area on day one. Raw `uint64` coordinates, a
global fixed-point coordinate and `double` positions are all rejected (§3, §17); `WorldPos` —
an `int64` sector pair plus a local float offset at `SECTOR_SIZE = 8192 m`, giving ~0.5 mm
precision uniformly across ±10¹⁹ m — is the committed representation. The name lands in phase 0
while it is still plain floats; the sector fields land in phase 8, behaviour-neutral for day-one
content, with the replay gate as proof.

**8. Client and server stay in one `Outpost.exe` for every phase of this document.** The split at
phase 2b is a code boundary — from there the client reaches the world only through the loopback
`Transport` — and the two-executable separation is a later step with an operational trigger, not a
phase here (§2, §15).

**9. Pathfinding is in scope, as phase 7.** The first draft excluded it; §12 now proposes a
clearance-grid A\* over the static store, server-side at order time, feeding the existing steering
seam, with the grid parameters in the replay contract (§14). Tangent graphs and flow fields are
recorded as rejected-for-now alternatives (§17).

### Still genuinely open

These have no answer yet because the information needed does not exist in the tree.

- **The target server tick rate** (§11). 60 Hz is inherited from a single-player build and is
  unlikely to survive contact with many regions on many machines. This must be decided **before
  phase 1a**, because `HullSpec`'s speeds and accelerations are authored against it and rewriting
  the tuning table later is exactly the large step this phasing exists to avoid.
- **Minimum region size** (§10). It follows arithmetically from the largest capped query radius once
  `AVOID_HORIZON_MAX_SEC` is fixed, but it is a world-layout constraint rather than a code one, and
  whoever designs the map needs the number before the map exists. With §3's sectors the answer is
  denominated in sectors — a region is an integer number of them and the ghost zone is a ring of
  border cells — which turns the question from "pick a distance" into "pick a small integer".
- **When phase 7 becomes urgent** (§10, §12). Capping the avoidance horizon means a Carrier cannot
  fully avoid on local steering alone. That is acceptable while capitals manoeuvre in open space and
  stops being acceptable the day they are expected to navigate near architecture. The proposal is
  written (§12) and its dependencies are early (phases 2 and 4); only its scheduling is open, and
  the honest trigger is "when Carriers become common near structures", not a calendar date.
