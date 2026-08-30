# Work order — Stations slice 6: docking and the response, on screen

Implements slice 6 of [`Stations.md`](Stations.md) §16, the last: a tap on a station with ships
selected sends them to dock, the affordance refuses before the wire does, a docked hull leaves the
screen without ceremony, and F6 provokes the Vanguard so the scramble can be watched (design §7.4,
§8.1, §9.1, §9.2, §9.4).

**Layer:** `Outpost` only.
**Depends on:** slice 3 (`DockOrder`, the docked list), slice 4 (`RecordAggression` and the
response), slice 5 (the scene, `IsHostileToMe`, `FACTION_NAMES`).
**Blocks:** nothing; it closes the phase.

---

## 1. Why this is a slice

Everything below the client is in and tested: `IssueDockOrder` gates and flies, the dock pass
captures, the wire says *docked*, and `RecordAggression` scrambles a garrison. None of it can be
reached from the screen. This slice is the wiring — one picker, one order, one consumer, one key —
and its acceptance is what a screenshot shows.

The one thing it must get right (`Stations-slice-plan.md` §9): the docked handles go through a path
that is **not** the explosion. Destroyed and docked arrive in the same message and differ by a
list; a consumer that treats both spans alike looks like a bug in the explosion rather than in the
drain.

---

## 2. Scope

### 2.1 `Outpost/WorldView.h` / `.cpp` — the tap

- The oriented-box ray test in `PickShip` becomes `RayHitDistance(index, origin, direction)`, and
  `PickShip` and the new `PickStation` are two filters over it: own hulls only, and records whose
  `flags` carry `SHIP_FLAG_STATION` of any faction. `PickShip` does not change what it answers —
  stations stay unselectable, unhoverable and un-boxable (design §9.1).
- `OnTap`, in order: own hull → select (unchanged); station hull **and a non-empty selection** →
  `IssueDockOrder`; double tap on ground → clear; ground → move (unchanged). With nothing selected
  a tap on a station does nothing, and no long-press is added (design §14).
- `IssueDockOrder(station)`: if `IsHostileToMe(station faction)` — `DOCKING REFUSED | %s HOSTILE`
  with the owner's name from the faction-name table the root hands in (`SetFactionNames`), and
  nothing is sent. Otherwise the selection's handles and the station's handle go up the wire as a
  `DockOrder` (same size gate and same `ORDER TOO LARGE` line as a move), the log says
  `DOCKING | %d SHIPS`, and a marker flashes at the station's displayed position in the station's
  faction colour — `OrderMarker` gains a colour, and a move marker keeps `MARKER_COLOUR`.
- `ShipView` remembers its record's `factionId`, so the departure consumer below can count the
  player's own.
- The docked list: `ExplodeTheLost` gains a first loop over the lost views against
  `m_receiver.Docked()` — an own hull that docked is counted and the log says
  `DOCKED | %d SHIPS`; no explosion, no shake, no `SHIP LOST`; `ClearDocked` after. The destroyed
  loop is untouched, and a handle is never in both lists.

### 2.2 `Outpost/OutpostApp.h` / `.cpp` — F6

- **F6** marks the first selected own ship an aggressor against the nearest Vanguard station:
  the root calls `World::RecordAggression` directly under F4's charter (a tuning aid may reach past
  the wire; a gameplay path never may — design §8.1), and logs `VANGUARD PROVOKED`. With nothing
  selected, or no Vanguard station, the key does nothing.
- The root hands `FACTION_NAMES` to the view beside the HUD.
- The key list comment in `OutpostApp.h` names F6.

### 2.3 Bookkeeping carried on this branch

Slice 5 merged (#24) without its merge-commit bookkeeping: its work order moves to `Archive/`,
§16 marks it landed, the plan says the same. Slice 6 is marked *in review*. When **this** slice
merges, `Stations.md` and every Stations work order move to `Archive/` with citations retargeted —
that is the merge commit's, or the next branch's, as it was for 1–5.

### 2.4 Out of scope

- No `GameLogic` file. No undocking, no management menu, no long-press, no selection of stations.
- No new information reaches the client: the record's flag, the header byte and the docked list
  were all already there.

---

## 3. What to build on

`PickShip`'s box test; `IssueMoveOrder`'s selection walk, size gate and marker; `ExplodeTheLost`'s
carry walk and `Destroyed()`; `SnapshotReceiver::Docked`/`ClearDocked`; `Game::WriteDockOrder`;
`World::RecordAggression`, `StationCount`, `StationOf`, `Resolve`; `WorldView::LiveryOf` for the
marker's colour; `FACTION_NAMES` and `Hud::Frame::factionNames` from slice 5.

---

## 4. Acceptance

Screenshots at two window sizes:

- a dock order flying (`DOCKING | 3 SHIPS`, the azure marker on the station) and, later, the hulls
  gone with `DOCKED | 3 SHIPS` and no explosion;
- F6: `VANGUARD PROVOKED`, the Vanguard hulls and dots turning red, `CONTACTS` jumping, and the
  garrison's Corvettes launching from the station's skin;
- the refusal line at the Vandal base: `DOCKING REFUSED | VANDAL HOSTILE`.

All four suites green and untouched; `CheckProjectFiles.py` and `CheckFormat.py` pass; Debug|x64
builds; `git diff --stat` names no file under `GameLogic/`. No decision record due.

---

## 5. Assumptions

- A docked NPC (a protector coming home) leaves the screen silently and is not counted in the
  `DOCKED` line, which is the player's own.
- The dock marker is a plain flash with no facing pip, since a dock order has no facing.
- F6 chooses the station by distance to the aggressor at the moment the key is pressed.
- **Found on review, fixed here:** with every own ship docked, `WorldSimulation::SubscriberCentre`
  fell back to the universe origin and the interest set jumped away from the station the player
  had just docked at, which then vanished from the screen. The centre now holds where it last was
  when there is no own ship to average. `Outpost/WorldSimulation.h` joins the slice's files.
