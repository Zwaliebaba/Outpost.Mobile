# Work order — Fleets slice 4: the defense

Implements slice 4 of [`Fleets.md`](Fleets.md) §16: `RecordHostileAct`, `HullSpec::combatant`, the
threat, the anchor and the alert, the posture they drive, and the ordered attack that shares the
same chassis (design §7, §6.5).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** slice 3 (orders at fleet grain), merged — the defense suspends and resumes a
standing order, so there has to be one.
**Blocks:** slice 5, whose status block carries the engaged and under-attack bits this slice
defines, and slice 6, which draws the glow off them and calls the trigger from F7.

---

## 1. Why this is a slice

"When a fleet gets attacked it defends itself" is one of the four sentences the owner's brief opened
with, and it is the last of them with no code behind it. It is also the second time this tree has
built a reaction with no combat to trigger it — the protector response was the first — so the shape
is already argued: a **stated act**, never a sense (ADR 0041). What is new is the grain. A protector
is one ship with one duty; a fleet is eight ships of which some carry weapons and some carry ore,
and the whole design of the posture is about which of them react and for how long.

It is the last `GameLogic` slice of the feature. After it the fleet is complete as simulation and
everything left is wire and screen.

---

## 2. Scope

### 2.1 `GameLogic/HullSpec.h` — `combatant`

One authored `bool` per row, after `collidable`:

```
true   Interceptor, Bomber, Corvette, Frigate, Battleship, Carrier
false  Miner, Hauler, Stargate, Structure
```

**Authored rather than derived**, for `avoidanceAuthority`'s reason: a hull that is armed but
precious — a Q-ship, an armed hauler — has to stay expressible, and a flag derived from a weapon
table that does not exist yet would be a guess with no way to disagree with it. In the replay
contract, like every other column: it decides which ships react.

The column header comment above the table gains it, and all ten rows gain a value. The rows are
positional aggregates, so this is a change every row has to make — which is the point of the header
comment being there.

### 2.2 `GameLogic/SimTuning.h` — the leash and the alert

```cpp
inline constexpr float FLEET_ENGAGE_RANGE_METRES = 1000.0f;
inline constexpr std::uint32_t FLEET_ALERT_TICKS = 600; // ten seconds
```

Both in the replay contract: the first decides when combatants stand down, the second how long a
single act keeps them roused. 1 000 m is argued in design §7.2 — half the interest radius, so a
defense never drags a watched fleet's escorts off the player's screen; about the span of the widest
formation this design allows; and comfortably past every dock range in the table, so a fleet
attacked at a station's door defends the door.

### 2.3 `GameLogic/World.h` — the threat on the row

```cpp
ShipHandle orderTarget;      // Attack: what the standing order is aimed at
ShipHandle threat;           // who was last stated to have attacked a member
WorldPos threatAnchorPos;    // where that act was stated -- the leash's anchor, not the fleet
std::uint32_t alertTicks = 0;
```

**`threatAimPos` is not here**, though design §4.1 lists it. The point a pursuer last aimed at is
already `m_routes[member].destination`, which is where the protector keeps it and where
`PURSUIT_REPLAN_METRES` is measured from today (§2.6). A fleet-level copy would be a second source of
truth for the same number, carried by the codec, that nothing needs — every combatant is aimed at the
target itself rather than at a formation slot around it, so their aim points are equal by
construction and the drift test gives them all the same answer on the same tick.

### 2.4 `GameLogic/World.cpp` — `RecordHostileAct`

```cpp
void RecordHostileAct(ShipHandle _attacker, ShipHandle _victim);
```

`RecordAggression`'s sentence one level down: the server judges, no client message exists or ever
will, and it arrives from outside the tick. A victim that is not a live ship, or that is in no fleet,
is recorded and ignored — a loose NPC has no response of its own until some design gives it one.
Otherwise the victim's fleet takes the attacker as its threat, the victim's **current position** as
the anchor, and a full `FLEET_ALERT_TICKS`.

The attacker's liveness is deliberately **not** checked. Being shot by something that then died is
still being shot: the alert lights, and the posture below finds nothing to pursue and stands down on
its own.

### 2.5 `GameLogic/World.cpp` — the posture, in `StepFleets`

Between the launch and the cruise-and-patience work slice 3 put there, and reading in this order:

1. **Decay.** `alertTicks` counts down one per tick. At zero the threat and its anchor are cleared:
   what is stale is not left lying in the row for the codec to carry.
2. **Engagement** is three things at once — the alert is still burning, the threat still resolves,
   and it is within `FLEET_ENGAGE_RANGE_METRES` of the anchor. Losing any of them stands the fleet
   down, once, and clears the threat.

   The alert being one of the three is a reading design §7 does not quite settle: §7.2 lists the
   other two and §7.3 says "the posture's stand-down reads it". It is the right reading, and design
   §8.2 confirms it by asking for **two** bits — an *engaged* bit and an *under attack* bit, which
   can only differ if the alert outlives the engagement. Without it, one shot from an attacker that
   then parks 900 m from the anchor and does nothing would hold a fleet's combatants out of their
   orders for ever.
3. **The posture.** While engaged, every member whose hull is `combatant` pursues the threat and its
   standing order is suspended for it; every other member carries on exactly as slice 3 left it —
   cruise cap, patience, and no fleeing. Fleeing is a judgment about where safety is, which is a
   sense, and this design has none.
4. **Standing down** re-lowers the fleet's standing order over every member, and that is a
   correction to design §7.2's "the patience of §4.4 re-issues their legs": patience re-issues a
   member to `m_routes[member].destination`, and pursuit **overwrote** that with the target's
   position. Patience alone would send a combatant back to where its quarry used to be. A fleet with
   no standing order stops its combatants instead, which is the honest answer to a fight ending with
   nothing else to do.

### 2.6 `GameLogic/World.cpp` — one pursuit, two masters

The protector's re-aim block becomes `PursueTarget(ShipId _ship, ShipId _target)`, and both the
protector duty and the fleet posture call it. Design §3 says the attack order and the defense "are
that chassis with a different master"; two copies of six lines would make that true in prose and
false in code the first time one of them was retuned.

It clears docking intent where the protector's copy did not have to — a fleet member may be halfway
into a station when the shooting starts — which is a no-op on the protector path, where a pursuing
ship has already had its docking cleared.

### 2.7 `GameLogic/World.cpp` — the ordered attack

`FleetOrderKind::Attack` stops being `Unsupported`:

- `FleetCommand` gains `ShipId target`; `FleetOrderResult` gains `NoSuchTarget`.
- Accepted, it stores `orderTarget` and lowers: **combatants pursue, non-combatants hold** where the
  order found them — `Stop`'s treatment, for them alone.
- **No leash.** An ordered pursuit runs until the target is gone or the order is replaced: it is a
  decision the player made, where the defense is a reaction the fleet had. The target dying or
  docking completes the order and the fleet reverts to `Idle` where it stands.
- The cruise rule does not apply: combatants chase at their own best speed, because a chase capped at
  a Miner's pace would be the rule misfiring, and the non-combatants are held at zero anyway.

An engaged fleet under a standing `Attack` order pursues its **threat**, not its order target, and
returns to the order when it stands down. The defense outranks the standing order for as long as it
lasts; an explicit order outranks the defense, clearing the threat but leaving the alert to burn —
the button should not stop glowing because you gave an order.

### 2.8 The wire and the codec

`FleetOrder` gains `EntityId target`, resolved by the publisher exactly as `station` is.
`WORLD_STATE_FORMAT` goes 4 → 5 for the four new fields on the row; `ReadWorldState` bounds nothing
new — a handle that resolves to nothing is a fleet that stands down on its first tick, which is the
fail-closed direction already.

### 2.9 What this slice does not touch

- **Combat.** Nothing deals damage; `RecordHostileAct` is a socket, and `GameLogicTests` is its only
  caller until slice 6 wires F7 to it.
- **The status block's bits.** Slice 5 states them; this slice defines what they mean.
- **`RecordAggression`** and the protector response's own behavior: an act against a *station* stays
  the Vanguard's business and is untouched.
- **`AGENTS.md` and `README.md`.** Nothing they say becomes false.

---

## 3. What to build on

- **`World::StepProtectors`** — the pursuit being factored out, and the stand-down-and-go-home shape
  the fleet's stand-down mirrors.
- **`World::RecordAggression`** (ADR 0041) — the stated-act shape, the "no client message exists or
  ever will" rule, and the debug key that stands in for combat.
- **`World::LowerFleetOrder`** (slice 3) — what a stand-down calls, and why patience cannot do it.
- **`HullSpec`'s `avoidanceAuthority`** — the precedent for an authored per-hull flag over a derived
  one.
- **`SimTuning.h`'s `PURSUIT_REPLAN_METRES`** — reused, not copied: it measures a pursued target's
  drift, which is the same quantity here.

---

## 4. Acceptance

**`Tests/GameLogicTests/FleetTests.cpp`** — extended.

| Test | Decides |
|---|---|
| `AHostileActRousesTheDefense` | combatants turn on the attacker; the Miner and the Hauler keep flying the fleet's Move order; the alert is full |
| `TheDefenseHoldsItsGround` | an attacker that runs past `FLEET_ENGAGE_RANGE_METRES` of the anchor releases the combatants, which return to the standing order rather than to where the quarry was |
| `TheAlertDecays` | the alert burns for exactly `FLEET_ALERT_TICKS`, a fresh act refills it, and the posture ends with it even if the threat is still standing inside the leash |
| `AnExplicitOrderStandsTheDefenseDown` | any accepted order clears the threat and re-tasks everyone, leaves the alert burning, and a fresh act re-rouses with a new anchor |
| `AnActOnAShipInNoFleetIsIgnored` | a loose ship's misfortune rouses nobody, and no fleet's row is touched |
| `AnOrderedAttackAimsTheCombatants` | Attack: combatants close on the target, non-combatants hold where they were, `NoSuchTarget` for a stale one |
| `AnOrderedAttackHasNoLeash` | the target runs far past the leash and the combatants keep coming |
| `AnOrderedAttackEndsWithItsTarget` | the target despawns: the standing order reverts to `Idle` and the fleet stops |
| `TheDefenseOutranksAStandingAttack` | ordered to attack A, then attacked by B: the combatants take B, and go back to A when they stand down |
| `TheHullTableSaysWhoFights` | every hull's `combatant` flag is what design §6.5 lists, and no immovable hull is one |
| `TheThreatSurvivesTheRoundTrip` | a fleet saved mid-defense reloads engaged, with the same anchor and alert, and resumes the same run |
| `TheSameDefenseProducesTheSameRun` | an act, a pursuit, a stand-down and a resumption in two worlds, compared state-for-state every tick |

**`Tests/GameLogicTests/SnapshotTests.cpp`** — `AFleetOrderRoundTrips` extended over `target`.

**The existing suites**

- Every existing `GameLogicTests` test passes without edits — `ProtectorTests` in particular, which
  is what says the pursuit refactor changed no behavior.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`, clang-tidy clean.
- Debug|x64 builds and all four suites run.
- No screenshot: nothing visual until slice 6.
- One decision record: **a fleet defends itself against stated acts, at fleet grain** — what it
  extends in ADR 0041 and what it deliberately does not add (senses, fleeing, per-member threat).
  Next free number is **0050**.
- `Fleets.md` §16 marks slice 4 *in review*; §7's two readings above are recorded as an amendment.

---

## 5. Assumptions the implementer may make

- **Nothing states an act** but the tests, until slice 6's F7.
- **Friendly fire is not defined here.** A `RecordHostileAct` naming an attacker that is itself a
  member of the victim's fleet would set the fleet chasing its own ship. Nothing can produce one, and
  what it should mean is the combat design's to say, so it is left alone rather than guessed at.
- **One threat per fleet.** A fleet attacked at both ends takes the latest act's attacker and anchor.
  Per-member threats would be eight postures where the design asks for one, and the wire carries one
  bit.
- **A combatant pursues alone**, not in formation: every one is aimed at the target itself, and
  avoidance and separation are what keep them off it and off each other. That is what shadowing is,
  and it is what the protector already does.
- **The alert is not a fight.** It says a member was attacked within the last ten seconds. What a
  fight *is* belongs to the combat design.
