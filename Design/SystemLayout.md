# A system is a sun, its sites, and the warp between them

Status: design; slice 0 landed 2026-09-06 (ADR 0072). Written 2026-09-06 against `8118956`.

**Supersedes, in part:** [`Design/Archive/Universe.md`](Archive/Universe.md) §11's "transit fiction" and
§12's decisions 2 and 7, which turned down a warp order and an in-transit state on 2026-09-01. That
design was right for the galaxy it built, where a system was 13 km across and everything in it was a
sub-light flight away. The owner has now decided a system is tens of kilometres across, which is a
decision those two rejections did not have in front of them. The archived sections stay as they were
and the record that lands with slice 2 names them, per `Design/README.md`. What is *not* superseded is
the part of decision 2 that matters: gates stay the only door between systems, so the map's
chokepoints are surrendered to nothing.

## 1. Problem

The layout puts a system's planets at 2 500 to 6 500 m from its star and its gates at 7 000 m, so the
whole of a system fits in one 16 km path island — which is what the path grid's ceiling demanded
and what `EverySystemFitsItsOwnPathIsland` proves. Until ADR 0068 that was invisible: a camera 900 m
out framed 745 m of the plane, and 3.5 km read as far. A camera that frames a sector puts the entire
system in one shot, three worlds a few hundred pixels apart, and it reads as a cluster.

The owner's ask is a system that reads as a solar system — a sun, planets spread around it, room for
a mining belt and a hostile pocket — and a way to cross it that is not thirty minutes at 24 m/s.
Three decisions were put and taken on 2026-09-06 (§12), and this design is what they imply.

The fault is not the distance. A 13 km system and a 130 km one are the same game if crossing either
costs forty seconds and both hold five places worth going to. The fault is that the tree has two of
the three movement tiers such a system needs and not the middle one.

| Tier | Mechanism | Scale | State |
|---|---|---|---|
| tactical | sub-light, formation, guns, collision | the interest circle, up to 4 096 m | shipped |
| in-system | — | site to site, tens of km | **this design** |
| interstellar | gate: despawn, respawn under one identity (ADR 0056), chained by Voyage (ADR 0069) | 57 km between stars today | shipped |

## 2. What this design builds on

- **`SystemLayout` and its sites.** `PlanetSite`, `VanguardStationSite` and `GateSite` are already
  positions derived from one seed in one fixed order, static content both halves read (ADR 0037).
  The design generalises the noun; it does not replace the recipe.
- **The jump.** `StepJumps` takes a whole fleet out of space at one gate and puts it back at another
  under the same identities, carrying hull, faction, damage and owner in a `Jumper` and re-deriving
  everything else (`Universe.md` §6.3). The fleet row is the one thing that survives it (ADR 0069).
  **A warp is this with a delay proportional to distance and no gate at either end.** Nothing in the
  crossing has to be invented; what is new is the wait.
- **The defense.** A fleet row already holds `threat`, `threatAnchorPos` and `alertTicks`. The
  *alert* is the tackle rule below, whole: `alertTicks` is set by a landed hit and by nothing else,
  lapses ten seconds after the last one, and is touched by nothing a client sends. The threat is not
  the primitive, and slice 0 found out why on contact: `IssueFleetOrder` clears it on every order,
  so a rule that read it would be lifted by re-issuing the order (ADR 0072). The alert is the
  server's own memory of being shot, stated off shots it observed (ADR 0041, 0052). No new state.
- **The status block.** `FleetStatus` carries a position for all five slots on every update whether
  or not a member is in the interest circle, and reserved bits 3 to 7 of `flags`, laid once so a
  later bit rides in without an ALPN bump.
- **The despawn log.** `DespawnCause` already tells a client *why* a record left so it draws a
  wink-out rather than an explosion (ADR 0027, 0040); `JumpedOut` is the precedent for `WarpedOut`.
- **The hostile base.** `SpawnHostileBase` puts a station and a three-ship patrol at a hand-written
  offset. That is a PVE site with its position spelled by a constant; the design gives the position
  to the layout and keeps the spawn.
- **ADR 0071.** A client may look only where it has presence, and the galaxy map's tap already obeys
  it. A system map inherits the rule for free: sites are static content and draw for everyone; what
  is *at* a site draws only for a client with a fleet on that grid.

## 3. Sites

A system is a star and a list of sites. Every site is a position, a kind and a seed, drawn from the
system's one generator in one fixed order — the invariant `LayOutPlanets` keeps and every later draw
inherits: a kind appended to the order is covered by appending its draws after the ones that exist,
never by inserting.

| Kind | Where it comes from | Simulation knows | Client draws |
|---|---|---|---|
| Star | the lattice: one, at `starPos` | its heat radius, if slice 4 lands it | the sun (§8) |
| Planet | the orbit ring recipe, as today | nothing (ADR 0016) | a world, as today |
| Station | one per system, off the first planet, as today | a `Structure` row (ADR 0038) | the hull, and a mark |
| Gate | one per link, on the bearing to the neighbour, as today | a `Structure` row plus a gate row | the Stargate, and a mark |
| Belt | 1–3 per system, between the orbits | a resource record, `Mining.md`'s (ADR 0016 reopened by its clause) | rocks, as today's asteroids |
| Anomaly | 0–2 per system, off the ecliptic | a hostile station and its patrol, spawned by genesis | the hulls, marked hostile |

The `Star` and the `Planet` are presentation until something says otherwise. The `Belt` is where
`Mining.md` puts its records and this design says only *where*; the `Anomaly` is `SpawnHostileBase`
generalised from one authored offset to one per site, with home's base staying authored through the
pin exactly as home's first planet does. Stations and gates keep deriving from planets and links,
because ADR 0055's rule — the client's mark and the server's row from one function — is what keeps
the picture and the row agreeing, and there is no reason to give it up for a bigger system.

A site has no motion. Planets **do not orbit**, and the reason is not taste: every other thing in a
system hangs off a planet's position — the station is `Lerp(star, planet, 0.45)`, the islands are
built over the architecture, the layout is a pure function of the seed that both binaries evaluate
without being told. A layout that is also a function of the tick makes a save's timestamp decide
where a station stands and a belt drift away from the fleet parked at it. Spin about the body's own
axis stays, because it costs nothing and moves nothing (§12, decision 1).

## 4. Scale

The numbers, argued as `Universe.md` §10 argued its own; shipped numbers in the left column, this
design's in the right.

| Value | Today | This design | Why |
|---|---|---|---|
| lattice pitch | 16 sectors, 131 072 m | **32 sectors, 262 144 m** | min star separation `(1 − 2√2·0.20)·pitch` becomes 113 853 m, against an 80 km system diameter: 33 km of nothing between two neighbours' gate rings at the worst jitter |
| cell jitter, rings, density | 0.20, 5, 0.55 | unchanged | the lattice is scaled, not redrawn: the jitter is `cellJitter · pitch` and a cell is `cell · pitch`, so every star moves by exactly 2× — exact in floating point — and the neighbourhood rule compares distances only, so **the shipped seed lays out the same 54 systems and the same 68 links bit for bit, at twice the distance** |
| innermost / outermost orbit | 2 500 / 6 500 m | **4 000 / 30 000 m** | wider than the widest interest circle at the inside, so two planets are never one grid; the outside leaves 10 km to the gate ring |
| planet radius band | 400–1 200 m | unchanged | a world 30 km out is past the far plane and is not drawn, which is right: on a grid you see that grid's world |
| home's pinned first planet | −23°, 3 500 m | unchanged | the pin overrides the band as it does today; the station stays at 1 575 m, the Vandal base at 1 202 m, and the opening scene is the opening scene |
| gate ring | 7 000 m | **40 000 m** | past the outermost orbit; the path-grid ceiling no longer bounds it (§6) |
| belts | — | 1–3, at 6–36 km, extent 800–1 500 m | between the orbits, on a jittered slot bearing like a planet's, so a belt never sits in a planet's lap |
| anomalies | — | 0–2, at 10–36 km | as belts, from the draws after them |
| `WARP_ENTRY_METRES` | — | 6 000 | past the widest interest circle (4 096), so a warp is always to another grid and never a way to cross the one you are on |
| align, by hull | — | 2–10 s | a column: Interceptor 2, Bomber and Corvette 3, Frigate 4, Miner and Hauler 6, Battleship 8, Carrier 10 |
| warp speed, by hull | — | 1 000–3 000 m/s | a column: Interceptor 3 000, Bomber and Corvette 2 400, Frigate 2 000, Miner and Hauler 1 500, Battleship 1 200, Carrier 1 000 |

Crossing times fall out: star to gate for a Frigate is 4 s of align and 20 s of warp; the same for a
Carrier is 10 and 40; a belt at 18 km for a Miner is 6 and 12. A fleet warps at its slowest member's
speed, as it cruises at its slowest member's, and arrives together, which is what a fleet is.

The sector index is `int64` and the wire's is `i32`; a five-ring galaxy at 32 sectors of pitch spans
±160 sectors, loose change against ±1 858 light-years (ADR 0046). What the scale *does* cost is the
save: the lattice pitch is part of what a save's galaxy seed means, and it is not in the header.
`Universe::ConfigureGalaxy` re-lays the galaxy from the seed for the voyage pass, so a format-10 file
re-laid at the new pitch would plan voyages toward gates that stand somewhere else. The change is a
format bump: a file at 10 or below is laid out with the parameters those formats implied, on read,
as ADR 0061 already does for every field a format added. `UniverseGen` writes the new file; the
three fixtures under `Tests/GameLogicTests/Assets/` stay exactly as they are, because they are what
proves the old formats still read.

## 5. The warp

Not a new order. A warp is **how a standing order crosses a distance past `WARP_ENTRY_METRES`**, and
the player never chooses it: a Move to a belt warps there, a Voyage warps to each door, a Dock warps
to the station's grid and then docks. One verb, as ADR 0049 wanted, and the galaxy map's tap and the
minimap's tap work unchanged. An Attack on a target off the fleet's grid is refused as out of range
rather than warped, for ADR 0071's reason: a client cannot see, and so cannot tap, a record it has no
presence beside.

The row carries a transit, and the transit has three states.

**Grounded.** Ordinary. Each tick the fleet pass asks whether the standing order's point — the Move's
point, the gate approach, the dock approach — is farther than `WARP_ENTRY_METRES` from the first live
member, and whether every member is in space and **the fleet is not engaged**. If so, it aligns.

**Aligning.** Members turn onto the bearing and hold formation at sub-light for the slowest member's
align time, counted on the row. This is the interruptible part and the whole reason it exists: a
threat raised during the align — `RecordHostileAct`, the same call that lights the alert today —
drops the fleet back to grounded, still flying sub-light at its order. A fleet under fire crawls at 28 m/s
toward a destination 30 km away, which is the sentence the owner asked the game to be able to say.

**In warp.** On the last align tick the members despawn together with `DespawnCause::WarpedOut`,
the row takes `transitTicksLeft = ceil(distance / slowestWarpSpeed · TICK_HZ)`, the arrival point
and the heading. Nothing can touch it: no record, no mount, no collision. On the last tick they
respawn at the arrival on `StepJumps`'s own terms — `JUMP_ARRIVAL_STANDOFF_METRES` short of the
point along the heading, `JUMP_ARRIVAL_SPACING_METRES` across — carrying exactly what a `Jumper`
carries (`Universe.md` §6.3), and the standing order simply continues from there: a Move arrives
sub-light inside its grid, a Voyage approaches its door, a Dock docks.

Two things follow from the row being the only thing in transit:

- **The status block's position is the transit's interpolation**, a pure function of the tick, so
  the system map animates the crossing and the minimap digit points where the fleet is going.
- **The interest centre must not follow it.** A server derives a session's centre from its fleets
  (`ShardSimulation::Step`); a centre that swept along a warp line would enter every record near the
  line and hand the player a free scan of the system's interior. A fleet in warp contributes nothing
  to the centroid, and the client's camera — gated to presence by ADR 0071 — sees what it saw at
  departure until the arrival enters as an ordinary enter. In warp you see nothing, which is what
  warp is.

**Tackle.** The alert is the gate on the align — nothing else. Any landed hit within the last ten
seconds is a tackle, and a hit that keeps landing keeps it. That is a soft rule and it is deliberate:
it needs no device, no new field and no new message, and a `Scrambler` device that renews the alert
without landing a hit is one row of the device table on the day the device design wants it. The
alternative — warp interruptible only by a named module — is EVE's, and it is the harder rule to
arrive at first, because until the module exists every fight ends when the loser says so.

**Gates take the same rule, and it landed first** (§12, decision 4; ADR 0072; slice 0). Until it,
`StepJumps` crossed a fleet under fire and cleared its threat on the far side, so a gate was a free
disengage — a camp could shoot at a fleet on the doorstep and watch it leave. With the rule on both, a
camp holds what it catches, and there is one sentence about leaving a fight rather than two. What it
costs is stated plainly: a fleet that reaches a camped gate under fire stays at the gate under fire,
and the way out is to outlast the alert, kill what is shooting, or be the hull that dies last.

## 6. Paths, interest, and what does not change

`PathIslands` partitions architecture into islands by surface gap, so two sites 20 km apart are two
islands by construction and nothing about the router changes. What changes is a *proof*:
`EverySystemFitsItsOwnPathIsland` asserts the whole system is one island and is exactly the claim
this design retires. Its replacement asserts what is still true — a **site's** architecture and its
margin fit one grid — which is the bound that actually protects a Carrier from a distant outpost
(`RegionalPathfinding.md` 3.3). `Universe.md` §3.4's "islands stay per-system" becomes "islands
stay per-site", a stronger statement, and it holds for the same reason at the new spacing.

The interest circle is unchanged and it is now the definition of a grid: the tactical view is what
is inside it, and a system is a set of grids a warp connects. The camera, the minimap and every
picker are unchanged: a warp destination is off the minimap's reach by construction, and that is
correct, because the minimap is the grid and the system map is the system.

The plane is a pillar (`GameDesignPlan.md` §2) and warp stays on it. There is no third axis and no
transit that leaves the sector arithmetic: a fleet in warp has a position on the plane every tick.

## 7. What the client shows

- **`WARPING`** on the fleet bar, from a status flag in reserved bit 3 (`FLEET_FLAG_WARPING`),
  outranking the kind the way `LAUNCHING` does, with the seconds left from the interpolation. The
  align shows as the kind it is under, since a fleet aligning to a Move is moving.
- **A wink-out and a wink-in** on `WarpedOut`, on the jump's terms, and never an explosion.
- **The system map**: the galaxy screen's second level, reached by tapping the system the camera is
  in. Sites as marks from static content, the player's fleets as their digits from the status block,
  the transit as a line. Tap a site with a fleet held and it is a Move there, which warps. A site
  shows nothing that is at it unless one of the player's fleets stands on that grid, which is ADR
  0071 applied without a new rule.

## 8. The sun

A `Star` site, drawn as a body: an emissive sphere at the star, larger than any world, as far below
the plane as the worlds are and further, sampled from one authored map like the planets are. It is
presentation, as every body is (ADR 0016).

It is also the reason the bodies can be lit properly. `LIGHT_DIR_X/Y/Z` is one constant for every
body in every system, and a sun in the middle says where the light comes from: the direction from
the body to the star, per body, one more field in the per-body constants `BodyOverlayPS` already
reads as `lightDirAmbient`. A world at 30 km lit from a fixed direction is a picture; lit from its
own sun it is a place.

A heat radius is the one thing a star could mean to the simulation — hulls inside it lose hull
points per tick, one number against the one damage number that exists (ADR 0052) — and it is
optional in slice 4 rather than required: it makes the star a place to hide and a place to be
cornered, which is a mechanic, and mechanics are what the belt and the anomaly are for.

## 9. Mining's shape, seen from here

`Mining.md` owes the record, the tool and the transfer (`GameDesignReview.md` E3, E5). What it gets
from this design is its loop's clock. A Miner warps to a belt in 6 s of align and 12 s of warp,
mines, warps to the station in the same, docks, and pours out again; a Hauler carries more and warps
the same. The transit is a minute a cycle and it is the pacing knob the economy wants: **warp speed
per hull is where Miner and Hauler balance lives**, and it is a column rather than a constant so
that a retune is one number in the table every other number is in.

## 10. Numbers the code names

| Name | Where | Value |
|---|---|---|
| `GalaxyDesc::latticePitchMetres` | `GalaxyLayout.h` | 262 144 |
| `GalaxyDesc::gateRingMetres` | `GalaxyLayout.h` | 40 000 |
| `SystemDesc::minOrbitMetres`, `maxOrbitMetres` | `UniverseLayout.h` | 4 000, 30 000 |
| `GalaxyDesc::minBeltCount`, `maxBeltCount`, belt band | `GalaxyLayout.h` | 1, 3, 6 000–36 000 |
| `GalaxyDesc::minAnomalyCount`, `maxAnomalyCount`, band | `GalaxyLayout.h` | 0, 2, 10 000–36 000 |
| `WARP_ENTRY_METRES` | `SimTuning.h` | 6 000 |
| `HullSpec::alignSec`, `HullSpec::warpSpeedMetresPerSec` | `HullSpec.h` | the table in §4 |
| `FLEET_FLAG_WARPING` | `UniverseSnapshot.h` | `0x08` |
| `DespawnCause::WarpedOut` | `Universe.h` | appended |
| `UNIVERSE_STATE_FORMAT` | `UniverseSnapshot.h` | 11: the transit on the row, and the lattice pitch implied by every format below it |
| `STAR_HEAT_RADIUS_METRES`, `STAR_HEAT_PER_TICK` | `SimTuning.h` | slice 4's, if taken |

Defaults are the shipped numbers, not placeholders, so every bound above is provable in the suite
against the values the game runs — `SystemDesc`'s rule, unchanged.

## 11. Deliberately left out, so nobody goes looking

- **Warp to a fleet member, a bookmark or a bare point off-map.** A Move names a point and the maps
  name sites; anything else is a UI the maps do not have yet.
- **Warp bubbles, interdiction, a scrambler device.** Tackle is the engaged bit. A device that sets
  it without a hit is `Devices.md`'s.
- **Aggression timers, session changes.** EVE's machinery for the same problem; the engaged bit is
  the whole of it here.
- **Moving bodies.** §3 and decision 1.
- **A star that is a body the simulation collides with.** ADR 0016 holds; heat is the one thing
  a star may do, and it is optional.
- **Belt contents, the rock record, mining itself.** `Mining.md`.
- **Named systems, named sites.** Still indices.
- **Cross-shard warp.** A warp is inside one system and a system is inside one shard by
  construction (ADR 0063); nothing here crosses a boundary.

## 12. Decisions taken by the owner

Put and taken 2026-09-06, against the sector-wide zoom's first screenshot.

1. **Planets are static** — over orbiting (a layout that is a function of the tick, and every site
   that hangs off a planet drifting with it) and over orbiting on a period too long to notice
   (which is static with a bug budget).
2. **The lattice pitch is raised** — over shrinking the system to fit the 57 km separation (a
   system that cannot be tens of kilometres across) and over lowering the jitter (a more regular
   galaxy for a number that was never the constraint).
3. **Warp is interruptible from day one** — over free warp with tackle added later (every fight
   ends when the loser says so until then, which is the opposite of the camp ADR 0071 protected).
4. **One rule for both: a gate does not cross a fleet whose alert is up** — taken 2026-09-06 and
   landed the same day as slice 0 (ADR 0072) — over EVE's split, where a scram stops warp and a gate
   is still a door. The cost is stated in §5. On contact the primitive moved from the threat to the
   alert, for the reason §2 gives.

## 13. Slices

| # | Slice | Layer | Size | Depends on | ADR |
|---|---|---|---|---|---|
| 0 | The gate half of the rule: `StepJumps` holds a fleet whose alert is up, `JumpTests` rewritten around it — **landed 2026-09-06** | `GameLogic` | S | — | [ADR 0072](Decisions/0072-a-gate-refuses-a-fleet-whose-alert-is-up.md) |
| 1 | The scale and the sites: `GalaxyDesc` and `SystemDesc` at §4's numbers, `SystemLayout::sites` with belts and anomalies drawn after the planets, the per-site island proof in place of the per-system one, format 11 carrying the pitch a lower format implies, `UniverseGen` re-run, `Universe.md` §3.4 and §10 named as superseded | `GameLogic` + `Tools` | L | — | yes: the lattice pitch is part of what a seed means |
| 2 | The warp on the fleet row: the three transit states, `WarpedOut`, the align and warp columns on `HullSpec`, the tackle on the alert (slice 0's primitive), `TryCentreOfOwnedFleets` excluding a fleet in transit, `FLEET_FLAG_WARPING`, `VoyageTests` extended and a `WarpTests` beside them | `GameLogic` | L | 0, 1 | yes: supersedes `Universe.md` §12 decision 2 in part |
| 3 | The client: `WARPING` on the bar, the wink-out on `WarpedOut`, the transit line on the minimap, the system map as the galaxy screen's second level, a site tap as a Move | `Outpost` | M | 2 | — |
| 4 | The sun: the `Star` body, per-body light direction from the star, the heat radius if taken | `NeuronClient` + `Outpost` (+ `GameLogic` for heat) | M | 1 | if heat lands: a star is the first thing the plane does to a hull |
| 5 | Anomalies: `SpawnHostileBase` generalised to one per anomaly site, home's kept through the pin | `GameLogic` | S | 1 | — |

Slice 1 alone is a bigger, emptier system nobody can cross except by flying, which is deliberate:
it is the smallest thing that proves the scale and the sites, and slice 2 is what makes it a game.
Slices 3, 4 and 5 are independent of each other. `Mining.md` depends on slice 1 and on nothing else
here — a belt is a position before it is a record — and `GameDesignPlan.md`'s slice 6 says so in the
same commit as this file.
