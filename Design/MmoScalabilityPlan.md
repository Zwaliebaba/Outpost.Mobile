# The MMO scalability plan — the review as slices

**Status: seventeen of the twenty-four slices have landed.** The seam: 2 (the despawn log is
cursored), 3 with 4 folded into it (the publisher), 5 and 6 (the reliable lane, and departures and
orders on it, which retires finding E1), 7 (listener slots recycled), and the loopback fallback is
gone (ADR 0028). The view: 8 (trail and glow batching), 9 (frustum culling), 10 (hull instancing).
The simulation: 11 (a localized gather radius), 13 (churn-gated static rebuilds), and 14 — all four
slices of its own design, so architecture is islands on a world-fixed lattice and the 16.4 km cliff
is gone. The tree: 21 (the guards widened) and 22 (the dead helper deleted). Slice 12 was decided
rather than built — 60 Hz stays — and its record is written at last (ADR 0045). Slice 18 opens the
client track's second half -- a copy queue and render handles that can be freed -- built and run on
Windows on arrival, which the branch it came from could not do. Slice 23 has taken its first
project, and slice 24 is in: a root is told what to be by a file rather than by a rebuild. **What
is left is phase 3 — slices 15 through 17, 19 and 20 — plus slice 23's promotion commit.**
This design converts [`MmoScalabilityReview.md`](MmoScalabilityReview.md)
(tree at `de12b6d`) into an ordered slice plan in the shape `Design/README.md` defines: one slice,
one branch, one pull request. The review is the evidence; this document is the work. Where a slice
already has a design in the tree — the reliable lane lives in [`QuicTransport.md`](Archive/QuicTransport.md)
§14 — this plan schedules it and does not restate it.

**On this branch:** the review, the plan and every landed slice go as one pull request rather than
one per slice (owner's call, 2026-08-30). The commits are ordered and each stands alone, so the
history still reads slice by slice; the convention resumes for whatever is cut next. Slice 14 kept
its own four commits, one per slice of its design.

**Owed before this merges: the screenshots for slices 8, 9 and 10**, two window sizes each. They
were written into those slices' acceptance and they have not been supplied — nothing in the
container this was built in has Windows, D3D12 or any way to put a frame on a screen. Each slice
says so where its acceptance is (§6 slices 8, 9, 10) along with what was checked instead: a code
read, arithmetic on the numbers the change turns on, and the whole test suite. A reviewer on Windows
should take the six before merging; everything else in the acceptance lists is met.

Work orders are cut from §6 one slice at a time, when a slice is actually next — not in advance,
because every slice that lands changes the ground the next order stands on. Each order carries the
scope, out-of-scope, build-on and acceptance seeded here, expanded to the day's tree.

---

## 1. Problem

The review's verdict: the architecture is MMO-shaped, the implementation is one-player-scale in
enumerable places, and no finding requires tearing a layer apart. Twenty-four findings reduce to
eleven work items; this plan cuts them into slices that respect the tree's rules — one slice per
layer at a time, GameLogic deterministic, engine never naming the game, decision records landing
with the change they explain.

## 2. Shape — the design-level placements

Most slices below change no architecture; four decisions do, and they are taken here so the slices
can be cut against them. Each gets its decision record in the same commit as its slice (§9 of
AGENTS.md).

**The publisher lives in GameLogic.** The N-subscriber table — each entry a transport, an
`InterestSet`, a `SnapshotWriter`, a faction, a phase, an order budget — re-runs ADR 0008's
three-way elimination and lands in the same place: `NeuronServer` may not name `InterestSet`;
`Outpost` owns it today and a second executable would strand it; GameLogic already owns both types
being tabled and may include `Transport.h`. `QuicTransport.md` §10 asked for "a design of its own"
for exactly this — §2 and slices 2–4 of this plan are that design. It landed as written and never
grew enough in review to want a document of its own; ADR 0030 is where the decision lives.

**Despawn delivery becomes sequence-cursored.** `World::DespawnLog` is drain-once by one publisher,
and `World.h` itself says it becomes per-subscriber "the day there are several." The shape: the log
keeps a monotonic sequence, each subscriber holds a cursor, and the log trims to the minimum
cursor. Deterministic (dense array, no iteration-order dependence), and a late-joining subscriber
starts its cursor at the head rather than replaying history.

**The reliable lane is QuicTransport.md's, scheduled.** Slices 3a/3b are already designed to the
method level (`SendReliable`/`ReceiveReliable`, the reserved bidirectional stream, `KIND_LEAVE`,
the ALPN bump). Scheduling them now reverses §12 decision 4 of that design — "waits until the
migration has landed and been lived with" — and that reversal is the owner's call to make, put
plainly here rather than smuggled: the review's finding E1 (a lost leave is a permanent ghost;
13-fragment updates complete 77% of the time at 2% loss) is the case that the waiting is over.

**The client rebuild happens above GpuDevice.** The review keeps the device/frame layer — three
frames in flight, per-slot fences, root constants, vertex rings — and rebuilds the submission
layer over it: batching, culling, instancing, then streaming. No renderer is rewritten below its
`Begin*`/`Draw*` seam; `MeshHandle`'s indirection is the rehousing point it was designed to be.

Everything else in this plan is repair inside an existing shape, and says so in its slice.

## 3. Tracks and what runs in parallel

Slices in different layers share no files and run in parallel; two in the same layer collide on the
project files and the umbrella header, so each track below is serial within itself
(`Design/README.md`, "one slice per layer at a time"):

| Track | Layer(s) | Slices, in track order |
|---|---|---|
| Wire | `NeuronCore` | 5, 7, 22 |
| Simulation | `GameLogic` | 2, 3, 6, 11, 12, 13, 15, 16, 17, then 14 |
| Presentation | `NeuronClient` + `Outpost` | 8, 9, 10, 18, 19, 20 |
| Root | `Outpost` only | 1, 4 |
| Guards | `Build/`, `.github/`, prose | 21, 23 |

The Root track shares `OutpostApp.cpp` and `WorldView.cpp` with the Presentation track: whoever
lands second rebases, as QuicTransport.md §14 already settled for that pair. Slice 6 (GameLogic)
depends on slice 5 (NeuronCore) across tracks; slice 4 on slice 3; the table in §6 carries every
dependency. Four tracks can be in flight at once; nothing requires it.

## 4. Decisions this plan puts to the owner

Named here so no slice takes them silently:

1. ~~**Schedule the reliable lane now**~~ — **taken 2026-08-30: scheduled.** The wait in
   QuicTransport.md §12 decision 4 is lifted there, and the two work orders
   ([3a](Archive/ReliableLane-work-order.md), [3b](Archive/ReliableFormat-work-order.md)) are written.
2. ~~**The tick rate**~~ — **taken 2026-08-30: 60 Hz stays.** Capacity is bought by putting fewer
   entities in a shard, not fewer ticks in a second. Slice 12 therefore has no code in it: it is the
   decision record, stating that the rate is fixed, that the tunneling margin in `SimTuning.h` is
   what a future change would have to re-earn, and that the per-shard multiplier is now a sizing
   input rather than an open question. Substepping stays un-built and un-needed.
3. ~~**How a dedicated server is told what to be**~~ — **taken 2026-08-30: a configuration file, read
   by the composition root alone; built as slice 24, ADR 0043.** This does not bend AGENTS.md §5: that rule bans `argv` and the
   environment, and says configuration is loaded by the composition root, which is exactly what a
   file read there is. Libraries keep receiving plain `Desc` structs and still never read a file
   themselves. It needs a format, a hand-written parser (§1 R7 — no generators), the fail-closed
   rule §5 already requires of anything parsing content, and a decision record. It is a slice of its
   own, not yet cut, and it is what gates a second process.
4. ~~**`FactionId` stays u8 for now**~~ — **answered by slice 16's record: it stays u8, and the
   premise was wrong.** A corporation is a membership and a faction is an identity every client maps
   to a relation (ADR 0013), so making one the other puts ten thousand rows into a table that is
   quadratic in its own size. Widening the id alone would buy nothing while `FACTION_LIMIT` is 8 and
   the wire's `hostileMask` is a byte; the three move together or not at all (ADR 0044).

## 5. Deliberately left out

The same fences the review drew. No combat, economy, or content systems — this plan makes the
engine able to carry them, not build them. No second-process migration. The seam and the
machinery are ready and §4 decision 3 is taken and built, so slice 24 cut the configuration file a
headless root reads — but the second process itself, and whatever runs it, stays out of this
plan. No 3D simulation: the plane is a product decision
(ADR 0016, `WorldPos.h`), and nothing here spends effort for or against it. No audio. Release CI
and the R11 documentation pass stay where AGENTS.md already tracks them.

---

## 6. Slices

Twenty-four, in four phases. Sizes: **S** is a sitting, **M** is a normal work order, **L** needs
its own design before it can be ordered. "ADR" marks a decision record due in the slice's commit.
Findings reference `MmoScalabilityReview.md`.

| # | Slice | Layer | Size | Depends on | Findings | ADR | Status |
|---|---|---|---|---|---|---|---|
| 1 | Ghost backstop: periodic full refresh | `Outpost` | S | — | E1 interim |  | dropped: 5–6 scheduled |
| 2 | Sequence-cursored despawn delivery | `GameLogic` | M | — | E2 | ADR | [landed](Archive/DespawnCursors-work-order.md) |
| 3 | The publisher: subscriber table, phases, budgets | `GameLogic` | M | 2 | E2 E4 E6 | ADR | [landed](Archive/Publisher-work-order.md) |
| 4 | The root joins the publisher | `Outpost` | S | 3 | E2 |  | folded into 3 |
| 5 | Reliable lane on both transports (= QuicTransport 3a) | `NeuronCore` | M | — | E1 |  | [landed](Archive/ReliableLane-work-order.md) |
| 6 | Leaves, destroys, orders go reliable (= QuicTransport 3b) | `GameLogic` | M | 5 | E1 | ADR | [landed](Archive/ReliableFormat-work-order.md) |
| 7 | Listener slot reclamation, per-role rings | `NeuronCore` | S | — | E3 | ADR | landed |
| 8 | Trail and glow batching | `NeuronClient`+`Outpost` | S | — | G1 |  | landed |
| 9 | Frustum culling | `NeuronClient`+`Outpost` | S | — | G2 |  | landed |
| 10 | Hull instancing | `NeuronClient`+`Outpost` | M | 9 | G2 |  | landed |
| 11 | Localized gather radius, threat pre-filter | `GameLogic` | M | — | U2 |  | landed |
| 12 | The tick-rate decision | `GameLogic` | S | — | E7 | ADR | [landed](Decisions/0045-the-tick-rate-is-fixed-at-60-hz.md) |
| 13 | Churn-gated static rebuilds | `GameLogic` | S | — | U4 |  | landed |
| 14 | Regional pathfinding | `GameLogic` | L | 13 | U1 | ADR | landed, all four slices of [its design](Archive/RegionalPathfinding.md) |
| 15 | The quantized wire | `GameLogic` | M | 6 | E5 |  |  |
| 16 | Global entity identity | `GameLogic` | M | 15 | U3 | ADR |  |
| 17 | The state codec and the replay gate | `GameLogic` | M | 16 | U3 |  |  |
| 18 | Copy-queue uploader, store eviction | `NeuronClient` | M | 10 | G3 | ADR | landed (ADR 0044) |
| 19 | Compressed textures, descriptor allocator | `NeuronClient` | M | 18 | G4 |  |  |
| 20 | Body LOD and culling completion | `NeuronClient` | M | 9 | G5 |  |  |
| 21 | Guard widening and the docs re-trued | `Build/`+prose | S | — | C2 C3 C4 |  | landed |
| 22 | Legacy helper cleanup | `NeuronCore` | S | — | C1 |  | landed |
rather than built: 60 Hz stays. Slice 18 opens the client track's second half -- a copy queue and
render handles that can be freed -- built and run on Windows on arrival, which the branch it came
from could not do. Slice 23 has taken its first project, and slice 24 is in: a root is told what to
be by a file rather than by a rebuild. **What is left is phase 3 — slices 15 through 17, 19 and
20 — plus slice 23's promotion commit.**

**Quick wins:** slices 1, 7, 13, 21 and 22 are each a sitting, depend on nothing, and retire real
findings; any idle track starts with its nearest one.

### Phase 1 — make it a networked game (slices 1–7)

#### Slice 1 — ghost backstop: periodic full refresh (`Outpost`, S)

**Scope.** `WorldSimulation::Publish` sends a full `SnapshotWriter::Write` every
`FULL_REFRESH_EVERY_TICKS` (a send knob beside `INTEREST_UPDATE_EVERY_TICKS` in `SimTuning.h` §
interest management — outside the replay contract, and the comment says so). A ghost from a lost
leave now heals within one refresh period instead of never.
**Out of scope.** The reliable lane; any receiver change (`Accept` already handles full snapshots).
**Build on.** `SnapshotWriter::Write`, `WorldSimulation.h:72`'s note that `Write` is currently dead
in the live path.
**Acceptance.** A code read (the adapter has no suite — ADR 0014's argument, stated as the slice's
assumption); `SnapshotTests` already cover `Write`; the knob's comment names slice 6 as what makes
this a belt-and-suspenders setting. **Skip this slice entirely if slices 5–6 are scheduled first.**

**Dropped, 2026-08-30.** They were scheduled first. This slice was a way to blunt E1 while the lane
waited; nothing waits now, and a periodic full refresh that exists only to cover a gap the lane
closes is a knob to explain and then remove.

#### Slice 2 — sequence-cursored despawn delivery (`GameLogic`, M)

**Scope.** `World`'s despawn log gains a monotonic sequence; `DespawnLog()`/`ClearDespawnLog()` are
replaced by a cursor read (`DespawnsSince(seq)` returning a span and the new head); the log trims
to the minimum outstanding cursor, owned by the caller that knows the cursors (slice 3's publisher;
until then, the single adapter). The header comment that promised "per-subscriber the day there are
several" changes in the same commit — the day arrived.
**Out of scope.** The publisher itself; any wire change.
**Build on.** `World.h:62-70`, `m_despawnLog`, `WorldSimulation::SplitTheLost`.
**Acceptance.** `GameLogicTests` rows: two readers each see every death exactly once; a reader
joining late sees only deaths after its cursor; the replay gate untouched (the log is publish-side).
**ADR.** World's despawn contract moves from drain-once to cursors — supersedes the header's note.

**Landed.** Work order: [`DespawnCursors-work-order.md`](Archive/DespawnCursors-work-order.md). `DespawnLog()`
and `ClearDespawnLog()` became `DespawnHead()`, `DespawnsSince(cursor)` and
`TrimDespawnsBefore(cursor)`; `WorldSimulation` holds one cursor; ADR 0027 records why. Three
`GameLogicTests` rows replace the one that drained.

#### Slice 3 — the publisher (`GameLogic`, M)

**Scope.** `Publisher` owns N `Subscriber` entries — `{Transport&, InterestSet, SnapshotWriter,
FactionId, phase, orderBudgetPerTick}` — and per tick: runs due subscribers' interest updates
(phase = index % `INTEREST_UPDATE_EVERY_TICKS`, retiring finding E4's same-tick spike), drains each
transport's orders under its budget (E6), delivers despawns off slice 2's cursors, and sends the
full refresh on slice 1's cadence where configured. Subscriber center comes from the entry, not a
world scan.
**Out of scope.** Authentication, session lifetimes, the dedicated-server root (§4 decision 3).
**Build on.** `InterestSet`, `SnapshotWriter::WriteInterest`, `ReadMoveOrder`,
`World::IssueMoveOrder`'s faction gate, slice 2's cursors.
**Acceptance.** `GameLogicTests` rows over paired `LoopbackTransport`s: two subscribers with
different radii get independent bytes; no two of ≤6 subscribers are due the same tick; the
(budget+1)th order in a tick is dropped and counted; every death reaches every subscriber once;
`TheSameOrderProducesTheSameRun` unchanged (interest stays outside the replay contract).
**ADR.** GameLogic gains the session responsibility — ADR 0008's elimination re-run, §2 above.

#### Slice 4 — the root joins the publisher (`Outpost`, S)

**Scope.** `WorldSimulation` drops its one-of-each members for a `Publisher` with one entry;
`SubscriberCentre`'s fleet-centroid scan goes, per its own retirement comment. The boot log line
and HUD are unchanged.
**Out of scope.** A second real client (nothing dials in yet); the dedicated root.
**Build on.** `WorldSimulation.h:110-179`, `OutpostApp` boot order.
**Acceptance.** Boot log reads as before over both transports; screenshots at two window sizes;
stated assumption: still one subscriber in practice, now as a table of one.

#### Slices 5 and 6 — the reliable lane (= QuicTransport.md slices 3a and 3b) — **scheduled**

Fully specified in [`QuicTransport.md`](Archive/QuicTransport.md) §14, steps and acceptance included —
`SendReliable`/`ReceiveReliable` with refusing defaults, the loopback lane exempt from
`dropOneInN`, the reserved bidirectional stream with 2-byte framing, `KIND_LEAVE`, orders going
reliable, the ALPN bump to `outpost-2`, and the drop-everything test in which every leave and
every order still arrives. The work orders are written from that design when the owner takes §4
decision 1 — taken on 2026-08-30 — and are now written:
[3a](Archive/ReliableLane-work-order.md) and [3b](Archive/ReliableFormat-work-order.md). Slice 1's backstop is
dropped rather than built: it existed only to blunt E1 while the lane was unscheduled, and the lane
is the real answer.

#### Slice 7 — listener slot reclamation (`NeuronCore`, S)

**Scope.** `QuicListener::Poll` detects transports that have reached `Closed`, closes and
re-reserves them into a free list `OnNewConnection` draws from; `Accepted()` gains a removal story
(the poll reports departures, or hands out spans the caller re-reads). Ring capacity moves to a
`Desc` knob so a server role sizes 1,000 connections without 562 MB of rings. The header sentence
"a number that changes and not a class that gets rewritten" becomes true in the same commit.
**Out of scope.** Any accept policy (limits, bans); TLS changes.
**Build on.** `QuicListener.cpp:146-176`, `QuicTransport::Reserve`/`Close`, ADR 0022's
workers-enqueue rule (reclaim happens on the owning thread, in `Poll`).
**Acceptance.** `NeuronCoreTests` rows: backlog 2 survives 100 connect/disconnect cycles still
accepting; a recycled slot serves a fresh connection; existing QUIC tests unchanged.

### Phase 2 — survive density (slices 8–14)

#### Slice 8 — trail and glow batching (`NeuronClient` + `Outpost`, S)

**Scope.** Thruster glows and trails leave the one-draw-per-sample `DrawGlow` path
(`WorldView.cpp:1027-1075`) for the vertex-ring pattern `SpriteParticles` already demonstrates:
`WorldView` pushes camera-facing quads into a per-frame ring, drawn in one call per blend mode.
`DrawGlow` stays for genuinely single glows (the order marker) or goes if nothing needs it.
**Out of scope.** Any change to the trail's look; culling (slice 9).
**Build on.** `FxRenderer`'s rings and `FxVertex`, `SpriteParticles.cpp:102-158`,
`ViewTuning.h`'s trail constants.
**Acceptance.** A code read: glow/trail submission is ≤ 2 draws per frame at any ship count;
screenshots at two window sizes showing the plume unchanged; the ring headroom stated against
`MAX_FX_VERTS` in the pull request.

**As landed**, submission is **one** draw, not two. The look could not be preserved by reusing the
sprite pass: a glow is a procedural disc, `pow(saturate(1 - d), falloff)`, and a sprite samples a
texture. So the pass moved rather than the appearance — `FxGlowPS` is `DecalPS`'s glow branch
arithmetic for arithmetic, the new pipeline's blend and depth state is the retired `m_glowPso`'s
field for field, and what changed is where the quad's local coordinate comes from: the vertex, not a
per-glow matrix. `SceneRenderer::DrawGlow` had exactly one caller and is gone, with its pipeline and
the `decalParams.z` branch that only it reached.

**Headroom**: 6 verts a glow, so a three-nozzle hull running a full 32-sample trail is 576 and
`MAX_FX_VERTS` (49,152) holds 85 of them, shared with explosion fragments and sprites; past that the
ring's existing `DroppedVerts` counter reports rather than the frame growing. The draw count it
replaces, for a hundred such ships, was 9,600.

**Screenshots are owed and not supplied** — this container has no Windows, no D3D12 and no way to
run the game. In their place the geometry is verified against the path it replaces: a harness runs
the retired billboard matrix and the new builder over four camera bases (including a tilted eye),
three centres and three radii, and requires every corner to agree to 1e-3. Seven `NeuronClientTests`
rows pin the builder — the quad's shared diagonal, corners one radius out along the camera basis,
the uv the shader measures from, turning with the camera, and the two guards `DrawGlow` used to
apply. A reviewer on Windows should still take the two screenshots before this merges.

#### Slice 9 — frustum culling (`NeuronClient` + `Outpost`, S)

**Scope.** A `BoundingFrustum` built from the camera each frame
(`CreateFromMatrix(…, rhcoords: false)` — §5's LH rule), sphere-tested against ships, bodies and
effects before submission in `WorldView::Render`. A culled/submitted counter joins the HUD's debug
line so the number is visible, not inferred.
**Out of scope.** Occlusion; instancing (slice 10); the minimap, which deliberately sees everything
subscribed.
**Build on.** `Camera`'s view-projection, `DirectX::BoundingFrustum`, hull bounding radii the view
already holds.
**Acceptance.** The HUD counter shows off-screen ships unsubmitted while the minimap still plots
them; screenshots at two window sizes, one framing half the fleet out; no visible pop at the frustum
edge (radius-padded test stated in the order).

**As landed.** `Camera` gained `View()` and `Proj()` — a `BoundingFrustum` is built from the
projection alone and carried out by the inverse of the view, and neither survives their product.
`Neuron::WorldFrustum` and `IsSphereVisible` hold that in `NeuronClient`, so the handedness flag is
decided by tests rather than by looking: `rhcoords` is **false**, which
[Microsoft's own documentation](https://learn.microsoft.com/windows/win32/api/directxcollision/nf-directxcollision-boundingfrustum-createfrommatrix)
defines as left-handed, matching §5 and the camera's `LookAtLH`/`PerspectiveFovLH`. Passing `true`
does not fail — it builds a frustum pointing the other way and empties the screen — which is why one
test row exists for that alone.

Culled: **hulls** (the win — thousands of draws), the **selection and hover rings** that belong to
them, **bodies** (one decision per body, reused by the terrain and outline passes so they cannot
disagree and leave an outline hanging where its land was culled), the **shock rings**, and the
**plume**, which follows its hull.
The minimap is untouched, as the order requires: it draws from the snapshot and never asks what is
on screen.

**Padding**, per the order: `CULL_RADIUS_PAD_METRES = 24` on every hull sphere, because a hull's
mesh bounds are tight and the trail streams *behind* the ship, outside them entirely — the pad is a
trail length with room over. Bodies carry their own extent (radius × widest ellipsoid axis + tallest
tile), read off `BodyDesc`, so it is right on either bake path; `BodyBuildStats::maxHeightMetres` is
only filled on the CPU one, and `BodyDesc::maxHeight` scales colour and is usually zero.

**One thing culling must not decide**: what is remembered. `lastWorld`, `lastVelMetresPerSec` and
`drawn` are updated for every ship whether or not it was submitted, because a ship that dies off
screen still explodes and its shards must start where the hull actually was.

**Deliberately not culled — fragment and sprite builds.** Both are already one draw each for the
whole frame, so there is no draw to save, only CPU vertex building; and doing it properly needs a
debris-spread bound on `ShipExplosion` that grows with age, which is a question of its own. The
reason to revisit it is named here rather than left to be rediscovered: since slice 8 the glows
share that ring, so off-screen deaths can now crowd out visible plumes.

**Screenshots are owed** for the same reason as slice 8 — no Windows, no D3D12 here. Seven
`ViewCullingTests` rows stand in: in front versus behind, both planes, off to the side, the
conservative straddle, that padding can only ever keep more, that the frustum follows the camera
rather than sitting at the origin, and a negative radius. A reviewer on Windows should still frame
half the fleet out and read `DRAWN`/`CULLED` off the debug line before this merges.

#### Slice 10 — hull instancing (`NeuronClient` + `Outpost`, M)

**Scope.** `SceneRenderer` gains an instanced mesh path: a per-frame instance ring (world matrix +
tint per instance, one buffer per frame in flight, the fx-ring pattern), `DrawMeshInstanced(mesh,
count)` binding it at input slot 1, and hull vertex buffers move to default heaps with the upload
done once (the review's PCIe-per-draw note). `WorldView` buckets visible ships by `hullId` and
draws one instanced call per hull family. Root constants stay for everything non-instanced.
**Out of scope.** Body instancing (bodies are unique meshes until slice 20); skinning, LOD.
**Build on.** Slice 9's visible list, `MeshLibrary`/`MeshHandle`, `Scene.hlsli`'s vertex contract
(gains the per-instance stream), `GpuHelpers` pipeline creation.
**Acceptance.** A code read: the fleet is one draw per hull family present; screenshots at two
sizes; the shader change is `SceneVS.hlsl` + `Scene.hlsli` only, compiled by the existing FxCompile
settings.

**As landed**, the shader change is `Scene.hlsli`, `SceneVS.hlsl`, `ScenePS.hlsl` and a new
`SceneInstancedVS.hlsl` — one more file than the order predicted, and for a reason the order did not
foresee. `baseColour` is a *pixel* root constant, and an instanced draw has one root constant and a
tint per ship, so the tint had to move into `VsOut`. Both vertex shaders write it there and one
pixel shader reads it, which is why `params[1]` is now visible to `ALL` rather than `PIXEL`. The
alternative — a second pixel shader for the instanced path — would have been two copies of the
lighting to keep in step.

`DrawMesh` now binds the scene pipeline itself. It used to rely on `BeginScene` having left it
bound, which stopped being true the moment `DrawMeshInstanced` could switch it: a `DrawMesh` after a
batch would have drawn through the instanced input layout with no instance buffer, which nothing
reports and which looks like corrupt geometry.

**Default heaps landed with it**, and they contradict a comment that argued the other way. The
hulls average 32 kB, so the vertex fetch is 0.3 GB/s at a hundred ships — where `GpuHelpers.h` was
right that a staging copy is not worth it — 1.4 at five hundred and 5.6 at two thousand, where it is
not. Instancing removes the draws and the heap move removes the fetch; neither substitutes for the
other. `UploadMesh` records a copy now, so its three call sites are bracketed and
`SceneRenderer::DiscardStaging` joins the two that already existed.

Verified by pinning `MeshInstance`'s layout against the offsets `SCENE_INSTANCED_ELEMENTS` declares,
and by transforming a point through a matrix assembled from four rows the way the shader does and
requiring it to match the row-major `XMFLOAT4X4` path. The ring holds 4,096 instances — 320 kB a
frame, 1 MB over three frames in flight — and reports what it drops past that, like the effect ring.

**Screenshots are owed**, for the third slice running: nothing here can put a frame on a screen.

#### Slice 11 — localized gather radius and threat pre-filter (`GameLogic`, M)

**Scope.** `GatherNeighbours` stops paying the global worst case: a coarse pre-pass finds the
largest bounding radius actually present in each region of the index (the index already walks every
entry to rebuild — the maximum per bucket neighborhood rides along), the per-hull query radius is
derived from hulls locally present, and candidates are threat-filtered (closing or within margin)
before the 40-byte record and its sqrt are built. Sort-then-truncate and the (distance², ShipId)
total order are untouched — cell size stays out of the replay contract.
**Out of scope.** The tick rate (slice 12); threading the gather (a decision record of its own the
day it comes, per ADR 0022's confinement rule).
**Build on.** `World.cpp:305-352`, `SpatialIndex::QueryCircle` and its rebuild,
`HullSpec.h:227-237`'s radius derivation, the brute-force agreement test at two cell sizes.
**Acceptance.** The agreement test still passes at two cell sizes; the replay gate green; a new
benchmark row asserting gathered-candidate count tracks local density, with the N=5,000 sweep
number in the pull request beside ADR 0007's baseline.

**As landed**, the pre-pass is not a new one: `SpatialIndex`'s rebuild already walked every entry
and `World::RebuildIndex` already walked every ship, so `NeighbourhoodExtent` — the largest mobile
radius, the largest static radius and the fastest speed *present* — rides along for nothing.
`QueryRadiusMetres` takes it; the no-argument overload stays, spelling the hull table's worst case,
because that number sizes a region's ghost zone and is a world-layout constant rather than a
per-tick one. A fighters-only world drops from **595 m to 151 m — 6% of the area**.

The per-region version the order describes is *not* what landed, and the reason is worth recording:
it is circular. The radius chooses the covering cells and the cells determine the radius, so a large
hull just outside the chosen set is exactly the case that breaks it. A fixed point over per-cell
maxima would resolve that and is a slice of its own.

**Two defects the localisation exposed**, both latent and both masked by the slack the table's
worst case was buying:

1. `QueryRadiusMetres`'s avoid term omitted `AVOID_MARGIN_METRES`, which is part of the clearance
   `ThreatAlong` actually tests against. Every query was 8 m short of its own threat test — an outer
   band in which a neighbour existed and was not returned. Invisible while the term carried a
   Carrier's 107 m against an Interceptor's 8.7 m; not invisible once it carries what is present.
   The table's worst case moves 647 m → **655 m**, still inside the 700 m ghost-zone budget, and the
   four places that stated 647 are re-trued.
2. The pre-filter's first draft wrote an immovable neighbour off as a separation problem, which is
   what `QueryRadiusMetres`'s own two-term split reads like. `ThreatAlong` scores statics: a fighter
   closing on a Structure at 34 m/s must see it from 497 m, and the shortcut cut that to **115 m**.
   Measured, not reasoned — the shortcut was written, and the harness said otherwise.

The filter that landed is `min(pair reach, query radius)`. Clamping is what makes it
behaviour-preserving without asking anything of the query's formula: the query decides what exists,
the filter decides what is worth a 40-byte record and a `sqrt`, and it can never be the narrower of
the two by accident. Verified exhaustively — every ordered hull pair against every subset of the
table, 23,040 combinations, checking both that the filter never cuts inside `ThreatAlong`'s reach
and that it is genuinely narrower than the query (it is, in 22,374 of them).

#### Slice 12 — the tick-rate decision (`GameLogic`, now S + ADR)

**Decided 2026-08-30: 60 Hz stays**, so this slice shrank from a substepping change to a record.

**Scope.** The decision record only: why the rate is fixed, what the tunneling margin is that a
future change would have to re-earn (`TUNNEL_HEADROOM`, and the 1.70 m against 1.115 m that 20 Hz
would produce), and that the per-shard cost multiplier is a sizing input now rather than an open
question. No code, and `SimTuning.h` is untouched.
**Out of scope.** Variable timestep, ever; per-region tick rates (a later record if wanted).
**Build on.** `SimTuning.h:22-29`, the tunneling test, `ServerHost` untouched (its `tickHz` is a
`Desc` field already).
**Acceptance.** The decision record; the tunneling suite green at the chosen rate; the replay gate
green; `TUNNEL_HEADROOM`'s margin restated in the record.

**Landed** as [ADR 0045](Decisions/0045-the-tick-rate-is-fixed-at-60-hz.md), five months after the
decision was taken and one slice after this plan started citing it as taken — which is the drift this
plan exists to catch, found by reading its own status table against `Design/Decisions/`. No code, and
`SimTuning.h` is untouched as the scope said; what the record adds over the scope is the
substepping-versus-slower-tick comparison, because "60 Hz stays" is only half a decision without the
thing it was chosen over.

#### Slice 13 — churn-gated static rebuilds (`GameLogic`, S)

**Scope.** `SpawnShip`/`DespawnShip` dirty the static index only when the hull is immovable (the
id-shift hazard handled by static entries carrying handles, resolved at rebuild); `PathGrid::Rebuild`
bumps `m_version` only when the obstacle set actually differs from the one it built from.
**Out of scope.** Regional grids (slice 14).
**Build on.** `World.cpp:44-48,82,265-287`, `PathGrid.cpp:24-26`, `RouteOf` for the assertion.
**Acceptance.** `GameLogicTests` rows: a mobile spawn/despawn leaves `PathGrid` version and every
`RouteOf` untouched; a static spawn still rebuilds and replans; the replay gate green.

**As landed**, the id-shift hazard is handled by reading the moved ship rather than by giving static
entries handles. `DespawnShip` reads two hulls before anything moves -- the one leaving, and the one
swap-and-pop is about to move into its place -- and dirties if either is immovable. Handles in the
static entries would also have worked and cost every entry a widening plus a resolve per rebuild, to
buy exactness the two reads already have: they are a superset of the store's own `immovable &&
collidable` filter, so the gate cannot miss a rebuild that was needed, only pay for one that was not
(a Stargate, which does not churn). Verified exhaustively over every ship table up to length five
against a brute-force static-set oracle, plus three `PathfindingTests` rows.

#### Slice 14 — regional pathfinding (`GameLogic`, L — design first)

The 16.4 km cliff (`PathGrid.cpp:64-65`) is real rework: per-cluster grids keyed by obstacle
islands, route stitching between them, and the decline path retired. It gets its own design
(`Design/Archive/RegionalPathfinding.md` — problem, cluster rule, memory budget, its slices) before any
work order, per `Design/README.md`'s "design (if non-trivial)". This plan only fixes its position:
after slice 13, before any content spreads architecture past one grid. The `FindPath`/waypoint seam
and the route follower are the interfaces that design must keep.

**As landed**, this slice became five commits: the design
([`RegionalPathfinding.md`](Archive/RegionalPathfinding.md), ADR 0033), then each of the four slices
it yields in its §9, in order. The cliff is gone — two stations 20 km apart both route, where before
neither did — and the measurements are in that design's §4 and §9. What the *writing* turned up,
before any of it was built, is still worth carrying back:

- **A third failure the review did not have. Now fixed.** The grid's lattice was a function of its
  contents — `m_origin` was the corner of the bounding box over the obstacles, so adding an obstacle
  900 m west shifted every cell centre under every fixed world point by 4 m, and a rock dropped 4 km
  west moved a fixed probe's clearance by 24.2 m. Invisible then because any change re-plans
  everything anyway; fatal to any regional scheme, because two neighbouring grids would disagree
  about where a cell is. That design's slice 1 has landed:
  `PathCellX`/`PathCellZ`/`PathCellCentre` are the world's lattice and a grid holds a window on it.
- **Islands beat sectors**, which is not what "regional" suggested. A sector-aligned grid is the
  natural unit and costs 256 kB whether it holds one Structure or none of it; a hundred populated
  sectors is 26 MB spent to say "open". Islands take the same fixed lattice and size themselves to
  the content: a lone Structure is 9 kB, thirty scattered stations about 270 kB.
- **The stitching already exists.** `World::AdvanceRoute` re-plans on arriving at the end of a route
  whose `reachesDestination` is false, which is exactly what a route across islands is. No portal
  graph, and none needed while the space between islands is open. This held: the crossing needed no
  new machinery in the follower, only a leg aimed at the open water between two islands and an
  answer that says the route is unfinished.

One decision the design did not anticipate came out of building it, and has its own record: the
router's version stays the whole world's rather than becoming per-island, because an island index is
not a handle and the partition renumbers on every change (ADR 0034). So a change to architecture
anywhere still re-plans every routed ship — the one place islands did *not* make the world local,
and the next thing to bite.

### Phase 3 — make it an MMO (slices 15–20)

#### Slice 15 — the quantized wire (`GameLogic`, M)

**Scope.** The ship record's positions become sector id + u16 local offsets (0.125 m steps in an
8,192 m sector), angles become turns16, and `prevPos` is derived receiver-side for handles already
held (sent in full on first sight). `ShipsPerSnapshotFragment` follows the record down
automatically, per its own derivation. ALPN bumps (`outpost-3`, or rides slice 6's bump if
scheduled together). The stale prose becomes true in the same commit: AGENTS.md R6's "a wire in
centimeters", `.clang-tidy`'s quantization note, Collision.md §3's promise — this is the slice they
have been describing.
**Out of scope.** Delta-against-baseline compression; encryption (QUIC's already).
**Build on.** `WorldSnapshot.cpp`'s ByteWriter/ByteReader, `WorldPos`'s invariant (local offsets
already live in [0, 8192)), `SnapshotTests`.
**Acceptance.** Round-trip error bounds asserted (≤ 6.25 cm position, ≤ π/2¹⁶ rad heading);
interpolation continuity across a sector boundary tested; records per fragment stated in the pull
request against the 13 of today; the replay gate untouched (the wire is not simulated).

#### Slice 16 — global entity identity (`GameLogic`, M + ADR)

**Scope.** A globally unique id — the record decides the shape; the candidate is u64
`{shard:16, slot:24, generation:24}` — issued by `World` from a shard id it is configured with, and
carried on the wire where `ShipHandle` goes today; handles stay the in-process reference.
`FactionId`'s width is revisited in the same record (§4 decision 4).
**Out of scope.** Actual handoff protocol between servers; ownership transfer.
**Build on.** `ShipHandle`, `WorldSnapshot`'s record, slice 15's format machinery.
**Acceptance.** A test with two `World`s: an entity despawned in one and spawned in the other under
the same global id is the same entity to a receiver (no destroy+enter); existing handle tests
unchanged.

#### Slice 17 — the state codec and the replay gate (`GameLogic`, M)

**Scope.** `WriteWorldState`/`ReadWorldState` beside the snapshot functions: the full authoritative
state — ships with routes, patrols, order intent, the pieces the snapshot deliberately withholds —
serialized field-by-field like everything else on ADR 0008's territory. And the payoff the tree has
been promising itself: a recorded-replay gate in `GameLogicTests` (Collision.md admits none exists
— only same-process rerun equality), now possible because state can be saved at tick T, reloaded,
and replayed to equality.
**Out of scope.** A save-file format for players; versioning beyond a format byte; persistence
scheduling.
**Build on.** `WorldSnapshot.cpp`'s writer/reader, `World`'s dense arrays (POD, no pointers — the
review verified nothing resists this), slice 16's ids.
**Acceptance.** Save→load→replay equality, every field, every tick, in the suite; AGENTS.md §8's
"replay-equality test" checklist line becomes literally true and Collision.md's admission is
updated in the same commit.

#### Slice 18 — copy-queue uploader and store eviction (`NeuronClient`, M + ADR)

**Scope.** `GpuDevice` gains a copy queue and an upload path that hands off by fence instead of the
drain-twice `BeginUploads`/`ExecuteAndWait` bracket; `SceneRenderer::m_meshes` and
`BodyRenderer::m_bodies` gain generation-tagged free lists behind `MeshHandle`, and F5's reseed
frees the scene it replaces instead of leaking it (`OutpostApp.cpp:490-492`'s admission retires).
**Out of scope.** Asynchronous disk IO (loads stay synchronous; they stop stalling the GPU);
texture formats (slice 19).
**Build on.** `GpuDevice.cpp:204-225`, `RenderTypes.h:31-39`'s indirection, the fence machinery the
frame loop already runs.
**Acceptance.** A code read: no full drain on the load path; free-list bookkeeping unit-tested
CPU-side; ten F5 reseeds hold the store count flat; frame-time across a mid-session bake measured
and stated in the pull request.
**ADR.** GpuDevice gains the copy queue — a library gains a responsibility.

**As landed**, it was written where there is no Windows, no D3D12 and no GPU, so on its first branch
what was measured was the half that could be — `HandleStore`'s bookkeeping, by eleven test rows and
by seven mutations — and every line of D3D12 in it was read rather than run. That is why the
remaining client slices were not stacked on it (owner's call, 2026-08-30). **Re-landed on its own
branch the same day, it was built and run on Windows before merging:** Debug|x64 compiles clean on
the first attempt, the game boots over the copy queue (hulls, bodies, sky), two F5 reseeds exercise
the mid-session path, and every suite passes with `HandleStoreTests` in it.

ADR 0044. The scope divided into two halves that turn out to be the same shape twice — nothing
could say a resource was finished with, on either side.

**The uploader.** `BeginUploads` reset allocator 0, which is also frame 0's, so it had to drain the
whole GPU first. `GpuDevice` now has a `COPY` queue with its own allocator, list and fence, and
`BeginCopies`/`SubmitCopies` around it: `SubmitCopies` signals the copy fence and makes the graphics
queue wait on it, and **the CPU is not blocked at all**. `BeginUploads` keeps its direct bracket for
the work a copy queue cannot do, and gains an allocator of its own so it waits for the previous
upload rather than for every frame.

**The plan's sentence for this slice does not survive contact with the bracket**, which is the
finding worth carrying forward. It asks for the upload path to move onto the copy queue; a copy
queue cannot run `BodyRenderer::BakeBody`'s **compute dispatch**, cannot express
`UploadCoverageTexture`'s transition to `PIXEL_SHADER_RESOURCE`, and cannot express `ReadBackBody`'s
transition out of a graphics state. So the bracket splits by *what the work is*: static buffers —
every hull, every body mesh, the sky — go to the copy queue, and bakes and readbacks stay. The
textures follow when slice 19 rewrites their upload for BC and mip chains, which is what untangles
them from the bake that reads one.

**The barriers came out rather than moving.** A buffer created in `COMMON` is promoted to
`COPY_DEST` implicitly, everything a copy queue touches decays back to `COMMON` on submission
completion, and the first graphics use promotes it to `VERTEX_AND_CONSTANT_BUFFER` for free. The
`ResourceBarrier` that used to follow the copy is now both unnecessary and illegal on that queue.
That is Microsoft's documented behaviour and it is cited at the code, because it is the kind of
claim a reader should not have to take on trust.

**The store.** `HandleStore` holds the slot/generation bookkeeping and no D3D12, which is what let
it be tested without a device. A handle is a slot and a generation packed into the 32 bits
`MeshHandle` already was, so no call site changed: 65,535 live meshes against a tree with eleven
hulls and eight bodies, and 65,535 reuses of a slot against a key somebody presses by hand. Both
renderers index by slot and resolve by handle, so a stale handle now draws nothing rather than
whatever took its place. **F5 frees the scene it replaces**, and `OutpostApp.cpp`'s admission beside
the key is retired.

**Measured, where measuring was possible.** Seven mutations were introduced into `HandleStore` one
at a time: no generation bump on free, a FIFO free list, the reserved slot allocated, the live flag
ignored on lookup, a generation wrapping to 0, and two more. **Five went red at once; two did not,
and both were holes in the tests** — nothing reached the store's 65,535-slot cap, and nothing
fabricated a handle carrying a freed slot's new generation, which is the one case the generation
alone cannot catch and the `live` flag exists for. Both closed, both now red. The comment claiming
the two tests were independent was *wrong* before that and is corrected.

**Not in this commit**: the frame-time measurement across a mid-session bake that the acceptance
above asks for. It needs Windows, a GPU and a stopwatch.

#### Slice 19 — compressed textures and the descriptor allocator (`NeuronClient`, M)

**Scope.** The upload path accepts what `DdsImage` already parses: BC formats and mip chains, via a
subresource-walking upload on slice 18's copy queue. One shared shader-visible descriptor heap with
a free-list allocator replaces the per-pass constant-sized heaps; passes keep their root
signatures and take offsets instead of owning heaps.
**Out of scope.** Runtime compression; texture streaming by residency (all-resident continues,
now 4× cheaper).
**Build on.** `DdsImage.h`'s subresource array ("already in D3D12 order" — the review verified),
`GpuHelpers.cpp:132-277`, slice 18.
**Acceptance.** Allocator alloc/free/reuse unit-tested; a BC-compressed DDS renders (screenshot
near and far — the far one is what mips fix); `TopMipAsBgra` remains only where a reader genuinely
needs CPU pixels.

#### Slice 20 — body LOD and culling completion (`NeuronClient`, M)

**Scope.** Three grids baked per body at boot (the bake already writes wherever it is pointed —
ADR 0020), selection by projected radius per frame; back-face culling enabled for the body pass
once winding is confirmed (`PlanetRenderer.md` says it is one line); distance culling for
asteroids rides slice 9's frustum pass. Sea-floor degenerate fill is dropped where the cheap
compaction exists, else stated.
**Out of scope.** Continuous LOD, morphing, per-body texture work.
**Build on.** `BodyMeshBuilder`, `BodyRenderer`, `PlanetRenderer.md` §13's own deferred plan,
slice 9.
**Acceptance.** Screenshots at two zooms with no visible LOD pop at the switch distances stated in
the order; memory per body at three grids stated against today's 4.13 MB; F5 reseed still works.

### Phase 4 — keep it consistent at team scale (slices 21–23)

#### Slice 21 — guard widening and the docs re-trued (`Build/` + prose, S)

**Scope.** `CheckProjectFiles.py` derives the game-header list from `GameLogic/*.h` instead of the
stale six, and gains the two greps AGENTS.md currently leaves to eyes: R2's banned affixes and
R11's family list. The six stale sentences fix: both lint configs' "nothing runs this" blocks,
`build.yml:31`'s FXC, AGENTS.md §2's map (the missing subsystems), §3/§8 admitting `*CS.hlsl`,
`WorldSnapshot.h:27`'s 120 bytes, `WorldPos.h:13`'s ±10¹⁹. `ShockRing-work-order.md` moves to
`Design/Archive/`.
**Out of scope.** The R11 prose sweep (its own documentation pass, already owed); any C++.
**Acceptance.** A deliberately planted violation of each new check goes red in a run linked from
the pull request (measured, not read — the tree's own standard); `CheckProjectFiles.py` and
`CheckFormat.py` green on the clean tree.

**As landed.** The game-header list is fifteen read off disk, not six spelled out. Four source
checks joined the guard: R2's affixes, R11's families, and two the slices above earned rather than
the review — a declarator named after a `<windows.h>` macro (`Link near;`, eight errors none of
which named the line), and a braced call argument of nothing but literals (`{0, 256, 3}` bound a
loss test's `dropOneInN` to a ring depth when a field landed between two others, so the test passed
while measuring nothing). Both cost a CI run each. Acceptance was met: five planted violations, five
reports, exit 1; the clean tree exits 0.

The R11 check reads only names the tree *declares*, told apart by R1's `_`, R8's `m_`/`sm_`, R3's
UPPER_CASE and type names — because D3D12 and Win32 spell `Color` and `Center` on every second line
and are not ours to rename. The literal-aggregate check refuses the general case for the same
reason: a positional aggregate of *variables* is the same hazard and needs types to see, so it is
declared uncaught at the check rather than implied covered. A guard that overstates its reach is the
thing this slice exists to remove.

Two candidate checks were measured and **not** shipped. A general "bare braced argument" rule scored
65 hits on the clean tree, nearly all legitimate — rows of `HULL_SPECS`, vertex constructions,
`std::max({_a.x, _b.x, _c.x})` — and a guard that cries wolf is worse than no guard. Narrowing it to
all-literal lists inside a call took that to zero while still catching the original defect, which is
the version that landed.

Six sentences re-trued, all of them measured rather than re-read: `ShipState` is **128** bytes and
`WorldSnapshot.h` claimed 120; `WorldPos` reaches **±7.6×10²² m**, about eight million light years,
and claimed ±10¹⁹; `build.yml` credited FXC for shaders the projects ask of DXC at shader model 6.7;
`.clang-tidy` still said nothing ran it, when it gates GameLogic; its "R2 and R7 have their own CI
step" is now true and names the file; AGENTS.md §3 and §8 admit `<Name>CS.hlsl`, which the tree has
had two of. `.clang-format` said the same thing `.clang-tidy` did and is corrected the same way.
§2's map gained the two subsystems it omitted outright — the planet pipeline and the star field,
nine headers between them — and a line on the repository's name (C4): it is historical, Outpost is a
Windows game, and the tree's own standard is that an unexplained thing gets one sentence rather than
a shrug. `ShockRing-work-order.md` moved to `Design/Archive/`, and `ViewTuning.h:108`'s citation
followed it — a citation meant to be followed (`Design/README.md`) is worth no more than its path.

Left standing deliberately: R11's prose sweep, which the work order puts in a pass of its own, and
`WorldSnapshot.h`'s "wire in centimeters", which is slice 15's layer and not yet built.

#### Slice 22 — legacy helper cleanup (`NeuronCore`, S)

**Scope.** The dead pieces of `NeuronHelper.h` go — `BaseException`, `NonCopyable`, `ENUM_HELPER`,
`ENUM_FLAGS_HELPER`, `IsValidEnum`, the commented-out block — and what survives (`ScopedHandle`,
`SafeHandle`) conforms to §1; `FileSys`'s `hFile` locals and bare `uint8_t` fixed, `sm_homeDir`
gains its thread-safety sentence.
**Out of scope.** New helpers; behavior changes.
**Acceptance.** Grep evidence of zero uses in the pull request; all four suites green; clang-tidy
clean on the touched files.

**As landed**, 161 lines of the header became 25. `AGENTS.md`'s macro-naming row cited `ENUM_HELPER`
as its example and its clang-tidy history cited the `NOLINT` over it, so both were re-trued in the
same commit -- a citation to a symbol that no longer exists is the drift this plan is about. Two
things the sweep turned up are deliberately left: `NeuronCore.h`'s `<type_traits>` now has zero
users in the tree, and `World.cpp:617` spells a bare `size_t`. The first needs a build to remove
safely (an umbrella include is exactly what something relies on transitively), and the second is
`GameLogic`, not this slice's layer; both belong with slice 21's R10 grep, which is what should find
them rather than a reader.

#### Slice 23 — clang-tidy widens a project (`.github/`, S)

**Scope.** The tidy job takes its next project (NeuronServer — smallest, headless, no D3D12
headers), landing non-blocking and promoted on a clean run — the tree's own pattern, recorded in
AGENTS.md §6 as the only way a linter should start gating. Repeat per project as slices of a
sitting each; NeuronCore follows slice 22 so the sweep meets a clean file.
**Acceptance.** Two green runs, then the promotion commit; AGENTS.md §6's scope sentence updated
in the same commit.

**As landed**, NeuronServer joined the step **advisory**: the exit code is GameLogic's count alone,
so a finding in NeuronServer is printed and the job stays green. The promotion commit is owed and is
one line — `exit $counts['GameLogic']` grows a `+ $counts['NeuronServer']` — after two runs on the
runner come back clean.

The step now walks a list of projects rather than one directory, so the third joins as one entry
rather than as a second copy of the loop. Its file list is still read off disk, for the reason the
test-suite list is: a file a later slice adds is checked the day it lands.

**Measured before landing, not asserted**: `clang-tidy 18` on Linux over `ServerHost.cpp` — which
reaches `ServerHost.h` and `Simulation.h` through the header filter — reports nothing at all. That
is why NeuronServer was the project to take next, along with being the smallest, headless, and one
of only two that do not reach the D3D12 headers. It is *not* why it gates: the runner ships the
VS-bundled LLVM 22, and `.clang-tidy`'s own status block records that the newer one has already seen
a defect the older one did not. "Clean" is always clean under something, and the something that
counts is the runner.

NeuronCore is next, and slice 22 swept its one header for exactly this.

#### Slice 24 — the server configuration file (`Outpost`, M + ADR)

**Scope.** The shape a headless composition root reads its port, backlog, world seed and subscriber
limits from, and a hand-written parser for it in `Outpost` — not in a library, and not a generator
(§1 R7). Libraries keep taking plain `Desc` structs; the root is still the only thing that reads
configuration, which is what AGENTS.md §5 has always said.

**Out of scope.** The headless executable itself, argv, the environment, and any live reload. A file
read once at boot is the whole of it.

**Acceptance.** A malformed file reports what was wrong and fails closed rather than throwing or
asserting (§5's rule for anything parsing content); every value the current `Outpost.exe` hard-codes
can be expressed; the existing boot is unchanged when no file is present. A decision record for the
format and for why a file does not bend §5.

**As landed**, the file is `Outpost/Assets/Server.cfg` — deployed beside the executable in the shape
every mesh and font already uses — and it carries five keys, each with a consumer today: `port`,
`backlog`, `interestRadiusMetres`, `interestUpdateEveryTicks`, `ordersPerTick`. The `shard` key the
first landing carried went with slice 16, whose `World::ConfigureShard` is its only consumer; it
returns with that slice. Work order:
[`ServerConfig-work-order.md`](Archive/ServerConfig-work-order.md); ADR 0043.

**"Fails closed" turned out to be two rules, not one**, and the split is the useful part of the
slice. The parser applies *nothing* on a refusal — so half the admin's file and half the defaults,
with nothing saying which, is a state that cannot exist — and it decides nothing about what to do
next. The root decides: this one logs `CONFIG REFUSED` and boots on the defaults, because a typo in
a tuning file should not be a black screen, while a headless root prints the same message and exits
non-zero. Same parser, two roots.

**An unknown key is a refusal**, which is the one design choice a reader will want argued: ignoring
it turns `prot = 40000` into a silent revert to 30081, and that looks exactly like the file working.

**The world seed is not in the file**, and this table's sentence for it is why that is stated rather
than quietly dropped. Nothing reads one: `UniverseLayout` exists (ADR 0037) and is not wired into
the root, and the two seeds the root does hold are presentation, live in `ViewTuning.h`, and are
tunable at any time by the rule that separates that header from `SimTuning.h`. A knob with no reader
is a knob to explain and then remove.

**Verified by harness, not by suite.** `Outpost` has none — ADR 0014's standing assumption, the one
slices 1, 8, 9 and 10 also made. The parser compiles on Linux unmodified, so the harness runs the
shipped translation unit rather than a copy: every refusal the order names, the shipped file parsing
to exactly the defaults, and 200,000 fuzzed inputs under ASan and UBSan without a throw, an abort or
a silent refusal. 200,127 checks, none failing.

**What it retires**: `OUTPOST_QUIC_PORT` as a literal in three places, and §4 decision 3 as an open
question. What it does not: the headless executable itself, which stays out of this plan (§5).

---

The plan ends where the review did: slices 1–7 make it a networked game, 8–14 make it survive
density, 15–20 make it an MMO, 21–23 keep a bigger team from eroding what makes this tree work.
