# Work order — Fleets slice 6: the fleet bar

Implements slice 6 of [`Fleets.md`](Fleets.md) §16: the five buttons rebound from control groups to
fleet slots, selection at fleet grain, `FleetOrder` sending, the group machinery retired, the boot
scene as Fleet 1, the F7 debug hook, and the minimap's fleet digits (design §9.1, §9.2, §9.5, §9.6).

**Layer:** `Outpost`, plus one separate commit in `GameLogic` (§2.10).
**Depends on:** slice 3 (the `FleetOrder` message) and slice 5 (the roster and the status block),
both merged.
**Blocks:** slice 7 (assembly needs a bar with an empty button to fill) and slice 8 (the sheet is
the hold gesture this slice stubs).

---

## 1. Why this is a slice

Five `GameLogic` slices have built a fleet nothing can see or touch. Everything the client needs has
crossed the seam since slice 5; this is the slice where the player gets hold of it, and where the
old unit of command goes away.

That second half is what makes it one slice rather than two. Control groups and fleets both claim
the same five buttons, the same number keys' worth of muscle memory, and the same
selection model — so a slice that added fleets beside groups would ship a game with two ways to
command that disagree about what a selection is. The design took that decision once
([ADR 0049](Decisions/0049-orders-name-a-fleet-not-ships.md)): an order names a fleet. This slice is
that decision reaching the screen, and the group machinery leaving with it.

---

## 2. Scope

### 2.1 `Outpost/WorldView.h`/`.cpp` — the fleet is what is selected

`m_groups[CONTROL_GROUPS]`, `m_activeGroup`, `AssignGroup`, `SelectGroup`, `GroupSize`,
`ActiveGroup` and `RecallableIndex` go. In their place:

```cpp
static constexpr int FLEET_SLOTS = Game::FLEET_SLOTS;

void SelectFleet(int _slot, bool _additive);   // the button, and a tapped hull
[[nodiscard]] bool IsFleetSelected(int _slot) const noexcept;
[[nodiscard]] bool IsFleetHeld(int _slot) const noexcept;      // the status block's mask
[[nodiscard]] int FleetCount(int _slot) const noexcept;        // the status block's count
[[nodiscard]] std::uint8_t FleetStatusBits(int _slot) const noexcept;
[[nodiscard]] Game::WorldPos FleetPosition(int _slot) const noexcept;
[[nodiscard]] int SelectedFleetCount() const noexcept;
```

The selection is `bool m_fleetSelected[Game::FLEET_SLOTS]` and nothing else. **`ShipView::selected`
stays, and becomes derived**: `RefreshSelection()` walks the records and sets each one's flag from
whether its entity is in a selected slot's roster. Called at the end of `ApplySnapshot` and after
every selection change, so the rings, the minimap dimming, the bottom bar and the debug keys all
keep reading the flag they already read and none of them learns what a fleet is.

Derived rather than carried is the point: a roster arriving a tick after a record is what a launch
looks like from here, and a flag that was carried would leave the new hull unringed until something
else touched the selection.

`IndexOfEntity` replaces `RecallableIndex` — the same linear scan, without the group's ownership
question, because a roster only ever names the subscriber's own ships.

### 2.2 `Outpost/WorldView.cpp` — the pointer, at fleet grain

- **Tap on an own hull** → `SelectFleet(FleetSlotOf(entity), shiftHeld)`. A hull whose roster has
  not arrived yet selects nothing and says nothing: it is a launch one tick old, not an error.
- **Band select** → every fleet any banded hull belongs to, whole. Sub-fleet selection does not
  exist (design decision 1).
- **Shift-tap** toggles one fleet in or out.
- **Tap with a selection**, in order: a station docks, a **hostile record attacks** (new — §9.3), the
  ground moves. `PickHostile` is `PickStation`'s shape, filtered by `IsHostileToMe` rather than by
  the station flag.
- **Double-tap the ground** still drops the selection.

### 2.3 `Outpost/WorldView.cpp` — orders name fleets

`IssueMoveOrder(point, hasFacing, facingRad)` and `IssueDockOrder(station)` keep their names and
stop building ship lists. Each sends **one `FleetOrder` per selected slot** — five messages at the
very most, each of fixed size, against a budget of eight (ADR 0049). `MaxShipsPerOrder` and the
`ORDER TOO LARGE` line go with the ship lists: a fleet order cannot be too large, which is the
property the message was shaped for.

`IssueAttackOrder(target)` joins them, in the same shape.

The order marker keeps its place and its arithmetic. `FormationHeading` is still fed the positions
of the selected records — every selected fleet's members together, which is what the server will
solve each fleet about individually. That is a difference the marker cannot show and the design
already accepted: five fleets ordered at once get five formations and one marker.

The dock refusal (`DOCKING REFUSED | %s HOSTILE`) stays exactly as it is: the affordance tells the
truth first, and the simulation's gate stands behind it.

### 2.4 `Outpost/WorldView.cpp` — the camera flies to a fleet

```cpp
void FocusFleet(int _slot);          // the tap half of the button
void UpdateFocus(float _dtSec);      // called once a frame by the app
```

`Camera::SetGoal` and `Camera::Follow` already exist and nothing has ever called them — this is what
they were for. While a focus is live the view re-reads the slot's stated position every frame (the
fleet is flying, and a goal fixed at the tap would land where it used to be) and eases the camera
with `Follow`. It ends on arrival within `FLEET_FOCUS_ARRIVE_METRES`, or the moment the player pans,
orbits or taps the world — a camera that keeps hold of the view after the player has taken it back
is the one failure this must not have.

`ViewTuning.h` gains `FLEET_FOCUS_ARRIVE_METRES` and `FLEET_FOCUS_HALF_LIFE_SEC`. Presentation, so
they are the view's and free to be tuned.

### 2.5 `Outpost/WorldSimulation.h` — the interest set follows the camera

`SubscriberCentre()`'s own-fleet centroid is replaced by the camera's ground target, pushed in by
the composition root each frame:

```cpp
void SetViewCentre(const Game::WorldPos& _centre) noexcept;
```

**This is the change that makes the button work at all.** Tapping fleet 3 flies the camera fifty
kilometres; under the old centre — the centroid of every own ship, wherever they are — the interest
set stays where the ships' average is and not one hull of fleet 3 is ever sent. The design says the
interest set follows the camera (§9.1) and the existing comment anticipates exactly this: *"the day
a real player has a camera on the wire, it comes from there instead"*. It is not on the wire; the
composition root holds both halves and may read it, which is the same standing F4 and F6 already
have.

Two things the old centre bought are not lost. A player whose camera is over empty space is still
told where all five fleets are — that is the status block, which is precisely why slice 5 stamped it
on every update and why this change was not safe before it. And a fleet that docks no longer drags
the view anywhere: the camera does not move when a ship docks, so the station the player flew into
stays on screen, which is the fix `Stations-slice-6.md` §5 made by hand and this makes structural.

### 2.6 `Outpost/Hud.h`/`.cpp` — the five buttons

`Layout::groups` → `Layout::fleets`; `m_pressedGroup` → `m_pressedFleet`;
`HUD_GROUP_*` → `HUD_FLEET_*` in `ViewTuning.h`.

Per button, from the mask, the status block and the roster:

- **Held**: the slot digit and `×count`, filled while the fleet is selected.
- **Empty**: dimmed digit, and a hold logs `FLEET %d | COMPOSE AT A STATION`. Taps on an empty
  button do nothing.
- **Tap**: `SelectFleet(slot, shiftHeld)` **and** `FocusFleet(slot)` — one gesture, because under
  decision 1 selecting a fleet *is* attending to it.
- **Hold** (`HUD_LONG_PRESS_MS`, unchanged): the sheet, which is slice 8. The stub logs the one line
  the sheet's header will show — `FLEET %d | %d SHIPS | MOVING` — so the gesture is discoverable and
  says something true before the panel exists.
- **Under attack** (status bit 7): the button's outline and digit pulse `HUD_ALERT_RED`, and
  `FLEET %d UNDER ATTACK` is logged on the rising edge only. The edge is held per slot in the HUD,
  which is presentation state and the right place for it.

The bottom bar's title becomes `FLEET %d` for one selected fleet, `%d FLEETS` for several,
`NO SELECTION` for none.

### 2.7 `Outpost/Hud.cpp` — the minimap knows where the fleets are

Each held slot draws its digit at its stated position, in the station marks' treatment: inside the
map at the position, beyond it clamped to the edge at `HUD_MINIMAP_MARK_CLAMPED_ALPHA`, direction
honest and distance saturated. Under attack it draws `HUD_ALERT_RED` and pulses with the button.
Drawn after the marks and before the dots, so a fleet's digit sits over a station's diamond and
under its own hulls.

"Spread over the universe" becomes something the player can see, which is the sentence the owner's
brief opened with and the first place it is visible.

### 2.8 `Outpost/OutpostApp.cpp` — the boot scene is Fleet 1

`SpawnStartingFleet` collects the ids it spawns and calls
`World::FormFleet(FACTION_PLAYER, 0, ships)`. Without it the game boots with three ships that
nothing can select, because selection is fleet-grain now and they would be in no fleet — which is
also the honest statement of the fleet-only model (design decision 1) reaching the composition root.

### 2.9 `Outpost/OutpostApp.cpp` — F7

Beside F4's and F6's, under the same charter: a tuning aid may reach past the wire and a gameplay
path never may. The first selected own ship is the victim and the nearest ship of a faction that
holds the player hostile is the attacker, and `World::RecordHostileAct` is called directly. It is
the socket slice 4 built and the only way to see the defense, the glow and the red digit before
combat exists. No client message exists for it or ever will (ADR 0041, ADR 0050).

`OutpostApp.h`'s key list gains the row.

### 2.10 `GameLogic` — the ship-list order messages retire, in their own commit

After §2.3 nothing in the tree writes `MoveOrder` or `DockOrder`, so `WriteMoveOrder`/`ReadMoveOrder`,
`WriteDockOrder`/`ReadDockOrder`, `KIND_MOVE_ORDER`, `KIND_DOCK_ORDER`, `ORDER_HEADER_BYTES`,
`MaxShipsPerOrder` and the two branches in `Publisher::ApplyOrders` go, with their tests.

`World::IssueMoveOrder` and `World::IssueDockOrder` **stay untouched**: fleet orders lower onto them,
so the simulation does not change at all. What retires is the wire, not the machinery.

**This is a `GameLogic` change inside a slice whose layer is `Outpost`, and §11's acceptance list
says an `Outpost` slice touches no `GameLogic` file.** The two are in conflict and §16's slice row is
the one to follow: the messages are listed there, and a wire message nothing writes is exactly the
kind of thing that rots into a second way to command. It is a **separate commit** so that the client
commit satisfies §11 as written and the deletion is reviewable on its own terms. Recorded as an
amendment to §11.

### 2.11 What this slice does not touch

The fleet sheet's contents (slice 8), the assembly screen and `LedgerRequest`/`ComposeOrder` sending
(slice 7), and every `GameLogic` file except §2.10's deletion. No new wire message. No simulation
behavior: `World` gains nothing and loses nothing.

---

## 3. What to build on

- **`SnapshotReceiver::FleetMask`/`FleetStatusOf`/`RosterOf`** (slice 5) — everything the bar draws.
- **`Game::WriteFleetOrder`** (slice 3) — the one message this slice sends.
- **`Camera::SetGoal`/`Follow`** — written, tested and never called; the fly-to is their first user.
- **`Hud`'s group button** — the layout, the press capture, the long-press split and the count are
  all already there; this slice changes what they read and what they call.
- **The station mark on the minimap** (`Stations` §9.3) — the clamp-and-dim treatment the fleet
  digit copies exactly.
- **F4 and F6 in `OutpostApp`** — the charter F7 joins, stated in their comments.

---

## 4. Acceptance

There is no `Outpost` test suite; client slices are decided by screenshots (Design/README.md) and by
what they must not touch.

**Screenshots owed in the pull request**, at two window sizes: the fleet bar with counts and one
button glowing red; a fleet selected with its rings; the minimap with a clamped fleet digit; and the
boot scene showing Fleet 1 selected.

**Stated here rather than claimed**: this environment has no MSVC, no D3D12 and no display, so the
screenshots cannot be taken from it. What this slice can be verified against here is the compile
(Windows CI's `Debug|x64`), the guards, and — for §2.10 — the `GameLogicTests` suite. The screenshots
are owed by whoever runs the build, and are named here so the gap is visible rather than silent.

**Verifiable here:**

| Check | Decides |
|---|---|
| `GameLogicTests`, all suites | §2.10 removed a wire message and nothing else |
| `Build/CheckProjectFiles.py` | no file left unregistered, no layer crossed, no shadowed macro |
| `Build/CheckFormat.py` | formatting |
| Windows CI `Debug|x64` | all five projects compile and the four suites pass |
| `grep` for the retired names | no `AssignGroup`, `SelectGroup`, `GroupSize`, `ActiveGroup`, `CONTROL_GROUPS`, `GROUP %d`, `MaxShipsPerOrder`, `WriteMoveOrder`, `WriteDockOrder` left in the tree |

---

## 5. Assumptions the implementer may make

- **Every own hull is in a fleet, or is about to be.** The roster can lag a record by a tick at a
  launch; nothing else produces an own hull with no fleet. Code the transient as a no-op, never as
  an error path.
- **The status block is the truth about a slot; the roster is the truth about its members.** A
  button's count comes from the block (members plus manifest); the rings come from the roster. They
  disagree during a launch, and that disagreement is the feature (§8.2's amendment).
- **Nothing here may write to the simulation** except through a message on the wire — with the
  single exception of the debug keys, which are the composition root's and say so at the call.
- **The HUD holds presentation state and the view holds intent.** The alert's rising edge is the
  HUD's; which fleets are selected is the view's.
