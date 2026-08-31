# Work order — Fleets slice 7: assembly

Implements slice 7 of [`Fleets.md`](Fleets.md) §16: the station long-press, the
`LedgerRequest`/`LedgerReply` flow on the client, the assembly screen, and compose-and-launch end to
end on screen (design §9.4, §5.2).

**Layer:** `Outpost`, plus one gesture in `NeuronClient`.
**Depends on:** slice 5 (the ledger exchange and `ComposeOrder`) and slice 6 (a bar with an empty
button to fill), both merged.
**Blocks:** nothing. Slice 8 is independent of it, and the design pairs them only for a demo.

---

## 1. Why this is a slice

Slice 5 put the ledger on the wire and slice 6 gave the five buttons somewhere to live, and between
them there is still no way to make a fleet. `World::ComposeFleet` has existed since slice 2 with
exactly one caller: its tests. This is the slice where a player can call it.

It is also where `PointerTracker` finally learns a long press, on the schedule
`Design/Archive/Stations.md` §14 set for it in as many words: *"`PointerTracker` learns long-press
when there is a menu to open, not before."* There is now a menu to open.

---

## 2. Scope

### 2.1 `NeuronClient/PointerTracker.h`/`.cpp` — the long press

`PointerListener` gains one method:

```cpp
virtual void OnLongPress(float _xPx, float _yPx) = 0;
```

and `Desc` gains `float longPressMs = 450.0f`.

**On release, not at the threshold.** The tracker is event-driven and sees nothing between a Down
and the next Update, so a contact held perfectly still generates no event to notice a threshold
crossing in. Firing at the threshold would need the tracker to be ticked every frame, which is a
different class than the one this is, and the gesture works without it. What it costs is the
mobile-idiomatic feel of the menu opening under a still finger; that is a trade to revisit the day
the tracker gets a frame tick, and it is named here rather than left to be discovered.

**A dead band between a tap and a hold, deliberately.** A release still taps at or under
`tapMaxDurationMs` (320 ms) and now long-presses at or over `longPressMs` (450 ms); between them it
does nothing, exactly as anything over 320 ms did before this slice. A slow, hesitant tap on a
station should not open a screen, and the way to guarantee that is to make the hold a thing the
player commits to.

A press that dragged is a drag and never a long press, which the existing `finished.dragging`
branch already returns on.

### 2.2 `Outpost/WorldView.h`/`.cpp` — asking, and being answered

```cpp
void OnLongPress(float _xPx, float _yPx) override;

// A ledger reply for the station this client asked about. Consumed: it answers true once per
// reply, so the screen opens once per ask.
[[nodiscard]] bool TakeLedgerReply(Game::LedgerReply& _outReply);

void SendComposeOrder(Game::EntityId _station, std::uint8_t _slot, std::span<const std::uint32_t> _hullCounts);
```

`OnLongPress` picks a station under the point, **checks the mask first**, and only then sends:

- a hostile owner logs `LEDGER REFUSED | %s HOSTILE` and sends nothing — the affordance tells the
  truth before the wire is touched, which is the rule `IssueDockOrder` already keeps and
  `Design/Archive/Stations.md` §9.2 states;
- otherwise `WriteLedgerRequest` goes up and the station's entity is remembered as the pending ask.

The reply is noticed by polling `SnapshotReceiver::LedgerReplyCount()` against the count at the
ask — which is what that counter exists for, since two replies for one station are identical and
the payload alone cannot say whether the wire has spoken (ADR 0051). A reply for a station other
than the pending one is dropped: it answers a question this screen is not asking.

`TakeLedgerReply` is a *consuming* read rather than an accessor, so a screen cannot be opened twice
by one reply and a stale reply cannot open one at all.

### 2.3 `Outpost/AssemblyScreen.h`/`.cpp` — the screen

Its own class and its own pair of files, not more of `Hud`. It holds state `Hud` has no business
in — a draft, a chosen slot, a station — it is modal where the HUD is an overlay, and `Hud.cpp` is
eight hundred lines already.

```
VANGUARD STATION                 DOCKED
FRIGATE   × 2     [ + ] [ − ]
CORVETTE  × 3     [ + ] [ − ]        DRAFT   × 5 OF 8
MINER     × 4     [ + ] [ − ]        SLOT  [1] [2] [·] [4] [·]
                                     [ LAUNCH ]
```

- **Rows**: one per hull the asker has docked here, in hull-id order, with its count. A hull with
  none is not listed — the ledger reply's zeros are absence, not a row.
- **`+` / `−`**: move one between the ledger column and the draft. `+` is refused past the hull's
  own count and past `MAX_FLEET_SHIPS` in total; the counts are what is drawn, so a refused press
  is a press that visibly does nothing rather than a message.
- **Draft**: `× n OF 8`, from `Game::MAX_FLEET_SHIPS` rather than from an 8 written here.
- **Slots**: five, occupied ones inert and marked, from `WorldView::IsFleetHeld`. One is chosen;
  the first free one is chosen when the screen opens, and there being none is what greys `LAUNCH`.
- **`LAUNCH`**: sends `ComposeOrder` and closes. Refused — a raced slot, a stale ledger — simply
  leaves the button empty and the screen's next opening asks again, which is fire-and-forget like
  every other order (design §9.4).

**Modal**: while it is open it consumes every pointer event, so nothing reaches the HUD or the
world behind it. Escape closes it, ahead of Escape's other two meanings.

Drawn in the HUD's own idiom from `ViewTuning.h` — `HUD_PANEL_FILL`, `HUD_PANEL_OUTLINE`, the accent
green, the scanlines — because it is the same instrument, and it is laid out from the window's
corners and scaled by DPI like every other panel, so it reads at any size.

### 2.4 `Outpost/OutpostApp.cpp` — the wiring

Four small things: the screen is drawn after the HUD; it gets first refusal on pointer events; it is
opened from `WorldView::TakeLedgerReply` once a frame; and Escape closes it first. The tracker's
`Desc` gains the long-press duration from `ViewTuning.h`, beside its other two.

### 2.5 What this slice does not touch

`GameLogic`, entirely — the ledger, the gates and `ComposeFleet` have been finished since slice 5,
and this slice is the caller they were waiting for. The fleet sheet (slice 8). Undocking, trade,
repair and cargo, which are the station management screen's and remain the next phase's
(`Stations.md` §14). The rail buttons, which still open nothing.

---

## 3. What to build on

- **`Game::WriteLedgerRequest` / `SnapshotReceiver::Ledger` / `LedgerReplyCount`** (slice 5) — the
  exchange, and the counter that tells a fresh answer from a stale one.
- **`Game::WriteComposeOrder`** (slice 5) — the draft going back.
- **`WorldView::IsFleetHeld`** (slice 6) — which slots are free.
- **`Hud::HandlePointer`** — the capture-and-release button idiom the screen copies, down to a press
  only firing if the contact lifts inside it.
- **`Hud::ComputeLayout`** — the anchored, DPI-scaled layout shape.
- **`IssueDockOrder`'s refusal** — the affordance-tells-the-truth-first rule, applied to a read.

---

## 4. Acceptance

No `Outpost` suite; client slices are decided by screenshots and by what they must not touch.

**Screenshots owed in the pull request**, at two window sizes: the assembly screen open over a
station with a draft part-built; the same with `LAUNCH` greyed because all five slots are taken; and
the bar a moment after launching, with the new fleet's button counting up as the metronome pours it
out.

**Stated rather than claimed**: this environment has no MSVC, no D3D12 and no display, so the
screenshots cannot be taken here. What is verifiable here is the compile (Windows CI's `Debug|x64`),
the guards, and the `GameLogicTests` suite staying green — this slice touches no `GameLogic` file,
so a change there would itself be the bug.

| Check | Decides |
|---|---|
| `GameLogicTests`, all suites | nothing in `GameLogic` moved |
| `Build/CheckProjectFiles.py` | the two new files are registered in both project files, and no layer is crossed |
| `Build/CheckFormat.py` | formatting |
| Windows CI `Debug|x64` | all five projects compile |
| An API check on the Linux harness | every `GameLogic` call shape the new client code makes, compiled and run against the real headers — the request, the reply, the draft, the compose, and the fleet that comes out |

---

## 5. Assumptions the implementer may make

- **A reply may never come.** The lane can refuse a request, and a station can die between the ask
  and the answer. The screen simply does not open; nothing retries and nothing times out, because
  a second long press is the retry and the player has it.
- **The ledger is a snapshot, not a subscription.** What the screen shows is what was true when the
  reply was written. A compose against a stale ledger is refused by the gate, which is the case
  §9.4 already covers and the reason a refusal needs no message.
- **Every gate is the simulation's.** The screen's `+` bound and its inert slots are affordances
  telling the truth first; `ComposeFleet` still refuses independently, and neither is the other's
  substitute (ADR 0014).
- **Nothing here may write to the simulation** except by a message on the wire.
