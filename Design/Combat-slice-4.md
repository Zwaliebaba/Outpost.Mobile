# Work order — Combat slice 4: the fight you can see

Implements the drawable half of slice 4 of [`Combat.md`](Combat.md) §16: muzzle, tracer and impact
off the fire block, damage on the fleet sheet, the completion edge in the log, and the retirement of
the two debug keys that stood in for acts the simulation now states for itself (design §10.2, §10.3,
§6).

**The turret slew is not in it.** It needs a renderer entry point that does not exist and a
screenshot to accept, and it is cut out as slice 6 (§2.7).

**Layer:** `Outpost`, plus two constants in its view tuning.
**Depends on:** slice 2, for the fire events and the hull fraction; slice 3 is beside it, not under
it — nothing here reads a submesh.
**Blocks:** nothing. Slice 5 tunes numbers this makes visible; slice 6 turns the turrets.

---

## 1. Why this is a slice

After slices 1 and 2 the simulation shoots, the wire says so, and the client throws all of it away.
A player watching a battle sees ships vanish with a shatter and has no way to know a shot was ever
fired. This is the slice where the feature becomes a thing you can watch rather than a thing the
tests agree about.

It is deliberately built out of what the client already draws. A muzzle flash, a tracer and an
impact are `GlowSample`s — the same billboards a running light already is — so nothing enters the
renderer's contract, no pipeline is added, and no shader is touched. That is the whole reason this
half can land without the screenshot the other half needs.

---

## 2. Scope

### 2.1 `Outpost/WorldView.h` / `.cpp` — the gunfire the wire carries

A small ring of live shots, filled from `SnapshotReceiver::Fire()` on the same pump that drains
departures, and drained with `ClearFire()` for `ClearDestroyed()`'s reason — two messages in one
pump must not lose the first one's tracers:

```cpp
struct GunShot
{
  DirectX::XMFLOAT3 fromWorld{}; // the shooter, where it was when the shot arrived
  DirectX::XMFLOAT3 toWorld{};   // the target, likewise
  Neuron::Rgba colour{};         // the shooter's livery, so a player can tell whose fire it is
  float ageSec = 0.0f;
};
```

Resolved **at arrival** rather than per frame, and both ends are frozen there: either ship may have
died on the tick the shot landed, and a tracer that chased a live record would snap to nothing
mid-flight. This is `ShipExplosion`'s own rule — it is built from the hull's last drawn matrix
precisely because by then the record is gone.

A shot whose shooter and target are both unknown to this client is dropped: it is fire between two
ships neither of which is on screen.

### 2.2 `Outpost/WorldView.cpp` — what it looks like

Three things per live shot, all `GlowSample`s pushed into the same `m_glowSamples` the nav lights
and plumes use, all fading over `GUN_TRACER_SEC`:

- **the muzzle**, a bright short-lived glow at the shooter end, largest on the first frame;
- **the tracer**, `GUN_TRACER_BEADS` glows interpolated along the line, so the shot reads as a
  direction rather than a dot;
- **the impact**, a glow at the target end, on the same fade.

The colour is the shooter's livery. That is the one piece of information a player most needs out of
a firefight — whose shot that was — and the livery table already answers it for every hull on
screen.

### 2.3 `Outpost/WorldView.h` / `.cpp` — condition

`ShipView` gains `hullFraction`, copied from the record like `faction` is, and
`ConditionOfMember(EntityId)` answers the sheet: the member's fraction as 0..1, or **-1 for a member
this client holds no record for**, which is a fleet somewhere the camera has never been. A caller
that cannot tell those apart would draw a healthy pip for a ship it knows nothing about.

### 2.4 `Outpost/FleetSheet.cpp` — the pip row

One `DrawScreenRect` per member of the roster, in roster order, each filled to its own condition and
coloured green through amber to red; a member with no record draws as an outline. It sits under the
hull-count line the sheet already draws, which is where design §10.3 put it.

Pips rather than a number, because the question a commander asks the sheet is "is anything about to
die", and eight small bars answer it in one glance where eight percentages do not.

### 2.5 `Outpost/WorldView.cpp` — one line in the log, and only one

`TARGET DESTROYED`, on the rising edge, when a ship that **this client's own fire was aimed at**
within the last few seconds leaves as destroyed. The client has both halves already: the fire block
says who shot at whom, and the departure run says who died.

It is edge-triggered and attributed, which is the whole of what keeps it out of the way: an 8-entry
ring in a firefight is exactly why `FLEET %d UNDER ATTACK` is edge-triggered, and a per-hit line
would evict everything else in under a second. No other line is added.

### 2.6 `Outpost/OutpostApp.cpp` — F6 and F7 retire

An attack order on a Vanguard asset *is* F6 now, and any landed hit *is* F7. Both keys go, with the
`RecordAggression` and `RecordHostileAct` calls behind them; `World` keeps both entry points, which
are still the simulation's own and still have no client message (ADR 0041). **F4 stays** — a tuning
hook for the explosion is still a tuning hook.

The sentences the two keys made true go with them: the key table in `README.md`, and the paragraphs
in `README.md` and `AGENTS.md` that say there is no combat, that the hostiles have no weapons, and
that a protector has no weapon either. Slices 1 to 3 deferred that truth maintenance to this slice
on the ground that the game a player boots should be the one the documents describe; this is where
it becomes true.

### 2.7 What this slice does not touch, and what moves out of it

- **The turret slew moves to slice 6.** `SceneRenderer::DrawMesh` takes one world matrix for a whole
  mesh, so turning one part needs a new submesh-range draw and a hull drawn as its complement — a
  new entry point into the D3D12 command list. It is the one piece of this feature whose acceptance
  is a screenshot and nothing else, and it is cleanly separable: everything above is visible without
  it, and a turret that does not turn is a hull that looks exactly as it does today.
- **Kill attribution on the wire.** §2.5's line is the client's own inference from two facts it
  already holds, and it stays that: the leave run still says only that a ship died.
- **`GameLogic` and the wire.** Nothing on either side changes.

---

## 3. What to build on

- **`ExplodeTheLost`** — the drain that already reads `Destroyed()` and `Docked()` and clears them,
  and the rule that an effect is built from what the client last drew rather than from a record that
  is gone.
- **The nav light and plume loops** — the `GlowSample` push, `HullPointToWorld`, and the fade
  arithmetic a tracer copies.
- **`ReportFleetEvents`** — the rising-edge idiom §2.5 follows, and the reason it exists.
- **`WorldView::LiveryOf`** — whose shot a tracer is drawn in.
- **`FleetSheet`'s status line and `DrawScreenRect`** — where the pip row goes and what draws it.

---

## 4. Acceptance

**There is no test suite for `Outpost`.** The executable has none and this slice does not invent
one; what can be checked by a machine here is that the tree builds and that nothing below it broke.
That is stated rather than glossed, because it is the honest reason the screenshots below carry more
weight in this slice than in any before it.

**The tree**

- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`, clang-tidy clean.
- Debug|x64 builds and all four suites run, with the debug STL's bounds checking on. No suite
  should change: nothing below `Outpost` is touched.
- **Screenshots at two window sizes**, of a fight and of a quiet frame, per AGENTS.md §8.
- No decision record: nothing moves between libraries and no rule changes. Slice 6 may owe one for
  the submesh draw.
- `Combat.md` §16 marks slice 4 *in review* and gains slice 6.

**What lands unverified, and is owed.** This session has no Windows, no D3D12 and no screen: the
`Outpost` layer cannot be compiled here, let alone run or photographed. Every line of this slice is
written against the patterns beside it and checked by eye, and CI's `Debug|x64` is the first compiler
to see it. The screenshots are owed and are recorded as owed — the same debt `Fleets.md` carries at
the top of itself for its own three client slices, and it is paid the same way: by somebody running
the game.

---

## 5. Assumptions the implementer may make

- **A tracer is a straight line between two frozen points.** No ballistics, no lead, no travel time:
  the shot already happened (ADR 0053), and the line is a drawing of it rather than a simulation.
- **A shot with one end off screen still draws.** The end that is known is where the interesting
  half is, and being shot at from outside your view is the case slice 2 spent a filter on.
- **The pip row is at most `MAX_FLEET_SHIPS` wide**, which is eight, and needs no scrolling.
- **`TARGET DESTROYED` may be missed** when the shot that killed something never reached this client
  — a lost datagram, or a kill made outside the interest set. It is a flourish on the log, not a
  fact the player is owed, and the ship still leaves with `SHIP LOST`.
