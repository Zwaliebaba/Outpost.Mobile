# Work order — Fleets slice 3: orders at fleet grain

Implements slice 3 of [`Fleets.md`](Fleets.md) §16: `FleetOrderKind`, the `FleetOrder` message,
`IssueFleetOrder` and the lowering onto the per-ship order machinery, the slowest-member cruise
rule, `Stop`, the two reserved kinds, and the patience that keeps a fleet on its standing order
(design §6, §4.4).

**Layer:** `GameLogic` and `GameLogicTests`.
**Depends on:** slice 2 (compose and launch), merged — it is the only way to have a fleet with
members.
**Blocks:** slice 4 (the defense reuses the standing order and the suspend/resume it establishes)
and slice 6 (the client sends this message).

---

## 1. Why this is a slice

This is where the design's title sentence becomes code: an order names a **fleet**, not a list of
ships. Everything before it built something to be ordered; everything after it either reacts to an
order (slice 4's defense suspends and resumes one) or sends one (slice 6). It is also the last
`GameLogic` slice that can be argued about purely as simulation, which is why the decision record
is due here rather than anywhere else.

The lowering is the whole of the risk. `IssueMoveOrder` and `IssueDockOrder` already fly
formations, and this slice must resist the temptation to reimplement any of that: what it adds is
one referent, one gate, one cruise rule and one rule about what happens when a member falls
behind.

---

## 2. Scope

### 2.1 `GameLogic/ShipState.h` — the kind

Beside `OrderState`, and in `ShipState.h` rather than `World.h` because the wire message names it
and `WorldSnapshot.h` does not include `World.h`:

```cpp
enum class FleetOrderKind : std::uint8_t { Idle, Move, Dock, Attack, Stop, Mine };
```

**Declared whole, so the byte never renumbers.** Two of the six are refused for now and each for a
different reason, stated at the definition: `Attack` until slice 4 gives it the pursuit chassis and
a `combatant` flag to aim, and `Mine` until there is a mining design and something in the world to
mine (design §6.6). `Stop` is a kind a *message* carries and never a standing order a row holds:
stopping is asking for `Idle`, and the row stores what it was left in.

### 2.2 `GameLogic/World.h` — the standing order on the row

`Fleet` gains what this slice reads, and nothing else — `orderTarget` is the attack's and arrives
with slice 4:

```cpp
FleetOrderKind orderKind = FleetOrderKind::Idle;
WorldPos orderPoint;                 // Move
float orderFacingRad = 0.0f;         // Move
bool orderHasFacing = false;         // Move
ShipHandle orderStation;             // Dock: the station's structure
```

A handle for the station, for ADR 0005's reason and the docking table's: a standing order that
outlives the station it names must stop resolving rather than name whatever took that index.

The API, beside `ComposeFleet`:

```cpp
enum class FleetOrderResult : std::uint8_t { Ordered, NoSuchFleet, NotAStation, RefusedStanding, Unsupported };

// Everything a fleet order asks for, resolved. The wire names entities and this names ships; the
// publisher is where the two meet (ADR 0047).
struct FleetCommand
{
  FleetOrderKind kind = FleetOrderKind::Idle;
  WorldPos point;                   // Move
  float facingRad = 0.0f;           // Move
  bool hasFacing = false;           // Move
  ShipId station = INVALID_SHIP_ID; // Dock
};

FleetOrderResult IssueFleetOrder(FactionId _issuerFaction, std::uint8_t _slot, const FleetCommand& _command);
```

**The authority gate is one comparison** — the issuer's faction must own a live fleet in that slot —
in place of the per-ship faction filter a ship-list order needs, and it stays in the simulation
(ADR 0014). That is the whole of what "orders name a fleet" buys on the gate side, and it is the
decision record's argument.

### 2.3 `GameLogic/World.cpp` — the lowering

Per kind, and each of them ends in machinery that already exists:

- **Move** → `IssueMoveOrder(members, point, hasFacing, facingRad, owner)`. The standing order is
  stored on the row.
- **Dock** → the station is resolved and gated exactly as `IssueDockOrder` gates it, and its
  refusals become this call's: a row that is not a live station is `NotAStation`, an owner that
  holds the issuer hostile is `RefusedStanding`. Neither changes anything.
- **Stop** → every member goes `Idle`, its docking intent is cleared, and its speed cap is zeroed;
  the row's standing order becomes `Idle`. A brake, not a destination — "order the fleet to where it
  already is" is a formation shuffle and not a stop, which is why this is a kind and not a UI
  nicety.
- **Attack**, **Mine** → `Unsupported`, changing nothing (§2.1).

An accepted order **replaces** the standing order whatever it was, and clears every member's
docking intent through the machinery that already does so (`IssueMoveOrder` clears it; `Stop`
clears it explicitly).

### 2.4 `GameLogic/World.cpp` — the cruise rule, applied every tick

The slowest member's `maxSpeedMetresPerSec` becomes every member's
`orderSpeedCapMetresPerSec`, for a fleet whose standing order is `Move` or `Dock`.

**Applied in the pass rather than once at lowering**, and that is a correction the tree forces
rather than a preference: `StepDockings` re-issues a docking ship's approach every time it goes
Idle and sets `orderSpeedCapMetresPerSec = 0.0f` when it does, so a cap written once at lowering
would be silently dropped by the first re-issue. A rule the pass restates every tick is a property;
a rule written once is a race with whatever else writes that field. It also covers a member that
joined after the order was given, with nothing added.

One float per member per tick, written by the fleet's own pass to the fleet's own ships, before
anything moves.

### 2.5 `GameLogic/World.cpp` — patience, and a late launch under a standing order

Two halves of one sentence: a fleet holds its order until it is given another.

- **Patience.** A member that is `Idle` and further from **its own route destination** than its
  arrival radius stopped short — shoved off by traffic, blocked, re-planned — and its leg is
  re-issued to the destination it already had, not to a re-solved formation. That is the dock pass's
  patience with the same shape, and re-solving instead would reshuffle who is where every time one
  ship was jostled.

  **The route's destination, and never the fleet's order point**, and that is the one line in this
  slice worth a second look. A route whose point the wall forbids ends as close as the geometry
  allows, and `AdvanceRoute` moves the destination to where the ship stands so that it is never
  re-planned back at a point it cannot reach (ADR 0042). Reading `route.destination` inherits that
  for free. Re-deriving the point from the fleet would discard it, and every member of a fleet
  ordered into a wall would be re-planned on every tick for ever — an A* a tick, and invisible from
  outside one, because the ship is set `Moving` at the top of `Step` and put back to `Idle` before
  the end of it.

  A guard against that loop was written first and then deleted: it was dead code, because ADR 0042
  already prevents the loop one layer down. What replaced it is a readout — `World::RoutePlanCount()`,
  in the shape of `GatheredCandidateCount` and for its reason: a planner quietly running every tick
  and one running only when something changed look exactly alike from the outside until somebody
  counts.
- **A late launch joins the order.** Slice 2's launch sends a new hull to its own slot of the rally.
  When the fleet already has a standing order, it must join *that* instead — so the launch re-issues
  the standing order over every member including the one just born, which re-solves the formation
  for the count that is actually out. Design §5.3's "later spawns simply join the formation solved
  for the order", read plainly.

  Re-solving is right here and wrong at the rally, and the difference is worth stating because it
  looks inconsistent: at the rally the fleet is packed against the station's door and a reshuffle
  makes ships cross at close quarters (slice 2 measured 1.0 cm of capsule overlap doing exactly
  that), while under a standing order the fleet is spread out and `IssueMoveOrder`'s slot
  assignment — by where the ships already lie across the formation — is precisely the property
  wanted.

`World` gains one readout with it — `RoutePlanCount()`, a running total incremented in `PlanRoute`,
outside the replay contract and outside the save format the way the neighbour-query counters are.
It is what lets §4's wall test assert "no A* ran" instead of a comment claiming it.

### 2.6 `GameLogic/WorldSnapshot.h`/`.cpp` — the message and the codec

```cpp
struct FleetOrder
{
  std::uint8_t slot = 0;
  FleetOrderKind kind = FleetOrderKind::Idle;
  WorldPos point;
  float facingRad = 0.0f;
  bool hasFacing = false;
  EntityId station = INVALID_ENTITY_ID;
};
[[nodiscard]] bool WriteFleetOrder(const FleetOrder& _order, Neuron::Transport& _transport);
[[nodiscard]] bool ReadFleetOrder(std::span<const std::uint8_t> _datagram, FleetOrder& _outOrder);
```

`KIND_FLEET_ORDER = 5`, on the reliable lane with the other orders (ADR 0029). It carries **no ship
list at all**, which is the whole point: one small fixed-size message whatever the fleet's size, so
`MaxShipsPerOrder` does not apply to it and never will. A slot at or past `FLEET_SLOTS` or a kind
past `Mine` is refused by the reader rather than passed on, because a malformed message is content
and content fails closed (AGENTS.md §5).

`WORLD_STATE_FORMAT` goes 3 → 4 for the standing order on the row: kind, point, facing, hasFacing,
station handle. `ReadWorldState` refuses a kind past `Mine`.

### 2.7 `GameLogic/Publisher.cpp` — the adapter

A third branch beside the two that exist, in the same shape: read, resolve the station's `EntityId`
to a `ShipId`, call `World::IssueFleetOrder` with the subscriber's faction. The subscriber's faction
is what the gate reads, so a client cannot order another faction's slot — the same thing the
per-ship path got from its filter, arrived at with one comparison instead of a loop.

### 2.8 What this slice does not touch

- **The ship-list order messages stay.** Design §16 puts their retirement here, and it cannot be
  here: `Outpost/WorldView.cpp` sends them, nothing sends a `FleetOrder` until slice 6, and a slice
  that deleted them would leave the game with no way to order anything for three slices. They retire
  with the client that sends them; §16's rows for slices 3 and 6 are amended in this commit to say
  so. Nothing about them changes here.
- **`World::IssueMoveOrder` and `IssueDockOrder`** are untouched. They are what a fleet order lowers
  onto, what the NPC passes use, and what every existing test drives.
- **The defense**, `orderTarget`, `HullSpec::combatant`, the threat and the alert: slice 4's. This
  slice never suspends a member's order, which is what slice 4 adds on top of the standing order it
  establishes.
- **`AGENTS.md` and `README.md`.** Nothing they say becomes false: no `Outpost` file changes, and
  nothing sends or issues a fleet order outside the tests.

---

## 3. What to build on

- **`World::IssueMoveOrder`** — the formation solve, the slot assignment by where ships lie, the
  route plan, and the clearing of patrol and docking intent. The Move lowering is one call to it.
- **`World::IssueDockOrder`** — its three gates are the Dock lowering's gates, and its
  `DockOrderResult` maps onto `FleetOrderResult` one to one.
- **`World::StepDockings`** — the patience shape, and the reason §2.4's cap is re-applied per tick.
- **`Movement.cpp`'s `SolveOrder`** — where `orderSpeedCapMetresPerSec` is read, and the guarantee
  that a zero cap is arithmetic that changes nothing.
- **`WriteMoveOrder`/`ReadMoveOrder`** — the message shape, the reserved order-id word, and the
  fail-closed reader.
- **`Publisher::ApplyOrders`** — the two branches this adds a third to, and the id resolution that
  belongs in the adapter and nowhere else.

---

## 4. Acceptance

**`Tests/GameLogicTests/FleetTests.cpp`** — extended.

| Test | Decides |
|---|---|
| `AFleetOrderLowersToAFormation` | Move: every member is under way, arrives about the point in wedge, and the ordered facing is honored |
| `AFleetOrderNamesAFleetNotShips` | an empty slot, a slot past the fifth, and another faction's slot are all `NoSuchFleet` and change nothing |
| `AFleetCruisesAtItsSlowestMember` | every member's cap equals the least `maxSpeedMetresPerSec` present, and it survives the ticks a docking approach re-issues over |
| `AFleetDockOrderCarriesTheDockGates` | `NotAStation` for a plain ship, `RefusedStanding` for a hostile owner, and the fleet dismantles when the accepted one completes |
| `StopHaltsAFleet` | every member Idle, docking intent cleared, the standing order back to `Idle`, and the fleet still holding its slot |
| `TheReservedKindsAreRefused` | `Attack` and `Mine` return `Unsupported` and touch nothing |
| `AFleetIsPatient` | a member shoved off its slot by a barging Carrier and left Idle is re-issued the leg it already had, and the others are not re-planned |
| `AFleetOrderedIntoAWallSettles` | a fleet ordered into the middle of a station settles, and `RoutePlanCount` says no route was planned over the next 300 ticks — the assertion a patience that re-derived its point from the fleet would fail, and the only one that would |
| `ADockOrderAtFleetGrainDismantlesIt` | an accepted Dock at fleet grain flies the members home, refills the ledger and frees the slot |
| `ALateLaunchJoinsTheStandingOrder` | a fleet ordered mid-launch: the hulls still to come fly to the order rather than to the rally |
| `AFleetOrderSurvivesTheWire` | the message round-trips every kind; a slot past `FLEET_SLOTS` and a kind past `Mine` are refused by the reader |
| `TheStandingOrderSurvivesTheRoundTrip` | a fleet saved under a Move order and under a Dock order reloads with both, and resumes the same run |
| `TheSameFleetOrderProducesTheSameRun` | order, launch, patience and a dock in two worlds, compared state-for-state every tick |

**`Tests/GameLogicTests/PublisherTests.cpp`** — one row: `AFleetOrderArrivesThroughTheSeam`, a
subscriber sending a `FleetOrder` over a `LoopbackTransport` and the fleet moving, plus the same
message from a subscriber of another faction changing nothing.

**The existing suites**

- Every existing `GameLogicTests` test passes without edits, including slices 1 and 2's twenty-two.
- The other three suites untouched and green.

**The tree**

- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`, clang-tidy clean.
- Debug|x64 builds and all four suites run; the game runs exactly as before.
- No screenshot: nothing visual until slice 6.
- One decision record: **orders name a fleet, not ships** — what the ship-list message cost, what
  the one-comparison gate buys, and why the two forms are not kept side by side. Next free number is
  **0049**; the index in `Design/Decisions/README.md` lists it.
- `Fleets.md` §16 marks slice 3 *in review* and moves the message retirement to slice 6's row; this
  file moves to `Design/Archive/` in the merge commit.

---

## 5. Assumptions the implementer may make

- **Nothing sends a `FleetOrder`.** The message is exercised by tests only until slice 6.
- **A fleet may be ordered while it is still launching**, and while it is outside every interest
  radius. The server has every position; design §6.4 already accepted that the client's order marker
  is the thing that goes without a formation heading in that case, and no code here cares.
- **A standing order outlives its members.** A fleet whose last member docks retires with its
  standing order still set, because the row is cleared whole.
- **A Dock standing order needs no re-check.** `StepDockings` re-checks standing at capture already
  (Stations §7.3), so an issuer that turns criminal mid-approach is turned away at the door by the
  pass rather than by anything here.
- **Patience does not resurrect an order the world refused.** A member whose route could not reach
  its slot ends up as close as the geometry allows and goes Idle there; `BLOCKED_WAYPOINT_TICKS`
  already guarantees it is never stuck, and patience re-issues to the same point, which is the same
  answer. It is not an attempt to find a better route.
