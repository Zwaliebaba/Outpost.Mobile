# Work order — Fleets slice 8: the sheet

Implements slice 8 of [`Fleets.md`](Fleets.md) §16: the fleet sheet a hold opens, its status and
member lines, its command row and the target tap those commands arm, and the last of §9.6's log
lines (design §9.3, §9.6).

**Layer:** `Outpost`.
**Depends on:** slice 6 (the bar, whose hold gesture this replaces), merged. Slice 7 is not a
dependency — the design pairs them only for a demo that composes a fleet and then opens its sheet.
**Blocks:** nothing. It is the last slice of the feature.

---

## 1. Why this is a slice

Two things are missing and they are the same thing. The commands have no *names* anywhere: a player
can move a fleet by tapping the ground and attack by tapping a hostile, and nothing on screen has
ever said either is possible. And a fleet outside the interest set can be seen on the bar and on the
minimap but not *read* — the status byte knows it is docking and nothing displays it.

The sheet is where a fleet is read and where its verbs are named, which is what a first session needs
and a tap cannot teach (§9.3).

It is also where the log finishes. Five of §9.6's lines exist; three do not, and two of those need a
distinction only this half can draw — whether a slot emptied because its fleet **docked** or because
its last ship **died**. The wire has carried the difference since ADR 0040 and nothing has read it
at fleet grain.

---

## 2. Scope

### 2.1 `Outpost/WorldView.h`/`.cpp` — the fleet's own events, in one place

The under-attack edge moves here from `Hud`, and joins the two that need departure causes. One
function, `ReportFleetEvents()`, called at the end of `ApplySnapshot`, holding four small arrays:

```cpp
std::vector<Game::EntityId> m_lastRoster[FLEET_SLOTS]; // the previous update's, for attribution
std::uint8_t m_lastFleetMask = 0;
bool m_wasUnderAttack[FLEET_SLOTS] = {};
bool m_wasLaunching[FLEET_SLOTS] = {};
bool m_slotDockedOut[FLEET_SLOTS] = {};   // its last departure was a docking, not a death
```

- `FLEET %d UNDER ATTACK` on the alert's rising edge (moved from `Hud::UpdateAlerts`, which keeps
  only the pulse clock).
- `FLEET %d | %d SHIPS OUT` when the status byte stops saying `Launching` — the manifest emptying,
  which is the moment the fleet is whole.
- `FLEET %d | DOCKED` and `FLEET %d LOST` when a mask bit clears, told apart by whether that slot's
  last departing member was in the receiver's `Docked()` list or its `Destroyed()` one.

**Why here and not in the HUD.** The alert edge sat in `Hud` because that is where it was needed;
the other two cannot, because only this class sees the departure lists, and they are cleared inside
`ExplodeTheLost` before anything else could look. Splitting §9.6 across two files by which line
happened to need what is the shape to avoid — the view already reports what happened
(`DOCKED | %d SHIPS`, `SHIP LOST`), so this is where a fleet's version belongs.

**Why a previous roster is kept.** A departing member is out of every roster by the time
`ExplodeTheLost` runs — the rosters arrive on the reliable lane and are applied before the records.
Matching against the roster of the update before is what says which slot a docking or a death
belonged to.

### 2.2 `Outpost/WorldView.h`/`.cpp` — remembered hulls

The sheet's member rows need a hull per roster member, and a roster names entities. A fleet the
camera is not at has no records, so the hull ids are not there to read.

```cpp
struct KnownHull { Game::EntityId entity; std::uint32_t hullId; };
std::vector<KnownHull> m_knownHulls;
[[nodiscard]] std::uint32_t HullOfMember(Game::EntityId _entity) const noexcept;
```

Refreshed in `ApplySnapshot` from the records in view and **pruned to the current rosters**, so it
holds at most `FLEET_SLOTS * MAX_FLEET_SHIPS` — forty entries — rather than growing for the length
of a match. A member never seen reads `HULL_COUNT`, which the sheet draws as an unnamed row rather
than as a wrong one.

This is the one piece of client state that outlives what is on the wire, and it is bounded by the
thing it exists for. Anything else that wanted to remember a departed ship would want a different
answer.

### 2.3 `Outpost/WorldView.h`/`.cpp` — the armed order

```cpp
enum class ArmedOrder : std::uint8_t { None, Move, Attack, Dock };
void ArmFleetOrder(ArmedOrder _kind);
[[nodiscard]] ArmedOrder Armed() const noexcept;
```

`OnTap` checks the arming **first**, before its own three meanings. Armed, the next world tap
supplies the target through the pickers taps already use — ground for Move, a hostile record for
Attack, a station for Dock — and anything else cancels with the log saying so (§9.3). The arming
clears either way: one prompt, one tap.

`PressFleetButton` returns what a press meant, so the panel is the caller's to open:

```cpp
enum class ButtonPress : std::uint8_t { Nothing, Selected, OpenSheet };
```

The view keeps knowing what a fleet slot is; the UI keeps knowing what a panel is. That is the same
division slice 6 drew and this is the first press that needs a third answer.

### 2.4 `Outpost/FleetSheet.h`/`.cpp` — the panel

Its own pair of files, `AssemblyScreen`'s precedent and its reasons.

```
FLEET 3                          × 6
ENGAGED — DEFENDING
CORVETTE × 2   MINER × 3   HAULER × 1
[ MOVE ]  [ ATTACK ]  [ DOCK ]  [ STOP ]
```

- **Over the bar, not modal.** It consumes only what lands on itself; the world keeps working behind
  it, which is what "a panel over the bar" means and is the opposite of the assembly screen's rule.
- **The status line** is `WorldView::FleetActivity` (slice 6) plus the engaged word, with
  `LAUNCHING %d OF %d` when the manifest still holds anything — the roster's size against the status
  block's count, which is the pair §8.2's amendment named this line as the use for.
- **Member rows** are the roster's hulls, counted, in hull-id order. Hull bars are left where the
  damage model will want them (§9.3).
- **`STOP` sends immediately and closes.** The other three arm a target tap and close to a one-line
  prompt in the sheet's own place — `ATTACK | TAP A TARGET` — so the gesture is visible while it is
  live rather than only in the log.
- **`MINE` is absent** until the mining design lands (§6.6). No queue is shown because none exists.

The sheet closes when its fleet stops being held, when another is selected, and on Escape — after
the assembly screen and before the selection, since it is the innermost thing open.

### 2.5 `Outpost/Hud.*`, `Outpost/OutpostApp.cpp` — the wiring

`Hud::UpdateAlerts` becomes `Hud::UpdatePulse(float)` and loses its log; `m_wasUnderAttack` goes with
it. `Hud::HandlePointer` passes `PressFleetButton`'s answer out so the app can open the sheet.
The sheet draws after the HUD and before the assembly screen, takes pointer events between them, and
joins Escape's chain.

### 2.6 What this slice does not touch

`GameLogic`, entirely. The assembly screen. `MINE`, order queues, hull bars, and the station
management screen — all named in §14 as deliberately absent.

---

## 3. What to build on

- **`WorldView::FleetActivity`, `FleetCount`, `IsFleetHeld`, `SelectFleet`** (slice 6).
- **`SnapshotReceiver::RosterOf`, `Docked()`, `Destroyed()`** (slices 5 and ADR 0040) — the rosters,
  and the departure cause the last two log lines turn on.
- **`AssemblyScreen`** (slice 7) — the panel class shape, its layout-once rule, and its capture idiom.
- **`Hud`'s bottom bar** — the sheet sits directly above it and shares its idiom.
- **`PickShip` / `PickStation` / `PickHostile`** — the three pickers the armed tap resolves through,
  unchanged.

---

## 4. Acceptance

No `Outpost` suite; client slices are decided by screenshots and by what they must not touch.

**Screenshots owed in the pull request**, at two window sizes: the sheet over a launching fleet
(`LAUNCHING 4 OF 8` with part of the roster listed); the sheet over an engaged one, its button
glowing red behind it; and the armed prompt after `ATTACK`.

**Stated rather than claimed**: no MSVC, no D3D12 and no display here, so those cannot be taken from
this environment.

| Check | Decides |
|---|---|
| `GameLogicTests`, all suites | nothing in `GameLogic` moved |
| `Build/CheckProjectFiles.py` | the two new files are registered, no layer crossed |
| `Build/CheckFormat.py` | formatting |
| Windows CI `Debug|x64` | all five projects compile |
| An API check on the Linux harness | the event edges — launching-to-out, docked, lost — against a real world driving a real receiver |

---

## 5. Assumptions the implementer may make

- **A member's hull may be unknown.** A fleet composed and launched while the camera was elsewhere
  has members this half has never held a record for. An unnamed row is the honest answer; a guessed
  one is not.
- **The sheet may outlive its fleet.** A fleet can dock or die while its sheet is open; the sheet
  closes rather than showing a slot nobody holds.
- **Every command still passes the simulation's gates.** The sheet names the verbs; it does not
  become a second authority (ADR 0014).
