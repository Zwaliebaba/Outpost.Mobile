# Work order — Stations slice 1: the layout

Implements slice 1 of [`Stations.md`](Stations.md) §16: `LayOutSystem`, a pure seeded function in
`GameLogic` that lays out the starting solar system — a star anchor and a few planets at real
positions on the plane — so that both halves of the game can read the same universe from the same
seed (design §5).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** nothing.
**Blocks:** slice 5, which calls it at boot to spawn the stations and place the worlds.

---

## 1. Why this is a slice

It is the only part of the feature with no dependency on any other part, and the only one that adds
a file rather than a field. Landing it alone means the layout's three properties — a seed means one
system forever, planets keep their distance, the span fits under the path grid's ceiling — are
proved by tests against nothing else in flight, which is what makes them worth citing later when a
station spawn goes wrong.

It is also the first randomness in `GameLogic`. AGENTS.md §5 has promised "one seeded PCG32 when
randomness arrives" since the tree began, and this is the arrival; the shape it arrives in — a pure
function of a caller-supplied seed, called at boot, whose output is then ordinary spawn input — is
the shape that keeps the replay contract seeing positions rather than a generator (design §10). A
slice that also spawned stations would settle that question in passing.

---

## 2. Scope

### 2.1 `GameLogic/UniverseLayout.h` — the types and the function

A new header, registered in `GameLogic.vcxproj`, its `.filters`, and the umbrella `GameLogic.h`
(after `Patrol.h`). `namespace Game`:

```cpp
struct PlanetSite
{
  WorldPos posWorld;          // where the planet -- and its station -- sit on the plane
  float radiusMetres = 0.0f;  // the body's visual radius, drawn by the client, ignored by the server
  float bearingRad = 0.0f;    // from the star, for anything that wants to face or frame it
  std::uint64_t bodySeed = 0; // what the client's BodyCatalogue generates the look from
};

struct SystemLayout
{
  WorldPos starPos;
  std::vector<PlanetSite> planets;
};

struct SystemDesc { /* the shipped bounds, as defaults -- 2.3 */ };

[[nodiscard]] SystemLayout LayOutSystem(std::uint64_t _seed, const WorldPos& _starPos, const SystemDesc& _desc);
```

### 2.2 `GameLogic/UniverseLayout.cpp` — the draw

One `Neuron::Pcg32(_seed)`, drawing **per planet in one fixed order**:

```
orbit metres      Float01 scaled into [minOrbitMetres, maxOrbitMetres]
radius metres     Float01 scaled into [minRadiusMetres, maxRadiusMetres]
bearing jitter    Signed(slot * PLANET_BEARING_JITTER * 0.5)
body seed         two Next() calls, high word first
```

**All four are drawn for every planet, including a pinned one.** `pinFirstPlanet` overwrites
planet 0's orbit and bearing *after* the draws; it does not skip them. Skipping would make the
pinned flag shift the stream for planets 1 and 2, so the same seed would mean two different systems
depending on a bool — which is exactly what "a seed means one system forever" forbids.

Bearings are not drawn free. Planet `i` sits at `i · 2π / planetCount` — its slot — plus a jitter
bounded to a quarter of a slot either side, so adjacent bearings are at least half a slot apart by
construction and no rejection loop is needed for the layout to be both deterministic and
non-overlapping (design §5.2).

Positions are built with `Translate` from `_starPos`, never by writing `localX`/`localZ`, so a
system laid out near a sector boundary holds `WorldPos`'s invariant (`WorldPos.h`).

**On the draw list.** Design §5.2 names three draws — "its orbit, its radius, its body seed" — and
in the same paragraph asks for a jittered bearing, which is a fourth. The list was naming the
varying quantities rather than counting draws; the order above is the contract and the header says
so at the definition.

### 2.3 The defaults are the shipped bounds

`SystemDesc`'s defaults are design §5.3's numbers: three planets, orbits 2 500–6 500 m, radii
400–1 200 m, unpinned. Slice 5 sets `pinFirstPlanet` and the two pinned fields and nothing else.

That is deliberate and it is what lets `TheLayoutRespectsTheGridCeiling` assert against the shipped
bounds from inside `GameLogicTests`, which cannot see `ViewTuning.h`. Hostiles slice 2 had the same
problem and had to manage it — its §5 says the scene numbers "become `ViewTuning.h` constants in
slice 3" and "the two must agree" — and putting the bounds in the defaults removes the duplication
instead of managing it.

### 2.4 `GameLogic/GameLogic.h` — one sentence that stops being true

The umbrella promises "no OS entropy — randomness, when it arrives, is one seeded generator **held
by World**". This slice is the arrival and it is not held by World. The sentence changes to say what
is actually true and is the stronger property anyway: a pure function of a caller-supplied seed,
called at boot, whose output is ordinary spawn input, so the replay contract sees positions and
never a generator.

AGENTS.md §5's wording ("one seeded PCG32 when randomness arrives") stays true and is not touched.

### 2.5 `AGENTS.md` §2 — the map row

The `GameLogic/` row names its headers. `UniverseLayout` joins them, with the one-clause reason it
is there and not in the executable.

---

## 3. What to build on

- **`Neuron::Pcg32`** (`NeuronCore/Pcg32.h`) — `Float01`, `Signed`, `Next`. `GameLogic` depends on
  `NeuronCore`, so the include is already legal.
- **`WorldPos`, `Translate`** (`GameLogic/WorldPos.h`) — the only two ways a position is ever set.
- **`SimTuning.h`** for the ceiling arithmetic: `PATH_CELL_SIZE_METRES` (32),
  `PATH_GRID_MARGIN_METRES` (512), `PATH_GRID_MAX_CELLS_PER_AXIS` (512).
- **`Patrol.h`** is the precedent for the shape — geometry owned by one function so that the root
  and the world cannot disagree about it. This one takes a `.cpp` where `Patrol.h` is header-only,
  because it is a loop with a generator in it rather than two closed-form expressions.
- **`BodyCatalogue::RandomBody`** (`Outpost/BodyCatalogue.cpp`) is the house pattern being followed
  — one generator, one fixed draw order, a seed means one body forever. It stays where it is: what
  a planet *wears* is still nobody's business but the client's.

---

## 4. Acceptance

**`Tests/GameLogicTests/UniverseLayoutTests.cpp`** — new file, in both project files under the
`Tests` filter.

- **`TheLayoutIsAFunctionOfItsSeed`** — two calls with one seed agree field for field, including
  `bodySeed` and the sector halves of every position; two adjacent seeds differ. A pinned layout and
  an unpinned one from the same seed agree on planets 1 and 2, which is §2.2's claim that pinning
  overwrites rather than skips.
- **`TheLayoutRespectsTheGridCeiling`** — the shipped `SystemDesc` defaults keep the worst-case
  static span under the ceiling: `2 · maxOrbitMetres + 2 · PATH_GRID_MARGIN_METRES` divided by
  `PATH_CELL_SIZE_METRES`, rounded up, is `< PATH_GRID_MAX_CELLS_PER_AXIS`. Asserted against the
  computed number, not a literal 439, so the day a tuning constant moves the test names the reason.
- **`PlanetsKeepTheirDistance`** — over many seeds, every pair of planets is at least
  `2 · minOrbitMetres · sin(π / (2 · planetCount))` apart, which is the bound the half-slot bearing
  separation guarantees. At the shipped numbers that is 2 500 m, design §5.3's "closest two
  stations". Positions stay inside `maxOrbitMetres` of the star, and a layout anchored near a
  sector boundary comes out with every `localX`/`localZ` inside `[0, SECTOR_SIZE_METRES)`.

**The existing suites**

- Every existing `GameLogicTests` test passes **without edits** — nothing calls the new function,
  so nothing in a tick changed. The pull request says the suite was run unchanged.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; the game runs exactly as before, because the composition root does not call
  `LayOutSystem` until slice 5.
- No screenshot: nothing visual until slice 5.
- One decision record: **the universe layout is static content in `GameLogic`** — ADR 0008's
  three-way elimination re-run for content both binaries need, with `BodyCatalogue` staying
  client-side (design §5.2, §13). Next free number after the NMO merge is **0037**; the index in
  `Design/Decisions/README.md` lists it.
- `Design/Archive/Stations.md` §16 marks slice 1 landed and this file moves to `Design/Archive/` — both in
  the merge commit, per Design/README.md. Until then §16 says *in review*.

---

## 5. Assumptions the implementer may make

- **Nothing calls it.** The function is dead code until slice 5, and that is the slice boundary
  rather than an oversight. It is what makes "every existing test passes unchanged" a real claim.
- **`sin`/`cos` under `/fp:precise` are same-binary-same-answer**, which is the only determinism
  this tree promises (Design/Archive/Collision.md 2). No lookup table, no fixed point — the same
  assumption `Patrol.h` already runs on.
- **One system.** `LayOutSystem` takes a star position precisely so a second call is content rather
  than redesign, but nothing calls it twice and no test needs two systems to coexist.
- **A `std::vector` in the return is fine.** This is boot-time content, not a tick, so the
  allocation is not on any hot path and does not need the fixed-capacity treatment `Route` has.
- **No star body.** The star is a layout anchor this phase; nothing draws it and no record exists
  for it (design §5.3, §14).
- **`planetCount` of 0 or 1 is well-formed** — an empty or single-planet layout, no pairs to
  separate. Not shipped, not special-cased, and the separation test skips what it cannot pair.
