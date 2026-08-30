# Work order — Collision slice 8: sectors

Implements slice 8 of [`Collision.md`](Collision.md) §19: `WorldPos` grows the `int64` sector pair
§3 specifies, so the universe stops having an edge.

**Layer:** `GameLogic`, plus the minimum in `Outpost` needed to keep the client correct.
**Depends on:** slice 0, which put every relative-vector operation behind `WorldPos`'s free
functions precisely so that this slice is a change to one header rather than a sweep of the tree.
**Blocks:** nothing. Slices 2b and 6 are independent of this one and may land before or after.

---

## 1. Why this is worth doing now rather than later

Nothing in the game today reaches a float's precision limit, so this buys no behaviour. It is on
the list now for one reason: **every stored position is denominated in `SECTOR_SIZE_METRES`**, so the
change invalidates recordings. It is cheap while nothing depends on recorded positions and it gets
steadily more expensive afterwards. Slice 0 was built to make this slice mechanical; this is where
that investment is cashed in, and the longer the gap, the more code accumulates that reads
`localX` as an absolute coordinate and quietly re-earns the problem.

---

## 2. Scope

### 2.1 `WorldPos` grows the sector pair

`GameLogic/WorldPos.h` becomes §3's struct, sector fields **first**:

```cpp
struct WorldPos
{
  std::int64_t sectorX = 0;
  std::int64_t sectorZ = 0;
  float localX = 0.0f;        // invariant: [0, SECTOR_SIZE_METRES)
  float localZ = 0.0f;
};
```

The field order is not cosmetic and is not negotiable — see §5.1.

### 2.2 `SECTOR_SIZE_METRES` is a simulation constant

Add to `GameLogic/SimTuning.h`, in the replay contract, where §14 already says it belongs:

```cpp
inline constexpr float SECTOR_SIZE_METRES = 8192.0f;
```

**`SECTOR_SIZE_METRES` must be a power of two, and that is a `static_assert`.** The carry in
`Translate` divides by it. At 8192 = 2^13 the division is an exponent adjustment and is exact, so
the carry is exact under `/fp:precise` on every machine. At, say, 10000 it is not, and the carry
becomes a rounding question — which is a determinism question, because two ships that reached the
same point by different routes could land in different sectors.

**Every cell size in use must divide it exactly**, or a cell straddles a sector boundary and its
index stops being a function of the position (§2.5). This one *cannot* be a `static_assert`, and
noticing why is half the work: `PATH_CELL_SIZE_METRES` (32) is a constant, but the index's cell
sizes are not — `baseCellSizeMetres` (256) and `staticCellSizeMetres` (512) are `SpatialIndex::Desc`
fields, deliberately runtime knobs because §14 puts them outside the replay contract and free to
retune per region on a live server. So:

- `static_assert` for `PATH_CELL_SIZE_METRES`, which is a constant and can be checked where it is
  defined.
- A check in `SpatialIndex::Configure` for the two `Desc` fields, in the shape `Configure` already
  uses — it sanitises rather than rejects, falling back to the default on a nonsensical value.

Make the rule **"a power of two, no greater than `SECTOR_SIZE_METRES`"** rather than "divides it".
Both give the same answer for every value in use, but the power-of-two form is one bit test instead
of a float remainder, it composes with the level cell sizes (which double per level) without a
second rule, and a request that fails it rounds *down to the next power of two* — which is
deterministic and explainable, where silently substituting 256 for a requested 300 is neither.

`WorldPos.h` must include `SimTuning.h` for the constant. That direction is safe today:
`SimTuning.h` includes only `<cstdint>`. Do not let it acquire an include of `WorldPos.h`.

### 2.3 The free functions carry the change

All four already exist and every caller in the tree already goes through them. Their bodies change;
their signatures do not.

| Function | Becomes |
|---|---|
| `OffsetX(a, b)` | `static_cast<float>(b.sectorX - a.sectorX) * SECTOR_SIZE_METRES + (b.localX - a.localX)` |
| `OffsetZ(a, b)` | the same in Z |
| `DistanceSquared`, `Distance` | unchanged — they are already written in terms of the offsets |
| `Translate(pos, dx, dz)` | add, then normalise (§2.4) |
| `Lerp` | unchanged — already written as `Translate` along an offset |

`OffsetX` is exact while `|sectorDelta| ≤ 2^24` (about 1.4 × 10^11 m of separation), and approximate
beyond it. That is not a defect to fix: §10 caps the query radius at 647 m, an order of magnitude
under one sector, so no interaction in this design spans even two sectors. Say so in the comment
rather than leaving the reader to work out whether the function is trustworthy.

### 2.4 `Translate` normalises eagerly

After displacing, carry whole sectors out of the local offset so the invariant in §2.1 holds on
every `WorldPos` the simulation ever stores:

```cpp
const float sectorsX = std::floor(_pos.localX / SECTOR_SIZE_METRES);
_pos.sectorX += static_cast<std::int64_t>(sectorsX);
_pos.localX -= sectorsX * SECTOR_SIZE_METRES;
```

**Eagerly, not lazily.** A lazy scheme lets the same point have several representations, and two
representations of one point hash to different cells, compare unequal, and sort differently. That
is a determinism bug that would surface as a replay divergence far from its cause.

`std::floor` is correct for negatives, which is the case that matters: translating a position at
`localX = 0` by −5 m must give `sectorX − 1` and `localX = SECTOR_SIZE_METRES − 5`, not `sectorX`
and `localX = −5`.

### 2.5 `SpatialIndex` cell indices widen

`CellOf(float, float)` currently takes a coordinate. It becomes sector-aware, taking the whole
position:

```
cellX = sectorX * (SECTOR_SIZE_METRES / cellSizeMetres) + floor(localX / cellSizeMetres)
```

Both terms are exact: the first because the divisor divides exactly (§2.2), the second because it
already is today. The result stays `std::int32_t`, which at the base cell size holds ±67 million
sectors — 5 × 10^11 m, past anything §3 contemplates. `CellHash` does not change.

`Rebuild` and `QueryCircle` both call it, and both must call it on the whole position. The existing
`Cell::cellX`/`cellZ` that reject ring-cell hash collisions keep working unchanged: they are now
sector-aware indices, and two cells in different sectors that hash alike still compare unequal —
which is exactly the property that guard exists for.

**`Cell` keeps a local offset, not a `WorldPos`.** This is the one place where the mechanical
change is not free, and it is measured: a `WorldPos` grows from 8 bytes to 24, which takes
`SpatialIndex::Cell` from **24 bytes to 48** and doubles the traffic through the hottest array in
the simulation. Storing the offset from the grid's own origin sector instead keeps `Cell` at 24
bytes. Since every cell in a grid is within a bounding radius of the query anyway, the offset is
small and the grid's origin sector reconstructs the full position on the way out.

This is a decision with a real alternative — pay the 2× and keep the code simpler — so it takes a
decision record (§6).

> **What happened.** The compact layout did not land. A grid origin taken from the first entry makes
> every stored offset, and so the rounding of every distance comparison in `Gather`, depend on the
> order entries arrived in — which is the exact failure
> `WorldTests::ArrayOrderCannotChangeTheAnswer` exists to catch. `Cell` is 48 bytes and the index
> costs about 25% more to rebuild and up to 30% more to query, measured. Two order-independent
> variants survive and one of them needs no origin at all;
> [ADR 0007](../Decisions/0007-the-index-stores-a-whole-position.md) has the numbers and the reasoning.

### 2.6 `PathGrid` origin becomes a position

`m_originX` / `m_originZ` (two floats, "the centre of cell (0, 0)") become one `WorldPos m_origin`.
Every `_metres - m_originX` becomes `OffsetX(m_origin, _pos)`; every
`m_originX + cellX * PATH_CELL_SIZE_METRES` becomes a `Translate` from `m_origin`. The bounds sweep
in `Rebuild` must take its minimum and maximum as **offsets from the first obstacle**, not as raw
field values, or a grid spanning a sector boundary gets a nonsense extent.

Nothing else in `PathGrid` changes. The A\* tie-break is on `cellIndex`, which is an index into
this grid and stays a total order.

### 2.7 The client keeps working, at sector zero

`Outpost/WorldView.cpp` (9 sites) and `Outpost/Hud.cpp` (2 sites) read `posWorld.localX` as an
absolute world coordinate. Each becomes an offset from a named origin:

- `WorldView` gets a `WorldPos m_viewOrigin`, set once per frame from the camera's focus. Ship
  placement, pick centres, selection rings and the two `at.localX` sites route through
  `OffsetX(m_viewOrigin, pos)`. This is what §3 means by "the renderer rebases camera-relative on
  its own side of the line", and it is what the renderer wants at these scales regardless.
- `Hud`'s minimap already maps world to map space through `toMapX`/`toMapY`. Feed those an offset
  from the map's own centre rather than a raw field.

At sector zero — which is all of day-one content — every one of these is arithmetically identical
to what it does today.

---

## 3. Out of scope

- **Anything that makes multi-sector play actually good.** Camera-relative rendering with a
  rebasing origin, double-precision view matrices, sector-aware LOD: all of that is a rendering
  precision slice of its own. This slice makes the client *correct at sector zero* and *explicit
  about that assumption*, no more.
- **Content that spans sectors.** The starting fleet stays three hulls near the origin. No sector
  is populated, no boundary is crossed by anything but a test.
- **Region sharding, ghost zones, snapshot compression.** §3 notes that the sector is the unit for
  all three. None of them is built here, and this slice must not add a hook, an interface, or a
  `// TODO` for any of them.
- **`SpatialIndex` cell size or level count.** Out of the replay contract and staying there; this
  slice adds the constraint in §2.2 and changes no value.
- **Slices 2b and 6.** Untouched.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `GameLogic/WorldPos.h` | The type and the four free functions every caller already routes through |
| `GameLogic/SimTuning.h` | The replay-contract side of the tuning split, with the header comment stating the rule |
| `GameLogic/SpatialIndex.cpp` | `CellOf`, `CellHash`, and the `Cell` that carries its own `cellX`/`cellZ` |
| `GameLogic/PathGrid.h/.cpp` | `m_originX`/`m_originZ`, `ClampedCellX`, `CentreOf` |
| `Design/Archive/Collision.md` §3 | The representation, the precision argument, and why the three obvious alternatives lose |
| `Design/Archive/Collision.md` §14 | `SECTOR_SIZE_METRES` is named there already as entering the contract when this lands |
| `Tests/GameLogicTests/` | 56 tests that must keep passing, and the slice-2 benchmark that must be re-run |

---

## 5. What will surprise the implementer

### 5.1 The compiler finds all 94 aggregate initialisations, and only if the sector fields go first

There are **94** `WorldPos{x, z}` sites in the tree — 91 in tests, 3 in production. With the sector
pair first, every one of them is a **hard compile error**, not a warning and not a silent
reinterpretation: `[dcl.init.list]` makes floating-point to integer narrowing ill-formed in a
braced initialiser list with **no constant-expression exception**, so even `WorldPos{0.0f, 0.0f}`
is rejected. Verified with clang 18; the projects build `/permissive-` (`ConformanceMode` is `true`
in every configuration), so MSVC is expected to agree — **confirm that in CI before trusting it**,
because if MSVC only warns, 94 sites silently start meaning sector coordinates.

Put the sector fields last and the property is lost: `WorldPos{0.0f, 600.0f}` keeps compiling and
keeps meaning the right thing, right up until someone writes a four-field one. Losing a compile
error that enumerates every site for you is a bad trade for a struct layout preference.

Fix the 94 with a factory rather than by writing `WorldPos{0, 0, x, z}` ninety-four times:

```cpp
[[nodiscard]] constexpr WorldPos LocalPos(float _x, float _z) noexcept { return {0, 0, _x, _z}; }
```

It is a one-token edit per site, it keeps the tests readable, and it names the assumption those
sites are making — sector zero — in a single place that a later debug assertion can check.

### 5.2 The 56 existing tests are the real acceptance

Every one of them runs inside sector zero, so every one must pass **unchanged apart from the
`LocalPos` rewrite**. That is the whole "day-one content never notices" claim from §3, and it is
the only evidence that will convince a reviewer this slice is behaviour-neutral. If a test needs
its expectations adjusted, something in §2 was done wrong — find that, do not adjust the test.

### 5.3 The tests read `localX` as a coordinate too, and this work order missed them

§2.7 caught the client's eleven field reads and the 94 constructions. It did not catch the
**assertions**, and they are the ones that bite: `Assert::IsTrue(southMinX < -1.0f)` measures "did
the ship break to port" by reading `posWorld.localX` directly. Once `Translate` normalises, a ship
at x = −3 reads `localX = 8189` and the assertion can never hold — the test fails while the
simulation is correct.

Two tests fail this way (`AnEqualHeadOnPairBreaksToStarboard`, `AConcavePocketIsEscaped`) and
several more pass only because their coordinates happen to stay positive. The fix is the same seam
the client got: a `WorldX`/`WorldZ` pair in the test header that goes through `OffsetX`/`OffsetZ`,
so a test measures the world the way the simulation does. Three determinism tests also compared the
local fields alone, which after this slice would pass two positions a whole sector apart — they take
an `IsSamePosition` that compares all four.

### 5.4 `LocalPos` has to normalise, or the invariant is a lie

The factory in §5.1 as first written — `return {0, 0, _x, _z}` — produces `LocalPos(0, -600)` as
sector zero at `localZ = −600`, which denotes the right point while breaking the invariant §2.1
promises. The same place then has two spellings, an exactness test comparing all four fields calls
them different, and anything that ever hashes or serialises a position inherits the bug.

So `LocalPos` normalises, and it shares one carry helper with `Translate` — a carry that disagreed
with itself between construction and displacement would put the same point in two sectors. The
helper cannot use `std::floor`, which is not `constexpr` before C++23 while this tree is C++20
(AGENTS.md §5), so it is a truncating cast with an adjustment for negatives.

### 5.5 `Cell` doubling is measured, not theoretical

24 → 48 bytes, confirmed by compiling both layouts. The slice-2 benchmark sweeps 5000 entities and
shows it: **+25% on rebuild, +13% to +30% on query**, with hits per query identical at all twelve
configurations — so the index returns the same neighbours and the cost is memory traffic alone.
[ADR 0007](../Decisions/0007-the-index-stores-a-whole-position.md) carries the table.

---

## 6. Decision records due

One, for §2.5 — and it went the other way from the way this work order predicted.
[ADR 0007](../Decisions/0007-the-index-stores-a-whole-position.md): **the spatial index stores a whole
`WorldPos` and pays the 24 bytes**, because the compact layout as specified here is order-dependent.
The record carries the measured cost and the two variants that survive, so whoever takes the
optimisation starts from the one that needs no origin at all.

No record is due for the field order (§5.1): it follows from a language rule, and the reasoning
belongs in the header comment where someone reordering the struct will meet it.

---

## 7. Acceptance

Done is all of the following, and nothing here is decided by inspection where a test could decide
it instead.

**Carry arithmetic** — new tests in `Tests/GameLogicTests/`:

- Translating past the far edge carries: from `LocalPos(0, 0)`, `Translate(p, SECTOR_SIZE_METRES + 5, 0)`
  gives `sectorX == 1` and `localX == 5.0f` exactly.
- Translating below zero carries the other way: from `LocalPos(0, 0)`, `Translate(p, -5, 0)` gives
  `sectorX == -1` and `localX == SECTOR_SIZE_METRES - 5.0f` exactly.
- The carry is exact and reversible: translating by `+d` then `−d`, for `d` spanning several
  sectors, returns a position whose four fields compare bit-identical to the original.
- Every stored position satisfies the invariant: `localX` and `localZ` are in
  `[0, SECTOR_SIZE_METRES)` after any `Translate`, including a displacement of several sectors in
  one call.

**Distance across a boundary:**

- Two positions 10 m apart that straddle a sector boundary measure 10 m, to the same tolerance as
  two positions 10 m apart inside one sector.
- `Lerp` at `t = 0.5` between positions in adjacent sectors lands on the boundary, not somewhere
  arbitrary.

**The index and the grid:**

- `QueryCircle` centred 5 m from a sector boundary returns a neighbour 5 m on the other side of it.
- A path is found between a start and a goal in adjacent sectors, and its waypoints are ordered and
  contiguous across the boundary.
- The A\* tie-break stays total across a boundary — the existing equal-cost test, run on a grid
  that spans one.

**Behaviour-neutrality:**

- All 56 existing tests pass, changed only by the mechanical `LocalPos` rewrite.
- `ArrayOrderCannotChangeTheAnswer` still passes.

**The tree:**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds clean in CI, clang-tidy included. **CI is the only gate on the client half** —
  say in the pull request that the `Outpost` changes are compile-verified there and nowhere else.
- The slice-2 benchmark numbers, before and after.
- No screenshot is due: at sector zero the client renders exactly what it renders today, and this
  work order says so as the stated assumption rather than asking for a picture of no change.

**The documents:**

- `Design/Archive/Collision.md` §19 marks slice 8 `landed`.
- `SimTuning.h`'s replay-contract comment names `SECTOR_SIZE_METRES`.
- The decision record from §6 exists and `Design/Decisions/README.md` lists it.
- `AGENTS.md`: nothing here makes a sentence in it false — check rather than assume, and if the
  check finds one, it changes in this pull request.

---

## 8. Assumptions the implementer may make

- **All content sits in sector zero.** Nothing needs to spawn, save, or load a non-zero sector; the
  tests are the only thing that will ever construct one.
- **`SECTOR_SIZE_METRES = 8192.0f` is settled.** §3 argues it from the 0.5 mm precision figure
  (8192 / 2^24 = 0.488 mm, uniform in every sector) and it is not this slice's job to revisit it.
- **The 647 m widest query radius holds.** It is what makes a float cross-sector offset safe, and
  slice 2 measured it. If a later slice raises it past a sector, that is that slice's problem and
  its work order should say so.
- **`WorldPos` has no Y and gains none.** The simulation is a plane. There is no `sectorY`.
- **Recordings break, and that is the point.** No migration path, no versioning, no compatibility
  shim: nothing depends on recorded positions yet, which is the entire reason this slice is
  scheduled now instead of later.
