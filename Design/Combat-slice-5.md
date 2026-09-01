# Work order — Combat slice 5: the measurement

Implements slice 5 of [`Combat.md`](Combat.md) §16: the hand-back that measures the shipped numbers
against §13's five pacing targets, and the retune that follows from it.

**Status: landed 2026-09-01 and in review, and it is the slice most changed by doing it.** Two
numbers moved, two of the five targets turned out to be unreachable as stated, and the two content
items the design listed beside the measurement are deferred with their reason (§3).

**Layer:** `GameLogic` tuning tables, and this document.
**Depends on:** slices 1 and 2, for a fire pass to measure and a battle to watch.
**Blocks:** nothing.

---

## 1. Why this is a slice

Every number in `DEVICE_SPECS` and every hull-point figure in `HULL_SPECS` was written to serve five
sentences in design §13 and none of them had ever been run. The design said so in as many words: the
Battleship's points are "short of the 1.5 min target, so either its points rise toward 6000 or the
target softens, and **that decision is taken by playing it**, slice 5's hand-back".

This is that hand-back. It is small in diff and it is the slice that decides whether the feature is
any good.

---

## 2. What was measured, and what moved

Every row below is a real `World` stepping the real fire pass to a real death — not arithmetic over
the table. The instrument is a standalone harness; the numbers are its output.

### 2.1 The five targets

| Target | Stated | Measured, as shipped | After |
|---|---|---|---|
| a fighter under one peer's gun | 10 s | **9.5 s** | 9.5 s |
| a fighter under a fleet's focus | 1.5 s | **0.83 s** | 0.83 s |
| a Frigate under two fighters | 40 s | **it survives** | it survives |
| a Battleship under a mixed eight | 90 s | **45 s** | **81.6 s** |
| every gun inside the leash | < 1000 m | 420 m | 420 m |

**Two numbers moved, and only two:**

- `HeavyTurret` damage **70 → 40**.
- Battleship `maxHullPoints` **2400 → 3800**.

**Why those two and not the Battleship's hull points alone**, which is what the design expected. The
matchup turned out to be bimodal rather than gradual: at 70 damage the Battleship puts out 65.8 a
second, which kills an Interceptor in 0.9 s and a Corvette in 3.6 s, so the fleet loses its own
damage faster than it can spend it. Raising hull points alone walks straight past the target — 2,400
gives 45 s, 3,600 and 4,400 both let the Battleship kill all eight and stand. There is no hull-point
value that buys a long fight, because the fight's length is set by how fast the *fleet* dies.
Lowering the capital's output is what opens the middle: at 40 damage and 3,800 points the fleet
grinds it down in **81.6 s** while losing ships doing it, which is what "a capital is an event, not a
target" was supposed to mean. The window is still narrow — 4,000 points and the Battleship wins — and
that narrowness is recorded here because it is the thing a later retune will trip over.

Lowering the heavy turret's *traverse* was tried first and rejected by measurement: 18°/s → 8°/s
moved the fight from 45 s to 40 s and nothing else. The heavies are not killing fighters — a
stand-off holds the fleet at ranges where an 18°/s turret tracks a closing ship easily — they are
killing the Corvettes and Frigates that carry the fleet's damage. Design §4's tactical claim about
crossing fighters is intact; it simply was not the mechanism here.

### 2.2 Two targets that cannot be met as stated, and are not defects

**A fighter under a fleet's focus: 1.5 s stated, 0.83 s measured, and the two cannot both hold.**
Target one fixes a fighter's hull points at ten times one peer's damage; eight Corvettes out-damage
one Interceptor by more than six to one, so the same fighter cannot also take a second and a half of
that. To reach 1.5 s a Corvette would have to do less damage than an Interceptor. Focus fire deleting
a fighter in under a second *is* the intent — the target's number was aspirational and the ratio
between the two is what should be read as agreed.

**A Frigate under two fighters: 40 s stated, and the Frigate wins.** The throughput is right — two
LightGuns need 43 s to chew through 520 points — but a Frigate returns 24 damage a second and kills
each 60-point fighter in 2.5 s. Two Interceptors cannot harass a Frigate; they die first. The
design's own sentence beside the target says "fighters harass capitals, **Bombers kill them**", and
that half holds exactly: two Bombers take a Frigate down in 17.3 s. The harassment half does not
survive contact, and making it true would mean a fighter that can trade with a Frigate, which breaks
target one. Recorded as a corrected target rather than a number to chase.

### 2.3 What did not move

Nothing else. The Interceptor, Bomber, Corvette, Miner, Frigate, Hauler and Carrier rows are as
shipped, and the four device rows other than `HeavyTurret` are untouched. A tuning pass that rewrote
the table would have been a tuning pass nobody could review.

---

## 3. What the design listed here and this slice does not do

Design §16 lists two content items beside the measurement. Both are deferred, and the reason is the
same in each case: slice 3 found the shipped hulls carry no rig, which changed what a `Gun` marker
would be *for*.

**Authoring `Gun` markers.** A marker's value is a muzzle position and a direction. The position is
already in the art, exactly, as the bind-pose centre of the weapon submesh slice 3 exposed —
`battleship_turret_0` pivots at (0.00, 13.19, 19.50) — and the direction is already in `HullSpec`'s
mount bearing. Authoring markers now would put a third copy of both into the content, before the
thing that would read them exists: slice 4 draws its muzzles from the hull origin, and the binding
that would pick a market for mount N is slice 6's. Authored with slice 6, one table reads them the
day they exist; authored now, they are a third number to drift.

**The mount-versus-marker consistency check.** It needs a place that can see both the simulation's
mount table and the game's art, and the tree has none: `NeuronClientTests` deploys the golden fixture
only, and giving the engine's suite the game's meshes points a dependency AGENTS.md §2 keeps shut;
`Tools/` sees the art but not a C++ `constexpr` table. `Tools/NmoShippedArtTest.py` (slice 3) checks
everything about the art that can be checked without the table. The cross-check wants either a small
generated table or a check that parses `HullSpec.h`, and choosing between those is slice 6's problem,
where the binding lives.

---

## 4. Acceptance

- The five targets, measured, above. Four hold; the two in §2.2 are corrected in the design.
- All 265 `GameLogicTests` methods green under `-D_GLIBCXX_ASSERTIONS` after the retune. The rows
  that assert pacing — `AFighterDiesUnderAPeersGun` — are on the untouched fighter numbers and did
  not move.
- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`.
- Design §13's table and its sanity paragraph carry the shipped numbers and the corrected targets.
- No decision record: two tuning constants moved inside a design that already argued for them.
