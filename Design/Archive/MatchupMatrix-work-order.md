# Work order — the matchup matrix test pins the balance

Slice 5 of [`GameDesignPlan.md`](../GameDesignPlan.md), phase 0. One slice, `Tests/GameLogicTests`
only, and independent of every other phase-0 slice: it adds a file to the suite and touches no
library.

The finding it retires is the first half of C13 in [`GameDesignReview.md`](../GameDesignReview.md)
§Combat 13: the instrument that measured combat slice 5 was "a standalone harness" in no file under
`Tests/` or `Tools/`, the window it found is narrow enough that 3,800 versus 4,000 hull points flips
who wins, and the suite pins one pacing assertion, one determinism test and one tracking number
(`CombatTests.cpp`). Items C12, C13 and C15 all move the numbers, and today nothing would notice a
retune that changed a fight's character rather than its degree.

It adds no feature and moves no number. What it adds is the instrument: every combatant hull
against every other, under the shipped tables, with the outcome of each cell written down.

## 1. Scope

1. **`Tests/GameLogicTests/MatchupTests.cpp`** — one test class, three rows:

   - **`TheDuelMatrixIsWhatItWas`**: every ordered pair of the six combatant hulls — Interceptor,
     Bomber, Corvette, Frigate, Battleship, Carrier — one on one. Both hulls are a fleet of one,
     both are ordered to attack the other, and they start 600 m apart, bow to bow, so that every
     range in the device table (420 m at most) is closed under way and a fixed-gun hull has to fly
     its pass. The universe steps until one side has nothing left or three minutes pass. Each cell
     asserts the **outcome** — mine, theirs, both, or stalemate — exactly, and the tick the fight
     ended within **15 percent** of the recorded value. Thirty-six cells, in a table in the file,
     with the numbers this order measured (§7).
   - **`TheGroupRowsAreWhatTheyWere`**: the fights the design's pacing targets name, as groups
     under the same rule — eight Corvettes on an Interceptor from 300 m, two Interceptors and then
     two Bombers on a Frigate from 600 m, and a mixed eight (two Interceptors, two Bombers, two
     Corvettes, two Frigates) on a Battleship and on a Carrier from 800 m — asserting the outcome,
     the survivors on each side, and the end tick within 15 percent.
   - **`TheMatrixIsDeterministic`**: one cell run twice in two universes, compared state for state
     on every tick, as `TheSameBattleProducesTheSameRun` already does for one fight; here so the
     matrix's claim to exactness is pinned beside the matrix.

   On any failure the row writes the whole measured matrix into the test log, one line per cell,
   so a retune reads what it did from CI rather than reconstructing it.

2. **The band is the contract.** Fifteen percent is wide enough to absorb the compiler this order
   was measured with (§7) and narrow enough that a retune which moves a fight by a fifth of its
   length is a retune somebody has to look at. A cell whose outcome flips fails regardless of time.

3. **`GameLogicTests.vcxproj` and `.filters`** gain the file; `python Build/CheckProjectFiles.py`
   agrees.

4. **`Design/Archive/Combat.md` §13** gains one paragraph, amended in place per ADR 0054: the matrix exists,
   where it is, and that its geometry differs from slice 5's harness (§7) so its numbers are not
   that harness's numbers.

## 2. Out of scope

- **Any number.** No hull row, no device row, no tuning constant moves. The matrix records what
  they do.
- **The intended counter graph.** C13's retune — size classes, effectiveness, engage range, arcs,
  speeds, the Battleship's light turrets, a Carrier with a job — is phase 4, slice 24, and it is
  what this matrix is for. The expected matrix here is the shipped one, not the intended one.
- **Reconciling with combat slice 5's measurements.** That harness is not in the tree and its
  geometry is not recorded; where this instrument disagrees with it, §7 says so and neither is
  corrected.
- **NPC helms, senses, formations under attack.** Every fight here is orders on both sides; how a
  hostile *chooses* to fight is Combat.md §14's and the review's C11.

## 3. What to build on

- `Tests/GameLogicTests/CombatTests.cpp`'s helpers — `Spawn`, `FleetOfOne`, `OrderAttack`,
  `TicksUntilALoss`, `FacingPair` — and its `TheSameBattleProducesTheSameRun` for the shape of a
  per-tick comparison. They are file-local; the new file carries its own, since the suite has no
  shared header beyond `pch.h` and a helper header would be a third place to keep them.
- `Universe::FormFleet`, `IssueFleetOrder` with `FleetOrderKind::Attack`, `ShipCount`, `Ships()`
  and `ShipState::factionId` for counting survivors by side.
- `HULL_SPECS` `fights` column for the six combatants; the Miner and Hauler are unarmed and are
  not in the matrix.

## 4. How it must behave

1. Every cell is deterministic: the same cell run twice gives the same tick.
2. A cell's outcome is one of four and is asserted exactly.
3. A cell's end tick is asserted within the band; a stalemate has no tick to assert.
4. A failing row logs every cell it measured before it fails.
5. The three rows together run in well under a minute on the CI runner in Debug|x64.

## 5. Acceptance

- `MatchupTests` green on CI with the table as measured here, or with the table corrected to
  what MSVC measured and the correction stated in §8 — the band exists for exactly that.
- Every existing `CombatTests` row unchanged and green; `AFighterDiesUnderAPeersGun` is still the
  pacing pin and this slice does not restate it.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- `Design/Archive/Combat.md` §13 names the matrix.
- No decision record: a test that pins existing behaviour decides nothing.

## 6. Assumptions the implementer may make

- **The geometry is this order's.** Six hundred metres bow to bow, fleets of one, orders both
  ways. Slice 5's harness used another, unrecorded geometry; its numbers (a fighter under focus in
  0.83 s, two Bombers killing a Frigate in 17.3 s, a mixed eight killing a Battleship in 81.6 s)
  are not reproduced here and are not expected to be.
- **The mixed eight is two of each armed non-capital hull.** The design says "a mixed eight-fleet"
  and no more.
- **Three minutes is the limit.** The longest decided cell (Battleship against Carrier) ends
  inside it; a cell that reaches it is a stalemate, and a retune that turns a stalemate into a
  three-minute kill fails on the outcome, which is right.

---

## 7. What changed on contact, and what the matrix found

**The instrument disagrees with slice 5's harness, and the order says which one is in the tree.**
Under this geometry — 600 m bow to bow, fleets of one, attack orders both ways — the shipped
tables do the following, and every cell below is what `MatchupTests.cpp` pins:

| mine \ theirs | Interceptor | Bomber | Corvette | Frigate | Battleship | Carrier |
|---|---|---|---|---|---|---|
| Interceptor | stalemate | loses 6.2 s | loses 11.7 s | loses 9.5 s | loses 8.8 s | loses 11.8 s |
| Bomber | wins 6.2 s | stalemate | loses 20.2 s | loses 13.0 s | loses 11.0 s | loses 16.4 s |
| Corvette | wins 12.2 s | wins 20.2 s | both 42.9 s | loses 17.0 s | loses 12.9 s | loses 20.8 s |
| Frigate | wins 9.5 s | wins 13.0 s | wins 17.0 s | both 48.5 s | loses 23.0 s | loses 30.0 s |
| Battleship | wins 8.8 s | wins 11.0 s | wins 12.9 s | wins 23.0 s | both 120.5 s | wins 169.9 s |
| Carrier | wins 11.8 s | wins 16.4 s | wins 20.9 s | wins 30.0 s | loses 154.1 s | stalemate |

The Carrier's two rows against the Battleship are not a mirror of each other: the hull named first
spawns at the origin and is ordered first, so the two cells are different fights.

And the groups: eight Corvettes delete an Interceptor in 4.2 s from 300 m with no loss; two
Interceptors lose to a Frigate in 12.1 s and two Bombers lose to one in 24.9 s; a mixed eight of
two each loses to a Battleship in 75.6 s with nothing left, and beats a Carrier in 91.8 s with
four left.

Three of those are worth a sentence each, because they are the cells a designer will want to
change and the review's C13 already names them. **Two fixed-gun hulls under mutual attack orders
never land a hit on each other**: an Interceptor chases an Interceptor, and a Bomber a Bomber, for
three minutes without a shot — the pursuit aims at the target, both fly at each other, pass, and
turn, and neither gets its bow arc on the other. `AFighterDiesUnderAPeersGun` measures 9.5 s
because its two fighters *face* each other and nobody moves. **Two Carriers under mutual orders
never fire either**: each holds a stand-off from a target that is itself holding a stand-off, and
the two light-turret ranges never overlap. **The mixed eight loses to the Battleship** here,
where slice 5 measured the fleet winning in 81.6 s; the composition, the spacing and whether the
Battleship was ordered back are all unrecorded for that harness, and this order does not guess
them. The Battleship's 3,800 points stand between "the fleet wins" and "the Battleship wins" by
geometry as much as by number, which is the narrowness slice 5 warned about, now with a second
data point.

**The table in the tree is CI's, and the numbers above are too.** They were drafted on Linux with
clang, from the same `GameLogic` the suite links, behind the shim slice 1 used, and then corrected
to what MSVC measured -- the same authority slice 1's fixture answers to, and for the same reason:
MSVC is the only compiler this tree builds with. Three of the forty-one cells differed between the
two toolchains, all by ulp-level drift compounded over a long fight, and none of them changed who
won:

| cell | clang | MSVC | apart |
|---|---|---|---|
| Battleship against Carrier | 9,240 ticks | 10,193 ticks | +10.3% |
| a mixed eight against a Carrier | 5,510 ticks, five left | 5,613 ticks, four left | +1.9%, one ship |
| Carrier against Battleship | 9,241 ticks | 9,242 ticks | +0.01% |

**Fifteen percent is thinner headroom than it looks.** Within MSVC the tree's determinism contract
makes every cell exact, so the band absorbs nothing on a normal run and exists only for a runner
image that brings a different build of the compiler. The Battleship-against-Carrier drift above was
10.3% of it, which is most of what there is, and that cell is the one to read first if this file
ever goes red on a day nobody touched a number.

**The row logs every cell whether or not it fails**, rather than only on failure as §1 said: a
green run's log is then the matrix as CI measured it, which is the number a designer wants and
the number this table would otherwise be the only record of.

## 8. What was verified, and how — and the honest gap

**Compiled and run here:** the harness that produced §7, against the changed tree, in 1.8 s at
`-O1`. `MatchupTests.cpp` compiles under clang behind a stub of the test framework, which catches
a typo and proves nothing about the framework. `python Build/CheckFormat.py` and
`python Build/CheckProjectFiles.py` pass.

**Run by CI (Debug|x64, run 233 on the pull request, 2026-09-02): 548 of 549 tests passed.** All
thirty-six duel cells held their outcome, thirty-four held their tick unchanged, and the two that
drifted stayed inside the band. Four of the five group rows held exactly. The one failure was
`TheGroupRowsAreWhatTheyWere` on the mixed eight against a Carrier, which came back with four
survivors where the draft had five -- the marginal row named above, failing on the survivor count
rather than on the outcome or the band, which is the instrument reporting the one place it is
fragile. The table was corrected to CI's numbers in the commit after this slice's. That is twice in
two slices that the CI log was the instrument the tree needed: slice 1's fixture came back the same
way, and both rows were built to hand back the right answer rather than only to fail.
