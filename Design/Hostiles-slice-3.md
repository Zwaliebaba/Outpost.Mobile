# Work order — Hostiles slice 3: the scene and the overview

Implements slice 3 of [`Hostiles.md`](Hostiles.md) §14: the station and its three patrolling
Interceptors spawned at boot, red blips and a real `CONTACTS` count on the minimap, hostiles that
cannot be selected, and an explosion that fires only for a ship the server said was destroyed
(design §6, §7, §4.4).

**Layer:** `Outpost` only. Plus the sentences in `AGENTS.md` and `Design/` this makes false.
**Depends on:** slice 1 (faction on the wire, `Destroyed()`) and slice 2 (`AssignPatrol`,
`PatrolRingPoint`).
**Blocks:** nothing; it closes the design.

---

## 1. Why this is a slice

Everything before this was invisible on purpose. This slice is the one the owner's brief
describes — a base, a slow patrol, red dots — and it is decided by screenshots and by what it must
not touch: no `GameLogic` file, no new information reaching the client outside the record. If the
overview needs something the record does not carry, that is a slice 1 change, not a shortcut here.

---

## 2. Scope

### 2.1 `Outpost/ViewTuning.h` — content constants

In the *starting scene* block beside `START_SPACING`:

| Constant | Value |
|---|---|
| `HOSTILE_BASE_EAST_METRES` | `850.0f` |
| `HOSTILE_BASE_NORTH_METRES` | `850.0f` |
| `HOSTILE_PATROL_RING_METRES` | `400.0f` |
| `HOSTILE_PATROL_CRUISE_MPS` | `10.0f` |
| `HOSTILE_PATROL_COUNT` | `3` |

In the HUD block: `HUD_MINIMAP_STRUCTURE_DOT_PX = 8.0f`.

The file's header comment says "nothing here feeds back into a tick". `START_SPACING` already
stretched that and these numbers break it: they are boot content the composition root passes into
the world. Replace the absolute with one honest sentence — tuning is never read by `GameLogic`;
the starting-scene block is content the root spawns from, and that is the only way any of it
reaches a tick.

The numbers must equal the ones `PatrolTests` uses (slice 2 §5); the pull request states that
they do.

### 2.2 `Outpost/OutpostApp.h/.cpp` — the boot

- **The mesh table splits from the fleet table.** `STARTING_HULLS` becomes two things: a
  `HULL_MESHES[]` table of `{mesh, hull}` for Bomber, Corvette, Frigate, Interceptor, Structure,
  loaded and registered by a new `LoadHullMeshes()` before either spawn; and the fleet spawn,
  which no longer loads anything. A missing mesh stays a logged diagnostic: the hull is skipped
  by `RegisterHullMesh`, never by `SpawnShip` — a ship without a mesh still simulates, which is
  what the view already handles (`INVALID_MESH` draws nothing). The comment that pairs mesh and
  hull by name rather than by index stays on the new table.
- `SpawnStartingFleet()` — unchanged three hulls and spacing, `FACTION_PLAYER` written
  explicitly.
- `SpawnHostileBase()` — the station at `LocalPos(HOSTILE_BASE_EAST_METRES,
  HOSTILE_BASE_NORTH_METRES)`, heading 0, `HullId::Structure`, `FACTION_HOSTILE`. Then
  `HOSTILE_PATROL_COUNT` Interceptors at ring indices `i * PATROL_RING_WAYPOINTS /
  HOSTILE_PATROL_COUNT` (0, 4, 8), positioned by `PatrolRingPoint` and headed by
  `PatrolRingHeadingRad`, each `AssignPatrol`'d with the ring and cruise constants. Called after
  the fleet, before the boot log line.
- **The boot log counts the player's ships** — records whose faction is `m_ownFaction`, not
  `m_world.ShipCount()`. `FLEET ONLINE | 3 SHIPS`, not 7.
- `Game::FactionId m_ownFaction = Game::FACTION_PLAYER;` on the app, with the "arrives with the
  session the day a login exists" comment. It is handed to `m_view.SetOwnFaction` at init and to
  `Frame::ownFaction` every frame.
- **`Frame::contacts`** is computed in `Render`: the count of `m_view.Ships()` records whose
  `factionId != m_ownFaction`. The station counts (design §11). The `Hud::Frame` comment stops
  calling it a mock.
- F4 is unchanged. It despawns *selected* ships, and hostiles cannot be selected (2.4), so it
  cannot reach one; no rule is needed.

### 2.3 `Outpost/WorldSimulation.h` — the subscriber's centre

`SubscriberCentre` averages only ships whose `factionId == m_subscriberFaction`. Its comment's
premise ("every ship is its own") has expired in this slice and the comment changes with it. No
own-faction ship at all returns `WorldPos{}` as an empty world does today.

### 2.4 `Outpost/WorldView.h/.cpp` — what a client may not do, and what it may explode

- `void SetOwnFaction(Game::FactionId _faction) noexcept;` stored as `m_ownFaction`, default
  `FACTION_PLAYER`.
- `PickShip` skips a record whose faction is not `m_ownFaction`; so hover, tap, shift-tap and
  double-tap can never land on a hostile. `OnBoxSelect` skips them too. No other selection path
  exists (`AssignGroup`/`SelectGroup` only ever hold what was selected), and the pull request says
  that was checked rather than assumed.
- **`ExplodeTheLost` consumes `m_receiver.Destroyed()`.** A carried-out handle explodes only if
  the last applied update lists it as destroyed; a bare leave removes the view silently. Camera
  shake and the `SHIP LOST` log line move inside the same condition. The function's comment —
  "consumes a despawn rather than a death" — is replaced: it now consumes what the server stated.
- Rendering is untouched: a hostile draws with the same mesh path, `SHIP_COLOUR`, trails and
  banking as a friendly (design §7). No tint, no bracket.

### 2.5 `Outpost/Hud.h/.cpp` — the overview

- `Frame` gains `Game::FactionId ownFaction = Game::FACTION_PLAYER;` beside `contacts`.
- The blip loop colours by allegiance:

  | Record | Blip |
  |---|---|
  | own faction, selected | `HUD_ACCENT_GREEN` |
  | own faction, unselected | `HUD_ACCENT_GREEN` at 0.7 alpha |
  | other faction, `HullSpecOf(hullId).immovable == false` | `HUD_ALERT_RED`, `HUD_MINIMAP_DOT_PX` |
  | other faction, immovable | `HUD_ALERT_RED`, `HUD_MINIMAP_STRUCTURE_DOT_PX` |

  The edge-clip test uses the dot's own size. The comment "Every ship in the world is friendly
  today; hostiles arrive with combat…" leaves in this change.
- `CONTACTS n` draws `HUD_ALERT_RED` when `n > 0`, `HUD_ACCENT_AMBER` otherwise, where it sits.
- Nothing else on the HUD changes. The map's `HUD_MINIMAP_HALF_RANGE` is already 1 400 m, which
  is the number design §7 argues against the 2 000 m interest radius; do not retune it here.

### 2.6 The sentences this makes false

- `AGENTS.md` "What is actually here": the game is no longer only "a fleet of three hulls"; add
  the enemy station and its patrol in one clause, and that hostiles are shown on the minimap and
  cannot be selected. Same commit.
- `Outpost/Hud.h`'s `Frame` comment: `contacts` is real; `sector` already is.
- `Outpost/ViewTuning.h` header (2.1), `WorldSimulation::SubscriberCentre` (2.3),
  `WorldView::ExplodeTheLost` (2.4), `Hud.cpp`'s blip comment (2.5).
- `Design/SpaceshipExplosion.md` §9/§12 if either says the explosion consumes every leave or that
  no destroy event exists: one sentence pointing at `Hostiles.md` §4.4.

### 2.7 What this slice deliberately does **not** do

- **No `GameLogic` file.** If one seems necessary, stop and say so; it is a slice 1 or 2 defect.
- No combat, damage, aggro, tint, target brackets, health bars, faction names, event-log line for
  first contact, red log severity (design §12).
- No NMO; `Structure.obj` and `Interceptor.obj` load through the OBJ path like the rest.
- No retuning of the camera, the interest radius or the minimap range.

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| `Outpost/OutpostApp.cpp` | `STARTING_HULLS`, `SpawnStartingFleet`, `HULL_NAMES` (already covers the whole table), the `Frame` fill in `Render`, the F4 despawn |
| `Outpost/WorldSimulation.h` | `SubscriberCentre`, `m_subscriberFaction` (slice 1) |
| `Outpost/WorldView.cpp` | `PickShip`, `OnBoxSelect`, `OnTap`, `ExplodeTheLost`, `ApplySnapshot`'s carry pass |
| `Outpost/Hud.cpp` `DrawMinimap` | the blip loop, the header's `CONTACTS` draw, `HUD_MINIMAP_DOT_PX` |
| `GameLogic/Patrol.h` (slice 2) | `PatrolRingPoint`, `PatrolRingHeadingRad` |
| `GameLogic/HullSpec.h` | `HullSpecOf(hullId).immovable` — how the HUD tells a station from a ship without a new wire field |
| `GameLogic/WorldSnapshot.h` (slice 1) | `ShipSnapshot::factionId`, `SnapshotReceiver::Destroyed()` |
| `Outpost/Assets/Meshes/Structure.obj`, `Interceptor.obj` | shipped, never loaded until now |
| `Design/Hostiles.md` §6, §7, §11, §12 | the scene, the display rules, the choices, the edges |

---

## 4. Acceptance

Visual, where only a screen can decide it (Design/README.md), at two window sizes — 1600×900 and
one other — in the pull request:

- **Boot.** The station northeast of the fleet in the scene (running lights, no trail motion);
  three Interceptors on the ring; the minimap shows three green dots at the centre, one 8 px red
  square and three 4 px red dots to the upper right, `CONTACTS 4` in red, `SECTOR 0,0`, and
  `FLEET ONLINE | 3 SHIPS` in the log.
- **Motion.** Two screenshots ~30 s apart: the red dots have moved clockwise around the square;
  the square has not moved.
- **Selection.** A tap on an Interceptor and a box drag over the station: `0 SELECTED`, no ring,
  no hover highlight. A tap on a friendly still selects it.
- **Death versus departure.** Select a friendly, F4: explosion, shake, `SHIP LOST`. Then order the
  fleet 2.5 km south-west so the base leaves the interest radius: the red dots drop off the map
  and `CONTACTS` reads 0 with **no** explosion, no shake, no `SHIP LOST`. Order the fleet back:
  `CONTACTS 4` again.
- **Authority.** With a friendly selected, a tap on empty ground beside a hostile moves the
  friendly only (the gate is tested in slice 1; this is the UI-side check that nothing here found
  a way around it).

Not visual:

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; all four suites green and unchanged — `GameLogicTests` in particular has no
  edits, because no `GameLogic` file does.
- `git diff --stat` shows no path under `GameLogic/`; the pull request says so.
- No decision record is due (the three the feature owes landed with slices 1 and 2); say so.
- `Design/Hostiles.md` §14 marks slice 3 `landed`; this file moves to `Design/Archive/`; the
  `AGENTS.md` sentence has changed.

---

## 5. Assumptions the implementer may make

- **The Structure's thruster faces glow.** The mesh carries `thruster`-material faces like every
  hull, so the view gives the station idle-intensity lights and zero-length trails. Acceptable —
  design §3 says so — and the screenshot shows it rather than hides it.
- **Structure and Interceptor OBJs load as they are.** If either fails to parse, it is logged and
  the hull draws nothing; the simulation and the minimap are unaffected, and the pull request
  reports it as a content defect rather than fixing the mesh here.
- **`CONTACTS` counts the subscription, not the map rectangle** (design §7). A contact beyond the
  map edge is counted and clipped.
- **The minimap's structure dot is not to scale** — 8 px fixed, design §11.
- **A hostile is never the last own-faction ship**, so `SubscriberCentre`'s empty-fleet path is
  reachable only by despawning the whole fleet with F4, where it returns the origin as today.
