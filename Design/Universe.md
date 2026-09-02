# Universe — the galaxy, its gates, and the shard that carries it

**Status: agreed with the owner on 2026-09-01 — the eight decisions in §12 were put and taken the
same day, each the recommended option, in an interactive session run against a live workbench: the
shipped layout algorithm with its knobs exposed, and a working prototype of the galaxy lattice and
all four candidate gate graphs.**

**Slices 1 to 5b have landed.** The galaxy exists — 54 systems, 164 Vanguard stations and 136 gates,
with home exactly where it always was — a player can select a fleet, press `JUMP`, tap a gate and
cross it, camera and all, the worlds, rocks and minimap marks on the far side are the far side's, and
the universe no longer dies with the process. It is also no longer *made* by the process: `UniverseGen`
writes one and `Outpost` runs it. And building in one system no longer re-plans routes in the other
fifty-three. **Every slice has landed.**

Five sections changed on contact and say so where they stand: §3.4's separation arithmetic (a
disc's, where the jitter is a square), §10's gate radius (a circle inside the structure, which
nothing could ever enter), §10's gate ring (past the path grid's ceiling), §9's scenery, which
slice 4 deliberately did not finish and 4b did, and §8's header, which was a byte short of being
able to tell a torn file from a whole one.

**Slice 1**: `LayOutGalaxy` is in `GameLogic` with its twelve-row suite, and
[ADR 0055](Decisions/0055-the-galaxy-is-one-seed-and-its-gates-are-the-relative-neighborhood-graph.md)
carries the two decisions it took.
**Slice 2**: gates, the `Jump` order, `StepJumps`, `JumpedOut` and the wire's fourth departure run
are in, with [ADR 0056](Decisions/0056-a-jump-is-a-despawn-and-a-spawn-under-one-identity.md).
**Slice 3**: genesis builds the galaxy at boot. **Slice 4**: the sheet's fifth command, the gate
pick, the silent removal and the camera that crosses with the fleet. **Slice 4b**: `Game::SystemAt`
answers which system a point is in, and the client rebuilds its worlds, rocks and marks when the
camera's answer changes.
**Slice 5**: the save file, with [ADR 0057](Decisions/0057-the-save-is-a-versioned-file-and-a-refused-one-stops-the-boot.md).
**Slice 5b**: genesis leaves the game for `Tools/UniverseGen`, with
[ADR 0058](Decisions/0058-a-universe-is-authored-by-a-tool-not-by-the-program-that-runs-it.md).
**Slice 6**: a route is scoped to the island that planned it, with
[ADR 0059](Decisions/0059-a-route-is-scoped-to-the-island-that-planned-it.md), superseding 0034.
This document is amended in place as its slices land (ADR 0054).

The player-facing sentence: **the frontier stops being one system.** Today the universe is three
planets from one seed, a government at each of them, one rival base, and nowhere else to go. After
this design it is a galaxy of systems laid out from one seed, joined by jump gates whose map has
real chokepoints, crossed by a fleet order, carried by one `Universe` per shard — and it survives
the process that ticks it, because the state codec finally gets a file.

The design argues the whole shape — the multi-system, eventually-sharded galaxy — and its slices
land system-first in the one process that exists. That is this tree's own pattern: QUIC ran across
`127.0.0.1` before there was a second machine, precisely so that nothing would need rewriting on
the day there was (ADR 0021, 0028). The fence around what the slices may *not* build yet is §11's.

---

## 1. What is being built

- **`LayOutGalaxy`** (§3): the galaxy as a pure function of one seed — a hex lattice of candidate
  cells walked in one fixed spiral order, each cell drawing occupancy, jitter and a system seed;
  an authored pin table that overwrites draws and never skips one; and per-system layouts grown
  from each system's own seed through the shipped planet loop.
- **The gate graph** (§4): the relative neighborhood graph over the laid-out systems — connected
  by theorem because it contains the minimum spanning tree, sparse enough that real chokepoints
  exist.
- **Gates as simulation rows** (§5): a gate is a Structure ship with a row in a gate side table
  naming its destination — the stations pattern (ADR 0038), re-run.
- **The jump** (§6): a fleet order. The fleet flies to the gate, and the tick in which every
  member stands inside the gate radius moves the whole fleet together — a despawn with a new
  cause, `JumpedOut`, and a spawn under the same identities at the far side. One door, and
  cross-shard handoff is the same door with a transport in the middle.
- **One `Universe` per shard** (§7): a shard's systems all live in its one universe, far apart in
  sector space. Nothing new ticks, nothing freezes, and the spatial index was already built for
  unbounded spread.
- **The save file** (§8): the state codec given a versioned file, an atomic write, a cadence, and
  a boot that restores what it finds — or stops, rather than quietly rebuilding genesis over a
  universe somebody was living in.

## 2. What this design builds on, and the sentences it retires

The tree has been writing IOUs against this design for a while, and most of the machinery is
already cut:

- **The jump-out door is anticipated by name.** `DespawnCause` was opened "the width of one list
  and no wider" with jump-out listed as a future cause (`Universe.h`, ADR 0040). Slice 2 walks
  through the door that comment holds open, and rewrites it to say so.
- **A second system is content, not redesign.** `LayOutSystem` takes a star position for exactly
  this day, and ADR 0037's consequence list says so in as many words. Its "nothing calls it twice
  today" retires with slice 3.
- **Identity already survives a handoff.** `EntityId` is `{shard:16, serial:48}` and
  `SpawnShipAs` spawns under an id minted elsewhere, advancing the serial counter past it
  (ADR 0047, `EntityIdentity-work-order.md` §3.4). The jump pass is the caller that machinery
  was built waiting for.
- **The index is unbounded by construction.** `SpatialIndex` hashes cells into buckets rather
  than gridding the entries' bounding box, because "two ships a hundred kilometres apart would
  otherwise allocate a cell table between them" (`SpatialIndex.h`). Two *systems* a hundred
  kilometers apart are the same case.
- **The state already round-trips.** `WriteUniverseState`/`ReadUniverseState` reload and replay
  to byte equality (`UniverseStateTests`, `WorldState-work-order.md`). The save file is that codec
  plus a header, a file and a rule — AGENTS.md's "no save *file*" sentence changes in slice 5's
  commit.
- **A deployment already has a file.** `Server.cfg` is where what a deployment may change without
  a rebuild lives, read by the composition root alone (ADR 0043). The save path and cadence are
  exactly that kind of value.
- **Pathfinding is islands on a world-fixed lattice** (ADR 0033), so architecture 52 km apart
  partitions into independent islands by construction — and ADR 0034's "the next thing to bite,"
  the route version that is the whole universe's, is the bite this design schedules slice 6 to
  take.

## 3. The galaxy from one seed

### 3.1 The lattice and the walk

The galaxy is candidate cells on a hex lattice, walked in one fixed order: ring 0 outward, each
ring traversed the same way every time. One `Neuron::Pcg32(galaxySeed)` serves the whole walk, and
**every cell takes exactly four draws, occupied or not, pinned or not**: an occupancy unit, a
jitter pair, and a 64-bit system seed assembled from two `Next()` calls — the body-seed idiom from
`UniverseLayout.cpp`, one level up.

That fixed spend is the property everything else stands on, and it is `LayOutSystem`'s own rule
re-run: a cell's draws belong to the *cell*, not to the census. Raise the shipped density and
every system that already existed holds its position, its seed and therefore its planets; new
systems appear between them. A galaxy can be retuned without being rerolled, which is what lets
density be a balance knob rather than a new universe.

A cell is occupied when its occupancy unit falls under the density. Bearings needed no such gate
at system scale because slots spread planets by construction; occupancy needs one here because a
galaxy wants unevenness — and the jitter stays bounded (§3.4) so the unevenness never becomes an
overlap. No rejection loop anywhere: the layout is deterministic and non-overlapping by
construction, not by retrying.

### 3.2 The pins

An authored pin table rides beside the seed, and a pin **overwrites what was drawn and never
skips a draw** — `pinFirstPlanet`'s sentence at galaxy grain, for its reason: a flag that shifted
the stream would make one seed mean two galaxies.

One pin ships: the home system, at cell (0,0), jitter overwritten to zero and system seed
overwritten to `UNIVERSE_LAYOUT_SEED` ("Sys1"), laid out through `LayOutSystem` with
`STARTING_SYSTEM` exactly as today — the pinned first planet, the carefully framed opening shot,
all of it. The galaxy grows around the scene the game already boots into; nothing about home
changes but the map it sits on.

### 3.3 Per-system layout

An unpinned system's layout comes entirely from its own 64-bit seed: one `Pcg32(systemSeed)`
draws the system's *description* first — planet count 2–5 by `Below(4)`, against the shipped
orbit and radius bounds — and then runs the same four-draw planet loop `LayOutSystem` runs today.
The loop is shared, not copied, so the two callers cannot drift: `LayOutSystem` keeps its
signature and its meaning (a described system from a seed), and the per-system recipe is a second
entry point that draws the description before handing the same generator to the same loop.

The draws stay position-and-seed only. What a planet wears is still the client's
(`BodyCatalogue`, ADR 0037's discipline), and each planet's body seed flows to it exactly as
today.

### 3.4 Scale, and the proofs that come with it

The lattice pitch is **16 sectors — 131 072 m** between neighboring cells, and jitter is bounded
to **0.20 of the pitch** per axis. Minimum separation between two systems' stars is therefore

    (1 - 2 * sqrt(2) * 0.20) * pitch = 0.434 * pitch = 56 926.5 m, by construction

— against a worst-case system half-width of 6 500 m plus a station's approach traffic, a gather
radius of 655 m and a ghost zone of 700 m, which is not a margin so much as a different order of
magnitude. The bound gets a test the way `PlanetsKeepTheirDistance` has one: a proof over the
construction, not a sample over seeds.

**The `sqrt(2)` is what this section got wrong first, and it is worth keeping visible.** This design
originally wrote 0.30 jitter and proved `(1 - 2 * jitter) * pitch`. That is a *disc's* arithmetic,
and the jitter is a *square* — two independent draws, which is the cheap and deterministic way to
draw one — so a star's worst displacement is its diagonal rather than its edge, and the real bound
was 19 851 m where the design claimed 52 428.8. `SystemsKeepTheirDistance` failed on its first run
and said so. The formula is now `MinimumStarSeparationMetres`, one function so the test and every
caller cannot each state it and drift, and the shipped jitter moved to 0.20 so that the separation
this design asked for still holds with room over (ADR 0055, `Universe-slice-1.md` §7).

Three consequences fall out of the spacing rather than needing machinery:

- **Islands stay per-system.** No static content of one system comes within a path grid's reach
  of another's, so `PathIslands` partitions the galaxy into per-system islands by construction and
  the per-island ceiling proof (`TheLayoutRespectsTheGridCeiling`) holds per system, unchanged.
- **Interest stays local.** No subscriber radius spans systems; a client sees the system it is in.
- **The wire never notices.** A seven-ring galaxy spans about ±130 sectors; the wire's i32 sector
  index saturates at ±1 858 light-years (ADR 0046). The light-years on a player's galaxy map are
  fiction the client draws; the meters on the plane are the simulation's, and there are not very
  many of them.

The simulation is still a plane and there is still no Y (ADR 0016, 0025). A galaxy does not
change that; it just puts more of the plane to work.

## 4. The gate graph

### 4.1 The rule

Gate links are the **relative neighborhood graph** over the laid-out system positions: systems A
and B are linked unless some third system C is strictly closer to both — `max(d(A,C), d(B,C)) <
d(A,B)` suppresses the edge. Computed once at genesis, at boot time, from positions the lattice
already made distinct; distances are compared squared, and the strict inequality plus distinct
positions is what makes the answer deterministic with nothing added.

The rule was chosen for a theorem: **the relative neighborhood graph contains the minimum
spanning tree, so the galaxy is connected for every seed** — a property a test states with a
union-find walk rather than a hope stated over samples. The alternatives lost on exactly this
line (§12, decision 8): k-nearest strands pockets and would need a repair pass, which is a
rejection loop by another name; a bare spanning tree makes every link a chokepoint; Gabriel is
the same theorem with a denser graph, and it trades away the map's strategy.

### 4.2 Chokepoints are the point

Sparse-but-connected is the gameplay. A relative neighborhood galaxy has bridge links — cut one
and the galaxy falls in two — and those are territory the day anything can be contested, and
routes worth knowing the day anything is traded. The design does not build contest or trade
(§11); it builds the map on which they will be worth building.

## 5. What a gate is

A gate is **a Structure ship with a row in a gate side table** — ADR 0038's pattern, re-run for
its reasons. The ship keeps doing everything a Structure already does: static index, obstacle
set, record on the wire, a thing a tap can name. The row is what knows it is a gate:

- the structure, held as a `ShipHandle` and read through `Resolve` (ADR 0005);
- the destination, held as the far gate's **`EntityId`** — resolvable in-universe through
  `HandleOfEntity` today, and already the currency that crosses shards tomorrow (ADR 0047), so
  the field never needs to change shape when the far side stops being local;
- an owner faction, seeded `FACTION_VANGUARD` and read by nothing this phase — the field exists
  so genesis can say what it means, exactly as `StationDesc` carried its garrison before the
  protector slice landed.

Every link gets a gate at each end, placed on a ring at `GATE_RING_METRES` from its star, at the
bearing toward the linked system — so a gate reads as the road it is, and the pair's positions
derive from the same layout both binaries hold. Gates are indestructible this phase the way a
Vanguard station is: an immovable hull discards its damage (ADR 0052), and gate ownership,
activation and destruction are all §11's.

`FleetOrderKind::Mine` waits for a design that gives it meaning; `Jump` arrives with one in
slice 2, and the gate table is most of it.

## 6. The jump

### 6.1 The order

`Jump` is a fleet order naming a gate, shaped like `Dock` naming a station: the wire names an
entity, the publisher resolves it, and the gate lives in the simulation (ADR 0014, 0049). The
gates: a named record that is not a live gate row refuses as `NotAGate`; there is no standing
refusal this phase — a gate takes anyone, and inventing half a gate-standings design here would
repeat the mistake the stations design declined (`Stations.md` §15, decision 3). An accepted
order clears the standing order it replaces and sends the fleet to an approach point short of
the gate, through the same move machinery every order uses.

### 6.2 The pass

The jump is **atomic at fleet grain**: the tick in which every live member of the fleet stands
inside the gate's capture range (`GateRangeMetres`, measured to the skins), the whole fleet goes through together. Members
still flying in simply have not arrived yet; the fleet cruises at its slowest member's speed
precisely so that arrival is a fleet-shaped event, and the door keeps that promise. A fleet never
exists in two systems, which is a sentence the fleet row can say and the trickle alternative
(§12, decision 7) could not.

`StepJumps` runs beside the dock pass at the top of the standing-intent slot — it is the second
pass that despawns, and it follows the dock pass's whole idiom: captures gathered during the
walk in array order, applied after it, so the tables the later passes iterate are already
repaired (`Stations.md` §10). A capture carries what the far side needs and nothing else — the
entity, the hull, the faction, the damage — and the apply half despawns each member through
`DespawnShip` with the new cause **`JumpedOut`**, then spawns them through `SpawnShipAs` at the
destination gate: same identities, fresh handles, hull damage carried, formed up clear of the
gate on the exit bearing. The fleet row is rebuilt on the new handles in the same apply, its
order set to `Idle` — the jump order completed — and its threat and alert cleared.

That rebuild is not bookkeeping, and slice 2 found out why: every member's handle dies in the
crossing, so a fleet left holding the dead ones is pruned to nothing by `StepFleets` at the end of
the very tick it arrived — the ships would be there and the fleet would not. **Fleeing through a
gate is escape**, and a leash anchored a galaxy away would never release.

### 6.3 What crosses, and what does not

Identity, hull, faction and damage cross. Routes, patrols, protector duties and mount state do
not — they are intent, they are re-derived, and a fresh spawn's rows are their defined rest
state, which is exactly what the state codec already requires of a reloaded row. A pursuer whose
quarry jumps holds a target that stops resolving and stands down on its own, through machinery
that already exists and already has tests.

### 6.4 The wire

The departure run's cause enum grows `JumpedOut`, so a client draws a wink-out instead of
inferring anything from an absence — the rule the despawn log was built to enforce (ADR 0027,
0040). The arrival is an ordinary enter. The format change bumps the ALPN with the slice that
makes it, as every wire change has.

## 7. One Universe per shard

A shard is **one `Universe` holding all of its systems**, far apart on the one plane. The
alternative — an instance per system — multiplied the publisher, the save, the tick loop and the
replay contract by N, and bought no property the sector math does not already provide (§12,
decision 5).

What makes this cheap is what the tree already built. Dense arrays cost what their *contents*
cost, so an empty system is free by construction rather than by a freezing mechanism nobody has
to design, test or explain. The index hashes buckets and never grids the spread. Interest sets
keep every subscriber local to its own neighborhood. The publisher's table does not care where
its subscribers are. One save file per shard falls out of one universe per shard.

Two honest consequences, named rather than discovered:

- **The replan scope — closed by slice 6.** A route's version was the whole universe's (ADR 0034),
  so architecture changing in any system re-planned every routed ship in the shard: tolerable at one
  system, and the bite that record predicted at fifty. A route now carries the *key* of the island
  that planned it — its lowest occupied path cell, which is world-fixed and survives a repartition —
  and that island's version, so building in one system costs the other fifty-three nothing
  ([ADR 0059](Decisions/0059-a-route-is-scoped-to-the-island-that-planned-it.md)).
- **Cross-shard is the same door, later.** An intra-shard jump and a cross-shard handoff are
  both `DespawnShip(JumpedOut)` + `SpawnShipAs`; the cross-shard case moves the capture over a
  transport first. This design proves the door intra-shard and deliberately stops there — the
  handoff protocol, its acknowledgment and its failure modes are a design of their own (§11),
  and the door's shape is what guarantees that design slots in without reopening this one.

## 8. The save file

The save is the state codec given a file:

- **A header the codec does not own**: a format version byte, the shard id, the galaxy seed, and
  **the length of the state** — then the `WriteUniverseState` bytes as they are. The seed rides in
  the file because the client-visible galaxy derives from it: a binary whose compiled seed has
  moved on still boots the universe the file holds, laid out from the header's seed, and a shard
  and its clients never disagree about where everything is.

  The length was not in this design and slice 5 added it, because "torn" turned out not to be
  checkable without it: `ReadUniverseState` stops at the end of the state and never looks past it,
  so a file with rubbish appended loads and is believed. Eight bytes at the front catch that *and*
  catch a truncation before the body parser allocates anything. The shard is then in the file twice,
  header and state; the reader cross-checks them and refuses a file that disagrees with itself,
  which is what keeps a header readable without the body from becoming a second answer.
- **An atomic write**: written to a sibling temporary and renamed over the last save, so a crash
  mid-write leaves the previous good universe instead of half of a new one.
- **A cadence**: every `saveEveryTicks` — a `Server.cfg` value beside the port, because a save
  cadence is exactly what a deployment may change without a rebuild (ADR 0043) — and once at
  clean shutdown. Always between ticks, never inside one; the codec's contract is a universe at
  rest.
- **A boot that fails closed**: an absent file means there is nothing to run, and the boot stops
  naming the tool that writes one. It meant "first boot, and genesis runs" until slice 5b took
  genesis out of the game (ADR 0058); the fallback went with it, because two things that can author
  a universe is one more than the number that can be right. A file *present but refused* — wrong version, torn, inconsistent — **stops the
  boot naming what refused**, exactly as a boot that cannot open the wire does (ADR 0028). It
  never falls through to genesis: a refused save quietly replaced by a fresh universe is the one
  mistake this file must not make, and "diagnostics, not crashes" (AGENTS.md §5) means the
  message names the byte, not that the universe gets discarded.

  Slice 5 found that this is not expressible with the file reader the tree had: `ReadFile` answers
  "no such file" and "I could not read the file" with the same empty buffer, which is right for a
  texture and catastrophic here — the second would have read as a first boot, and the next periodic
  save would have landed on top of the file nobody could read. `Neuron::FileSys::Exists` exists for
  that one distinction (ADR 0057).

- **A reader that accepts a window of formats**, added by slice 1 of
  [`GameDesignPlan.md`](GameDesignPlan.md) and not in this design as written. The writer writes one
  format; the reader accepts any from `UNIVERSE_STATE_FORMAT_OLDEST` to the current, reading every
  field a later format added behind a gate on the byte the file carries and defaulting it where the
  file predates it, so a file in an older format is a current universe the moment the read returns
  and the next save writes it forward. A fixture per retired format, written by `UniverseGen` at
  the commit before the bump, is what proves the reader against a file rather than against itself;
  a boot that migrated a file keeps the file it read beside the save, under its format's name,
  once; and a format is retired only by a decision record ([ADR 0061](Decisions/0061-the-save-is-migrated-on-read.md)).
  The refusal above is unchanged for a format outside the window.

The shot log stays out of the file, as it already is by design: a reloaded universe with no
tracers pending is the correct picture of one that has just resumed.

## 9. What the client does

The smallest client that makes the galaxy real, and no more:

- **`JUMP` joins the fleet sheet's commands**, arming the next gate tap the way `DOCK` arms a
  station tap. A gate is a record; a tap already knows how to name one.
- **The camera crosses with the fleet.** A fleet that jumps while selected takes the camera to
  its arrival — the fleet-button fly-to that already exists, fired by the jump.
- **Bodies and marks follow the system**, landed in slice 4b. The view places worlds, rocks and
  station marks for the system the camera is in, from the same layout both halves derive; the
  minimap keeps its 4 km half-range and marks the local system's static content. Which system that
  is turned out to belong in `GameLogic` rather than the client — `Game::SystemAt`, the nearest star
  to a point — because a server asking the same question would otherwise need its own copy, and
  because that is the only half of it a suite can reach (`Universe-slice-4b.md` §7). The trigger is
  where the camera **is**, not that a jump happened, so a galaxy map or a reloaded save gets it
  free. The wink-out a `JumpedOut` departure draws landed as a plain removal, stated as the
  placeholder it is.
- **The sky stays one sky**, and slice 4b turned that from an assumption into an argument: a
  crossing moves the camera from one system's gate ring to the other's — 43 km at the guaranteed
  minimum, 117 km on the shipped lattice pitch — and a background that visibly turned over at that
  range would be claiming the galaxy is a few hundred kilometres across. A per-system sky is a later nicety and
  F5's semantics (looks reroll, places never) do not change either way.

The galaxy *map* — the screen where the graph is a picture and a destination is a tap — belongs
on the HUD function rail whose screens are not built yet, and it is deliberately not in slice 4.
A fleet can jump without it; a map screen is UI work that should not gate the mechanism.

## 10. Numbers

The shipped values, and the rule `SystemDesc` already established: **defaults are the shipped
numbers, not placeholders**, so every bound below is provable in the suite against the values the
game actually runs.

| Value | Shipped | Why this number |
|---|---|---|
| Lattice pitch | 16 sectors = 131 072 m | far past every radius that exists; loose change against the wire's ±1 858 ly |
| Cell jitter | 0.20 × pitch per axis | min star separation `(1 - 2√2 · j) · pitch` = 56 926.5 m, by construction |
| Lattice rings | 5 → 91 cells | a first galaxy in the dozens of systems, not hundreds |
| Density | 0.55 | unevenness without emptiness; retunable without moving anyone (§3.1) |
| Planets per system | 2 + `Below(4)` → 2–5 | home keeps its authored 3 |
| Orbit / radius bounds | the shipped `SystemDesc` defaults | unchanged, and the ceiling proof holds per system |
| `GalaxyDesc::gateRingMetres` | 7 000 | outside the widest orbit (6 500); **8 000 breaks the path grid** — see below |
| `GATE_CAPTURE_METRES` | 400, **to the skins** | below: a flat centre-to-centre radius cannot be satisfied at all |
| `GATE_APPROACH_METRES` | 120 clear of the skin | comfortably inside the capture range, so arriving implies crossing |
| `saveEveryTicks` | 18 000 (5 min at 60 Hz) | a `Server.cfg` default, not a constant |
| Galaxy seed | one `constexpr` u64 beside `UNIVERSE_LAYOUT_SEED` | content, like every seed that places things |

**The gate ring is 7 000 m, not the 8 000 this table first specified.** A gate stands further from
its star than any planet, so it decides a system's static span — and at 8 000 m that span is 532
cells against a path-grid ceiling of 512. `PathIslands` declines past its ceiling *quietly*, so the
symptom would have been ships that stop routing, a long way from this number. It now lives in
`GalaxyDesc` rather than in the composition root, so the bound is a test rather than a hope
(`Universe-slice-3.md` §7).

**The gate radius is measured to the hulls' skins, and this table said otherwise until slice 2
built it.** It specified a flat `GATE_RADIUS_METRES` of 120 m, centre to centre. A Structure's centre
sits 251 m from its own skin, so that is a circle *inside the building* — space the blocking pass
exists to keep empty — and a fleet ordered through such a gate flies at it forever. The range is now
derived per pair as `DockApproachRangeMetres` already is, both hulls' radii plus a margin, and it is
wider than the docking one because a jump is atomic where a docking is one ship at a time: the
doorstep has to hold a whole formation at once (`Universe-slice-2.md` §7, ADR 0056).

**The shipped seed's census, measured** (galaxy seed `0x46726F6E74696572`, "Frontier", at the
values above): **54 systems, 68 gate links, 5 chokepoints**, a widest crossing of **8 jumps** from
home, a mean of 4.07, and no system carrying more than 4 gates. Nothing is unreachable, which the
graph guarantees rather than the seed. That is the galaxy the design is arguing about, and it is a
number rather than a hope because slice 1 has landed.

## 11. Deliberately left out, so nobody goes looking

- **The cross-shard handoff protocol** — the door is proven intra-shard; the transport, the
  acknowledgment, and what happens when the far shard is down are a design of their own.
- **Gate ownership, activation, contest and destruction** — the row carries an owner so genesis
  can say what it means; nothing reads it.
- **The galaxy map screen** — the mechanism lands without it (§9).
- **Economy, trade, mining** — the map gives them somewhere to live; they are not here, and
  `Mine` stays `Unsupported`.
- **NPC presence beyond genesis** — Vanguard stations stand in every system; the Vandal base
  stays home's, and hostile spread across the galaxy is content for a hostiles design.
- **Per-system tick rates, threading, second processes** — one universe, one thread, 60 Hz
  (ADR 0045), as before.
- **Wire-delivered layout** — the layout ships in both binaries as it does today; ADR 0037
  already names download as the eventual delivery and why it is not a redesign when it comes.
- **Transit fiction** — no in-warp state, no travel time between systems beyond flying to the
  gate. A jump is a tick.

## 12. Decisions taken by the owner

Put and taken 2026-09-01, in two rounds against the live workbench; each with what lost and why.

1. **Ambition: design the galaxy, land system-first** — over designing only a few systems (fences
   nothing and decides nothing), over polishing one system (management of the universe without a
   universe), and over building the sharded galaxy now (several designs' worth ahead of any
   second process existing).
2. **Travel: jump gates** — over a free warp order (needs an in-transit state and surrenders the
   map's chokepoints), over continuous flight (light-years of empty gameplay and a real i32
   saturation bug to solve), and over no travel yet (a galaxy nobody can cross).
3. **Authorship: one seed plus authored pins** — over pure procedural (the framed opening shot
   dies by die roll), over a fully authored universe file (as big as somebody writes, no bigger),
   and over server-authored download (a delivery question, not a creation one — ADR 0037 already
   holds its place).
4. **Persistence: the save file lands in this design** — over designing it without building it,
   and over deferring it: management of a universe that resets with the process manages nothing.
5. **Runtime shape: one `Universe` per shard** — over an instance per system: N publishers, N
   saves, N replay contracts and an in-process handoff, for no property the sector math and the
   bucketed index do not already provide.
6. **A gate is a Structure plus a side-table row** — over bare layout content (nothing to tap,
   nothing to ever own or contest) and over a new entity kind (every table, the codec and the
   wire learn a second kind — the exact cost ADR 0038 declined for stations).
7. **The jump is atomic at fleet grain** — over the launch-metronome trickle (a fleet straddling
   two systems is a sentence the fleet row cannot say) and over a transit state (the widest
   change of the three, for fiction this game does not need).
8. **The gate graph is the relative neighborhood graph** — over Gabriel (denser, fewer
   chokepoints, a more forgiving and less strategic map), over MST plus extras (every skeleton
   link a chokepoint until a knob covers it, and the knob needs its own argument), and over
   k-nearest (strands pockets; connectivity would need a repair loop).

## 13. Slices

One slice, one branch, one pull request; work orders are cut from here one at a time, when a
slice is actually next. Slices 1, 2 and 6 share the GameLogic layer and are serial among
themselves, as 3 and 5 are in `Outpost`; 4 rides `NeuronClient` + `Outpost`. Slice 5 spread wider
than that on contact — the file's format is `GameLogic`'s, beside the state codec it fronts, and the
atomic write is `NeuronCore`'s, because "replace a file without ever being able to lose both
versions" has nothing game-shaped about it. Slice 4b was cut from
4 on contact rather than planned, and it crossed a layer while being built: written as client work,
landed as `GameLogic`+`Outpost`, because its one piece of real logic belonged in the simulation
library where a suite could reach it (`Universe-slice-4b.md` §7).

| # | Slice | Layer | Size | Depends on | ADR |
|---|---|---|---|---|---|
| 1 | [`LayOutGalaxy`](Universe-slice-1.md): lattice, walk, pins, per-system recipe, gate links | `GameLogic` | M | — | [ADR 0055](Decisions/0055-the-galaxy-is-one-seed-and-its-gates-are-the-relative-neighborhood-graph.md) — **landed** |
| 2 | [Gates and the jump door](Universe-slice-2.md): gate table, `Jump` order, `StepJumps`, `JumpedOut`, codec, ALPN | `GameLogic` | M | 1 | [ADR 0056](Decisions/0056-a-jump-is-a-despawn-and-a-spawn-under-one-identity.md) — **landed** |
| 3 | [Genesis composes the galaxy](Universe-slice-3.md): root lays out, spawns stations and gates, boot log | `Outpost` | S | 1, 2 | — **landed** |
| 4 | [The client crosses](Universe-slice-4.md): `JUMP` on the sheet, gate pick, silent removal, camera follow | `NeuronClient`+`Outpost` | M | 3 | — **landed** |
| 4b | [The scenery follows the camera](Universe-slice-4b.md): `Game::SystemAt`, per-system bodies and marks | `GameLogic`+`Outpost` | S | 4 | — **landed** |
| 5 | [The save file](Universe-slice-5.md): header, atomic write, cadence in `Server.cfg`, restore-or-stop boot | `GameLogic`+`NeuronCore`+`Outpost` | M | 2 (3 in practice) | [ADR 0057](Decisions/0057-the-save-is-a-versioned-file-and-a-refused-one-stops-the-boot.md) — **landed** |
| 5b | [Genesis moves into a tool](Universe-slice-5b.md): `UniverseGen` writes a universe, the game only loads one, and the starting content leaves the client header | `GameLogic`+`Tools/UniverseGen`+`Outpost` | M | 5 | [ADR 0058](Decisions/0058-a-universe-is-authored-by-a-tool-not-by-the-program-that-runs-it.md) — **landed** |
| 6 | [The replan scoped to its island](Universe-slice-6.md): a route carries the key of the island that planned it | `GameLogic` | M | 2 | [ADR 0059](Decisions/0059-a-route-is-scoped-to-the-island-that-planned-it.md) — **landed** |

**Acceptance texture, seeded now for the orders to expand**: slice 1 proved determinism (one
seed, one galaxy, twice), pin stability under reroll, census monotonicity with survivors fixed
(§3.1), connectivity by union-find for every tested seed, and the separation bound as a property —
twelve rows in `GalaxyLayoutTests`, and it also gained the row nothing predicted, that a distance
tie leaves a link alone; slice 2 kept both replay gates green with gates and jumps in the state, and added the
rows: a fleet ordered through a gate arrives whole, once, with its damage and its identities and
without its alert; a gate that leads nowhere strands nobody; and a jumped ship reaches a subscriber
as *jumped* rather than as *destroyed*; slice 3's boot log states the galaxy
(`GALAXY | 54 SYSTEMS | 136 GATES`) and home boots pixel-identical to today; slice 4 owes
screenshots at two window sizes, on both sides of a jump; slice 4b added the four rows that pin
`SystemAt` — every system owns its own star, every planet and gate of a system resolves to it, a tie
keeps the lower index, an empty galaxy answers zero — the second of which is what now bounds
`gateRingMetres` against the lattice pitch from a second direction; slice 5 proved the codec half —
a restored universe replays to byte equality, and a wrong magic, an unknown format, a length that
disagrees with the file, an appended byte and a header that contradicts its own body are each
refused and change nothing — while the disk half (the atomic write, the cadence, the boot that
stops) is compiled by CI and demonstrated by nobody, which `Universe-slice-5.md` §8 states rather
than implies; slice 5b made genesis provable at all — the census, the gate graph's integrity, the
determinism and the round trip through the save file are seven rows in `StartingUniverseTests` where
they used to be a thing you checked by launching the game — and proved the move byte-identical to the
code it replaced over 900 ticks; and slice 6 proves exactly what this line asked it to — a static
spawn in one system re-plans no route in another — measured through `RoutePlanCount` rather than
asserted, with the mutation that restores ADR 0034's behaviour turning that row red.

**The screenshots slices 4 and 4b owed, and the play-quit-relaunch pass slice 5 owed, were waived by
the owner on 2026-09-01** rather than run. Each work order records the waiver where the check stood,
because a check that quietly stops existing looks exactly like one that passed.
