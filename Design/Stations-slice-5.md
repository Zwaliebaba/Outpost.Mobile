# Work order — Stations slice 5: the Vanguard scene

Implements slice 5 of [`Stations.md`](Stations.md) §16: the starting system laid out, a Vanguard
station at every planet of it, the planets drawn where the layout says, the Vanguard's azure on the
hulls and the overview, the marks on the minimap, and `CONTACTS` counting what the wire says is
hostile rather than what is merely not the player's (design §5.3, §9.3, §9.4).

**Layer:** `Outpost` only.
**Depends on:** slice 1 for `LayOutSystem`; slice 2 for `MakeStation`, `FACTION_VANGUARD`, the
record's flags byte and the header's `hostileMask`.
**Blocks:** slice 6, which gives the stations a tap and the response a key.

---

## 1. Why this is a slice

Everything the simulation half knows about stations is in the tree and covered by tests, and none
of it is on screen: the layout is a function nobody calls, `MakeStation` has no caller in the
running game, and `LiveryOf`'s `_hostileToMe` parameter is fed by the old "not my faction" test with
a comment saying the mask will replace it. This slice is the wiring — the composition root calling
what exists, and the view and the HUD reading what already arrives — and its acceptance is what a
screenshot shows, which is the only way an `Outpost` slice can be judged.

It stops short of the tap. A station that can be seen but not docked at is a coherent state, and a
tap that arrives before the affordance colours are right is not; slice 6 lands both halves of the
interaction together.

---

## 2. Scope

### 2.1 `Outpost/ViewTuning.h` — the starting-system block and the Vanguard's content

In the starting-scene block:

```cpp
inline constexpr std::uint64_t UNIVERSE_LAYOUT_SEED = ...;        // beside BODY_START_SEED
inline constexpr Game::SystemDesc STARTING_SYSTEM = { pinFirstPlanet = true,
                                                      firstPlanetBearingRad = radians(BODY_START_PLANET_BEARING_DEG),
                                                      firstPlanetOrbitMetres = BODY_START_PLANET_DISTANCE_METRES };
```

Every other `SystemDesc` field keeps its default, which is the shipped number by
`UniverseLayout.h`'s own rule: `GameLogicTests` proves the grid-ceiling bound against the defaults,
and a root that overrode the orbit band would be running a system the test never saw.

The Vanguard garrison, as content beside the hostile base's patrol numbers (design §8.2):
`VANGUARD_PROTECTOR_HULL = HullId::Corvette`, `VANGUARD_PROTECTOR_COMPLEMENT = 3`,
`VANGUARD_LAUNCH_EVERY_TICKS = 90`, `VANGUARD_TARGET_CAP = 4`.

In the HUD block: `HUD_VANGUARD_BLUE`, derived from `LIVERY_VANGUARD` exactly as `HUD_ALERT_RED` is
derived from `LIVERY_VANDAL`, with the same sentence about why the overview derives from the
faction's paint and never from a selectable livery. A mark's edge-clamped alpha,
`HUD_MINIMAP_MARK_CLAMPED_ALPHA`, and the mark's size, `HUD_MINIMAP_MARK_PX`.

### 2.2 `Outpost/OutpostApp.h` / `.cpp` — the boot

- `Init` calls `Game::LayOutSystem(UNIVERSE_LAYOUT_SEED, Game::WorldPos{}, STARTING_SYSTEM)` once,
  before the fleet is spawned, and keeps the result in `m_layout`. The star anchor is the universe
  origin (design §5.3); nothing draws a star.
- `SpawnVanguardStations()`: one `Structure` at each `PlanetSite::posWorld`, heading 0, in
  `FACTION_VANGUARD`, then `MakeStation` with the Vanguard's content. Called after
  `SpawnStartingFleet` and before `SpawnHostileBase`, so the player's ships keep the ids they have.
- `SpawnHostileBase` registers the base through `MakeStation` with `ownerFaction = FACTION_VANDAL`
  and `protectorComplement = 0` (design §6.1, §15 decision 4). Nothing else about it changes.
- `SpawnStartingBodies(_seed)` places one world per `PlanetSite`: at the site's bearing and orbit
  from the origin, at the site's `radiusMetres`, from the site's `bodySeed`, at
  `-BODY_START_PLANET_DEPTH_METRES` — the framing device stays. The six asteroids are drawn from
  `_seed` exactly as before. `Planet1.dds` dresses all three worlds (design §5.3, §14).
- **F5 reseeds looks only.** `ReseedBodies` still offsets `BODY_START_SEED` and `SKY_SEED` by the
  press count; the layout is read from `m_layout` and never re-rolled, so the sites — and the
  stations on them — hold still under a debug key.
- `FACTION_NAMES` beside `HULL_NAMES`, reading `PLAYER`, `VANDAL`, `VANGUARD`, with a
  `static_assert` against `FACTION_LIMIT` that the table covers the factions that exist. Handed to the
  HUD in `Hud::Frame` beside `hullNames`; slice 6's refusal line is the first thing that prints one.
- `CONTACTS` is counted by mask: a record whose faction `m_view.IsHostileToMe` — not one that is
  merely not the player's (design §9.4).
- A `STATIONS ONLINE | %u` line after `FLEET ONLINE`, counting `m_world.StationCount()`, so the
  boot says the grid spawned what the layout described. The Vandal base is a station row too, so the
  number is four; the line says how many rows there are, not how many are the Vanguard's.
- The view is handed the mark list at boot: one `WorldView::StationMark` per planet site, in
  `FACTION_VANGUARD`, the way it is handed body placements.

### 2.3 `Outpost/WorldView.h` / `.cpp` — the mask reaches the paint

- `LiveryOf`'s `_hostileToMe` is fed from `m_receiver.IsHostileToMe(factionId)`. The comment that
  said this would happen goes with it.
- `IsHostileToMe(FactionId)` is exposed for the HUD and the root, so there is one reading of the
  mask in the executable and it is the receiver's.
- `StationMark { WorldPos posWorld; FactionId faction; }`, `AddStationMark`, `StationMarks()`.
  Presentation state that happens to be static content: nothing on the wire carries a mark.

### 2.4 `Outpost/Hud.h` / `.cpp` — the overview

- A blip is a structure dot by `SHIP_FLAG_STATION` in the record's flags, not by
  `HullSpecOf(...).immovable`: the wire now states it, and the client inferring it from the hull
  table is the thing design §6.2 exists to stop. The comment beside the loop that says the hull
  table is how a station is told from a ship changes with it.
- A blip's colour is the §9.3 overview column: own — green as today; hostile by the mask —
  `HUD_ALERT_RED`; `FACTION_VANGUARD` with the bit clear — `HUD_VANGUARD_BLUE`; anything else —
  red, for `LiveryOf`'s reason. One function, `OverviewColourOf`, used by the dots and the marks.
- **Marks are a second draw path** beside the dot loop, not a parameter on it
  (`Stations-slice-plan.md` §2.5): for every `StationMark`, a hollow diamond of
  `HUD_MINIMAP_MARK_PX` in the mark's overview colour at the station's map position. A mark past the
  map's edge is **clamped** to the edge, inset by its own half-size, at
  `HUD_MINIMAP_MARK_CLAMPED_ALPHA` — direction honest, distance saturated — where a dot is clipped.
  Marks draw before dots, so a live record at the same spot draws its filled dot over the hollow
  mark.

### 2.5 `AGENTS.md` — the what-is-here sentences

The paragraph that describes the scene gains the Vanguard: three worlds rather than one, each with
a Vanguard station the player can see on the minimap as a mark and reach as an azure structure, the
`CONTACTS` definition, and the sentence that the stations cannot be docked at yet.

### 2.6 What this slice deliberately does **not** do

- No `GameLogic` file is touched. That is the first thing to check in review.
- No `PickStation`, no tap, no dock order, no refusal line, no F6 — slice 6.
- No new information reaches the client outside the record, the header byte and the layout call.
- The Vandal base gets no mark: marks are the layout's (design §9.3), and the base is 1.2 km out
  and a live record from the first update, so it has never needed one.

---

## 3. What to build on

- **`Game::LayOutSystem`** (slice 1) — the sites. `SystemDesc`'s defaults are the shipped numbers.
- **`World::MakeStation` / `StationDesc`** (slice 2) — the row. `SpawnHostileBase` shows the
  spawn-then-register shape on a Structure.
- **`SnapshotReceiver::IsHostileToMe`** (slice 2) — the one reading of the mask.
- **`SHIP_FLAG_STATION`** (slice 2) — what a station record says about itself.
- **`WorldView::LiveryOf`** — the §9.3 table, already written with the Vanguard row.
- **`SpawnStartingBodies`'s `place` lambda** — how a world is generated, uploaded and handed to the
  view; it grows a seed argument and loses its own planet draw.
- **`Hud::DrawMinimap`'s dot loop** and `toMapX`/`toMapY` — the projection the marks share.
- **`HUD_ALERT_RED`'s derivation** — the pattern `HUD_VANGUARD_BLUE` copies.

---

## 4. Acceptance

**Screenshots**, at two window sizes, in the pull request:

- the opening scene, with the pinned world where it was, a Vanguard mark on the minimap at its
  bearing — clamped to the edge, since 3 500 m is past the map's 1 400 m half-range — and the
  `STATIONS ONLINE | 4` line in the log beside `FLEET ONLINE | 3 SHIPS`;
- the fleet arrived at a Vanguard station: the azure structure in the scene, its filled blue dot
  on the minimap over the mark, `CONTACTS` unchanged by it.

**What the HUD must still say:** `CONTACTS 4` at boot — the Vandal four by the mask, the same
number under an honest definition (design §9.4). A reviewer checking the HUD is unchanged is
checking the right thing for the wrong reason, and the pull request says so.

**The tree**

- All four suites green and untouched.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; `git diff --stat` names no file under `GameLogic/`.
- `Design/Stations.md` §16 marks slice 5 landed and this file moves to `Design/Archive/` in the
  merge commit.

**No decision record is due.**

---

## 5. Assumptions the implementer may make

- **One picture for three worlds.** `Planet1.dds` dresses every planet; more maps are content
  for a later slice (design §14).
- **The asteroid field changes under this slice.** The planet's radius and seed no longer come from
  the scene generator, so the six rocks draw different numbers from `BODY_START_SEED` than they did.
  The scene is still one seed and still reproduces; it is a different scene from the last
  screenshot, and no design pinned the old one.
- **Stations beyond the interest radius are marks and nothing else** until a ship gets near
  enough to subscribe them — which is what "static so can be marked" bought.
- **`FACTION_NAMES` is plumbed and not yet printed.** It reaches `Hud::Frame` so that slice 6's
  refusal line has it to read; nothing in this slice displays a faction's name.
- **A station's heading is 0.** A Structure is symmetric enough that nothing reads it; the day a
  station's facing matters, the bearing from the star is on the site.
