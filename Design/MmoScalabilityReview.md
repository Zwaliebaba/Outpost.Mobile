# MMO scalability and consistency review

A point-in-time review of the tree at `de12b6d` (2026-08-29), asking one question: **is the
implementation consistent, and is it scalable to an MMO-size game?** Focus areas, as requested:
core game engine functionality, universe management, and graphics processing.

**Method.** Four independent reviewers each audited one slice — the engine core (tick, transport,
snapshot pipeline, threading, hosting), universe management (world model, coordinates, spatial
structures, movement, interest management), graphics processing (device, renderers, content path,
shaders), and cross-cutting consistency (the tree against its own AGENTS.md). A synthesis pass then
re-verified every headline claim against the code before it was allowed into this document; both
build guards were actually run and pass. Findings cite `file:line` at `de12b6d`. Every finding is
tagged: **[MMO-blocker]** breaks at MMO density without a rebuild of that part; **[Rework-before-MMO]**
is contained, enumerable work that must land first; **[Scale-risk]** degrades before it breaks;
**[Consistency]** is drift against the tree's own rules.

A declared gap (AGENTS.md "Deliberately not here yet") counts here only when the current shape would
force rework to fill it — this review judges headroom, not completeness.

---

## Verdict

**The architecture is genuinely MMO-shaped; the implementation is one-player-scale in specific,
enumerable places; and no finding requires tearing a layer apart.** The decisions that are expensive
to retrofit — sector-based coordinates with uniform precision across a universe wider than any game
needs, a headless server library behind a datagram transport seam, an order-independent tick built
for threading and region handoff, real area-of-interest replication measured at N=5,000, view-record
snapshots that withhold intent from clients — are already in the tree, tested, and in several cases
benchmarked. What stands between this tree and MMO scale is concentrated: a replication stream that
cannot heal packet loss, a single-subscriber publisher, a listener that leaks connection slots, a
pathfinding grid that switches off beyond 16.4 km, a broad phase whose per-tick cost breaks the 60 Hz
budget at fleet density, and a render-submission layer that draws one object per call with no
culling or instancing. Consistency at the code level is excellent wherever a machine gate enforces
it, and measurably drifting exactly where the contract says "check by eye" — the meta-finding is
that the gates need widening before many hands arrive, not that the rules need rewriting.

| Area | Scalable to MMO? | The short version |
|---|---|---|
| Core engine (tick, transport, replication) | **Yes, after contained rework** | Seams are right; fan-out, loss recovery, and slot reclaim are the debts |
| Universe management | **Yes — strongest area** | Coordinates, identity, tick and AoI are MMO-grade; PathGrid and the broad phase need regional bounds |
| Graphics processing | **Foundation yes, submission no** | Device/frame layer keeps; everything between the snapshot and the command list needs culling, instancing, batching, streaming |
| Consistency | **Clean where gated, drifting where not** | Zero dependency or C++-rule violations found; six stale doc sentences; guards under-scoped for many-hands development |

---

## What already scales

Credit first, because these are the decisions that are usually gotten wrong and here were not.

- **Coordinates are universe-scale by construction.** `WorldPos` is an `int64` sector pair times
  8,192 m plus a float local offset: 0.49 mm precision, uniform in every sector, with an exact
  power-of-two carry guarded by a `static_assert` (`GameLogic/WorldPos.h:27-33`,
  `GameLogic/SimTuning.h:43,56`). All simulation math runs in sector-local frames through
  `OffsetX`/`OffsetZ`; bit-identical stepping across sector boundaries is tested.
- **The client/server seam is structural, not conventional.** `NeuronServer/Simulation.h` is
  `Step()`/`Tick()` and nothing else; `ServerHost` is a pure fixed-tick accumulator with
  spiral-of-death clamping, pinned by a test (`NeuronServer/ServerHost.cpp:19`,
  `Tests/NeuronServerTests/ServerHostTests.cpp:55-68`). A dedicated headless server is a composition
  root away — `FrameClock + ServerHost + QuicListener + adapter`, all in libraries the build guard
  keeps free of graphics headers.
- **The tick is order-independent on purpose.** Five whole-array passes over start-of-tick
  snapshots, gather-not-scatter separation, sort-then-truncate sensing with a total order
  (`GameLogic/World.h:98-112`, `World.cpp:341-350`) — the properties that let the array be split
  across threads or region servers without changing answers, locked by a permutation test.
- **Entity identity survives churn.** Slot+generation handles (ADR 0005) keep references valid
  across swap-and-pop despawn from both sides; handles, not indices, go on the wire
  (`GameLogic/ShipState.h:38-42`, `WorldSnapshot.h:43`).
- **Interest management is real AoI, and measured.** Spatial radius, 10 Hz cadence,
  enter/leave/destroyed deltas, distance-weighted refresh priority; per-subscriber cost tracks the
  neighborhood, not the world — 18 KB vs 417 KB per update at N=5,000, asserted in a test
  (`GameLogic/InterestSet.cpp:90-108`, `Tests/GameLogicTests/InterestTests.cpp:321`).
- **The spatial index is deterministic and cheap.** Hashed-bucket uniform grid, O(N) counting-sort
  rebuild per tick (0.107 ms at N=5,000, ADR 0007), no unbounded cell tables between distant ships,
  multi-level machinery already in place (`GameLogic/SpatialIndex.cpp:92-151`).
- **The transport threading model is per-connection.** One mutex per connection held for one
  memcpy, lock-free CAS slots outbound, zero worker-side allocation, fixed-stride arenas
  (`NeuronCore/QuicTransport.cpp:71-91,346-355`) — nothing serializes all connections through one
  lock.
- **The wire anticipates separation and cheating.** Field-by-field little-endian encoding, view
  records that withhold `steerTargetPos` and every other intent field, faction as identity not
  relation, command authority gated in the simulation rather than the adapter (ADRs 0008, 0009,
  0013, 0014).
- **The D3D12 frame architecture is correct.** Three frames in flight with per-slot fences,
  root-constant per-draw delivery, persistently mapped per-frame vertex rings, all device queries
  cached at init (`NeuronClient/GpuDevice.cpp:239-249`, `FxRenderer.cpp:33-41`).
- **Zero steady-state allocation on every hot path** — world tick, index rebuild, interest update,
  snapshot encode, transport send/receive all reuse owned scratch; verified by read, and stated in
  the headers.

---

## Findings — core engine

### E1. [Rework-before-MMO] The delta stream cannot heal: a lost leave or destroy is a permanent ghost

There is no sequence/ack model — `snapshotId` only groups fragments, `tick` only rejects stale
(`GameLogic/WorldSnapshot.cpp:356-358`). `InterestSet` emits each leave exactly once
(`InterestSet.cpp:83-87`), the live path only ever calls `WriteInterest` (full-state `Write` is dead
code in it, `Outpost/WorldSimulation.h:72`), and the receiver's own header states the consequence:
"unlike a full snapshot a delta stream cannot resynchronise by waiting for the next one"
(`WorldSnapshot.h:133-135`). Over a lossy wire a lost *enter* heals in ≤ 8 updates and a lost
*upsert* on the next refresh, but a lost *leave/destroyed* list is a client-side ghost until the
ship happens to re-enter view. Updates are also atomic across fragments and order-sensitive
(`WorldSnapshot.cpp:369-375`), so completeness is (1−p)^F: at 2% loss a 3-fragment update completes
94% of the time, a 13-fragment fleet-battle update 77%. The fix is already designed — leaves,
destroys and orders on the reserved reliable QUIC stream (`PeerBidiStreamCount = 1` is negotiated,
`NeuronCore/QuicApi.cpp:26`; Design/QuicTransport.md slices 3a/3b) — but unscheduled; until it
lands, correctness is loopback-only, not merely configuration. A zero-new-code interim backstop
exists: a periodic full `Write` per subscriber.

### E2. [Rework-before-MMO] Fan-out is single-subscriber by construction, and the despawn log loses deaths the day there are two

`WorldSimulation` holds exactly one `Transport*`, one `InterestSet`, one `SnapshotWriter`, one
hard-coded subscriber faction (`Outpost/WorldSimulation.h:167-179`). The engine below needs no
signature change for N subscribers — the declared plan ("a table of {transport, interest set,
faction}", Design/QuicTransport.md §10) checks out — but two things the one-liner undersells:
`World::DespawnLog` is drain-once by the single publisher (`World.h:62-70`, and the header itself
says "the day there are several it becomes per-subscriber"), so a second subscriber would silently
miss deaths; and the session table lives in `Outpost/`, so a dedicated server executable must move
or duplicate it — the same argument ADR 0008 used to keep the wire format out of the executable
applies and is currently unapplied. Also in this slice's blast radius: `SubscriberCentre()` scans
every ship per subscriber per update (`WorldSimulation.h:110-132`), a subscribers×N term its own
comment already retires.

### E3. [Rework-before-MMO] QuicListener never reclaims a slot — the server dies by attrition, not load

`OnNewConnection` only ever increments `m_adopted`; nothing returns a closed connection's transport
to the pool, and `m_pool`/`m_accepted` are cleared only in `Stop()`
(`NeuronCore/QuicListener.cpp:164-176,130-133`). The listener therefore supports `backlog`
**cumulative** accepts per process lifetime, not `backlog` **concurrent** connections: a 1,000-slot
server that has seen 1,000 connect/disconnect cycles refuses everyone until restart, silently.
Secondary sizing note: each pooled transport pre-reserves 576 KB of rings (256 × 1,152 × 2,
`QuicTransport.h:47`) — 562 MB up front at backlog 1,000; capacity should become per-role. Minimal
fix: detect `Closed` transports in `Poll()`, recycle them into a free list, and give `Accepted()` a
removal story.

### E4. [Scale-risk] Every subscriber's interest update lands on the same tick

`IsDueOn` is `(_tick % updateEveryTicks) == 0` with no per-subscriber phase
(`GameLogic/InterestSet.cpp:18-21`). With N subscribers, five idle ticks are followed by one tick
carrying N × (2 km query + sort + serialize + fragment sends): the server's worst frame is ~6× its
average and all egress bursts on it. One-line fix at the future session table: phase =
subscriberId % updateEveryTicks. Do it in the same slice as E2, because the failure only exists once
that slice does.

### E5. [Scale-risk] The wire record is 82 uncompressed bytes, and the docs describe a quantized wire that does not exist

Verified from the encode (`WorldSnapshot.cpp:21-23,191-205`): 8 B handle + 48 B for two full
`WorldPos` (2 × [i64, i64, f32, f32]) + 20 B floats + 6 B = 82 B per ship, 34 B header, **13 ships
per 1,152 B fragment**. Meanwhile AGENTS.md §1 R6 says "a wire in centimeters" and `.clang-tidy`
speaks of pervasive deliberate quantization ("metres to centimetres, radians to turns16") — no such
layer exists; grep finds no centimeter identifier and no turns16 anywhere, and Design/Collision.md
§3's "a snapshot position compresses to a sector id plus a quantised local offset" is unbuilt. The
sector-relative model is *made* for it (a local offset fits u16 at 12.5 cm inside an 8,192 m
sector), and `prevPos` could be derived client-side for already-known ships; either halves the
record. At k=100 in view the raw format is ~28 KB/s per client (viable; ~28 MB/s egress at 1,000
clients); at k=500 it is ~138 KB/s and 13-fragment updates, colliding with E1's loss amplification.
The field-by-field format means this is a format revision behind the ALPN bump, not a rework.

### E6. [Scale-risk] Order intake is unthrottled — wire bytes convert to server CPU at high leverage

`ApplyIncomingOrders` drains every waiting datagram each tick and applies each valid one
(`Outpost/WorldSimulation.h:140-164`); each `IssueMoveOrder` can trigger a formation solve and route
planning for up to 139 ships (`MaxShipsPerOrder`, `WorldSnapshot.cpp:184-186`). The faction gate
(ADR 0014) limits *whose* ships, not *how often*: a hostile client saturating its send rate buys
pathfinding CPU no other message can. Minimal fix at the session table: per-connection
orders-per-tick budget, excess dropped exactly as a full queue already is.

### E7. [Scale-risk] 60 Hz is a per-shard cost multiplier the tree names but has not decided

`SimTuning.h:22-28` says it plainly: an MMO server is unlikely to keep 60 Hz, and lowering it trips
the tunneling gate (at 20 Hz an Interceptor covers 1.70 m/tick against a 1.115 m capsule) — the
test is parameterized to go red naming the hulls. A well-guarded open decision, but it gates shard
capacity math and the CCD/substep work lands in GameLogic; settle it before dedicated-server sizing,
together with U2 below.

---

## Findings — universe management

### U1. [Rework-before-MMO] PathGrid is one global grid over every obstacle in the universe, and it declines to exist beyond 16.4 km

`PathGrid::Rebuild` sweeps a single bounding box over *all* obstacles and refuses to build past
`PATH_GRID_MAX_CELLS_PER_AXIS` (`GameLogic/PathGrid.cpp:64-65`) — 512 × 32 m = 16,384 m per axis.
Two stations 20 km apart disable A* for the whole universe, degrading every route to straight-line
steering — while `HullSpec.h:83-85` states capitals *cannot* avoid on local steering and need
routes. Rebuild is also exact O(cells × obstacles) (`PathGrid.cpp:73-84`): a legal 262,144-cell
grid with 30 stations is ~7.9 M distance evaluations per rebuild. The cliff is known — the comment
says "sectors are what will bound this properly" — but the rework is real: per-island or per-region
grids keyed by obstacle clusters, with route stitching between them. The `FindPath`/waypoint seam
and the route follower survive unchanged.

### U2. [Rework-before-MMO] The per-tick neighbor gather breaks the 60 Hz budget at fleet density, by the tree's own benchmark arithmetic

`QueryRadiusMetres` is dominated by global worst-case terms (fastest hull speed, largest mobile
radius — `GameLogic/HullSpec.h:227-237`), so every mobile hull queries a ~587–647 m circle every
tick, and `GatherNeighbours` builds a 40-byte record (with a sqrt) per candidate and sorts them
*all* before truncating to K ≤ 16 (`World.cpp:319-350`) — K bounds what is used, never what is
gathered. ADR 0007 measured the sweep at 9.28 ms for 5,000 queries at 250 m radius (~53 hits per
query at ~156 ships/km²); at the real ~600 m radii the returned set grows ~4.7×, putting the broad
phase alone around ~45 ms per tick at N=5,000 battle density — ~3× the 16.7 ms budget, before
record construction, sorting, and separation. (That figure is an extrapolation from the recorded
benchmark, not a new measurement; the mechanism is verified.) Fix that preserves
sort-then-truncate determinism: derive the query radius from the largest hull *locally present*
via a coarse pre-pass, pre-filter candidates by threat before building records, and take the E7
tick-rate decision with it.

### U3. [Rework-before-MMO] Nothing gives an entity an identity outside one World — cross-shard handoff would read as death

Handles are allocated per-`World` instance (`World.cpp:26-41`); a ship handed between region
servers gets a fresh slot/generation at the destination, so the wire cannot say "same ship, new
region" — clients keyed on handles would see destroy + enter, defeating exactly the continuity ADR
0005 exists to provide. There is also no serialization of authoritative state at all: routes,
patrols and speed caps are deliberately withheld from snapshots (`WorldSnapshot.h:33-36`), so the
snapshot path structurally cannot carry a save or a handoff, and no other writer exists. Both fixes
are additive and cheap now, expensive after combat stores targets: a globally unique id (e.g.
shard:slot:generation) mapped at the wire boundary, and a state codec beside `WorldSnapshot`.
Persistence otherwise costs no rework — state is dense POD arrays with no pointers. (`FactionId` at
u8 allows 256 factions: fine while factions are identities, tight if player corporations become
factions.)

### U4. [Scale-risk] Any spawn or despawn rebuilds the static index and PathGrid and forces every routed ship to re-plan

`SpawnShip`/`DespawnShip` set `m_staticIndexDirty` unconditionally (`World.cpp:44-48,82`); the next
tick rebuilds the static grid and `PathGrid`, whose `Rebuild` bumps `m_version` unconditionally
even when the obstacle set is byte-identical (`PathGrid.cpp:26`), so `AdvanceRoute` re-plans every
Moving ship (`World.cpp:221-225`) — at minimum a clearance walk sampling every 16 m of each route.
Amortized fine when the fleet changes rarely; inverted at MMO churn, where logins and deaths land
every tick: O(n) rescan + O(cells×obstacles) + a universe-wide replan wave, per tick. Fix is small
and local: dirty the static set only when an immovable spawns/despawns, and version-bump only when
the obstacle set actually changed.

### U5. [Scale-risk] Quadratic little loops at the seams

Four sites, each individually flagged or benign today, all quadratic at MMO k: the interest sort is
an O(k²) insertion sort per subscriber per update (`InterestSet.cpp:45-58` — ADR 0010 itself names
it "the first thing to revisit"); the client-side snapshot apply upserts and removes by linear scan
(O(k²) per update, `WorldSnapshot.cpp:456-490`); `WorldView::ApplySnapshot` matches incoming ships
against carried state by linear scan (`Outpost/WorldView.cpp:96-105`, honestly flagged in a
comment); and `MeshLibrary` lookups are linear. All four are one-place fixes (index-permutation
`std::sort`, handle-sorted sets or slot-indexed tables) that fit the tree's no-hash-map determinism
rule.

### U6. Universe model limits, stated for the record

The simulation is a 2D plane by declared design (`WorldPos.h:26` — no Y; heading is one scalar),
with celestial bodies as 3D presentation only (ADR 0016). That is a product decision, not a defect,
and the tree is consistent about it — but it is load-bearing for "MMO-size game" claims: altitude,
orbits or 3D combat would be a new simulation model, not a field added. Determinism is correctly
scoped for multi-server (same binary, same inputs; x64-only servers; no lockstep) and interest
constants are per-region tunable by design (`SimTuning.h:288-299`).

---

## Findings — graphics processing

### G1. [MMO-blocker] Thruster trails cost one draw call per trail sample per nozzle, with a PSO set on every call

`WorldView::DrawFeedback` walks every ship × every nozzle × up to 32 trail samples and calls
`DrawGlow` for each (`Outpost/WorldView.cpp:1027-1075`); each `DrawGlow` sets the pipeline state,
two root-constant groups, the vertex buffer, and issues a two-triangle draw
(`NeuronClient/SceneRenderer.cpp:201-215`). Idle intensity (0.12) never falls below the 0.002 cut,
so stationary ships pay too. Today's 7 ships already emit roughly 220–670 trail draws — the
dominant submission cost of a 3-hull scene; at 300 ships × 2 nozzles it is ~19,000 draws with a
redundant PSO set each, an estimated 10–20 ms of CPU record/driver time before the first hull
scales. The sprite system already demonstrates the fix in-tree (`SpriteParticles.cpp:102-158`):
build all trail quads into a per-frame vertex ring and draw them in one call per blend mode —
19,000 draws become 2.

### G2. [MMO-blocker] No culling, no instancing, no batching layer — game code issues immediate per-object draws

`WorldView::Render` calls `DrawMesh` once per ship and twice per body
(`Outpost/WorldView.cpp:780-833`); there is no frustum test anywhere in the tree (no
`BoundingFrustum`, no visibility check between snapshot and draw), cull mode is NONE tree-wide
(`NeuronClient/GpuHelpers.cpp:83`), and 500 ships sharing 5 hull meshes would still be 500
one-instance draws — the exact case `DrawInstanced(n, instanceCount)` exists for. Hull vertex
buffers deliberately stay in upload heaps (`SceneRenderer.cpp:118-127`), so every draw re-crosses
PCIe. The bodies make it concrete: 8 bodies ≈ 28 MB of vertex fetch per frame today (two passes ×
~14 MB); "planets everywhere" at 30–50 bodies is 15–25 GB/s of vertex traffic — bandwidth-dead on
the mobile-class hardware ADR 0019 targets. Minimal fix: a sphere-vs-frustum visibility pass
(`rhcoords: false` per §5) before submission, and per-hull instance buffers for the ship pass; the
root-constant scheme stays for everything else.

### G3. [Rework-before-MMO] The content path is boot-synchronous, all-resident-forever, and the upload bracket drains the GPU twice

All hulls parse text OBJ synchronously at boot (`Outpost/OutpostApp.cpp:276-288`); textures upload
under `BeginUploads`/`ExecuteAndWait` brackets that fully drain the pipeline — twice
(`NeuronClient/GpuDevice.cpp:204-225`). There is no copy queue, no async load, no residency
management, and no way to free anything: both mesh stores are append-only
(`SceneRenderer.cpp:133`, `BodyRenderer.cpp:438`), and F5's reseed deliberately leaks the replaced
scene (~14 MB per press, acknowledged at `OutpostApp.cpp:490-492`). At MMO asset counts, boot time
grows linearly with content, VRAM monotonically with everything ever seen, and any mid-session load
is a guaranteed frame hitch. Minimal fix: a dedicated COPY-queue uploader with fence handoff, and
generation-tagged free lists in the two mesh stores — the `MeshHandle` indirection
(`RenderTypes.h:31-39`) was designed for exactly this rehousing.

### G4. [Rework-before-MMO] Textures: top-mip uncompressed BGRA8 only; descriptor heaps sized for today's content exactly

`DdsImage` parses BC formats, mip chains, arrays and cubemaps (`DdsImage.h:20-26`), but every
upload path takes one mip of R8/BGRA8 via `TopMipAsBgra`, which refuses block-compressed input
(`DdsImage.h:89-93`, `GpuHelpers.cpp:132-277`); the body outline's missing mips are papered over
with an `fwidth` fade in the shader (`BodyOverlayPS.hlsl:16-19`). Descriptor heaps are
constant-sized per pass (Body 1 slot, Fx 3, Text 10) with no allocator — every new texture is a
code change. At MMO texture counts this is 4× the memory (no BC), shimmer at distance (no mips),
and a heap scheme that cannot grow. Fix: one shared shader-visible heap with a free-list allocator
and a subresource-walking upload — `DdsImage` already provides subresources in D3D12 order.

### G5. [Rework-before-MMO] Bodies: 4.1 MB per unique planet, unshared vertices, no index buffer, no LOD, both hemispheres shaded

A grid-6 planet is 147,456 unshared `FxVertex` = 4.13 MB (`BodyMeshBuilder.h:26-28` — "nothing in
this tree carries an index buffer"), drawn twice per frame (terrain + overlay), with cull NONE
shading the far hemisphere too (acknowledged, `BodyRenderer.cpp:184-186`), and the GPU bake writes
sea-floor cells as degenerates rather than culling them (`OutpostApp.cpp:322-327`). LOD is
explicitly deferred (Design/PlanetRenderer.md §13) and every body is a unique mesh. The bake itself
is the right foundation (three dispatches per body, straight into a default-heap VB — ADRs 0017,
0020); what breaks at dozens of bodies is drawing and memory. Fix is the design's own plan:
projected-radius LOD from pre-baked grids, back-face culling once winding is confirmed, distance
culling for asteroids.

### G6. [Scale-risk] One global effects budget; single-threaded submission; small redundancies

The sprite pool (4,096, `ViewTuning.h:101`) and the fx ring (49,152 verts ≈ "five simultaneous
deaths", `FxRenderer.h:26-28`) are single global caps: a 30-death fleet battle starves after two
explosions (drops are at least counted). One direct command list recorded on one thread
(`GpuDevice.h:93-94`) is fine to a few thousand draws but is the ceiling after instancing lands.
`DrawDecal`/`DrawGlow` set the PSO per call and `BodyRenderer`/`FxRenderer` re-issue
`OMSetRenderTargets` per draw — lift these into the existing `Begin*` pass boundaries.
`TextRenderer::PushQuad` silently drops past 4,000 quads with no counter
(`TextRenderer.cpp:143-144`), unlike `FxRenderer`, which counts — an MMO-density HUD (hundreds of
minimap contacts) loses its topmost elements invisibly. Right fix for effects at MMO density is
distance-scaled emission, not pool growth — the fixed-pool philosophy is correct.

---

## Findings — consistency

The mechanical audit came back **clean everywhere a guard enforces the rule**: zero dependency
violations (no engine file names the game, client and server never reference each other, no
graphics header reaches the headless libraries, GameLogic's include surface is exactly
NeuronCore + DirectXMath + std), zero §5 violations (no stored `XMVECTOR`, zero `RH` calls, no
WRL/raw `Release()`, `catch` only in `Main.cpp`, no argv/env reads, GameLogic free of clocks,
entropy and pointer keys), shared constants single-definition and derived rather than restated
(`MAX_DATAGRAM_BYTES`, `TICK_HZ`, per-datagram caps), ADR index complete at 0001–0023, and both
build guards pass when run. The drift sits precisely where AGENTS.md says "check by eye":

### C1. [Consistency] The legacy helper header breaks the tree's own "nothing is grandfathered" rule — and most of it is dead

`NeuronCore/NeuronHelper.h:133` declares `class BaseException` — the exact form R2 bans by name —
alongside R1/R8 naming breaches inside macro bodies and a dead-code block; `BaseException`,
`NonCopyable`, `ENUM_HELPER`, `ENUM_FLAGS_HELPER` and `IsValidEnum` have zero uses in the tree,
while `Transport`/`QuicListener` spell deleted copies by hand. `FileSys` keeps Hungarian locals
(`hFile`) against R6 and a bare `uint8_t` against R10. The cheapest honest fix is deletion of the
dead pieces; the QUIC files, by contrast, conform tightly to the core idiom — the debt is
concentrated in one file.

### C2. [Consistency] The guards are narrower than the contract claims, and drift tracks the gate boundary exactly

`Build/CheckProjectFiles.py:192` checks engine files against a hard-coded list of **six** GameLogic
headers; GameLogic now has fourteen — an engine file including `WorldSnapshot.h` or `SpatialIndex.h`
would pass CI. `.clang-tidy:33-35` claims R2's banned suffixes "have their own CI step"; no such
step exists (AGENTS.md honestly says "by eye"). clang-tidy gates one project of nine, and the
clearest scale signal in the whole review is that local-`constexpr` casing splits exactly along
that boundary: GameLogic (gated) is clean, seven sites across the ungated projects drifted
(`AppWindow.cpp:30`, `SceneRenderer.cpp:26`, `OutpostApp.cpp:405`, …). For a many-hands MMO
codebase the fix is wider gates, not more prose: derive the header list from `GameLogic/*.h`, add
the trivial R2-suffix and R11-family greps to the guard, extend clang-tidy project by project.

### C3. [Consistency] Six stale sentences in the enforcement layer itself, plus assorted prose drift

Both lint configs still say CI does not run them (`.clang-tidy:11-14`, `.clang-format:13-15`) —
false since the commit that made both gate, which updated AGENTS.md and touched neither config;
`build.yml:31` still says shaders compile under FXC (ADR 0018 made it DXC the same day). AGENTS.md
§2's map omits whole subsystems (`InterestSet`, `Patrol`, `WorldSnapshot`; the entire planets
pipeline), §3/§8 don't admit compute shaders exist (`BodyBakeCS.hlsl` cannot pass the checklist as
written), `WorldSnapshot.h:27` says `ShipState` is 120 bytes (it is 128 — `factionId` pushed it
without the sentence changing), `WorldPos.h:13`'s "±10¹⁹ m" is the sector *count* unit-slipped (the
range is ±7.6×10²² m), and the "wire in centimeters" claim is E5's unbuilt layer. One process
breach: the ShockRing work order landed without being archived, alone among 22 correctly archived
orders. Post-R11 UK spellings crept in outside the standing families (`Normalise` in
`CubeSphere.h:165`, the `BodyCatalogue` file pair, `HOSTILE_PATROL_CRUISE_MPS` beside 60+
`MetresPerSec` identifiers, ~40 UK-prose sites). Individually trivial; collectively they violate
the tree's flagship rule that a false sentence changes in the same commit — and they cluster in
exactly the documents future contributors will trust first.

### C4. Neutral note: the repository is named Outpost.Mobile; the tree is a Windows game

AGENTS.md says "Outpost is a Windows game" and never mentions the name. Mobile-adjacent provisions
that actually exist: WM_POINTER touch input, MSIX packaging, ARM64 configurations §6 declares
unverified — nothing else. Either the name encodes an intention no document states, or it is
historical; the tree's own standard suggests a one-line explanation belongs somewhere.

---

## What to do before MMO scale, in order

Dependency-ordered; each item is one slice family in this tree's terms.

1. **Reliable lane for leaves/destroys/orders** (E1) — the one correctness gap; designed, needs
   scheduling. Interim: periodic full snapshot per subscriber.
2. **The session table**: N × {transport, InterestSet, SnapshotWriter, faction}, per-subscriber
   despawn delivery, per-subscriber phase offset, per-connection order budget, session-derived
   subscriber center (E2, E4, E6) — one slice, in a library rather than the executable.
3. **QuicListener slot reclamation + per-role ring sizing** (E3).
4. **Trail batching** (G1) — the client's current ceiling, and the cheapest big win in the tree.
5. **Frustum culling + hull instancing** (G2).
6. **Tick-rate decision + localized gather radius** (E7, U2) — settle 60 Hz vs CCD before shard
   sizing; localize `QueryRadiusMetres` and pre-filter the gather.
7. **Regional PathGrid** (U1) and **churn-gated static rebuilds** (U4).
8. **Wire quantization** (E5) — build the centimeter wire the docs already describe; halves the
   record.
9. **Global entity identity + state codec** (U3) — cheap before combat stores targets, expensive
   after.
10. **Streaming content path** (G3, G4, G5): copy-queue uploader, mesh/texture eviction, BC + mips,
    body LOD.
11. **Widen the guards and re-true the docs** (C1–C3): derive the dependency list, add R2/R11
    greps, extend clang-tidy, fix the six stale sentences, archive the work order.

Items 1–3 make it a networked game; 4–7 make it survive density; 8–10 make it an MMO; 11 keeps a
bigger team from eroding what makes this tree work.

---

## Numbers appendix

All verified against the code at `de12b6d` except where marked as derived.

| Quantity | Value | Source |
|---|---|---|
| Tick rate / catch-up clamp | 60 Hz; 0.25 s (max 15 ticks/frame) | `SimTuning.h:28`, `ServerHost.h:26` |
| Sector size / precision / range | 8,192 m; 0.49 mm uniform; ±7.6×10²² m (int64 sectors) | `SimTuning.h:43`, `WorldPos.h` |
| `ShipState` / wire record | 128 B in memory (docs say 120); 82 B on the wire | `ShipState.h`, `WorldSnapshot.cpp:21-23` |
| Datagram / ships per fragment | 1,152 B; 13 full records + 34 B header | `Transport.h:21`, `WorldSnapshot.cpp:179-186` |
| Interest | 2,000 m radius, 10 Hz, edge refresh ⅛× | `SimTuning.h:297-303` |
| Measured AoI economy | 18 KB vs 417 KB full-world per update at N=5,000 | `InterestTests.cpp:321` |
| Per-client bandwidth (derived) | k=100: ~28 KB/s; k=500: ~138 KB/s, 13 fragments/update | from the encode |
| Update completeness at 2% loss (derived) | 3 fragments 94%; 13 fragments 77% — dropped whole | `WorldSnapshot.cpp:369-375` |
| Connections, structurally | backlog is cumulative, not concurrent; 576 KB rings/connection | `QuicListener.cpp:167-175`, `QuicTransport.h:47` |
| Spatial index | 256 m cells, O(N) rebuild 0.107 ms at N=5,000 | ADR 0007 |
| Broad phase at battle density (derived) | ~45 ms/tick at N=5,000 at the real 587–647 m radii vs 16.7 ms budget | extrapolated from ADR 0007 |
| PathGrid | 32 m cells, ≤512²; declines past 16.4 km extent; O(cells×obstacles) rebuild | `PathGrid.cpp:64-84` |
| Hard caps found | no ship-count cap (u32 ids); K ≤ 16 neighbors; 16 waypoints; 256 factions | headers as cited |
| Frames in flight / draws today | 3; ~250–700 (trail glows dominant) | `GpuDevice.h:17`, `WorldView.cpp` |
| Body meshes | grid-6 planet 147,456 verts = 4.13 MB, unindexed, drawn ×2 passes | `BodyMeshBuilder.h:26-28` |
| Effects budgets | 4,096 sprites; fx ring ≈ 5 simultaneous deaths; 4,000 HUD quads (silent) | `ViewTuning.h:101`, `FxRenderer.h:26-28` |
