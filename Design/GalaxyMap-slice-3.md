# Work order — Galaxy map slice 3: tap to go

Implements slice 3 of [`GalaxyMap.md`](GalaxyMap.md) §7 — the gate-graph search, the multi-hop fleet
order, and the tick that advances it.

**Layer:** `GameLogic` + `Outpost`.
**Depends on:** slices 1 and 2, both landed 2026-09-02.
**Blocks:** nothing. Slice 4 names the systems this one flies between.

---

## 1. The hole in the design, and what filled it

§4.3 said the tick "finds the next hop's gate in the new system". Nothing in the tree could do that.

`Universe` held gates as `{structure, destination EntityId, ownerFaction}` and had no notion of a
system at all — `GalaxyLayout.h` opened by saying so. The client lays out a galaxy
(`OutpostApp::m_galaxy`); `ShardApp` read the seed and laid out nothing; `Publisher`, which is where
a `FleetOrder` becomes an `IssueFleetOrder` call, holds neither. So "plan a route on the order and
resolve each hop to a gate" had no home, and the design's pre-committed decision — a route lives on
the fleet, because a ship does not survive its own jump — did not cover it.

The owner chose the seam on 2026-09-04: **the universe is given the galaxy**.
[ADR 0069](Decisions/0069-a-voyage-lives-on-the-fleet-and-is-planned-from-where-it-is.md) is that
decision and the three alternatives it beat.

## 2. Scope

1. **`Game::RouteAcrossGates`** in `GalaxyLayout` — breadth-first over `GateLink`, from one system to
   another, writing the hops into a caller's span. Deterministic by walking the link list in its own
   order, which `LinkGates` already fixes as ascending pairs. Zero for every way there is no route,
   including a route too long for the span: fail-closed, because half a route delivered whole is a
   fleet flown to the wrong system and told it arrived. `MaxRouteHops` beside it, because a shortest
   path visits no system twice and a caller sizing a buffer should not have to know that.

2. **`Universe::ConfigureGalaxy`** — the layout, from the composition root, beside `ConfigureShard`
   and on its terms. Not saved: it is derived from the seed the header carries, and a saved copy
   could disagree with its own file (ADR 0057). A universe never given one refuses every voyage.

3. **`FleetOrderKind::Voyage`**, appended for Jump's reason — the wire reads this enum as a byte and
   a value that moved would make one build's Dock another's Attack. A standing order, not a message
   kind: the row holds `Voyage` for the whole crossing, `orderPoint` is where it is going and
   `orderGate` is the door it is at now. Both fields already exist and are already saved.

4. **`Universe::StepVoyages`**, immediately after `StepJumps` so a fleet that crossed this tick is
   given its next door on the same tick rather than one later — fourteen gates makes a tick of
   hesitation per door visible. `StepJumps` clears a voyaging fleet's *gate* where it clears a
   Jump's *kind*, which is the whole of what makes an order survive its own hops.

5. **`Universe::GateBetween`** — the door in one system that comes out in another, by asking the
   layout where both ends stand. Exact, and no tolerance constant.

6. **`UniverseView::IssueVoyageOrder`** and the map's second meaning: with a fleet selected a tap
   orders it there, with nothing selected it flies the camera (slice 2, unchanged). The order names
   the star's **position**, not a system index, so the two halves never have to agree about an
   ordering of an array.

7. **`VOYAGING` in the fleet bar**, off the status byte the block was widened to hold.

8. **Prose in the same commit**: ADR 0069, `GalaxyMap.md` §4.3, §6.4, §6.5, §7 and §8 amended in
   place (ADR 0054), `GalaxyLayout.h`'s opening claim, and `AGENTS.md`.

## 3. Out of scope

- **System names.** Slice 4. A voyage names a place and never a name.
- **Drawing the route on the map.** No line, no highlight, no estimate of arrival. The map draws the
  graph and the fleets, as it did; what says the order landed is the status block.
- **Ordering anything but a move.** No "attack that system", no "dock there on arrival". What a
  fleet does when it arrives is the orders that already exist (§5).
- **Crossing a shard boundary.** A fleet row does not travel in a handoff, so a voyage stands down at
  the border rather than stepping through it. Named in ADR 0069 as the day the gate row wants the
  cell it leads to.
- **Anything on the wire's shape.** `FleetOrder` is the same message with a new value in a byte it
  already carried.

## 4. How it must behave

1. One order crosses every gate on the way, and the fleet arrives whole under the identities it
   left with (ADR 0056).
2. A fleet under a voyage stands in the systems of a shortest route, in order, and in no others.
3. A voyage to the system the fleet is already in is an arrival: accepted, and the row left Idle.
4. A destination with no route is refused with `NoRoute` and **changes nothing**, which is why the
   first hop is planned before the row is touched.
5. A voyage that loses its road stands down with the fleet whole and in a system — never mid-crossing
   and never waiting at a door that leads nowhere. This is where a voyage parts company with a Jump,
   which waits on purpose (§7).
6. An explicit order replaces a voyage completely, leaving nothing behind in the row.
7. A voyage survives its own save file, because there is nothing to save: the row carries the
   destination and the next hop is planned from where the fleet is.
8. Nothing in the tick reads the galaxy. `Step` is unchanged for every fleet not holding a voyage.

## 5. Acceptance

- `GameLogicTests`: `VoyageTests` for §4's rules and `GalaxyLayoutTests` for the search —
  every ordered pair of the shipped galaxy reachable, every hop a real link, shortest and
  length-symmetric, and every refusal refused whole.
- `UniverseStateTests`: `UniverseFormat9.sav` committed with its `FIXTURES` row, per ADR 0061 —
  `UniverseGen 0`'s output at this branch's base commit.
- The whole suite set green; `CheckProjectFiles.py`, `CheckFormat.py`, `CheckViewAccess.py`.
- **Screenshots**, which this design now owes four of. Recorded as owed if the environment still
  cannot produce one.
- A decision record, which this slice is the one in the design that has: ADR 0069.

## 6. Assumptions the implementer may make

- **The galaxy does not change while a fleet is flying.** Gates cannot be built or destroyed
  (`GalaxyMap.md` §8), so the layout the order was given is the layout the arrival is planned
  against. Planning from the fleet's position rather than from a stored plan is what makes this an
  assumption that costs nothing when it expires.
- **A fleet is in one system.** It crosses whole and travels in formation, so the first live member's
  nearest star is the fleet's system.
- **A voyage is slow.** Seven kilometres of flying per system at a cruise speed, times fourteen at
  the galaxy's diameter. That is the design's intent (§8) and nothing here shortens it.

---

## 7. What changed on contact, and what was found

- **The route is not stored, and §6.4 is amended rather than obeyed.** The design said "planned once,
  at the order, not re-planned per hop". Storing hops needs a fixed array on the fleet row and
  therefore a cap; the shipped galaxy's diameter is **fourteen gates**, measured, which is close
  enough to any cap worth writing that a differently-seeded galaxy would reach it — and a destination
  a player can see and cannot be sent to is worse than the work saved. Re-planning is a search over
  68 links at human cadence, it cannot go stale, and the design's own reason for the rule (the graph
  is a pure function of the layout) is exactly why the two answers agree. ADR 0069 has the argument;
  §6.4 now says what was built.
- **A voyage does not wait at a door that leads nowhere, where a Jump does — and that is a rule the
  first draft got wrong.** `StepJumps` holds a fleet at the near gate when the far side does not
  resolve, deliberately: losing a fleet into a gate that leads nowhere is the failure that pass must
  not have. Under a voyage that is a fleet parked at a broken door for ever, which contradicts §6.5.
  `VoyageTests::AVoyageStandsDownWhenItsRoadIsGone` failed on exactly this, and the fix is that a
  voyaging fleet releases the door instead of standing at it: `StepVoyages` then finds another way or
  stands it down where it is. The test was written from §6.5 before the code was, which is why it
  caught it.
- **A tap on the map orders or looks, never both.** §4.3 says "Go" where §4.2 says "Go and look", and
  the naming is the decision: a camera that flew ahead of a fleet ordered across fourteen gates would
  leave the player watching an empty system for minutes while the thing they ordered is behind them.
  With a fleet selected the camera stays; with nothing selected slice 2's flight is unchanged.
- **`orderPoint` carries the destination rather than a new field.** A voyage is a Move whose road runs
  through doors, and it aims at a point exactly as a Move does. The field's comment now says
  `// Move, Voyage`; nothing else about the row moved.
- **The order kind is `Voyage` and not `Travel`,** because `Universe.cpp` already calls
  `Move || Dock || Jump` "an explicit travel order" in two places whose meaning the new kind joins.
  A kind named `Travel` beside a predicate named `travelling` that does not mean it is a comment
  trap.
- **The state format moved to 10 with no field added.** Judged a format change anyway: a format-9
  file written by this build can carry an order byte a format-9 reader refuses, and a stamp that
  names two shapes makes every gate under it a guess. The fixture the rule demands is committed with
  it (§8).

## 8. What was verified, and how

**The whole of `GameLogicTests` runs in this container.** That is new, it is the finding with the
longest reach in this slice, and it was not expected: `GameLogic` names exactly four things from
DirectXMath (`XM_PI`, `XM_2PI`, `XM_PIDIV2`, `XMFLOAT2`, plus `XMScalarModAngle`) and five from
NeuronCore (`Pcg32`, `Transport`, `LoopbackTransport`, `Ease`, `FileSys`), so it compiles under
clang 18.1.3 on Linux behind a shim for those and nothing else.

It is not a claim about compiler-independence in general — it is a measurement:

```
UniverseGen 0's output at 21786e1^, rebuilt here from that commit's own sources
  126,906 bytes, byte-for-byte identical to the committed UniverseFormat8.sav
  census identical to its FIXTURES row: 54 systems, 136 gates, 165 stations, 307 ships, 1 fleet
```

Two compilers, two C runtimes, two machines, and the same save file to the byte. That is what makes
`UniverseFormat9.sav` in this commit a real fixture rather than a hand-made one: it is
`WriteShippedUniverse()` at this branch's base, produced by the same method, and its row records the
census the code counted — 54 systems, 136 gates, **55 stations, 197 ships**, 1 fleet, 80,502 bytes.
The station count drops by 110 from format 8's because the station rule changed to one per system
between them, which is a change to the universe and not to the format.

The suite, before and after, built with `-D_GLIBCXX_ASSERTIONS` so every `operator[]` is
bounds-checked:

```
359 rows | 358 pass | 1 fail: MatchupTests::TheGroupRowsAreWhatTheyWere
```

That one row fails **identically at this branch's base commit**, with no change of mine in the tree.
It records a combat matrix to two decimal places over thousands of ticks of gunnery, and glibc's
`sin`/`cos`/`atan2` differ from the MSVC CRT's in the last bits — which is the one place a
cross-compiler run of this suite cannot agree. It is not a regression and it says nothing about
MSVC's own result; CI is what judges that row.

The search, run over the shipped galaxy:

```
54 systems, 68 links | 2,862 ordered pairs, 0 unreachable, 0 hops that are not links
diameter 14 gates (36 -> 46) | mean 5.71 | degree 1..4 | 0 pairs of unequal length either way round
a span one hop short of the longest route returns 0 and writes nothing
```

**One of the nine `VoyageTests` rows is there because it caught a defect this slice was about to
introduce.** `StepVoyages` is the first thing in the tree to lower a fleet order from *inside* a
tick: every previous caller ran between ticks or after `StepFleets` had compacted the row, so
`LowerFleetOrder`'s Jump branch could read "the fleet's position" off `members[0]` and get a live
ship by accident. It cannot here — the prune is four passes further down the same tick — so a member
killed on the tick before a crossing would have indexed `m_ships` with `INVALID_SHIP_ID`.
`AVoyageOutlivesAMemberLostOnACrossing` times a loss into exactly that tick, and against the code as
first written it trips `vector::operator[]`'s bounds assertion. The fix is one line, reading the
first live member off the scratch the function already built, and it closes the same hazard for the
Jump path that has always had it.

The nine `VoyageTests` rows run against a hand-built four-system chain rather than the shipped
galaxy, for the reason §1 of the suite gives: the shipped diameter is fourteen gates and several
thousand metres of flying each, which is a fine thing to measure and a terrible thing to assert
against. `AVoyageVisitsEverySystemOnTheRouteInOrder` is the row that watches every hop.

`CheckProjectFiles.py`, `CheckFormat.py` (clang-format 18.1.3) and `CheckViewAccess.py` all pass.
`clang-tidy` 18.1.3 under this repository's own `.clang-tidy` reports nothing on `GalaxyLayout.cpp`,
`Universe.cpp` or `UniverseSnapshot.cpp` — which is the gating step for `GameLogic` in CI, run here
at a different LLVM version than the runner's, so it is a strong signal and not the verdict.

**The gaps, and they are the same two this design has owed since slice 1:**

- **No screenshot.** There is no MSVC, no D3D12 and no window here. This design now owes four.
- **`Outpost` is not compiled.** `UniverseView.cpp` and `OutpostApp.cpp` reach the graphics stack
  through `NeuronClient`, so nothing here builds them; `CheckViewAccess.py` is what stands in for the
  composition root, and it passes. The client half of this slice is three call sites and a `switch`
  arm, which is the smallest it could have been made, and CI is what proves it.
