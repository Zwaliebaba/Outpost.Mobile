# Hostiles — an enemy station and its patrol

Nothing below is implemented. Four decisions were put to the owner on 2026-08-29 and taken (§13);
each was the recommended option. §14 lists the slices and what each depends on.

This document proposes the game's first hostile presence: one enemy station, using the `Structure`
hull and its shipped mesh `Outpost/Assets/Meshes/Structure.obj`, with three enemy ships patrolling
a ring around it. Hostiles appear on the overview — the HUD's minimap — as red dots. They do
nothing else yet: no combat, no damage, no reaction to the player. Just a base, and a slow patrol
that says somebody else lives here.

The feature is small on purpose. What it forces into existence is not: the first ship that is not
the player's makes the tree answer **whose is a ship** (allegiance), **who may command it**
(authority), and **where a non-player's intent comes from** (NPC behaviour). All three questions
have MMO-shaped answers and cheaper answers, and the cheaper ones are the kind that work perfectly
in one process and cost a rewrite the day the halves separate. This design exists to take the
MMO-shaped answers now, while they are still one field, one filter and one pass — it is written
against the same target Design/Collision.md 1 is: many players connected in parallel, one
authoritative server.

---

## 1. What is being built

- **Allegiance**: every ship carries a `FactionId`. It is simulation state, it travels in the ship's
  snapshot record, and the simulation refuses move orders whose issuer's faction does not match the
  ship's (§4).
- **A patrol behaviour**: a deterministic pass inside `World::Step` that walks an assigned ship
  around a ring of waypoints centred on an anchor ship, at a cruise speed below the hull's maximum,
  through the same order machinery a player's click uses (§5).
- **Content**: one `Structure` at ~1.2 km north-east of the starting fleet, three `Interceptor`s
  assigned to a 400 m patrol ring around it, all in the hostile faction, spawned by the composition
  root at boot the way the starting fleet already is (§6).
- **Display**: minimap blips coloured by allegiance — own faction green as today, everything else
  `HUD_ALERT_RED` — with a larger blip for structures; the minimap's `CONTACTS` placeholder becomes
  the real count of hostile records in the snapshot; hostiles cannot be selected, hovered or
  ordered (§7).

The Hud already planned for this. `Outpost/Hud.cpp`'s blip loop says *"Every ship in the world is
friendly today; hostiles arrive with combat, and they draw in HUD_ALERT_RED when they do"* — this
design lands the hostiles and the red without the combat, and that comment leaves in the same
commit (AGENTS.md: a change that makes a sentence false changes the sentence).

---

## 2. What the tree already guarantees

Constraints, not preferences; a proposal below that broke one of them would be wrong regardless of
how well it patrols.

| Constraint | Where it comes from |
|---|---|
| `GameLogic` depends on `NeuronCore` and nothing else; the engine never names the game | AGENTS.md §2 |
| No wall clock, no OS entropy, no iteration order that is not dense-array order | AGENTS.md §5 |
| Two identical runs are bit-identical, tick for tick | `GameLogicTests`, the replay gate |
| The client sees the world only through snapshots over a `Transport`; what a record carries is a reviewable list | `GameLogic/WorldSnapshot.h`, ADR 0009 |
| Stored cross-tick references are `ShipHandle`, never `ShipId` | ADR 0005 |
| Presentation state does not live in the simulation; a value that would go over a wire belongs to `World` | AGENTS.md §5 |
| Interest management sends each subscriber only what it can see | Design/Collision.md 1, `InterestSet` |
| Tuning is `constexpr` — contract values in `SimTuning.h`/`HullSpec.h`, look-and-feel in `ViewTuning.h` | AGENTS.md §5 |

---

## 3. What needs no new design

More of this feature already exists than not, which is what makes the slices small:

- **The station is one spawn call.** `HullId::Structure` is a row in `HULL_SPECS` — immovable,
  collidable, 251.77 m bounding radius, authored against the shipped mesh. A ship spawned with it
  enters the static index and the pathfinding obstacle set automatically
  (`World::RebuildStaticIfDirty`), traffic is projected out of it, routes are planned around it,
  and the tunnelling and separation suites already spawn Structures. Nothing about "a station"
  is new to the simulation.
- **The mesh path is the ship mesh path.** `ObjParser` → `MeshLibrary::Load` → `RegisterHullMesh`,
  keyed by `HullId` for the whole table. `Structure.obj` and `Interceptor.obj` ship today beside
  the three hulls the fleet flies; they have never been loaded, and loading them is two more rows
  in the composition root's table. The runtime stays OBJ; ADR 0011's NMO format is the authoring
  pipeline's future and nothing here touches it. The `Structure` mesh carries `thruster`-material
  faces like every hull, so the view will find attach points and give the station idle-intensity
  glows and zero-length trails — running lights, acceptable, checked by slice 3's screenshot.
- **NPC ships are ships.** Snapshots, interest, interpolation, avoidance, separation, formation
  authority, trails and banking all operate on records and hull ids. Not one of those systems
  learns the word "enemy".
- **Visibility is already MMO-shaped.** Hostiles reach the client only inside the interest radius
  (2,000 m today), and the server simulates them whether or not anyone is subscribed — `InterestSet`
  changes what is sent, never what is simulated. Red dots appearing when a base comes into range is
  not a feature this design adds; it is what the existing machinery does the moment hostile records
  exist.

---

## 4. Allegiance — `FactionId`

### 4.1 The field

```cpp
// ShipState.h, beside ShipId and ShipHandle
using FactionId = std::uint8_t;
inline constexpr FactionId FACTION_PLAYER = 0;
inline constexpr FactionId FACTION_HOSTILE = 1;
```

`ShipState` gains `FactionId factionId = FACTION_PLAYER;`. It is simulation state under AGENTS.md
§5's own test — a spectator would need it — and it is *identity*, not relation: the server states
whose the ship is, and each client decides what that means to it (§13, owner decision 2). A
relation ("hostile to you") is a per-viewer fact, and per-viewer facts fork record contents per
subscriber; identity is one byte shared by every subscriber, and the day standings or diplomacy
exist they are a client-side mapping change plus whatever the server then chooses to *write* for a
given viewer. Deception — a spoofed transponder, stealth — remains expressible later exactly
because the field is "what the server says the faction is", not "what the faction is".

`World::SpawnShip` gains the parameter, defaulted:

```cpp
ShipId SpawnShip(const WorldPos& _posWorld, float _headingRad, std::uint32_t _hullId,
                 FactionId _factionId = FACTION_PLAYER);
```

The default is semantically honest, not merely convenient: every existing caller — the starting
fleet, every test — spawns the player's own ships, so the default states what those call sites
already mean (§12).

### 4.2 The wire record

`ShipSnapshot` gains `FactionId factionId`, written as one `U8` after `order` and before `hullId`;
`SHIP_RECORD_BYTES` goes 81 → 82 and `ShipsPerSnapshotFragment` follows it automatically. Both
ends are one binary over the loopback today, so there is no version negotiation to design; the
field-by-field format is what lets the record grow (its own header comment says so).

Two deliberate absences, recorded here because the record's comment calls its contents a
reviewable list:

- **No NPC flag.** A client cannot tell an NPC from a player, which is the MMO-correct amount of
  knowledge — the day real players fly beside NPCs in the same faction, nothing on the wire
  changes.
- **The cruise cap does not travel** (§5.4). It is intent, and it joins `steerTargetPos`,
  `orderFacingRad`, `orderHasFacing` and `avoidHeadingRad` on the list of things deliberately kept
  from clients, in the same comment.

### 4.3 Command authority — the gate lives in the simulation

Today `World::IssueMoveOrder` steers whatever ids it is handed, and
`WorldSimulation::ApplyIncomingOrders` hands it whatever handles arrived on the wire. The moment a
hostile ship exists, that is a client ordering the enemy's fleet around — in one process a
curiosity, over a real wire the first exploit. The gate is one filter, and where it goes is the
decision that matters:

```cpp
float IssueMoveOrder(std::span<const ShipId> _ships, const WorldPos& _point,
                     bool _hasFacing, float _facingRad,
                     FactionId _issuerFaction = FACTION_PLAYER);
```

A ship whose `factionId` differs from `_issuerFaction` is dropped from the order exactly as a
stale handle already is — left out, not an error. The gate sits in `World` rather than in
`WorldSimulation` because the executable's adapter has no test suite and every future host — the
dedicated server, a second adapter, a replay driver — would otherwise have to remember to
duplicate the check. The simulation refusing is a property of the simulation; an adapter refusing
is a convention (§12). `WorldSimulation` passes its subscriber's faction — a member initialised to
`FACTION_PLAYER` with the same "the day a real player connects, this comes from the session"
comment `SubscriberCentre` already carries.

The granularity is faction, and that is knowingly coarse: an MMO gates command per *player*, and
two players in one faction must not steer each other's ships. Per-ship ownership is one more field
and a different comparison in this same function the day there are two subscribers; what cannot be
retrofitted cheaply is the *existence* of the gate, which is why it lands with the first hostile
rather than with the second player (§10).

On the client, the same rule shapes the UI (§7): what you cannot command, you cannot select.

---

## 5. The patrol — NPC intent inside the tick

Owner decision 1 (§13): NPC behaviour lives in `GameLogic`, inside the tick — not in the
executable's server half, and not behind a bot client on the transport. The reasons are ADR 0008's
in a new coat: the wire format lives in `GameLogic` because the day there are two executables it
must not be in one of them, and NPC behaviour is the same shape of thing. In the tick it is
covered by the replay gate, costs no wire hop, and ships with the tree to any future server
binary. A bot client would put fleet AI outside the replay contract and pay latency for the
privilege; an adapter-side driver would be authoritative code in the one layer without a test
suite.

### 5.1 The assignment

Patrol state is a dense array in `World`, parallel to `m_ships` and swap-and-popped with them —
the exact pattern `m_routes` set:

```cpp
struct Patrol
{
  ShipHandle anchor;                     // the station; a handle, so its death ends the patrol (ADR 0005)
  float ringRadiusMetres = 0.0f;
  float cruiseSpeedMetresPerSec = 0.0f;
  std::uint32_t waypointIndex = 0;       // the ring waypoint last issued
  bool active = false;
};
std::vector<Patrol> m_patrols;           // parallel to m_ships
```

It is not on `ShipState`, deliberately: that struct's comment promises "nothing in it a renderer
needs that a snapshot could not carry", and a patrol assignment is exactly the kind of intent the
snapshot exists to withhold. Routes made the same choice for the same reason (§12).

```cpp
// Assigns _ship to walk the ring around _anchorStation. The entry waypoint is the ring point
// nearest the ship's current bearing from the anchor, so assignment never teleports intent.
void AssignPatrol(ShipId _ship, ShipId _anchorStation, float _ringRadiusMetres,
                  float _cruiseSpeedMetresPerSec);
```

Called by the composition root at boot, after the spawns, the way `IssueMoveOrder` is called by
the adapter. `World::IssueMoveOrder` clears the patrol of every ship it orders: an explicit order
outranks a standing behaviour, and the rule costs one line now against a ghost behaviour later.

### 5.2 The ring

`PATROL_RING_WAYPOINTS = 12` waypoints, in `SimTuning.h` because it is in the replay contract — it
changes which points are steered at. Waypoint `i` sits at angle `θ = i · 2π / 12` from the anchor,
at offset `(sin θ, cos θ) · ringRadius` — the same "0 points north, +Z" convention `headingRad`
uses. Increasing `i` walks north → east → south: clockwise on the north-up minimap.

With the content numbers of §6 (ring 400 m around a 251.77 m station), the geometry needs no new
mechanism anywhere:

| Quantity | Value | Against |
|---|---|---|
| Leg length (chord) | 2 · 400 · sin 15° = **207 m** | Interceptor arrival radius 1.5 m |
| Chord's closest approach to centre | 400 · cos 15° = **386 m** | station bounding 251.77 m + clearance margin 8 m + hull 3.5 m = 263 m — legs clear by ~120 m, so `PlanRoute` returns straight legs |
| Avoidance clearance needed | 3.5 + 251.77 + `AVOID_MARGIN_METRES` 8 = **263 m** | 386 m miss distance — the station never even scores as a threat on the ring |
| Ships on the ring | 3, at waypoints 0, 4, 8 | 120° apart at equal speed: they never converge on each other |

### 5.3 The pass

A new first pass in `World::Step`, before `SnapshotPreviousTick` — the position
`ApplyIncomingOrders` already occupies from outside, so an NPC order and a player order entering
the same tick are indistinguishable to every pass below:

```
for each ship with an active patrol, in array order:
  if order != Idle            -> nothing; it is still flying the last leg
  else resolve anchor;  fails -> active = false            (the station died; the ship stands down where it is)
  else waypointIndex = (waypointIndex + 1) % PATROL_RING_WAYPOINTS
       order = Moving, orderHasFacing = false
       orderSpeedCapMetresPerSec = cruiseSpeedMetresPerSec
       PlanRoute(ship, anchorPos + ring point, own bounding radius + PATH_CLEARANCE_MARGIN_METRES)
```

Arrival is the existing machinery's: the ship reaches the waypoint, `SolveOrder` reports
`Idle`, and the next tick's patrol pass issues the next leg. One tick of Idle between legs is
16 ms — invisible, and it keeps the pass free of arrival logic of its own. Everything between
waypoints — steering, avoidance of player ships crossing the ring, separation, replan when shoved
more than `PATH_REPLAN_DEVIATION_METRES` off the leg — is the tested path player ships already
fly.

### 5.4 Cruise speed — "slowly"

The owner's brief says *slowly patrolling*, and today nothing between `SolveOrder` and the hull
table can say slower than the hull's maximum. `ShipState` gains:

```cpp
float orderSpeedCapMetresPerSec = 0.0f;  // 0 = uncapped; set per order, never sent to clients
```

`SolveOrder` clamps its desired speed to the cap when the cap is positive; `AvoidNeighbours` only
ever sheds speed, so the cap holds through avoidance. `IssueMoveOrder` writes 0 for the ships it
orders — player orders stay full speed, and the existing suites stay bit-identical because a zero
cap is arithmetic the current code already performs. The cap is a property of the *order*, not the
hull, which is what lets the same Interceptor cruise at 10 m/s on patrol today and burn at 34 m/s
towards a fight in some later design.

### 5.5 What the pass must not do

No randomness — the ring walk is a pure function of state, so the seeded-PCG32 rule stays a rule
for the future. No reactions: the pass never reads the player's ships, never changes speed or
target because of anything it sees. The first behaviour that *responds* (aggro, pursuit, flight)
is a different design with senses and thresholds; this pass is deliberately a metronome.

---

## 6. The scene — what the composition root spawns

All content numbers live beside `START_SPACING` in `ViewTuning.h`'s *starting scene* block: they
are boot content the composition root reads and passes into the world, the pattern that block
already set. (Slice 3 sharpens that file's header comment, which promises "nothing here feeds back
into a tick" — true of the tuning, already stretched by the spawn spacing, and worth one honest
sentence about boot content rather than a false absolute.)

| Constant | Value | Meaning |
|---|---|---|
| `HOSTILE_BASE_EAST_METRES` | 850.0f | station east of the universe origin |
| `HOSTILE_BASE_NORTH_METRES` | 850.0f | station north of it — 1,202 m out on the diagonal (§13, owner decision 4) |
| `HOSTILE_PATROL_RING_METRES` | 400.0f | ring radius; 148 m clear of the station's skin |
| `HOSTILE_PATROL_CRUISE_MPS` | 10.0f | cruise cap; 29 % of an Interceptor's maximum, a lap in ~4.2 minutes |
| `HOSTILE_PATROL_COUNT` | 3 | ships on the ring |

Boot order in `OutpostApp`:

1. The per-hull mesh table splits from the fleet table. Today `STARTING_HULLS` is one array that
   drives both *which meshes load* and *which ships spawn*; it becomes a mesh table — Bomber,
   Corvette, Frigate, Interceptor, Structure, each loaded and registered via `RegisterHullMesh` —
   and a spawn function per side. A missing mesh stays a logged diagnostic, never a failed boot.
2. `SpawnStartingFleet()` — unchanged three hulls, unchanged spacing, explicit `FACTION_PLAYER`.
3. `SpawnHostileBase()` — the station at `LocalPos(850, 850)`, heading 0, `HullId::Structure`,
   `FACTION_HOSTILE`; three Interceptors at ring waypoints 0, 4 and 8, heading tangent to the ring
   (`θ + π/2`, the direction of travel), each then `AssignPatrol`'d with the ring and cruise
   numbers above.
4. The boot log line counts the *player's* ships. Today it prints `m_world.ShipCount()`, which
   would greet the player with `FLEET ONLINE | 7 SHIPS`, four of them the enemy's.

Two placement checks, so the numbers are argued rather than liked: the farthest patrol point is
1,602 m from the origin — inside the 2,000 m interest radius, so the base is subscribed from the
first update and the overview shows red immediately (the owner chose visible-at-boot over an
interest-management demo, §13); and the pathfinding grid over this architecture spans
`[86, 1614] m` per axis — 48 cells of 32 m against the 512-cell ceiling, so the grid builds with
an order of magnitude to spare.

One server-half correction lands with the content: `WorldSimulation::SubscriberCentre` averages
*every* ship to find where the subscriber is looking, because until now every ship was the
subscriber's. Four hostiles at 1.2 km would drag that centre ~690 m towards the enemy base — the
premise of the comment on that function has expired, and it filters to the subscriber's own
faction in the same slice that makes the premise false.

---

## 7. The overview — red dots, and what a client may not do

The minimap blip loop colours by allegiance instead of by nothing:

| Record | Blip |
|---|---|
| Own faction, selected | `HUD_ACCENT_GREEN`, as today |
| Own faction, unselected | `HUD_ACCENT_GREEN` at 0.7 alpha, as today |
| Any other faction, mobile hull | `HUD_ALERT_RED` — the red dot the brief asks for |
| Any other faction, immovable hull | `HUD_ALERT_RED`, drawn at `HUD_MINIMAP_STRUCTURE_DOT_PX` (8 px against the ships' 4 px) — a base reads bigger than a fighter without pretending to scale |

The comparison needs the viewer's own faction, which is session identity the HUD cannot know:
`Hud::Frame` gains `Game::FactionId ownFaction`, supplied by the composition root beside the other
things "the composition root knows and the HUD does not", and `WorldView` learns the same value
through `SetOwnFaction` for the filters below. Today it is `FACTION_PLAYER` at both sites; the day
a login exists it arrives with the session, and only the root changes.

`CONTACTS` stops being a mock: the composition root counts snapshot records whose faction is not
its own and writes `Frame::contacts` — 4 at boot, the station included, because a contact is a
hostile *record*, not a hostile ship. The value draws `HUD_ALERT_RED` when non-zero, in the header
where it already sits. It counts the subscription, not the map rectangle: the map's 1,400 m
half-range sits inside the 2,000 m interest radius, so every dot on the map is backed by a live
record, while a contact beyond the map edge is counted but clipped — both are the honest reading
of "what my client knows".

What a client may not do, enforced twice on purpose — once in the UI because affordances should
tell the truth, once in the simulation because clients are not trusted (§4.3):

- `PickShip`, hover and box-select skip records of other factions: hostiles cannot be selected, so
  they cannot be ordered, grouped, or double-tap-selected, and no selection ring or hover
  highlight ever appears on one.
- The 3-D scene draws hostiles exactly as it draws friendlies — mesh, `SHIP_COLOUR` mix, trails,
  banking. Silhouette and the overview carry the IFF for now; in-scene hostile treatment (tint,
  target rings, health bars) belongs to combat and is deliberately absent (§11).

Snapshots, interpolation, trails and the explosion design's despawn detection are untouched: a
hostile leaving the interest set is an ordinary leave, indistinguishable from any other by design
(Design/Archive/Collision-slice-6.md 6.2), and today nothing on the client despawns ships anyway.

---

## 8. Determinism

The patrol pass is inside the replay contract, so its order-independence has to be argued, not
assumed:

- It runs before anything moves in the tick, so every read — the ship's own state, the anchor's
  `posWorld` — is end-of-last-tick state, identical whatever the array order. It writes only the
  ship it is visiting. Two ships patrolling the same anchor read the same anchor state by
  construction. (The anchor is immovable today besides, but the argument must not depend on that.)
- `PlanRoute` inside the pass is the same deterministic planner orders already call, and calling
  it from the pass is no different from calling it from `IssueMoveOrder` on the same tick.
- No clock, no entropy, no pointers: waypoints derive from an integer index and `sin`/`cos` under
  `/fp:precise`, which is same-binary-same-answer — the determinism Collision.md 2 requires, and
  the only kind this tree promises.
- The new constants split as the tuning headers demand: `PATROL_RING_WAYPOINTS` is contract and
  lives in `SimTuning.h`; ring radius and cruise speed are *inputs*, recorded the way spawns are,
  passed by the root; dot sizes and content placement are `ViewTuning.h`.
- A world that assigns no patrols ticks bit-identically to today: the pass visits nothing, and a
  zero speed cap clamps nothing. The existing `GameLogicTests` suites passing unchanged is the
  proof, and slice 2 claims it explicitly.

---

## 9. Tests

`GameLogicTests`, in the house style of naming the property:

| Test | Decides |
|---|---|
| `FactionSurvivesTheWire` (SnapshotTests) | a spawned faction id arrives in the decoded record, through `Write` and `WriteInterest` both |
| `AnOrderFromTheWrongFactionSteersNothing` (OrderTests) | `IssueMoveOrder` with a mismatched issuer leaves every ship's order state untouched, and a mixed list steers only the matching ships |
| `TheSamePatrolProducesTheSameRun` (new PatrolTests) | two runs of the §6 scene, compared field-for-field every tick — the replay gate extended over the new pass |
| `APatrolWalksItsRingInOrder` (PatrolTests) | waypoints are visited in index order, clockwise, each within the arrival radius |
| `TheCruiseCapHolds` (PatrolTests) | a patrolling Interceptor's speed never exceeds the cap; the same hull under a player order still reaches its maximum |
| `APatrolNeverEntersItsStation` (PatrolTests) | over laps of the §6 geometry, ship-to-station distance never dips below the two hulls' summed radii |
| `APatrolStandsDownWhenItsAnchorDies` (PatrolTests) | despawn the station: the ship finishes its leg, goes Idle, and stays Idle |
| `ADespawnRepairsThePatrolTable` (PatrolTests) | swap-and-pop moves the last ship's patrol with it; the moved ship keeps its ring |

Slice 3's acceptance is visual where only a screen can decide it (per Design/README.md): the
station and its patrol in the scene and on the minimap — red dots moving, green fleet, `CONTACTS
4` — at two window sizes, plus the existing suites green and a hostile tap/box-select
demonstrably selecting nothing.

---

## 10. The MMO ledger

What this design commits to, kept deliberately in the shape the target needs:

| This feature adds | Where it lands | The day the MMO arrives |
|---|---|---|
| Allegiance | `FactionId` on `ShipState`, in the record | identity stays; relations become a client mapping; per-viewer writes stay possible |
| NPC intent | a pass in `World::Step`, replay-gated | ships with `World` onto the server binary; nothing to port |
| Command authority | a faction filter in `IssueMoveOrder`, issuer supplied by the host | the comparison widens to per-player ownership; the gate itself is already load-bearing |
| Hostile display | client-side mapping of server-stated identity | unchanged; that is how it should work over a real wire |
| Hostile visibility | the interest set, unchanged | unchanged — you already only see what the server sends |
| "Who am I" on the client | one value injected by the composition root | supplied by login/session instead; only the root changes |

And the traps this design is specifically stepping around, named so review can check them: no
client-side spawning or steering of NPCs, no renderer-readable `World` state, no per-viewer record
contents, no NPC flag on the wire, no behaviour keyed off wall time, no trust in a handle a client
sent.

---

## 11. Design choices

The choices made inside the design, as opposed to the ones put to the owner (§13); each with what
it cost.

- **The patrol table parallels the ships, like routes — not fields on `ShipState`.** Keeps
  `ShipState`'s "nothing a snapshot could not carry" promise true and follows the established
  precedent for per-ship server-only state. Costs: one more array despawn must repair, and a test
  that proves it does.
- **Waypoint hops through the existing order machinery — not a continuous "carrot on the ring".**
  A carrot (steering at a point led along the circle every tick) is smoother, but it bypasses
  `PlanRoute`, so a patrol ship shoved off the ring by traffic would steer straight at its carrot
  — potentially through the station — where the order machinery replans around it. The cost is a
  12-sided patrol instead of a circle, gentle at 30° a leg, and reading as a patrol pattern rather
  than a defect.
- **The anchor is a handle, not a position.** A position would keep three ships solemnly orbiting
  the site of a station that no longer exists. Costs one `Resolve` per leg issued.
- **The speed cap is order state, clamped in `SolveOrder`, absent from the wire.** The alternative
  — a per-hull patrol speed — welds "slow" to the hull, and the same hull will want to cruise slow
  and fight fast. Absent from the wire because it is intent, and the snapshot's whole point is
  that intent is withheld.
- **The authority gate is in `World`, not the adapter.** The adapter has no test suite, and every
  future host would have to re-remember the check. Costs a parameter on `IssueMoveOrder` and the
  test that proves the filter.
- **Defaulted parameters (`FACTION_PLAYER`) on `SpawnShip` and `IssueMoveOrder`.** Every existing
  caller means the player's faction, so the default states their meaning rather than papering over
  it; a hard break at ~every spawn in every suite would buy churn, not safety — unlike `WorldPos`'s
  famous brace-break, which caught real semantic change. A caller that means someone else must say
  so, which is the property that matters.
- **A contact is a hostile record, station included** — `CONTACTS 4`, not 3. The counter reads
  "hostile things my client currently knows about", which is the only definition that stays honest
  as content grows.
- **Structures blip at a fixed 8 px, not to scale.** To scale, a 500 m station is 25 px — a
  quarter of the map for one base; iconography beats cartography at 0.05 px per metre.

---

## 12. Deliberately left out

Named so nobody goes looking, and so the next design knows where its edges are:

- **Combat, damage, aggro, pursuit, weapons range** — the patrol is a metronome by the owner's
  brief; the first *reacting* behaviour is a new design with senses and thresholds.
- **Per-viewer IFF, standings, diplomacy, stealth** — identity on the wire is the door to all of
  these (§4.1); none is built.
- **Ownership finer than faction** — arrives with the second subscriber, as a field and a
  comparison inside the §4.3 gate.
- **Spawner and respawn systems, station lifecycle, hostile production** — the base is boot
  content, spawned once, indestructible today.
- **In-scene hostile treatment** — tinting, target brackets, health bars belong to combat.
- **An event-log line for first contact** (and a red log severity) — the log has three severities
  and gains none here; `CONTACTS` carries the information.
- **A second NPC faction, neutrals, faction names in the HUD** — `FactionId` is a `u8` and content
  uses two values; a name table can join `HULL_NAMES` when something displays it.
- **Patrol formations** — three ships share a ring at spaced waypoints; nothing flies in formation.
- **NMO at runtime** — hulls load as OBJ today, station included; ADR 0011's format lands on its
  own schedule.

---

## 13. Decisions taken with the owner

Put to the owner on 2026-08-29 and answered as follows; each was the recommended option.

| Question | Decision | What lost |
|---|---|---|
| Where does NPC patrol behaviour live? | **`GameLogic`, inside the tick** — deterministic, replay-gated, ships with `World` to any server binary (ADR 0008's argument) | an adapter-side driver in the executable — authoritative behaviour in the untested layer, duplicated per host; a bot client over `Transport` — fleet AI outside the replay contract, paying wire latency, a shape no MMO server uses for its own NPCs |
| How does a client learn a ship is hostile? | **`FactionId` in the ship record** — the server states identity, each client maps it to a relation (§4.1) | a per-subscriber friend/foe bit — forks record contents per viewer for no gameplay gain today; inferring from the hull — wrong the day both sides fly the same hull, presentation guessing baked into a protocol |
| Which hulls fly the patrol? | **Three Interceptors** — the unused light hull; the silhouette says "not ours" before the dot does | a mixed wing (2 + 1 Corvette) and all-Corvette — both share a silhouette with the player fleet |
| Where does the station sit? | **~1.2 km north-east** (`LocalPos(850, 850)`) — inside interest and the minimap edge, so the overview shows red from the first frame | 3 km out — the purer interest-management demo, at the price of an empty overview at boot; 600 m — looms in the start camera and crowds the grid the fleet manoeuvres in |

---

## 14. Slices

Three, in dependency order. 1 and 2 are both `GameLogic` and therefore serial (one slice per layer
at a time); 3 is `Outpost` and needs both — its display half needs 1's field on the wire, its
scene needs 2's `AssignPatrol`. Work orders are not yet written; each slice below is one branch,
one pull request, in the shape Design/README.md gives.

| # | Slice | Layer | Depends on | Work order |
|---|---|---|---|---|
| 1 | Allegiance: `FactionId` on ship and record, `SpawnShip` and `IssueMoveOrder` parameters, the authority gate, the subscriber faction in `WorldSimulation`, SnapshotTests + OrderTests | `GameLogic` (+ two lines in `Outpost`) | — | to write |
| 2 | Patrol: `m_patrols` + despawn repair, `AssignPatrol`, the pass in `Step`, `orderSpeedCapMetresPerSec` + the `SolveOrder` clamp, `PATROL_RING_WAYPOINTS`, PatrolTests (new file, both project files) | `GameLogic` | 1 | to write |
| 3 | The scene and the overview: mesh table split, `SpawnHostileBase`, `SubscriberCentre` faction filter, `SetOwnFaction` + selection filters, blip colours + structure dot, real `CONTACTS`, boot-log count, `ViewTuning` content constants, comment and AGENTS.md sentence updates, screenshots at two sizes | `Outpost` | 1, 2 | to write |

Slices 1 and 2 are decided by tests (§9) and by the existing suites staying green — slice 2's
claim that an unassigned world is bit-identical is exactly `GameLogicTests` passing unchanged.
Slice 3 is decided by screenshots and by what it must not change: no `GameLogic` file touched, no
new information reaching the client outside the record.

Decision records due, written with the slice that lands each, in the same commit: slice 1 owes one
for allegiance-as-identity on the wire and one for the authority gate living in the simulation
(both turn down alternatives someone will propose again); slice 2 owes one for NPC behaviour
living in `GameLogic` rather than behind the transport. AGENTS.md's description of what the game
*is* changes in slice 3, when it stops being true that every ship is the player's.
