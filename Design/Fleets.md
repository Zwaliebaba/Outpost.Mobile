# Fleets — composition at a station, five slots, and command at fleet grain

**Status: agreed with the owner on 2026-08-31. All eight slices are written and in review** — the
table, compose and launch, orders at fleet grain, the defense, the fleet wire, the fleet bar,
assembly and the sheet — with work orders [1](Fleets-slice-1.md), [2](Fleets-slice-2.md),
[3](Fleets-slice-3.md), [4](Fleets-slice-4.md), [5](Fleets-slice-5.md), [6](Fleets-slice-6.md),
[7](Fleets-slice-7.md) and [8](Fleets-slice-8.md). **This design moves to `Design/Archive/` in the
commit that marks the slices landed, which is after the pull request merges** — `Design/` holds what
is unfinished, and nothing has merged yet (Design/README.md). Four decisions were put to the owner
and taken (§15); each was the recommended option. §16 lists the slices and the dependencies between
them.

This document designs the game's unit of command: the **fleet**. A player holds at most **five
active fleets** and each fleet holds at most **eight ships**, of any mix of hulls. A fleet is
composed **at a station, from the ships docked there**, launches into space as one thing, takes
orders as one thing — move, dock, attack, and one day mine — and **defends itself** when something
attacks it. Docking dismantles it: the ships return to the station's ledger and the slot frees.
The five HUD buttons that today hold control groups become the five fleet buttons — tap to select
and fly the camera to the fleet, hold to open its sheet, and a fleet under attack glows red.

This is the design that turns the informal control model into the real one. Today the player flies
loose hulls, selection is any subset, and a control group is a client-side memory of one
(`WorldView::CONTROL_GROUPS`). After this design, **every undocked player ship belongs to a
fleet** (§15, owner decision 1): selection takes hold of fleets, orders name a fleet rather than a
list of ships, and the five slots are the whole of the player's command surface. It is also the
design that brings **undocking** — launching a fleet is the first path a ship takes out of a
ledger, the door `Design/Archive/Stations.md` §14 left closed on purpose — and the first
**client-to-server request**, because a composition screen has to ask what is docked before it can
offer it.

Two of the standard commands outrun the tree, and the design is honest about both the way Stations
was about combat. **There is still no combat**: *attack* therefore lands as pursuit and shadowing
on the protector chassis (ADR 0041), and *defense* as the same reaction pointed the other way,
both driven by a stated hostile act — a socket the combat design will call, and one debug key can
exercise today. **There is still no economy**: *mine* needs resource sites the simulation can see,
which ADR 0016's presentation-only rocks are not, so the order kind is reserved on the wire and
the mining design owns everything behind it (§6.6, §14).

---

## 1. What is being built

- **A fleet table in `World`** (§4): at most `FLEET_SLOTS` (5) live fleets per faction, at most
  `MAX_FLEET_SHIPS` (8) members each, members held as `ShipHandle`s. Simulation state, because the
  caps and the composition gates must be authoritative (ADR 0014), because the defense is standing
  behavior inside the tick (ADR 0015), and because a fleet keeps acting beyond every interest
  radius, exactly as a protector does.
- **Composition and launch** (§5): the client asks a station for its ledger (the first
  request/reply on the wire), picks up to eight docked ships and a free slot, and the station
  launches them on a metronome — one hull per `FLEET_LAUNCH_EVERY_TICKS`, spawned at the skin,
  rallying into formation. Undocking exists after this design, and only as this.
- **Orders at fleet grain** (§6): one reliable-lane `FleetOrder` naming a slot and a kind — Move,
  Dock, Attack, Stop, with Mine reserved — lowered inside `World` onto the per-ship order
  machinery that already flies formations. The ship-list order messages retire with it.
- **The cruise rule** (§6.3): a fleet travels at its slowest member's speed, through the per-order
  speed cap patrols already cruise on (owner decision 3).
- **The defense** (§7): `RecordHostileAct(attacker, victim)` — the combat design's trigger, F7
  today — rouses the victim's fleet: hulls whose spec says `combatant` pursue and shadow the
  attacker while it stays within `FLEET_ENGAGE_RANGE_METRES` of where the act was stated;
  everyone else keeps doing what it was told. The alert lights the wire flag for
  `FLEET_ALERT_TICKS` and the button glows red off it.
- **The wire** (§8): a reliable **roster** message per fleet change (which entities are whose), and
  a per-update **status block** in the interest header (where each fleet is, coarse; what it is
  doing; whether it is under attack) — so the buttons, the glow, the minimap and the camera jump
  tell the truth about fleets the player is not even looking at.
- **The client** (§9): the five group buttons become the five fleet buttons — tap selects and
  flies the camera to the fleet, hold opens the fleet sheet (status, members, and the command row,
  owner decision 4), red glow on the alert bit. Tap and band selection take hold of fleets. A
  station gains its first screen: the fleet assembly view, on the long-press Stations §14 left
  "with nowhere yet to go".
- **The starting scene** (§5.5): the three starting hulls boot as **Fleet 1**, so the game opens
  in the model it now keeps.

---

## 2. What the tree already guarantees

Constraints, not preferences; a proposal below that broke one would be wrong however well it
flies.

| Constraint | Where it comes from |
|---|---|
| `GameLogic` depends on `NeuronCore` and nothing else; the engine never names the game | AGENTS.md §2 |
| No wall clock, no OS entropy, no iteration order that is not dense-array order | AGENTS.md §5, ADR 0012 |
| Two identical runs are bit-identical; a saved world replays to byte equality | `GameLogicTests`, `WorldStateTests` |
| Every field `Step` reads is carried by the state codec or rebuilt by `ReadWorldState` | AGENTS.md §8, `WorldState` work order |
| Stored cross-tick references are `ShipHandle`; the wire names `EntityId` | ADR 0005, ADR 0047 |
| Command authority is gated in the simulation, not the adapter | ADR 0014 |
| NPC and standing behavior lives in `GameLogic`, inside the tick | ADR 0015, Hostiles §5 |
| Orders and departures take the reliable lane; positions heal as datagrams | ADR 0029 |
| A departure is stated with a cause, never inferred from absence | Hostiles §4.4, ADR 0040 |
| The snapshot record is a reviewable list and exists to withhold intent | ADR 0009, `WorldSnapshot.h` |
| Reactions start from stated acts, not senses | ADR 0041 |
| Presentation state does not live in the simulation; bodies are presentation | AGENTS.md §5, ADR 0016 |
| Contract tuning in `SimTuning.h`/`HullSpec.h`; look-and-feel in `ViewTuning.h`; content passed in by the root | AGENTS.md §5, Hostiles §5.1 |

---

## 3. What needs no new design

More of a fleet exists than not, which is what keeps the slices small:

- **Flying together is solved.** `IssueMoveOrder` takes a span of ships, solves wedge slots
  spaced off the largest hull, plans routes, and the collision design flies them there without
  anything passing through anything. A fleet order *lowers onto this*; nothing about steering,
  avoidance, separation or arrival changes.
- **The cruise cap exists.** `ShipState::orderSpeedCapMetresPerSec` is already how a patrol
  cruises slowly (Hostiles §5.4). The slowest-member rule is a value for a field that is there.
- **Leaving and rejoining the world is solved.** Docking captures a ship into a station's ledger
  (`World::DockedShip`) and states *docked* on the wire; the ledger row was designed to be spawned
  back out (Stations §7.3). Launching is that spawn, and the protector launch pass is the
  in-tick spawn pattern it copies (collect, apply after the walk).
- **Pursuit is solved.** The protector duty re-aims at a moving target past
  `PURSUIT_REPLAN_METRES` and shadows it on arrival because avoidance holds it off the hull
  (Stations §8.3). The attack order and the defense are that chassis with a different master.
- **Stated acts have a precedent.** `RecordAggression` is a server-side judgment arriving from
  outside the tick, with a debug key (F6) standing in for combat (ADR 0041). `RecordHostileAct`
  is the same shape one level down: against a ship rather than a station.
- **The gesture set is bought.** The bottom bar already lays out five buttons
  (`HUD_GROUP_BUTTON_*`), distinguishes tap from hold (`HUD_LONG_PRESS_MS`), and draws a ×count
  on each; the atlas carries the glyphs. The buttons change meaning, not mechanics.
- **Per-subscriber truth has a lane.** The update header already states `hostileMask` every
  update because datagrams are lossy and one byte is cheap idempotence (Stations §4.3); the fleet
  status block travels on the same argument. The reliable lane already carries what must not be
  lost (ADR 0029); the roster rides it.

---

## 4. The fleet — simulation state at fleet grain

### 4.1 Why the simulation, when groups were the client's

A control group is a remembered selection: it changes what a tap does and nothing else, so it
lives in `WorldView` and the server never hears of it. A fleet is none of that:

- **The caps are rules.** Five fleets and eight ships are gameplay limits an adapter must not be
  able to talk its way past. The gate lives in the simulation or it is a convention (ADR 0014).
- **The defense is behavior.** "When a fleet gets attacked it defends itself" runs whether or not
  anyone is watching, beyond every interest radius, exactly as the protector pursuit does — and
  behavior lives in `GameLogic`, inside the tick (ADR 0015).
- **Orders name it.** An order at fleet grain needs a server-side referent for its gate and its
  lowering (§6).
- **A spectator would need it.** Which ships are one fleet changes what the buttons, the status
  block and the replay say — AGENTS.md §5's own test for simulation state.

So `World` gains a fleet table, beside the station table and in its shape:

```cpp
// ShipState.h, beside FACTION_LIMIT — both are per-faction ceilings a wire byte leans on
inline constexpr std::uint32_t FLEET_SLOTS = 5;     // live fleets per faction; the HUD's five buttons
inline constexpr std::uint32_t MAX_FLEET_SHIPS = 8; // members per fleet (§15, owner decision 2)

// World, beside the station table
struct Fleet
{
  FactionId ownerFaction = FACTION_PLAYER;
  std::uint8_t slot = 0;                 // 0..FLEET_SLOTS-1, unique among the owner's live fleets
  ShipHandle members[MAX_FLEET_SHIPS];   // live members; a stale handle is pruned as it is read
  std::uint32_t memberCount = 0;

  // The launch manifest: hulls composed into the fleet and still inside the station (§5.3). The
  // rows left the station's ledger at compose time; the metronome below turns them into ships.
  ShipHandle launchStructure;            // the station's structure ship; its death strands the manifest
  std::uint32_t manifest[MAX_FLEET_SHIPS];
  std::uint32_t manifestCount = 0;
  std::uint32_t launchCooldownTicks = 0;

  // The standing order, at fleet grain (§6). Members' per-ship intent derives from it and is
  // re-derived for as long as it stands — the patience the dock approach already has.
  FleetOrderKind orderKind = FleetOrderKind::Idle;
  WorldPos orderPoint;                   // Move
  float orderFacingRad = 0.0f;
  bool orderHasFacing = false;
  ShipHandle orderTarget;                // Attack
  ShipHandle orderStation;               // Dock

  // The defense (§7): the attacker last stated against a member, where the act was stated, and
  // the point the combatants were last aimed at.
  ShipHandle threat;
  WorldPos threatAnchorPos;
  WorldPos threatAimPos;
  std::uint32_t alertTicks = 0;
};
```

Members are handles, not a parallel array, and that is the cheap half of a lesson already paid
for: the patrol, docking and protector tables are parallel to `m_ships` and each extended the
despawn repair, while a handle needs none — a despawned member resolves to nothing and the fleet
pass prunes it in place, dense and in order. The fleet table itself is indexed by its own dense
id, swap-and-popped only when a fleet retires, and nothing stores a fleet index across a tick —
the pair `(faction, slot)` is the stable name, found by a linear scan of a vector whose length
the rules cap at `FLEET_SLOTS` per faction (the station table's own argument at Stations §6.1).

Everything in the row is read by `Step`, so all of it goes through `WriteWorldState` /
`ReadWorldState`; `ASavedWorldReplaysToTheSameRun` is the gate that says nothing was missed
(AGENTS.md §8).

### 4.2 The caps are contract

`FLEET_SLOTS` and `MAX_FLEET_SHIPS` change which orders are accepted, so they are in the replay
contract, spelled beside `FACTION_LIMIT` with the same one-line reason. Eight, argued rather than
liked (§15, decision 2):

| Pressure | At 8 | At 12–16 |
|---|---|---|
| Formation footprint — wedge spacing is `2 × largest bounding radius × 1.15`, a Carrier's 247 m | span ≈ 1 km, framed by the camera and inside the 2 000 m interest radius | 1.5 km and past it: a fleet whose far wing is outside its own player's interest set |
| Separation worst case — a compressed parallel pack, measured in `SimTuning.h` | unjams in ~0.4 s | 2.5 s at 16, 6 s at 24: a launch jam the player can watch |
| The ceiling on player ships in space | 5 × 8 = 40, inside the envelope every measurement in the tree was taken at | 60–80, a new performance regime nothing has measured |
| Doctrine — the hull table's eight flyable hulls | one of everything fits; 2 Miners + a Hauler + 5 escorts fits | more of the same, at the cost of everything above |

A hull-weighted point budget (a Carrier costing what four Interceptors cost) was offered and
declined with the size question — richer balance, but a budget must be taught in the composition
screen where a count teaches itself, and nothing stops a later design from re-basing the cap on
points inside the same gate.

### 4.3 The lifecycle

```
compose (station, ledger rows -> manifest, free slot)          §5.2
   -> launch (one spawn per cadence, member joins, rallies)    §5.3
      -> active (orders §6, defense §7, losses prune members)
         -> gone: last member docks   -> rows in the ledger, slot freed   ("dismantled")
                  last member destroyed -> slot freed, FLEET %d LOST
```

"Active" in "five active fleets" means exactly *occupying a slot*: a fleet exists from compose
until its last ship has left space, and docked ships are not a fleet — they are ledger rows again,
recomposable into anything. There is no other exit: no disband-in-space order (ships cannot be
loose, decision 1), no transfer between fleets, no merge — dock and recompose is the one shape
(§14).

A fleet ordered to dock dismantles *gradually*: each ship that reaches capture range becomes a
ledger row through the existing dock pass, the fleet prunes it as a stale handle, and the fleet is
gone when the count reaches zero with an empty manifest. A change of mind mid-dock diverts the
ships still flying and leaves the captured ones docked — the fleet continues smaller, which is the
same sentence a mid-dock move order already means for loose ships.

### 4.4 The fleet pass

Standing intent, in the slot Hostiles built and Stations extended, in a fixed order the tick
states once: **dockings, patrols, protectors, fleets**. Per fleet, in array order:

```
prune: drop members whose handle no longer resolves (docked or destroyed)
retire: memberCount == 0 and manifestCount == 0 -> clear the row, free the slot
launch: manifest non-empty -> metronome (§5.3); spawns collected, applied after the walk
defense: resolve threat; stand down or re-aim the combatants (§7)
patience: a member Idle while the standing order wants it somewhere -> re-issue its leg
```

Reads are end-of-last-tick state, writes touch only the fleet being visited and the ships it
names, spawns and despawns are collected and applied after the walk — every argument Stations §10
made, inherited whole. Nothing in the pass draws randomness or reads a clock.

---

## 5. Composition and launch

### 5.1 Asking the station — the first request on the wire

The ledger is deliberately not broadcast: Stations §6.2 kept it off the record and said the
management menu "will be asked for, not broadcast". This is that ask — the wire's first
client-to-server request/reply pair, both on the reliable lane (a lost reply would be a screen
that never opens; ADR 0029's test):

```
LedgerRequest { EntityId station }                                   client -> server
LedgerReply   { EntityId station; u8 dockedCounts[HULL_COUNT] }      server -> client
```

The reply states **the asker's own faction's rows only**, as hull counts — who else is docked
where stays nobody's business, which is the withholding rule the snapshot already keeps. The
station screen (§9.4) opens on the reply and shows what can be composed. A request naming a
structure that is not a station, or a station whose owner holds the asker hostile, is answered
with all-zero counts rather than silence, so the screen can say so instead of spinning.

### 5.2 Composing — the gates live in `World`

```
ComposeOrder { EntityId station; u8 slot; u8 hullCounts[HULL_COUNT] }   client -> server, reliable

// World — the gate and the act, in the simulation for ADR 0014's reason
enum class ComposeResult : std::uint8_t { Composed, NotAStation, RefusedStanding,
                                          SlotTaken, TooMany, NotDocked };
ComposeResult ComposeFleet(StationId _station, std::uint8_t _slot,
                           std::span<const std::uint32_t> _hullCounts, FactionId _issuerFaction);
```

Gates, in order, none of them the adapter's: the station must be live; an issuer the owner holds
hostile is refused whole (the dock gate's mirror — you do not assemble a battle group in a hostile
port); the slot must be one of `FLEET_SLOTS` and not currently held by a live fleet of this
faction; the total must be 1 to `MAX_FLEET_SHIPS`; and every requested hull must be covered by
the issuer's own rows in this station's ledger. An accepted compose removes the rows from the
ledger into the fleet's manifest and the fleet exists from that tick — slot occupied, roster
stated (§8.1), launch beginning. A refused one changes nothing; the affordance already knew
(§9.4) and the roster's silence is the confirmation, the fire-and-forget shape every order
already has.

The rows leave the ledger at compose time rather than at each spawn so that two things cannot
happen: a second compose claiming the same rows, and a ledger the screen shows disagreeing with
what launch will find. The cost is that a manifest stranded by its station's death loses its
ships — tolerable today (nothing can destroy a station), stated here because the user-station
design inherits it and may prefer the manifest to fall back into wreckage or a refund.

> **Amendment, 2026-08-31 (slice 5).** `ComposeOrder` is on the wire, and two details of it are
> narrower than the sketch above. It carries `u8 hullCount` ahead of the array, so a reader whose
> hull table is a different size refuses rather than reading one hull's count as another's — the
> `LedgerReply` carries the same byte for the same reason. And it carries **no size gate at all**:
> how many ships a fleet may hold is `ComposeFleet`'s rule, and a codec enforcing it too would be a
> second copy of it to keep in step (ADR 0014). A draft of a hundred Battleships decodes cleanly and
> is then refused by the gate that owns the number.
>
> The screen the order comes from is fed by a **request**, not a broadcast: `LedgerRequest` up,
> `LedgerReply` down, answered on the tick the request was read
> ([ADR 0051](Decisions/0051-the-ledger-is-asked-for-not-broadcast.md)). Both rules this section
> states about whose rows count — the issuer's own, and none at all in a hostile port — moved into
> one `World::LedgerFor` that the reply and this gate both call, so a screen cannot offer what the
> compose will refuse. That disagreement is the one this section's last paragraph is about, and it
> was reachable until the two shared a function.

### 5.3 Launching — one hull per cadence

While the manifest is non-empty, the fleet pass spawns one ship per `FLEET_LAUNCH_EVERY_TICKS`
(45 — 0.75 s), at the station's skin on the bearing from the structure to the member's own rally
slot, heading outward, entered into `members` as it spawns. Cadence and bearing together are what
make a launch never reproduce the dense-spawn worst case the separation design measured: ships
appear 0.75 s apart, already fanned toward their formation slots, and the first is well clear
before the second exists. Eight ships are in space in 5.25 s, which reads as a launch sequence
rather than a jam.

The **rally point** sits on the station's outward bearing from the system's star anchor, at
`DockApproachRangeMetres(station, largest member) + SlotSpacingMetres(largest member)` — clear of
the no-go band, clear of the dock approach lanes, in the open the layout guarantees on the far
side of a planet's station. Each spawned member is issued a formation leg to its rally slot
through `IssueMoveOrder`'s machinery; a fleet with a standing order already (the player ordered it
mid-launch, which is allowed) rallies to the order instead — later spawns simply join the
formation solved for the order, which the per-ship patience of §4.4 does with nothing added.

`FLEET_LAUNCH_EVERY_TICKS` is contract (it decides on which tick a ship starts existing —
`DOCK_CAPTURE_METRES`'s own sentence, reversed); the rally geometry is derived from constants
already in the contract, so nothing new joins it there.

> **Amendment, 2026-08-31 (slice 2).** Three things about this section are narrower in the code than
> on the page, and each is a place where the design named an intent the simulation could not reach.
>
> **"Outward" is the station's own heading**, not the bearing from the system's star. `World` has no
> star and must not learn about one: the layout is content the composition root reads (ADR 0037), so
> a simulation reaching for the universe origin would be baking content into the tick. A station's
> facing is already simulation state and already authored by whoever spawns it, which makes it the
> honest place to keep "this way is out".
>
> **The fan runs to one side of the door**, indexed by the slot the hull is launching into — which
> is the manifest's own remaining count, so it strictly decreases and no two launches of one fleet
> ever share a bearing, not even across a loss. A fan centred on the outward bearing would shift by
> half a step whenever the composed count changed, and two launches could then land on one bearing.
>
> **"Later spawns join the formation solved for the order" is one order per ship**, not one order
> re-issued over every member. The composed size is `memberCount + manifestCount` read back, so each
> hull can be given its own final slot the moment it launches, and the spawn fan and the slot lane
> run the same way round. The re-issuing form was written first and reshuffles who is where on every
> launch, so ships already on station cross each other: measured at 1.0 cm of capsule overlap during
> a Corvette launch, against none once each ship keeps the slot it was born for
> ([slice 2](Fleets-slice-2.md) §2.4).

### 5.4 What launching is not

There is no per-ship undock and no other path out of a ledger — the fleet-only model (decision 1)
is enforced by construction, not by a check: the only code that turns a ledger row into a ship is
the fleet launch. NPC ships are untouched — patrols and protectors fly loose because a fleet is a
*command* concept and nothing commands them from five buttons; the table is faction-generic even
so, because an NPC raid wing is an obvious later tenant and nothing in the row assumes
`FACTION_PLAYER`.

### 5.5 The starting scene

The three starting hulls boot as **Fleet 1**, composed directly by the composition root after it
spawns them — the root reaching past the wire under F4 and F6's charter (a boot is not a gameplay
path). Buttons 2–5 open empty. F5 reseeds looks and never fleets; F4 still shatters a selected
fleet's hulls, and the fleet prunes them exactly as it would combat losses. The first minutes of
play teach the loop by themselves: fly Fleet 1 to a Vanguard station, dock it — the button
empties — then open the station and compose it back.

---

## 6. Orders at fleet grain

### 6.1 One message, one gate, and the ship lists retire

```cpp
enum class FleetOrderKind : std::uint8_t { Idle, Move, Dock, Attack, Stop, Mine /* reserved, §6.6 */ };

// The wire, reliable lane (ADR 0029). One per selected fleet; five selected fleets are five
// messages, inside the ordersPerTick budget of 8 with room to spare.
FleetOrder { u8 slot; u8 kind;
             kind == Move:   WorldPos point, u16 facing?, u8 hasFacing
             kind == Dock:   EntityId station
             kind == Attack: EntityId target }

// World — the lowering and the gate
FleetOrderResult IssueFleetOrder(FactionId _issuerFaction, std::uint8_t _slot, ...);
```

The authority gate becomes one comparison — the issuer's faction must own the fleet in that slot —
in place of the per-ship faction filter, and it stays in the simulation (ADR 0014). The
ship-list `MoveOrder` and `DockOrder` messages retire in the same phase: the client never sends
them once selection is fleets, and a wire with a second way to say the same thing is a path
nobody tests (ADR 0028's argument at message grain). `World::IssueMoveOrder` and `IssueDockOrder`
themselves stay exactly as they are — they are what a fleet order lowers onto, what the NPC passes
use, and what every existing test drives.

An explicit order **clears the threat** (§7.4) and replaces the standing order; per ship it does
what it always did. `Stop` sets the standing order Idle and clears every member's intent, which is
the brake the sheet needs and costs one kind byte.

### 6.2 The lowering

- **Move** → `IssueMoveOrder(members, point, hasFacing, facing)`, then the cruise cap of §6.3 on
  every member. The formation, the routes, the arrival: all existing machinery.
- **Dock** → `IssueDockOrder(members, station)`, cruise-capped the same way. Standing gates
  (hostile owner refuses) apply unchanged; capture dismantles gradually per §4.3.
- **Attack** → the combatant members are aimed at the target — pursue and shadow, §6.5; the
  non-combatants hold where the order found them (`Stop` semantics for them alone). No leash: an
  *ordered* pursuit runs until the target is gone or the order is replaced, the protector's own
  "flight is postponement".
- **Stop** → everything Idle, threat cleared, alert left to decay.

The standing order persists after its first issue — §4.4's patience re-issues a leg to any member
that goes Idle short of it, which is what the dock approach already does and what makes a fleet
arrive whole through traffic, and what makes late launches join the formation with no special
case.

> **Amendment, 2026-08-31 (slice 3).** Two of this section's sentences are narrower in the code.
>
> **A late launch joins the order by its own step, not by patience.** "Late launches join the
> formation with no special case" is one case: the launch re-issues the standing order over every
> member including the one just born, which re-solves the formation for the count that is actually
> out. Patience cannot do it, because a hull that has never been given the order has nothing to be
> patient about.
>
> **Patience reads the member's route destination and never the fleet's order point**, and that is
> load-bearing rather than incidental. A route whose point the wall forbids ends as close as the
> geometry allows, and `AdvanceRoute` moves the destination to where the ship stands so that it is
> never re-planned back at a point it cannot reach
> ([ADR 0042](Decisions/0042-a-route-never-asks-for-a-point-the-wall-forbids.md)). Patience inherits
> that by reading the route; a patience that re-derived the point from the fleet would discard it and
> re-plan every member of a fleet ordered into a wall on every tick, for ever — invisible from
> outside a tick, because the ship would be set Moving at the top of `Step` and put back to Idle
> before the end of it. `World::RoutePlanCount()` is the readout that makes it visible, and
> `AFleetOrderedIntoAWallSettles` is the test that reads it ([slice 3](Fleets-slice-3.md) §2.5).

### 6.3 The cruise rule — slowest member (owner decision 3)

At lowering, every member's `orderSpeedCapMetresPerSec` is set to the slowest member's
`maxSpeedMetresPerSec`. Slots are solved at the destination, not maintained en route, so a common
ceiling is all "arrive together" needs: nobody outruns the wedge, and the spread the hull table
deliberately keeps (34 m/s Interceptor over a 20 m/s Carrier) becomes a *choice the composition
screen shows* — escort a Hauler and the fleet moves at 22. The field is intent, already off the
snapshot, so nothing new leaks. An attack order caps the *non-combatants'* hold at zero and
leaves the combatants at their own best speed — a chase capped at a Miner's pace would be the
rule misfiring, and the miners are not going anywhere anyway.

### 6.4 Ordering what you cannot see

A fleet order is legal with the fleet outside the issuer's interest set — select fleet 3, pan
across the map, tap. The server has every position and solves the true formation heading; the
client draws its marker with `FormationHeading`'s fallback when it lacks member records, and the
two disagreeing about a *marker* for a fleet off screen is a cost of exactly nothing. Stated here
because it is the one place the shared-arithmetic promise of Collision slice 2b §2.5 is knowingly
relaxed.

### 6.5 Attack, before combat exists

An attack order does today what a protector does: each combatant re-aims at the target when it has
moved `PURSUIT_REPLAN_METRES` from the point last aimed at (the constant is reused, not copied —
it measures the same quantity, a pursued target's drift), flies the leg on existing machinery, and
*shadows* on arrival, avoidance holding it off the hull. The teeth are the combat design's; the
socket it inherits is a fleet already holding weapons range on a target, which is the same socket
the protector handed over (Stations §8.3). The target dying or docking completes the order: the
fleet reverts to Idle where it stands.

`combatant` is a new authored `HullSpec` field: true for Interceptor, Bomber, Corvette, Frigate,
Battleship, Carrier; false for Miner, Hauler, Stargate, Structure. Authored rather than derived
(from, say, a future weapon table) for `avoidanceAuthority`'s reason — a hull that is armed but
precious, a Q-ship, an armed hauler, must stay expressible.

### 6.6 Mine — a kind reserved, a design owed

Mining needs something to mine. The rocks are presentation and a ship flies through them
(ADR 0016); a mineable site is simulation content the way a planet site is — a position and a
yield the layout states — plus extraction, cargo and unload, which is an economy design of its
own. This design reserves `FleetOrderKind::Mine` so the byte never renumbers, refuses it in
`IssueFleetOrder` until then, and keeps it off the sheet — no half-landed affordance, the line
Stations §9.1 already held. What the mining design inherits from here: a fleet whose Miners are
its hands and whose escorts already defend them (§7), and a command surface with the slot
waiting.

---

## 7. The defense

### 7.1 A stated act, one level down

```cpp
// World. The server judges; no client message exists or ever will for this — RecordAggression's
// sentence, at ship grain.
void RecordHostileAct(ShipHandle _attacker, ShipHandle _victim);
```

The combat design calls this on the first hostile act against any ship — it owes the trigger, this
design owes it a socket that already works, the exact contract Stations §8.1 wrote for
aggression. Until then: `GameLogicTests`, and **F7** — mark the nearest non-own ship an attacker
of the selected fleet's first member, the composition root reaching past the wire under F4 and
F6's charter, so the glow, the scramble and the leash are tunable and screenshottable today. When
the victim is a fleet member, the fleet's row takes: `threat = _attacker`,
`threatAnchorPos = the victim's position now`, `alertTicks = FLEET_ALERT_TICKS`. A hostile act
against a station stays `RecordAggression`'s; an act against a loose NPC ship is recorded and
ignored — no fleet, no reaction, until some later design gives loose NPCs a response of their own.

There are, deliberately, **no senses** (ADR 0041's line holds): no aggro radius, no scan, no
reaction to an attack on a *different* fleet or on a stranger. A fleet reacts to what was done to
it, and only to that.

### 7.2 The posture

While `threat` resolves and the threat sits within `FLEET_ENGAGE_RANGE_METRES` (1 000 m) of
`threatAnchorPos`:

- **Combatant members** pursue and shadow it — the §6.5 chassis, re-aimed past
  `PURSUIT_REPLAN_METRES`, standing order suspended for them.
- **Non-combatant members** carry on with the standing order, unmoved. They do not flee: fleeing
  is a judgment about where safety is, which is a sense, and the combat design can add a
  withdraw-behind-the-line refinement when there is a line to withdraw behind.

The leash is anchored where the act was stated, not on the fleet or the fight: pursue the attacker
a kilometer from the ground it struck and it is released — the defense holds ground, it does not
follow a provocation across the map, which is what distinguishes it from an *ordered* attack and
what makes hit-and-run a real tactic against the player rather than a way to drag five fleets
around by their tempers. A fresh act re-states the anchor. When the threat dies, docks, or breaks
the leash: combatants return to the standing order (the patience of §4.4 re-issues their legs)
and the alert is left to decay.

`FLEET_ENGAGE_RANGE_METRES` is contract, and 1 000 is argued: it is half the interest radius, so a
defense never drags the escorts of a watched fleet off the player's screen; it is roughly the span
of the widest formation this design allows, so "within the leash" and "among the fleet" are the
same neighborhood; and it comfortably exceeds every dock range in the table, so a fleet attacked
at a station's door defends the door.

A fleet of nothing but Miners and Haulers glows and does not shoot back — the button turning red
*is* the design working, and what happens next is the player's order. That honesty is cheaper
than teaching a Hauler to fight.

> **Amendment, 2026-08-31 (slice 4).** Three things about this section are settled differently in the
> code, and the first two are corrections rather than refinements.
>
> **Engagement is bounded by the alert as well as the leash**, and a fleet is engaged only while both
> hold. §7.2 lists the leash and §7.3 says "the posture's stand-down reads it" without the two ever
> meeting; §8.2 settles it by asking for *two* bits, which can only differ if the alert outlives the
> engagement. Without the alert as a bound, one shot from an attacker that then parks 900 m from the
> anchor and does nothing would hold a fleet's combatants out of their orders for ever.
>
> **The two bounds bind in different situations.** No hull in the table covers a kilometer in the ten
> seconds one act buys, so a **hit-and-run attacker is released by the alert**, not by the leash this
> section credits — and the leash is what releases a **sustained** fight, whose repeated acts keep
> refilling the alert, once it has drifted off the ground it started on. Both bounds are real; the
> sentence "pursue the attacker a kilometer from the ground it struck and it is released" describes
> the second case only.
>
> **Standing down re-lowers the standing order; patience cannot do it.** Pursuit overwrites a
> combatant's route destination with the target's position, and patience re-issues a member to its
> own route destination — so leaving it to patience would send a combatant back to where its quarry
> used to be. A fleet with no standing order stops its combatants instead
> ([slice 4](Fleets-slice-4.md) §2.5, ADR 0050).
>
> There is also no `threatAimPos` on the row, though §4.1 lists one: what a pursuer last aimed at is
> its own route's destination, which is where the protector already keeps it, and every combatant is
> aimed at the target itself rather than at a slot around it — so their aim points are equal by
> construction and a fleet-level copy would be a second source of truth for the codec to carry.

### 7.3 The alert

`alertTicks` counts down from `FLEET_ALERT_TICKS` (600 — ten seconds) and any act on any member
resets it. The wire's under-attack bit (§8.2) is `alertTicks > 0`, so one volley lights the
button for ten readable seconds and a running fight holds it lit. Contract, because the posture's
stand-down reads it. The client's red glow, its pulse and any camera nudge are `ViewTuning.h`'s
and free to be tuned (§9.1); the *definition* of under attack is the simulation's and is this
field.

### 7.4 What outranks what

One line, applied uniformly: **an explicit order outranks the standing behavior** — the sentence
`IssueDockOrder` already carries for patrols. A player order to the fleet clears `threat` and
re-tasks everyone, combatants included; if the attacker persists, the next act re-rouses the
defense one tick later with a fresh anchor. The defense in turn never cancels the standing
*order* — it suspends the combatants and returns them to it — so a mining fleet bushwhacked on
its way home resumes going home, short its attackers' attention, not its instructions.

---

## 8. The wire — roster and status

### 8.1 The roster, reliably

The client must know which entities are whose fleet — to select a fleet by tapping any member, to
count the button, to draw fleet-grain selection — and must not infer it. A reliable-lane message
per fleet change:

```
FleetRoster { u8 slot; u8 count; EntityId members[count] }    server -> client
```

sent to the owning subscriber on compose, on each launch, on each pruned loss, and on retire —
`count == 0` is the slot freeing, which is how `FLEET %d LOST` and the emptied button both learn.
A subscriber added mid-match is sent every occupied slot's roster as it joins, the despawn
cursor's own joining rule applied to fleets. Fleet membership never travels in the ship record —
a record is per-update and membership changes at human speed; the roster is the delta and the
record stays 47 bytes.

> **Amendment, 2026-08-31 (slice 5).** **`count == 0` is not the slot freeing; the mask is.** This
> section and §8.2 disagree, and §8.2 is right. A composed fleet whose manifest has not begun to pour
> has an empty roster and a live slot, so an empty membership cannot mean the slot is free — and the
> two facts want different carriage anyway. A roster is stated once on a lane that can refuse it; the
> mask rides every update and heals itself, which is the trade `hostileMask` already made. So
> occupancy is the mask's, membership is the roster's, and `FLEET %d LOST` fires on the mask bit
> clearing (§9.6).
>
> It follows that **there is no roster on compose**: a composed fleet has no membership to state, and
> what the client needs — the slot is held, it is launching, it will be this big — is the whole of
> what the status block already carries. The three remaining events are unchanged, and the publisher
> is told about none of them: it holds the last membership it sent each subscriber and diffs it every
> tick, which finds compose, launch, loss and retire without a single call site knowing fleets exist
> ([slice 5](Fleets-slice-5.md) §2.6).
>
> The diff also **delivers to a joining subscriber for free** — its stored lists are empty, so its
> first publish finds every occupied slot changed — which is what this section asked for as a rule of
> its own.

### 8.2 The status block, every update

The buttons must tell the truth about all five fleets at all times — position for the camera jump
and the minimap, doing-what for the sheet, under-attack for the glow — and four of five fleets
are routinely outside the interest set, which is the point of them. So the interest update header
gains a block beside `hostileMask`, stamped on every update for the same idempotence-under-loss
argument:

```
u8 fleetMask                       // bit s set: this subscriber's faction has a live fleet in slot s
per set bit, ascending:
  i32 sectorX, i32 sectorZ         // the wire's own narrowed sector pair (ADR 0046)
  u16 qx, qz                       // local offset on the 0.125 m lattice — the record's lattice
  u8  status                       // bits 0-2 kind shown (Launching while the manifest holds
                                   // anything, else the standing order), bit 6 engaged, bit 7 under attack
  u8  count                        // members in space + manifest
```

One to 71 bytes against a 1 KB datagram, ten times a second. The **position is derived at publish
time** — the centroid of live members, or the launch station while none is out — because it is a
readout, not simulation state: the publisher already owns per-subscriber derivation
(`SplitTheLost`), sits outside the replay contract, and a centroid nobody simulates against
cannot desynchronize anything. `alertTicks`, by contrast, is simulated (§7.3) and merely
*sampled* here.

> **Amendment, 2026-08-31 (slice 5).** Three things about the block are settled more narrowly in the
> code than on the page.
>
> **It costs a ship record per fragment.** `ShipsPerSnapshotFragment()` takes no arguments — every
> caller and three tests agree on one number — so it is sized against the block's *worst* case, five
> fleets and 71 bytes, whatever a given update carries. That is 22 records a fragment against 23.
> Sizing it against what an update actually holds would buy the record back and cost a number nobody
> can state, which is the trade `MaxShipsPerOrder` already argues for in its own comment. The two
> placements that would have cost nothing were both worse: a separate datagram loses the
> idempotence-under-loss the block is bought for, and writing it only in fragment 0 gives back the
> invariant that every fragment carries records and nothing else.
>
> **A slot is stated only when its position can be derived** — live members, or a live launch
> structure. The one case with neither is the tick between a manifest being dropped for a dead
> station and the next tick's retire freeing the slot; clearing the bit there tells the truth one
> tick early rather than stating a position that means nothing.
>
> **The status byte's kind is a `FleetOrderKind` or the value 6, `Launching`**, which is not a
> `FleetOrderKind` and must not become one: nobody can issue it, `IssueFleetOrder` would have to
> refuse it, and adding it to the enum would make the fleet order codec's own range check accept a
> value no order may carry. `count` is members plus manifest, so the button states the composed size
> throughout a launch; the roster's own count is how many are out, and `LAUNCHING 4 OF 8` is the two
> read together rather than a third number on the wire.

### 8.3 What stays withheld

The standing order's *target and point* (the status byte says docking, not where), the threat's
identity, the manifest's contents, the rally geometry, and every member's intent — the snapshot
exists to withhold intent, and a fleet does not change that; the sheet shows what the status byte
and the client's own records can say. Other factions' fleets do not appear in anyone's header:
slots are the owner's business, and an enemy fleet is exactly as visible as its ships are.

---

## 9. The client

### 9.1 The five buttons, rebound

The bottom bar's five buttons stop being control groups and become the fleet slots; the group
machinery (`AssignGroup`/`SelectGroup`, `m_groups`, hold-to-assign) retires with the ship-list
orders, and `GROUP %d` log lines go with it. Per button, from roster + status block:

- **Occupied**: slot digit and ×count, in the bar's own type; filled treatment while the fleet is
  selected. **Empty**: dimmed digit, inert to taps, and holding it says the one-line truth in the
  log — `FLEET %d | COMPOSE AT A STATION`.
- **Tap**: select the fleet and fly the camera to its stated position — one gesture, because
  under decision 1 selecting a fleet *is* attending to it. The fly-over is a `ViewTuning.h` ease
  (`FLEET_FOCUS_*`), and the interest set follows the camera through the `SetCentre` wiring that
  already exists, so the records stream in as the camera arrives and the coarse centroid hands
  over to real hulls.

> **Amendment, 2026-08-31 (slice 8).** **Hold does not open the sheet on its own — it selects the
> fleet first.** Reading a fleet and taking hold of it are the same act under decision 1, and a sheet
> whose four commands went to some other fleet is the one way this panel could lie. It follows that
> the sheet closes when its fleet stops being selected, as well as when its slot empties.

> **Amendment, 2026-08-31 (slice 6).** "`SetCentre` already exists" is true and "the interest set
> follows the camera" was not: `SetCentre` was being fed the centroid of the subscriber's own ships,
> so tapping a distant fleet flew the camera to a sky the server never sent a hull to. Slice 6 makes
> the sentence true, and the composition root is where it happens — `WorldSimulation` is handed the
> camera's ground target each frame, which the class's own note already anticipated.
>
> **It was not safe to do before slice 5.** The centroid was there so a player never lost sight of
> their own ships; what replaces that guarantee is the status block, stamped on every update, which
> tells them where all five fleets are whether or not any is in view. A fleet docking no longer drags
> the view either, which `Stations-slice-6.md` §5 had fixed by hand.
>
> The **fly-over ends on arrival or the moment the player pans** — and a pan only, not an orbit or a
> zoom. Pan, orbit and zoom reach `Camera` straight out of `PointerTracker` and never touch the
> view, so the root notices one by watching the camera's *target*, which a pan moves and the other
> two do not. That falls out as the right rule rather than a convenient one: orbiting or zooming
> while the camera flies is watching the flight.
>
> **Hold is the sheet, and the sheet is slice 8.** Until then a hold logs the line the sheet's header
> will carry — `FLEET %d | %d SHIPS | MOVING` — so the gesture is discoverable and says something
> true ([slice 6](Fleets-slice-6.md) §2.6).
- **Hold** (`HUD_LONG_PRESS_MS`, the constant that already tells the two apart): the fleet sheet,
  §9.3.
- **Under attack** (status bit 7): the button pulses `HUD_ALERT_RED`, and the log states
  `FLEET %d UNDER ATTACK` on the rising edge only — the bit holds for ten seconds by §7.3, so the
  edge is readable and the log is not a metronome.

### 9.2 Selection takes hold of fleets

`PickShip` resolves a tapped own hull to its fleet via the roster and selects the whole fleet;
band select takes every fleet it touches; shift-tap toggles one fleet in or out. The selection is
a set of slots, the rings draw on every member, and an order issues one `FleetOrder` per selected
slot — each fleet its own formation, its own cruise speed, its own arrival, which is what five
messages against a budget of eight buys. Sub-fleet selection does not exist (decision 1): the day
a ship must leave a fleet, the fleet docks and the station screen is where it happens.

### 9.3 The fleet sheet (owner decision 4)

The hold gesture opens one panel over the bar — status above, commands below, in the HUD's own
overlay idiom:

```
FLEET 3                          × 6
ENGAGED — DEFENDING              // from the status byte + roster; MOVING / DOCKING / LAUNCHING 4 OF 8 / IDLE
CORVETTE × 2   MINER × 3   HAULER × 1
[ MOVE ]  [ ATTACK ]  [ DOCK ]  [ STOP ]
```

`STOP` sends immediately and closes. The other three arm a **target tap**: the sheet closes to a
one-line prompt (`ATTACK | TAP A TARGET`), and the next world tap supplies it — ground for Move,
a hostile record for Attack, a station for Dock — through the same pickers taps already use;
anything else cancels with the log saying so. Tapping the world with the fleet selected keeps
working as the shortcut it always was (ground moves, station docks — and, new, a hostile record
attacks); the sheet is where the commands are *named*, which is what a first session needs and a
tap cannot teach. Member rows show hull names and counts today — the column for hull bars is
left where the damage model will want it.

`MINE` is absent until the mining design lands (§6.6). No queue is shown because none exists
(§14).

> **Amendment, 2026-08-31 (slice 8).** Three things about the sheet are narrower in the code.
>
> **A member's hull may be unknown.** A roster names entities and a fleet the camera is not at has
> no records, so the hull ids are not there to read. The client remembers one hull per roster member
> — bounded to the current rosters, forty entries at the very most, rather than a growing memory of
> every ship it has ever held — and a member it has never seen draws as `UNKNOWN` rather than as a
> guess ([slice 8](Fleets-slice-8.md) §2.2).
>
> **`ENGAGED | DEFENDING`, not an em dash.** The UI atlas holds Latin-1 from 192 up and an em dash
> is not in it, so the bar the HUD already uses as a field separator stands in.
>
> **The armed prompt is drawn, not only logged.** §9.3 has it replacing the sheet, and it does: the
> panel's own place carries the one line while a command waits for its tap, so the state is visible
> while it is live rather than only in the log entry that announced it.

### 9.4 The station screen — assembly

Long-press on a station record of a faction that admits you (mask-checked first, the affordance
telling the truth before the wire is touched) sends `LedgerRequest` and opens the assembly view on
the reply:

```
VANGUARD STATION                 DOCKED
FRIGATE   × 2     [ + ] [ − ]
CORVETTE  × 3     [ + ] [ − ]        DRAFT   × 5 OF 8
MINER     × 4     [ + ] [ − ]        SLOT  [1] [2] [·] [4] [·]
                                     [ LAUNCH ]
```

Counts move between ledger and draft; slots show occupied ones inert; `LAUNCH` sends
`ComposeOrder` and closes, and the world does the rest — the roster fills the button, the
metronome pours the fleet out of the dock, the rally forms it up on the station's far side. A
refused compose (raced slot, stale ledger) simply leaves the button empty, and the screen's
next opening asks again — fire-and-forget, like every order. This is `PointerTracker`'s
long-press finally landing, on the schedule Stations §14 set for it: when there is a menu to
open. The rest of the management screen — trade, repair, cargo, undocking *without* a fleet,
which does not exist (§5.4) — stays the next phase's.

> **Amendment, 2026-08-31 (slice 7).** Four things this section did not settle.
>
> **The long press fires on release, not under the finger.** `PointerTracker` is driven by events
> and sees nothing between a Down and the next Update, so a contact held perfectly still generates
> nothing to notice a threshold crossing in; firing under the finger needs the tracker to be ticked
> every frame, which it is not. There is also a **dead band** — a release taps at or under 320 ms
> and long-presses at or over 450 ms, and does nothing between — so a slow, hesitant tap on a
> station cannot open a screen over it.
>
> **The screen is modal, and modality has a cost this section did not price.** It consumes every
> pointer event while it is up, which means a contact that went down before it opened never reaches
> the tracker or the HUD to be released. Both are told to drop what they are holding when it opens;
> without that, touch ids being per-contact rather than per-device, the tracker's four slots fill
> with contacts that never lifted and the game stops answering fingers
> ([slice 7](Fleets-slice-7.md) §2.3).
>
> **The mask is checked before the request, not just before the compose.** A hostile port answers a
> ledger request with zeros by the same gate that refuses a compose there, so asking would spend a
> message to be told nothing — and the player would read an empty station rather than a closed one.
> The refusal is a log line and no message goes up.
>
> **The reply carries no station owner and does not need one.** A station being long-pressed is on
> screen, so it is in the interest set and the client already knows its faction from the record.

### 9.5 The minimap learns where the fleets are

Each occupied slot draws its digit at its stated position — inside the map's half-range at the
centroid, beyond it clamped to the edge at reduced alpha, direction honest and distance
saturated, exactly the station marks' treatment (Stations §9.3) fed by the status block instead
of by static content. Five digits is the whole cost, and "spread over the universe" becomes a
thing the player can *see* — with the under-attack digit pulsing the alert red, which is the
minimap agreeing with the button.

### 9.6 The log lines

`FLEET %d | LAUNCHING %d SHIPS` on compose, `FLEET %d | %d SHIPS OUT` when the manifest empties,
`FLEET %d UNDER ATTACK` on the alert's rising edge, `FLEET %d | DOCKED` on the last capture,
`FLEET %d LOST` on a retire with a destroyed last member, and the refusal lines the gates imply
(`FLEET SLOTS FULL`, `COMPOSE REFUSED | %s HOSTILE`). All through the existing
`EventLog::PushFormat`, severities per the house palette: launches Friendly, alerts Alert,
refusals Alert, counts Info.

> **Amendment, 2026-08-31 (slice 8).** **All of these live in `WorldView`, including the alert edge
> that started in the HUD.** Two of them — `DOCKED` and `LOST` — turn on a departure's stated cause
> (ADR 0040), which only that half sees and which `ExplodeTheLost` clears as it reads; splitting one
> section's log across two files by which line happened to need what is the shape to avoid.
>
> **`DOCKED` and `LOST` are told apart by the *last* departure, not by any docking seen.** A fleet
> that lands one hull and then loses the rest was lost, which is what "on the last capture" means.
>
> **The two refusal lines the gates imply have moved with their gates**: composing in a hostile port
> is refused before the ledger is even asked for, so the line is `LEDGER REFUSED | %s HOSTILE` on the
> long press (§9.4's amendment), and a full slot table is not a refusal a player can reach — the
> assembly screen draws taken slots inert, so there is nothing to refuse.

---

## 10. Determinism

The fleet pass runs in the standing-intent slot, fixed order **dockings, patrols, protectors,
fleets**, and inherits every argument made for the passes before it (Stations §10): end-of-tick
reads, per-visit writes, spawns and despawns collected and applied after the walk, no clock, no
entropy, no pointer keys, no map iteration. Specifics worth their own line:

- **The fleet table is dense** and walked in array order; rows retire by swap-and-pop, and
  nothing stores a fleet *index* across a tick — cross-tick references are `(faction, slot)`,
  resolved by scan, the station table's shape.
- **Members are handles**, so despawn repair gains no fifth table: a stale member resolves to
  nothing and is pruned in the pass, deterministically, in member order.
- **Launch spawns** are collected and applied after the walk exactly as protector launches are;
  a spawned member enters pass 0 with `prevPos = posWorld` like any boot spawn.
- **`RecordHostileAct` arrives from outside the tick** — adapter, root, tests — like every order
  and like `RecordAggression`; nothing inside `Step` states acts.
- **New contract constants**: `FLEET_SLOTS`, `MAX_FLEET_SHIPS`, `FLEET_LAUNCH_EVERY_TICKS`,
  `FLEET_ENGAGE_RANGE_METRES`, `FLEET_ALERT_TICKS`, and `HullSpec::combatant` per hull — each
  with its one-line reason in `SimTuning.h`/`HullSpec.h`. `PURSUIT_REPLAN_METRES` is read, not
  duplicated. The rally geometry and cruise rule derive from constants already in the contract.
- **The status block is publish-side** (§8.2) and changes what is sent, never what is simulated;
  the roster likewise. A recording replays identically with no subscriber at all.
- **A fleetless world ticks bit-identically to today** — an empty table, a pass that visits
  nothing, and the existing `GameLogicTests` passing unchanged is the claim, made explicitly by
  the first slice.
- **The codec carries the row** (§4.1), and `ASavedWorldReplaysToTheSameRun` gates it.

---

## 11. Tests

`GameLogicTests`, naming the property:

| Test | Decides |
|---|---|
| `AFleetIsComposedFromTheLedger` | compose moves rows to the manifest, occupies the slot, and refuses hulls the ledger does not hold |
| `TheSixthFleetIsRefused` | five slots held → `SlotTaken`; a retire frees the slot and compose succeeds again |
| `TheNinthShipIsRefused` | `TooMany` past `MAX_FLEET_SHIPS`; zero ships likewise refused |
| `AHostilePortRefusesComposition` | standing gate mirrors the dock gate |
| `TheManifestEmptiesOnTheMetronome` | one spawn per cadence, at the skin, fanned to slots; members join as they spawn |
| `ALaunchNeverJams` | eight launched hulls never overlap past the separation clamp on any tick |
| `AFleetCruisesAtItsSlowestMember` | every member's cap equals the least `maxSpeedMetresPerSec` present |
| `AFleetOrderLowersToAFormation` | Move → members arrive in wedge about the point, facing honored |
| `AFleetIsPatient` | a member shoved off and gone Idle is re-issued its leg; late launches join the standing order |
| `ADockDismantlesTheFleet` | captures append ledger rows one by one; the last frees the slot |
| `AChangeOfMindMidDockLeavesTheDockedBehind` | diverted fleet continues smaller; captured rows stay |
| `TheLastLossRetiresTheFleet` | destroyed members prune; the last clears the row and the roster says count 0 |
| `AHostileActRousesTheDefense` | combatants aim at the attacker; Miners and Haulers hold their orders |
| `TheDefenseHoldsItsGround` | the attacker past 1 000 m of the anchor releases the combatants back to the standing order |
| `AnOrderedAttackHasNoLeash` | Attack pursues arbitrarily far until the target is gone |
| `AnExplicitOrderStandsTheDefenseDown` | any player order clears the threat; a fresh act re-rouses with a fresh anchor |
| `TheAlertDecays` | the bit holds `FLEET_ALERT_TICKS` and clears; any act resets it |
| `TheRosterFollowsTheFleet` | reliable messages on compose, each launch, each loss, retire; a joining subscriber receives every occupied slot |
| `TheStatusBlockStatesEveryFleet` | mask, centroid on the lattice, status bits and count decode for 0–5 fleets |
| `TheLedgerAnswersItsOwner` | request/reply carries the asker's rows only; a hostile asker reads zeros |
| `AFleetlessWorldTicksAsToday` | empty table, bit-identical run — the first slice's claim |
| `TheSameFleetProducesTheSameRun` | compose, launch, orders, an act, the defense — twice, field-for-field per tick |

plus `ASavedWorldReplaysToTheSameRun` extended over the fleet row, which is the codec gate, and
the collision suite's dense-spawn test left exactly as it is — the launch cadence must make it
moot, not lean on it.

The client slices are decided by screenshots at two window sizes (Design/README.md): the fleet
bar with counts and one glowing button; the sheet open over a fleet in three states; the assembly
screen with a draft; the minimap with a clamped fleet digit; a launch sequence; and no `GameLogic`
file touched by an `Outpost` slice.

> **Amendment, 2026-08-31 (slice 6).** The last of those and §16's slice 6 row are in conflict, and
> the row wins. Slice 6 is where nothing writes a `MoveOrder` or a `DockOrder` any more, and the row
> lists their retirement — which is a `GameLogic` change. A wire message nothing writes is a second
> way to command lying around waiting to be found, which is worse than the rule the deletion breaks.
>
> The rule is kept where it is worth keeping: the client work is **one commit that touches no
> `GameLogic` file**, and the deletion is a second commit reviewable on its own. What the rule is
> really guarding against is a client slice quietly changing simulation behavior, and this changes
> none — `World::IssueMoveOrder` and `World::IssueDockOrder` are untouched, because a fleet order
> lowers onto them. What retires is the wire, not the machinery.

---

## 12. The MMO ledger

| This design adds | Where it lands | The day the MMO arrives |
|---|---|---|
| Fleets at faction grain, five slots | a dense table in `World` | widens to per-player exactly as standings and authority planned: a keyed owner, the same gates — one subscriber is one faction today, so the coarseness is the same debt in the same place |
| Orders naming a fleet, not ships | one small reliable message | already the cheap shape: an order stops scaling with fleet size, which is what an MMO wire wants |
| The roster | reliable, per change, per owner | per-player delivery falls out of per-player fleets; nothing is broadcast today that would have to be unshared |
| The status block | per-subscriber header bytes | already per-subscriber; a spectator subscriber gets whichever faction's block its entry names |
| A stated hostile act | `RecordHostileAct`, server-judged | the combat design's trigger at any scale; no client can state one, so no client can lie about one |
| Launch and dismantle | the ledger ↔ space seam | region handoff moves members as ships and the row's handles are per-`World` already — a fleet crossing a shard is the handoff design's listed problem, priced here, not hidden |

Traps stepped around, named for review: no behavior keyed on interest membership (the defense
runs unobserved), no client-declared membership or acts, no senses, no wall clocks, no maps, no
per-viewer simulation, and no special-cased player — the fleet table is faction-generic and the
Vanguard could fly one tomorrow.

---

## 13. Design choices

The choices made inside the design, each with its cost; those put to the owner are §15.

- **Fleets are simulation state.** Argued at §4.1. Cost: a table in the replay contract and the
  codec, a pass in the tick, and the client giving up a purely local concept it already had —
  paid for by authoritative caps, unobserved defense, and orders that stop scaling with ships.
- **Slots are the identity.** `(faction, slot)` names a fleet for the wire, the buttons and the
  player; there is no fleet id, no name, no reorder. Cost: a retired slot's number is reused by
  the next compose — accepted, because the button *is* the identity a player tracks.
- **The manifest empties on a metronome, not in a burst.** Cost: a launching fleet is vulnerable
  for seconds and its cadence is one more contract constant; bought: the measured worst case of
  the separation design never happens on a player's screen (§5.3), and a launch reads as one.
- **Rows leave the ledger at compose.** No double-claim, no screen/launch disagreement; cost: a
  stranded manifest if the station could die — stated for the user-station design (§5.2).
- **The defense reacts at fleet grain to ship-grain acts.** One threat, one anchor, one alert per
  fleet — not per member — so the posture is one judgment and the wire one bit. Cost: a fleet
  attacked at both ends anchors on the latest act; acceptable at a kilometer of leash.
- **The leash anchors on the act, not the fleet.** A pursuing escort cannot drag the anchor with
  it (a min-distance leash never releases — the pursuer keeps the distance small), and hit-and-run
  stays a tactic. Cost: one more `WorldPos` in the row.
- **`combatant` is authored, not derived.** The armed-but-precious hull stays expressible;
  cost: one flag to keep honest when the weapon table exists.
- **The status position is publisher-derived.** A centroid is a readout; deriving it at publish
  keeps it out of the contract and off the codec. Cost: the block is only as fresh as the update
  cadence — 100 ms, invisible under a camera flight.
- **The ship-list order messages retire.** One way to say a thing on a wire that gates in one
  place; cost: tests that drove the wire forms move to the fleet form once, in that slice.
- **`Stop` is an order kind, not a UI nicety**, because the sheet needs a brake and "order the
  fleet to where it is" is a formation shuffle, not a stop.
- **New identifiers extend standing families as spelled**: `FLEET_ENGAGE_RANGE_METRES` beside the
  `_METRES` family, `*_TICKS` beside `BLOCKED_WAYPOINT_TICKS`, per AGENTS.md R11.

---

## 14. Deliberately left out

Named so nobody goes looking, and so the next design knows its edges:

- **Combat** — damage, weapons, hit points. The defense and the attack order meet it at two
  sockets: `RecordHostileAct` as the trigger, shadowing combatants as the delivery. Until then F7
  and the tests state the acts.
- **Mining and the economy** — resource sites, extraction, cargo, unload, and the `Mine` kind's
  semantics beyond its reserved byte (§6.6). The rail's wallet and storage panels stay fed by the
  root's placeholders.
- **Split, merge, transfer, reinforce in space.** Composition changes at a station, full stop
  (§4.3). A "detach damaged ships homeward" order is a fine later slice on top of this model.
- **Order queues and waypoints.** One standing order; the sheet shows it. A queue is a later
  design that must argue with the patience rule first.
- **Formation choice.** `FormationShape` has four shapes and the order machinery uses the wedge;
  exposing the choice per fleet is a byte on `FleetOrder` when a design wants it.
- **Stances and rules of engagement** — hold-fire, flee thresholds, escort assignments. The
  defense is one posture by design; toggles arrive with the combat design that gives them
  meaning.
- **Fleet names.** `FLEET 3` is the name. Naming, like station designations, belongs to a screen
  with a keyboard story.
- **Composition presets** — "launch the last mining fleet again" is a QoL slice on the assembly
  screen, not a simulation concept.
- **NPC fleets.** The table is faction-generic on purpose (§5.4) and nothing composes one yet;
  Vandal raid wings are a content design away.
- **Cross-system travel.** The `Stargate` hull waits; a fleet jumping is a departure cause
  (`ADR 0040`'s door) plus arrival choreography, in the design that gives a second system
  something to be reached by.
- **The rest of the station screen** — trade, repair, cargo, per-ship undock (which stays
  nonexistent, §5.4).
- **Damage on the sheet and button** — the column and the pip row are placed (§9.3) and read
  nothing until the damage model exists.

---

## 15. Decisions taken with the owner

Put to the owner on 2026-08-31 and answered as follows; each was the recommended option.

| Question | Decision | What lost |
|---|---|---|
| Are fleets the only way player ships fly, or do loose ships remain beside them? | **Fleet-only.** Every undocked player ship is in a fleet; a lone ship is a fleet of one in a slot; the five buttons, selection and orders all speak fleet | the hybrid — looser play, but two command models forever, a cap that stops governing, and per-ship wire orders kept alive for the exception |
| The maximum size of one fleet | **Eight ships**, a flat count (§4.2) | 12–16 — formation span outgrows the interest radius and the measured envelope; hull-weighted points — better balance for a budget the composition screen must teach; both re-openable inside the same gate |
| How a mixed fleet travels | **At its slowest member's speed**, via the existing per-order cap (§6.3) | own-best-speed — fleets string out over kilometers and "under attack" means the head of a column the rest cannot reach |
| What long-pressing a fleet button opens | **The fleet sheet: status and commands** (§9.3), target taps for the targeted kinds, world taps still the shortcut | a bare command palette — status stays guesswork; a bare order queue — commands stay undiscoverable and no queue exists to show |

---

## 16. Slices

Eight, in dependency order. 1–5 are `GameLogic` and strictly serial (one slice per layer at a
time); 6–8 are `Outpost`, serial with each other, each starting only when its `GameLogic`
dependencies have merged. Each is one branch, one pull request, in Design/README.md's shape; work
orders are written per slice when it is picked up. AGENTS.md's what-is-here sentences move in
whichever slice makes them false.

| # | Slice | Layer | Depends on | Decision records due |
|---|---|---|---|---|
| 1 | **The table**: `Fleet`, `FLEET_SLOTS`/`MAX_FLEET_SHIPS`, the pass skeleton (prune/retire only), codec coverage, `AFleetlessWorldTicksAsToday`, the replay and save gates over the row — *in review*, [work order](Fleets-slice-1.md) | `GameLogic` | — | [fleets are simulation state at fleet grain](Decisions/0048-fleets-are-simulation-state-at-fleet-grain.md) |
| 2 | **Compose and launch**: `ComposeFleet` + gates, the manifest, the metronome + rally, `FLEET_LAUNCH_EVERY_TICKS`, dismantle-on-dock and retire-on-loss falling out of the prune, their tests — *in review*, [work order](Fleets-slice-2.md) | `GameLogic` | 1 | — |
| 3 | **Fleet orders**: `FleetOrderKind`, the `FleetOrder` message + `IssueFleetOrder` + lowering, the cruise rule, `Stop`, `Attack` and `Mine` reserved-and-refused, patience, their tests — *in review*, [work order](Fleets-slice-3.md). The ship-list messages' retirement moved to slice 6, which is where the client stops sending them | `GameLogic` | 2 | [orders name a fleet, not ships](Decisions/0049-orders-name-a-fleet-not-ships.md) |
| 4 | **The defense**: `RecordHostileAct`, `HullSpec::combatant`, threat/anchor/alert + the posture, `FLEET_ENGAGE_RANGE_METRES`/`FLEET_ALERT_TICKS`, the ordered attack sharing the chassis, their tests — *in review*, [work order](Fleets-slice-4.md) | `GameLogic` | 3 | [a fleet defends itself against stated acts, at fleet grain](Decisions/0050-a-fleet-defends-itself-against-stated-acts.md) |
| 5 | **The fleet wire**: `FleetRoster` + join-time delivery, the status block + publish-side centroid, `LedgerRequest`/`LedgerReply` and the `ComposeOrder` that has no use without them (§5.2 — unlisted here until slice 2 placed it), receiver surfaces, their tests — *in review*, [work order](Fleets-slice-5.md) | `GameLogic` | 2 (roster), 4 (status bits) | [the ledger is asked for, not broadcast](Decisions/0051-the-ledger-is-asked-for-not-broadcast.md) |
| 6 | **The fleet bar**: buttons rebound (tap = select + fly-to, hold = sheet stub, glow, counts), selection at fleet grain, `FleetOrder` sending, group machinery and its log lines retired, the ship-list order messages retired with them, the boot scene as Fleet 1, F7, minimap fleet digits, screenshots at two sizes — *in review*, [work order](Fleets-slice-6.md) | `Outpost` | 3, 5 | — |
| 7 | **Assembly**: the station long-press, `LedgerRequest` flow, the assembly screen, compose + launch end to end on screen, screenshots — *in review*, [work order](Fleets-slice-7.md) | `Outpost` | 5, 6 | — |
| 8 | **The sheet**: status lines, member rows, the command row + target-tap arming, refusal and alert log lines complete, screenshots of the three states — *in review*, [work order](Fleets-slice-8.md) | `Outpost` | 6 (7 for a docked-adjacent demo) | — |

The whole of `GameLogic` is now written: slices 1–5 are decided by their tests and by the suites
staying green; 6–8 by screenshots and by what they must not touch. The seam holds throughout: nothing reaches the client outside the
roster, the status block, the ledger reply and the records it already had.

What this phase leaves ready: a fleet that knows it is under attack for a combat design to arm, a
Miner-shaped hole for a mining design to fill, a faction-generic table for NPC wings, a slot
identity the MMO widening keys on, and a station screen with room for everything else a station
will do.
