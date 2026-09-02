# Game design review — the road to the next Homeworld, or the next EVE Online

A point-in-time review of the tree at `caf9814` (2026-09-01), asking one question: **what is
missing, and what must change, for Outpost: Frontier to become the next Homeworld or the next EVE
Online?** The two benchmarks are read precisely rather than as slogans. Homeworld is fleet-grain
command with tactical readability, formations that matter, ballistic combat where geometry decides,
hull classes with sharp counters, a persistent fleet carried forward, harvesting that paces a
campaign, and a feel where every order answers in a frame or two. EVE is one persistent shard, an
economy where every item is built by players from mined and refined material, faucets balanced by
sinks, destruction as the master sink, markets and industry chains, standings and sovereignty, and a
server that survives thousands in one fight by stating its rate rather than freezing. Every
recommendation below names the benchmark it serves and says why the other does not need it.

**Method.** Eight specialists in two panels — Economy & Systems (currency and ledger, resource
loops, sinks and inflation, markets and industry) and Combat & Mechanics (responsiveness, class
roles, the ability framework, synchronisation at scale) — each audited the tree and its design
documents. Every finding was then refuted or confirmed against the code by an independent pass;
what survived went to two discipline leads, who merged, corrected and ranked it into a roadmap each,
and one director wrote this document from the two. Findings cite `file:line` at `caf9814`; the one
commit after it (`793d57f`) adds only `.claude/workflows/game-design-review.js`, so every citation
reads the same at the head. Before writing, ten citations spanning both panels were opened at the
cited line — `GameLogic/UniverseSnapshot.h:212-220`, `GameLogic/ShipState.h:205-218`,
`GameLogic/Universe.cpp:686-690`, `Outpost/Hud.h:71-88`, `NeuronServer/ServerHost.h:20-27`,
`GameLogic/ShipState.h:17-36`, `GameLogic/Universe.cpp:1028-1038`,
`GameLogic/UniverseSnapshot.cpp:119-120`, `GameLogic/Publisher.cpp:203`, `GameLogic/HullSpec.h:279`
— and all ten say what the leads say they say; nothing was dropped or corrected. The leads' own
line corrections (recorded in their dropped-findings lists) are already applied in what follows.

**Tags.** Every roadmap item is **[Missing]** — nothing in the tree does this and the benchmark
needs it — or **[Change]** — something in the tree does this and does it in a shape the benchmark
cannot use. Each carries a priority: **P0** is on the critical path to the first playable loop of
either benchmark; **P1** is what the loop needs to be the benchmark rather than a sketch of it;
**P2** stands behind P1 work and is named now so the P1 work leaves it a door. Each names the
benchmark it serves: **Homeworld**, **EVE**, or **both**. A gap AGENTS.md or README.md already
declares still counts — this review judges completeness against the goal, not the honesty of the
documents — and says so where it does.

---

## Verdict

**The chassis is right for both benchmarks and the game is built on top of it for neither.** The
decisions that are expensive to retrofit and usually gotten wrong are in the tree, tested and
argued: a deterministic 60 Hz tick that reads orders before it steps and replays to the byte from a
save; fleet-grain orders gated in the simulation with the issuer taken from the subscriber and never
from the message; a view record on the wire that withholds intent; two lanes chosen by what a lost
message costs; a ledger that is asked for rather than broadcast; a jump that carries identity and
damage through a despawn and a spawn; a galaxy that is one seed with chokepoints the graph was
chosen to have; and a gunnery pass with no dice. On that chassis stands a game with no currency, no
item, no site to mine, no build, no fee, no market, and no owner finer than an eight-value faction
byte; whose fleet is a wedge only at the endpoints of a move and dissolves into chases on contact;
whose roster is two working counters short of a counter graph; whose device is exactly one verb;
whose orders are never answered; whose client has no clock of its own; and whose publisher, at a
thousand players in one neighbourhood, sends everything, atomically, from the tick thread, with no
rate governor and no way to resync. Most of that is declared absent by the tree itself. A handful of
items are undeclared and owed today: the `FLEET_STATUS_LAUNCHING`/`Jump` byte collision, the free
and silent total repair on every dock, the F4 debug key that is a real host-side destruction, and
the permanent empire-wide confiscation one attack order on a Vanguard station or its garrison
causes. Nothing found requires tearing a
layer apart; the roadmap is thirty items, twelve of them P0, and every one is a table, a pass, a
message or a record in a shape the tree already uses.

| Discipline | Ready for the goal? | The short version |
|---|---|---|
| Economy & Systems | **No — every seam is built and nothing hangs on them** | An owner, a wallet, an item, a site, a Mine order and a build cost are the six P0s; Homeworld's loop ends there, EVE's needs a journal, priced services, a market, scalar standings and a persistence story on top |
| Combat & Mechanics | **Chassis yes, game no** | Answer the order, measure the tick, budget the wire, publish off the thread, make a device an effect and author the counter graph; then intent, prediction, formations, stances and a second combat quantity |
| Cross-cutting | **Decide before either panel lands** | One ledger row, one owner key, one wire re-lay, one save-format migration path, one prediction contract and one telemetry line are shared by both panels and owned by neither today |

---

## What already serves the goal

Credit first, because these are the decisions a Homeworld or an EVE would have made and here were
made before either was the stated goal.

- **The tick is the simulation's second and the replay is bit-exact.** `TICK_HZ = 60.0f` is a
  compile-time constant (`GameLogic/SimTuning.h:28`) and ADR 0045 buys capacity by entity count
  rather than by lowering it. `Universe::Step` is five ordered passes before motion — dockings,
  jumps, patrols, protectors, fleets — then the index and the neighbour gather
  (`GameLogic/Universe.cpp:2274-2283`), and `ASavedUniverseReplaysToTheSameRun`
  (`Tests/GameLogicTests/UniverseStateTests.cpp:97`) saves a universe, reloads it and replays both
  to byte equality; AGENTS.md:773-775 makes that a hand-back gate. EVE's tick-driven server and
  Homeworld's deterministic replays both start here, and the integer damage path
  (`GameLogic/Universe.cpp:1841-1844`: a saturating subtract, a death on the transition only) is the
  precedent every economic quantity below inherits.
- **Command authority is gated in the simulation, with the issuer taken from the subscriber.**
  `Publisher::ApplyOrders` resolves entity ids and calls
  `IssueFleetOrder(subscriber.faction, fleetOrder.slot, command)` (`GameLogic/Publisher.cpp:200-203`);
  the message never says who it is from. ADR 0014 is EVE's "no client authority, ever", already
  spelled into the seam.
- **Orders name a fleet, not ships** (ADR 0049), the order kinds are declared whole so a byte never
  renumbers (`GameLogic/ShipState.h:205-218`), and `Mine` is a kind already spent and refused
  `Unsupported` at the gate (`GameLogic/Universe.cpp:686-690`) rather than absent — Homeworld's
  harvest order has its byte waiting.
- **The wire carries a view record and withholds intent** (ADR 0009). Steer targets, order facing
  and speed caps are absent from `ShipSnapshot` for every subscriber, and the record says that
  anything the view starts needing "has to be added deliberately, in this file, with a reason"
  (`Design/Decisions/0009-a-snapshot-carries-a-view-record.md:47-48`). That is the door owner-only
  intent walks through below, and it is why prediction cannot become a cheat.
- **Two lanes chosen by one question** (ADR 0029): positions ride datagrams and heal, departures and
  orders ride the reliable stream, and the ledger is asked for (ADR 0051) so a large, private,
  slow-changing value never rides an update. The market reply, the journal reply, the order reply
  and the assets reply below all reuse the request/reply idiom `Publisher.cpp:205-220` already has.
- **A jump is a despawn and a spawn under one identity** (ADR 0056). `Jumper` carries entity, hull,
  faction and `hullPoints` through the gate (`GameLogic/Universe.h:1201-1207`), and `JumpTests`
  pins it. That is the shape a cargo hold, a fit and a docked-with-damage row copy; the tree already
  knows how to carry a ship's state through a door.
- **A station is a ship with a side table** (ADR 0038), an immovable hull is indestructible by a
  `static_assert` that names the design it is waiting for (`GameLogic/HullSpec.h:279`), and a rock is
  presentation with a recorded route into the simulation (ADR 0016). Resource sites, wrecks,
  deployable stations and a Carrier with a hangar are all "one more row plus a side table" on this
  precedent.
- **The galaxy is one seed with a graph chosen for chokepoints** (ADR 0055, 0058): 54 systems, gates
  on the relative-neighbourhood graph, a mean crossing of four jumps, authored once by
  `Tools/UniverseGen` into `Universe.sav`. EVE's high-sec-to-null-sec gradient and Homeworld's
  finite, placed fields both have a map to live on; neither has anything reading it yet.
- **Standing is simulation state, per subscriber** (ADR 0039), keyed on the faction and flipped by a
  stated act (`GameLogic/Universe.cpp:179-192`), and the record itself reserves the widening to
  per-owner rows and a repair path (`Design/Decisions/0039-...:54-57, :74-76`).
- **The save is versioned and a refused one stops the boot** (ADR 0057; `UNIVERSE_STATE_FORMAT = 7`
  at `GameLogic/UniverseSnapshot.cpp:119-120`), the periodic write is atomic
  (`Outpost/Assets/Server.cfg:31-45`), and what a deployment may change lives in one file (ADR
  0043). A persistent shard needs exactly this discipline; it needs a migration path beside it.
- **Gunnery is deterministic and every act is stated** (ADR 0052, 0053): a miss is geometry the
  player can see, a fire event is an instant on a datagram, and the consequence is state that heals.
  Homeworld's ballistic readability is this rule; the effect framework below keeps it.
- **The scaling review already priced the seam.** `Design/Archive/MmoScalabilityReview.md` measured
  interest management at N = 5,000 and named loss amplification and the single-thread publish as
  debts; the tree landed the publisher table and the reliable lane on that schedule. This review
  inherits its method and its register.

---

## Economy & Systems

**The lead's verdict.** The tree has built every seam an economy will hang on — a saved,
faction-scoped, asked-for hull ledger; a cursored append-only log pattern; a deterministic tick with
gather-then-apply passes; an integer-damage precedent; a jump that carries identity and damage
through a despawn and a spawn; and a galaxy with chokepoints and distance that costs time — and it
has zero economy on them: no currency, no item, no site to mine, no build, no fee, no market, no
owner finer than an eight-value faction byte, and a HUD that invents its wallet in the composition
root. Against Homeworld the missing loop is short (sites, a Mine order, cargo, a build cost, a
harvest cycle) and every piece is a table plus a pass in the shape the tree already uses. Against
EVE the loop is the same six P0 items plus an owner identity the tree has ruled out spelling as a
faction, a journal that books destruction, priced station services and a station-local market — and
the tree's own persistence and cross-shard decisions constrain where a wallet may live before the
first credit is minted.

**Principles the roadmap holds to.** Every economic quantity is simulation state that `Step` writes
at 60 Hz, deterministic and replay-equal to the byte (ADR 0045; AGENTS.md:773-775;
`Tests/GameLogicTests/UniverseStateTests.cpp:97`); money, yield, stock and price are whole integers
with one rounding rule, on the precedent `hullPoints` (`ShipState.h:274-278`) and damage
(`DeviceSpec.h:45-52`) already set. Nothing outside GameLogic writes a balance: every faucet and
every sink posts through one function inside the tick, gated in the simulation with the issuer taken
from the subscriber (ADR 0014, 0051; `Publisher.cpp:205-220`). Value that is large, private and
wanted at a moment is asked for; a per-subscriber number wanted every update rides the interest
header (ADR 0039, 0051); prices and inventories never enter the ship record (ADR 0009). Orders stay
at fleet grain and name a fleet, a station, a site or a gate, never a ship (ADR 0048, 0049).
Ownership is a key the simulation compares, and players and corporations are not factions (ADR
0013:44-48; ADR 0047:79-89; `ShipState.h:36`): an `OwnerId` is a second namespace beside
`FactionId`. New content walks the recorded routes in — a hull row plus a side table (ADR 0016,
0038), one more `DespawnCause` (ADR 0040), one more codec block under a format bump (ADR 0057).
Passes that spawn or despawn are gather-then-apply (`Universe.h:888-895`; `Universe.cpp:1435`), walks
are in array order, cadences are tick metronomes like `launchEveryTicks` (`Universe.h:112`;
`Universe.cpp:1337`). The galaxy is one seed and economic geography is appended draws after the
planet loop (ADR 0037, 0055, 0058). A shard is one Universe and no design ever needs a distributed
transaction (`Design/Archive/Universe.md:285-288`; `Design/CrossShard.md:94-96`): a book, an escrow and a
fill are scoped to the station that hosts them.

### 1. Owner identity finer than faction, and admitting an owner after genesis [Missing] [P0] [EVE]

Every ownership fact in the tree is a `FactionId`: the ledger row, the fleet's owner and its five
slots, the subscriber, the standing gate. `FactionId` is a `u8` with `FACTION_LIMIT = 8` tied to the
`hostileMask` (`GameLogic/ShipState.h:17-36`); `DockedShip{hullId, factionId}` is the ledger row and
`LedgerFor` counts `docked.factionId == _asker` (`GameLogic/Universe.h:96-100`;
`Universe.cpp:471-475`); `ComposeFleet` draws by issuer faction (`Universe.cpp:517-520`);
`Fleet::ownerFaction` and `CanTakeSlot(FactionId, slot)` make `FLEET_SLOTS = 5` per faction
(`GameLogic/Universe.h:245-250`; `Universe.cpp:378`; `ShipState.h:50`); a subscriber is "a transport
and a faction that somebody hands in" (`GameLogic/Publisher.h:29-31, :53`;
`Outpost/UniverseSimulation.h:45-49`). The tree has ruled three times that players and corporations
must not be factions and that widening the byte is the wrong move
(`Design/Decisions/0013-allegiance-is-identity-on-the-wire.md:44-48`;
`0047-identity-is-a-shard-scoped-serial.md:79-89`; `0039:44-47`). So one faction of players would
share one wallet, one ledger and five fleets. And the only grant of value is genesis: one `FormFleet`
in `GameLogic/StartingUniverse.cpp:14-32`, "there is no ledger at tick zero", called only from
`Tools/UniverseGen/Generate.cpp:117` and its tests (`Tests/GameLogicTests/StartingUniverseTests.cpp`)
and from no running program (ADR 0058), with `backlog = 1` in
`Outpost/Assets/Server.cfg:16-23`. A second player owns nothing and no function can give them
anything. Declared (`Hostiles.md:520-521`; ADR 0013:44-48; `MmoScalabilityPlan.md:227`) and still
the gate every EVE item below stands behind.

*Proposal.* Add an `OwnerId` — a global 64-bit account or corporation serial minted outside any
Universe, a different namespace from ADR 0047's shard-scoped `EntityId` — and make it the key of
everything owned: `DockedShip{hullId, ownerId}` beside the faction, `Fleet::owner`, the slot table
per owner, every wallet, stock, item and book row that follows. `FactionId` stays what ADR 0013 made
it and standings stay faction-keyed. `Publisher::Desc` carries an owner beside the faction, supplied
by a login the composition root does not yet have; every gate that compares `factionId == issuer`
becomes `ownerId == issuer` inside the same function. Add `Universe::AdmitOwner(OwnerId, home
station)` at the tick boundary: it creates the wallet row and writes the starting grant as ledger
rows in the home station's ledger plus a journal entry, so a new player composes their first fleet
through the door that already exists. Serves EVE; Homeworld's single genesis fleet is what
`StartingUniverse` already does, and a single-owner deployment keys everything with one id.

*Depends on:* account identity and login (the networking/identity panel's work).

### 2. A wallet as simulation state: one integer currency, one posting function, the balance on the header [Missing] [P0] [both]

The only value the simulation knows is a whole hull. `Outpost/Hud.h:71-88` says it in its own words
— "The economy has no home in the universe yet, so its numbers arrive from here" — and supplies
`int credits = 12480`, `creditsPerMin = 42`, `alloy = 3215`, `alloyPerMin = 8`, which
`Outpost/Hud.cpp:226-228` draws as CR and ALLOY. `DockedShip{hullId, factionId}` is the only
owned-but-not-flying value (`GameLogic/Universe.h:96-100`); a grep of GameLogic, Outpost and Tests
for credit, wallet, currency or money finds no monetary code: comments using "currency" for route
stamps and identity (`Universe.h:401`; `PathIslands.h:89`; `UniverseSnapshot.cpp:1395`;
`Publisher.cpp:404`), the view's kill-credit timer `GUN_KILL_CREDIT_SEC` (`Outpost/ViewTuning.h:464`),
the rail's WALLET icon (`Outpost/Hud.cpp:18`) and the HUD placeholders. The type discipline the wallet needs is already stated
for damage: `hullPoints` is a `u32` "bit-exact under every summation order on every machine"
(`GameLogic/ShipState.h:274-278`; `GameLogic/DeviceSpec.h:45-52`), and `ByteWriter`/`ByteReader`
already carry `U64`/`I64` (`GameLogic/UniverseSnapshot.cpp:286-292, :399`). The state codec is at
`UNIVERSE_STATE_FORMAT = 7`, bumped once per added table (`UniverseSnapshot.cpp:119-120`;
`Universe.h:55-58`), and the interest-update header (`UniverseSnapshot.cpp:63-84`) carries
kind through `hostileMask` plus the fleet status block and no balance. Declared: README.md:92-94,
AGENTS.md:92, `Design/Archive/Fleets.md:1018-1020`, `Design/Archive/Universe.md:425-426`. Two refuter
corrections fold in: alloy is a material and belongs in the item table (item 4), not a second
currency; and the wallet's shard home must be chosen (item 15) before the row lands.

*Proposal.* A wallet table in `Universe`: one row per `OwnerId` holding a balance in one currency as
a signed 64-bit integer in minor units, one more block under a format bump. State the money
discipline once beside `FactionId`: `using Credits = std::int64_t`, quantities `u64`, rates as
integer basis points, round toward zero with the remainder to the house, checked arithmetic that
refuses rather than wraps, and a `constexpr` guard in the `EveryLoadoutFitsItsMounts` idiom
(`HullSpec.h:243-250`) that no price, yield or fee column is floating point. Every mutation goes
through one function, `Universe::Post(owner, kind, amount, counterparty, subject)`, that refuses an
overdraft, runs only inside `Step` or at the tick boundary, and appends the journal entry item 7
defines — so faucet minus sink is one subtraction over one log. Put the owner's balance (one `I64`)
in the interest-update header beside `hostileMask` and the fleet block, which passes ADR 0039's test
(small, per-subscriber, wanted every update, self-healing under loss); never put an income rate on
the wire, the client derives it from the journal. Retire the HUD placeholders the day the header
carries the number. Serves both: Homeworld's RU is a wallet paced by harvesting, EVE's ISK is the
unit of account for the whole loop.

*Depends on:* item 1 for EVE (a single owner id suffices for Homeworld); item 15's shard-home
decision before the row lands.

### 3. Resource sites as simulation entities, with resource geography and a record-driven rock [Missing] [P0] [both]

Every rock is client-side presentation: `Design/Decisions/0016-bodies-are-presentation.md:20-24,
:36-40` says "no entity, no table, no snapshot record, no tick that reads one" and records the route
in as a hull row; `Outpost/OutpostApp.cpp:406-409, :541-552` draws six rocks from the client seed
at the radius `ViewTuning.h:280-281` names, `BODY_START_ASTEROIDS` at `ViewTuning.h:374`, and
`Outpost/UniverseView.cpp:1698-1718` distance-culls them by pixel size; F5 rerolls them. `HullId`
ends at `Stargate, Structure` (`GameLogic/HullSpec.h:20-32`) and `Mine` "waits ... for something in
the universe to mine, since a rock is presentation" (`GameLogic/ShipState.h:197-199`).
`Design/Archive/Combat.md:473-479` already prescribes the shape: "a minable rock is a ship on an immovable
hull with a side-table row (resource, remaining units), placed by LayOutSystem so both halves agree
where it is, which today they do not". The galaxy generator draws occupancy, jitter and planets only
— `SystemSite` is starPos, cell, systemSeed, pin (`GameLogic/GalaxyLayout.h:47-55`) and
`GalaxyLayout.cpp:166-173` spends the seed on planet count and orbits — so nothing makes one
system richer than another (`Design/Archive/Universe.md:137-147, :202-207, :425-426`). Three costs the
refuter named: there is no unowned `FactionId` and `Standing::Neutral` is a relation
(`GameLogic/ShipState.h:18-31, :60`); a new `HullId` grows `hullCounts[HULL_COUNT]`, which sizes the
ledger reply (`GameLogic/UniverseSnapshot.h:183-187`); and `Step` walks every record, skipping
immovable hulls for motion only (`GameLogic/Universe.cpp:2271-2303`), inside a forty-ship envelope
(`ShipState.h:52-54`). `Tools/UniverseGen/Generate.cpp:133-138` prints the census a sites line
belongs in.

*Proposal.* Walk ADR 0016's own route in and record a decision that reopens it by its clause. Add
`HullId::Asteroid` (immovable, collidable, non-combatant, `maxHullPoints 0` so the existing
`static_assert` keeps it indestructible) and a side table `ResourceSite{structure, ResourceId kind,
u32 remainingUnits, u32 regenTicks}` made by `MakeResourceSite` as `MakeStation` and `MakeGate` are,
with `SHIP_FLAG_RESOURCE_SITE` on the record. State an ownerless-record rule before the first site —
either an explicit `FACTION_NONE` that `MountTargetStands` and `ChooseMountTarget` never acquire and
`RecordAggression` never judges, or a rule that a site record's faction is not read — pick one and
test it, because today a Vanguard-owned rock is a target an outlaw's escorts pick up under priority
4. Exclude non-ship hulls from what `LedgerFor` counts and from the `ComposeOrder` span, and take
the `HULL_COUNT` growth as a ledger-message and state-format change. Place sites in GameLogic's
layout as appended draws after the planet loop — count, kinds and total units from the system seed,
weighted by graph distance from the home pin over `GalaxyLayout::links` — so the frontier is richer
and rarer ore sits past chokepoints; spawn them in `BuildStartingUniverse` so UniverseGen writes
them into `Universe.sav` and prints a sites-and-units census. Sites deplete by extraction and
regenerate on a per-site tick counter advanced by the industry pass (item 6), whole integer units.
Set a per-system site cap and measure the cost of static records in the index, gather and publisher
at the shipped count before widening it. On the client, a flagged record registers to a
`BodyCatalogue::RandomBody` seeded from its `EntityId` and sized from the `HullSpec` capsule so the
collision rock and the drawn rock agree; remaining units ride the `hullFraction` byte; a site is
never distance-culled below the size an order can be aimed at. Serves both: Homeworld's harvestable
field paces a campaign, EVE's belt is the root every player-built item traces back to.

*Depends on:* nothing.

### 4. Items, cargo holds and station stock: the noun a player can own, carry, store and count [Missing] [P0] [both]

A device is a column of the hull table, not a thing a player holds: `MountSpec`/`MountLoadout` are
authored per hull (`GameLogic/HullSpec.h:44-59, :71-88`) and `DeviceKind` is `{Gun, MiningTool}`
(`GameLogic/DeviceSpec.h:18-34`). `ShipState` is position, heading, order, hull, faction and
`hullPoints` with no cargo (`GameLogic/ShipState.h:227-279`); `HullSpec` has no hold column and the
Miner and Hauler are `LOADOUT_NONE` (`GameLogic/HullSpec.h:90-141, :227, :229`). `Station` holds
owner, garrison, targets and docked rows and no stock (`GameLogic/Universe.h:96-100, :125-143`);
`LedgerReply` is `hullCounts[HULL_COUNT]`, "the array IS the format"
(`GameLogic/UniverseSnapshot.h:178-187`). What crosses a jump is entity, hull, faction and damage
(`GameLogic/Universe.h:1201-1207`; `Universe.cpp:1149, :1174-1181`; `Design/Archive/Universe.md:268-273`;
`Design/CrossShard.md:105-112`), so a full Hauler would arrive empty by ADR 0056's own rule.
`Design/Archive/Combat.md:474-480` hands "cargo, unloading into a ledger, and the meaning of a full hold" to
a mining design that does not exist (`Design/Archive/Stations.md:861-863`;
`Design/Archive/Fleets.md:1018-1020`); `Tests/GameLogicTests/UniverseStateTests.cpp:250-294` pins
the two-field docked row; AGENTS.md:92-93 still says no format versions the codec, stale against
`UniverseSnapshot.cpp:119-120`. Homeworld needs only one resource unit and a hold; EVE needs the
whole table.

*Proposal.* A small fixed `ItemId` table beside `HullSpec` and `DeviceSpec` (ore grades first,
refined material, then hulls and devices as items so a shipyard can sell a Corvette), declared whole
like `HullId`, with an `ItemSpec{volumeUnits, ...}` row in the replay contract; the HUD's "alloy"
becomes an item row. On `ShipState`: `ItemId cargoKind` and `u32 cargoUnits`; on `HullSpec`: a
`holdUnits` column (Miner a few cycles' worth, Hauler an order of magnitude more, warships 0). On
`Station`: `ItemStack{ItemId, OwnerId, u64 quantity}` rows beside the docked rows, with `DockedShip`
kept as the hull-shaped view of the same store so `ComposeFleet` keeps working. Cargo is state, not
intent: it crosses a gate in the `Jumper` beside `hullPoints` and in the cross-shard outbox entry,
and dies with the hull; station stock stays where it is docked. The dock pass's capture deposits a
ship's cargo into the owner's stock before it appends the `DockedShip` row. Widen the asked-for
reply to a typed list with a count (reader failing closed on unknown ids), and add a second
asked-for subject, "my assets, everywhere" keyed by owner. The ship record gains a `cargoFraction`
byte; a `FleetStatus` cargo pip can use bits 3-5 of the status byte — which item C1 below re-lays,
so the two must be budgeted together. Pin a `JumpTests` row (a full Hauler arrives full) and a
`UniverseStateTests` row (cargo and stock survive the save). Serves both: Homeworld's RU return trip
and EVE's ore hold are the same two integers; the table beyond ore is EVE's.

*Depends on:* item 1 for per-owner stock (faction-keyed stock is acceptable for a Homeworld
deployment only); item 3 for what an item is and where it comes from.

### 5. The Mine order and the extraction cycle: a MiningTool row and a fleet-grain Mine with its own stands rule [Missing] [P0] [both]

`FleetOrderKind::Mine` returns `FleetOrderResult::Unsupported` (`GameLogic/Universe.cpp:686-690`;
`GameLogic/ShipState.h:197-212`); `MiningTool` has no row and "the fire pass skips any mount
carrying one" (`GameLogic/DeviceSpec.h:24-34`; `Universe.cpp:1779-1782` `if (device.kind !=
DeviceKind::Gun) continue;`); the Miner is `LOADOUT_NONE` with "The mining design arms the Miner
with tools on these same mounts" (`GameLogic/HullSpec.h:222-227`;
`Tests/GameLogicTests/CombatTests.cpp:284` pins `MountCount()==0`); the sheet omits MINE
(`Outpost/FleetSheet.h:61-63`). The extraction machinery is the fire pass by `Design/Archive/Combat.md
:460-481` §12's design, and `ChooseMountTarget` priority 2 already resolves `row.orderTarget`, but
`MountTargetStands` rejects any target of the shooter's own faction — "not its own faction, ever"
(`GameLogic/Universe.h:962-971`) — and every priority resolves to a hostile ship, so a tool needs a
stands rule of its own. `FleetOrderResult` never crosses the wire (`GameLogic/Universe.h:706-707`;
no use under Outpost/ or NeuronServer/); the dock affordance refuses client-side by `IsHostileToMe`
(`Outpost/UniverseView.cpp:1291-1294`). Declared: `Design/Archive/Fleets.md:492-501` §6.6 "a kind
reserved, a design owed".

*Proposal.* `DeviceId::MiningLaser{kind MiningTool, rangeMetres ~120, cooldownTicks one cycle,
damage reinterpreted as yieldUnits, slow or fixed traverse}` and `LOADOUT_MINER` with two bow tool
mounts while `combatant` stays false. Extend `FleetCommand` with a site `ShipId` and give
`IssueFleetOrder` a Mine branch gated as Dock is (`NotASite` when the record has no `ResourceSite`
row); the standing order lowers as an Attack does — Miners pursue the site to the standoff of their
tool range, escorts fly formation on the Miners and keep the existing defence. In `StepMounts`
branch on `device.kind`: a `MiningTool`'s target is the fleet's ordered site only (no opportunistic
acquisition, no hostility gate, ships of any faction may work one rock) and its "shot" is a
saturating integer transfer `site.remaining -> ship.cargo` capped by `holdUnits`, recorded beside
`ShotRecord` so the client draws a beam from the same `FireEvent` datagram. The sheet gains MINE
beside DOCK, arms on a site tap, and refuses client-side on the site flag the way DOCK refuses on
standing. Inside the tick, gather-then-apply, deterministic by construction. Serves both:
Homeworld's harvesters and EVE's mining barges are one fleet-grain order with different tool numbers.
Item C10 below widens the same device row into an effect descriptor with a `Resource` target class;
the two panels agree on the shape and this item is the first row through that door.

*Depends on:* item 3; item 4.

### 6. Hull production: a cost table, a BuildOrder, an industry pass and a build queue that feeds the existing ledger [Missing] [P0] [both]

Every `SpawnShip` caller runs at genesis, relaunches a garrison ship, launches a hull already
composed out of a ledger, or re-spawns a jumper under its identity (`GameLogic/Universe.cpp:1482`,
`:1174-1181`, `:1345`; `GameLogic/StartingUniverse.cpp:18-19, :28, :54, :77-79, :99, :113`); the
only append to a station's ledger is the dock pass (`GameLogic/Universe.h:143` "the ledger: who is
inside. Filled by the dock pass."; `Universe.h:705`). `DockShips` is the only way a test fills a
ledger (`Tests/GameLogicTests/FleetTests.cpp:81-100`); `STARTING_FLEET = {Bomber, Corvette,
Frigate}` (`GameLogic/StartingUniverse.h:66`); `HullSpec.h` has no cost, price or value column.
Nothing the player can do creates a hull, so the total can only fall, mining would have nothing to
pay for, and destruction is a countdown rather than a sink. The standing-intent slot where an
industry pass belongs is `Universe.cpp:2271-2277`. Declared: `Design/Archive/Hostiles.md:522-523`
"Spawner and respawn systems, station lifecycle, hostile production" left out; `Outpost/Hud.h:62-67`
Research/Wallet/Storage screens not built.

*Proposal.* A `constexpr HullCost` table beside `HullSpec` (per `HullId`: units of each `ItemId`,
credits, `buildTicks`), in the replay contract. A `BuildOrder{station, hullId}` on the reliable
lane, metered by `ordersPerTick`, gated in the simulation: `RefusedStanding`, `NoSuchHull`,
`Unaffordable`, `QueueFull`. Acceptance debits stock and wallet at once through the posting function
and appends `BuildJob{hullId, owner, ticksLeft}` to the station's queue. A `StepIndustry` pass in
the standing-intent slot — a metronome `INDUSTRY_EVERY_TICKS` in `SimTuning.h`, walking the station
table in array order, advancing jobs, site regeneration (item 3) and refine batches (item 9) by
whole units — appends a `DockedShip` row at zero, so a built hull flows through
`LedgerFor`/`Compose`/launch with no new spawn path. Vanguard stations build for anyone they do not
hold hostile, at a fee. Blueprints and research follow as a later slice of the same pass: a
per-owner unlocked-hull mask defaulting to all-ones, checked by the `BuildOrder` gate. Serves both:
Homeworld builds hulls from harvested RU; EVE's premise is that every hull was built by a player from
mined material.

*Depends on:* item 4; item 2.

### 7. A loss and value journal: destruction booked with owner, hull and killer-of-record; the garrison flag; F4 fenced; economy counters [Missing] [P1] [EVE]

Every value move is a table mutation with no journal. The two logs the tree has carry no owner or
amount: `DespawnRecord{handle, entity, cause}` is cursored and trimmed by the publisher's minimum
cursor (`GameLogic/Universe.h:36-44, :336-360`; `Universe.cpp:100-110`), and `ShotRecord{shooter,
victim, mount}` is "Deliberately NOT in the save format" (`GameLogic/ShipState.h:179-184`;
`Universe.h:360-376`). The death transition pushes only `shot.victim` and discards `shot.shooter`
(`GameLogic/Universe.cpp:1840-1843`); step 4 is `DespawnShip(Destroyed)` and nothing else
(`:1866-1873`); `ComposeFleet` compacts rows with no record (`Universe.cpp:540-556`). Two hazards
the refuter found: F4 calls `m_universe.DespawnShip(handle)` on the host with the default
`Destroyed` cause and its comment "there is no health, no damage" is stale since combat landed
(`Outpost/OutpostApp.cpp:764-787`; `GameLogic/Universe.h:321-324`), so any payout keyed on the
cause alone pays out on a keypress; and "a protector drops nothing" is restated in
`Design/Archive/Stations.md:557-563, :640-645`, `Design/Archive/Combat.md:313-315`,
`Design/Decisions/0041:80-82` and `Tests/GameLogicTests/ProtectorTests.cpp:201-202` and coded
nowhere, while `LaunchedProtectorCount` is derived so a dead protector is replaced next launch tick
(`GameLogic/Universe.h:554-562`; `Universe.cpp:130-134`). `StepFleets` retires a fleet at
`memberCount==0 && manifestCount==0` and zeroes a stranded manifest silently
(`Universe.cpp:1354-1381, :1397-1404`). `RoutePlanCount` and two siblings are the only simulation
counters, "a readout, never read by the simulation" (`Universe.h:818-821, :1256-1258`;
`Outpost/Hud.h:28-55`). Kill attribution on the wire was deliberately deferred
(`Design/Archive/Combat.md:373-375, :568`; `Design/Decisions/0053:64-66`); ADR 0027 is the cursor pattern.

*Proposal.* A journal in the despawn log's shape, saved with a base sequence like `m_despawnBase`:
`LedgerEntry{u64 tick; OwnerId owner; EntryKind kind; ItemId or currency; i64 amount; OwnerId
counterparty; EntityId subject; shooter-of-record (may be none); bool garrison}`, appended only
inside `Step` or at the tick boundary so a replay produces the identical journal. Every posting
(item 2) writes one; a `Destroyed` despawn of an owned hull writes one before the row is popped,
capturing `shot.shooter` at the step-2 transition and the garrison flag from `m_protectors`; a
stranded manifest writes one per hull instead of `manifestCount = 0`. Two readers behind per-reader
cursors — the publisher answering an asked-for "journal since cursor" per owner, and a persistence
drain (item 15) — and trim only behind both. Make the garrison rule a tested invariant now, before
any kill-keyed faucet: every bounty, wreck, insurance payout or standings gain refuses a flagged
row, a row with no shooter, or a same-owner shooter — one predicate, pinned in `ProtectorTests`.
Fence F4 off a dedicated server through `ServerConfig` (ADR 0043) and fix its comment. Keep an
`EconomyCounters` block beside the journal (cumulative issued and sunk per category per owner,
money supply), sampled by the composition root on the `saveEveryTicks` cadence into an economy log
beside `Universe.sav`; the HUD's `creditsPerMin` reads the real delta. Serves EVE: destruction is
only a master sink if the destroyed value is a number somebody can total. Homeworld needs only the
harvest/spend counter pair from the same block.

*Depends on:* item 1; item 2; item 6's `HullCost` so a loss can be priced.

### 8. Multi-leg standing orders: the harvest cycle that unloads without dismantling, and a route that ends in a dock [Change] [P1] [both]

A fleet that docks is captured, despawned and dismantled into the ledger; "That is the only way out
of a ledger: undocking as such still does not exist" (AGENTS.md:49-50;
`Design/Archive/Stations.md:432-486` §7.3, `:866-867`); the dock pass "is the pass that despawns"
(`GameLogic/Universe.h:888-892`; `Universe.cpp:1028-1038`). A `Fleet` row holds one `orderKind`
and one point/station/target/gate (`GameLogic/Universe.h:245-296`;
`Design/Archive/Fleets.md:1024-1025` "Order queues and waypoints. One standing order"), and
`Fleets.md:216` is the Miner+Hauler+escorts doctrine that needs more than one. A full Miner
therefore has no way to drop ore and go back, which is the whole of Homeworld's harvester feel; and
a trade run across a mean of 4.07 crossings (`Design/Archive/Universe.md:413-414`) is babysat jump by jump,
with "dock at that station" excluded even from `Design/GalaxyMap.md:112-113`'s draft (`:3`
"drafted, not yet agreed"; `:81-95` §4.3 the route on the fleet row; `:133-140` slice 3
undelivered). The loop technically closes without this (a full Miner docks, a new fleet is
composed), which is why it is P1.

*Proposal.* Split the capture rule by the fleet's standing order: a Dock order still despawns and
dismantles (unchanged, tested), while a Mine order's members reaching `DOCK_CAPTURE_METRES` of the
fleet's drop-off station transfer cargo into the owner's stock and turn back without despawning.
The Mine order carries `orderSite` and `orderStation` plus a phase byte (Extracting, Returning,
Unloading) the codec carries and `StepFleets` advances; Stop or any other order ends it. A Hauler
shuttling on its own while the Miners keep drilling is the second slice. Separately, agree and land
GalaxyMap slice 3 as drafted and add one terminal action: the route ends in a Dock at a named
station `EntityId` resolved on arrival through the existing `IssueFleetOrder` gates so
`RefusedStanding` still applies at the door. Keep the patience rule; ADR 0049 holds. The harvest
cycle serves Homeworld above all; the route-to-dock serves EVE's logistics, and Homeworld's single
map does not need it. Item C11's order queue is the general form of the same widening and should
be designed in the same document.

*Depends on:* item 5; item 4; `Design/GalaxyMap.md` slice 3.

### 9. Refining: ore becomes material through a station process with a yield and a fee [Missing] [P1] [EVE]

There is one placeholder material (`Outpost/Hud.h:87-88` `int alloy = 3215; int alloyPerMin = 8;`)
and no process that turns anything into anything: `StationDesc` is `ownerFaction` plus four
garrison fields (`GameLogic/Universe.h:105-115`), and a grep for refine, blueprint or industry across
GameLogic finds nothing. `Design/Archive/Stations.md:862-865` names "cargo, repair, trade. Next
phase"; `Design/Archive/MmoScalabilityPlan.md:136` "No combat, economy, or content systems". EVE's
industry chain is ore to minerals to item, and the refine step is where yield, station quality and a
fee sink live. Homeworld collapses it to RU and is served by a 1:1 identity row, so the pass costs
the Homeworld shape nothing.

*Proposal.* A `RefineSpec` row in the item table `{ItemId in; u32 unitsIn; ItemId out; u32
unitsOut; u32 ticks}` and a `RefineOrder{station, spec, batches}` on the reliable lane, gated by
the issuer's standing at the station and the issuer's own stock, refused whole or accepted whole
like `ComposeFleet`, debiting the fee through the posting function. `StepIndustry` (item 6)
advances batches by whole units in array order (`Universe.cpp:2271-2277`) and lands output in the
same stock rows. `StationDesc` gains a capability mask (refines, builds, hosts a book) so genesis
says which Vanguard stations are refineries; the mask is what the system profile (item 12) varies.

*Depends on:* item 4; item 6.

### 10. Wrecks and salvage: a death leaves a site behind, through its own DespawnCause, spawned after the death loop [Missing] [P1] [both]

`DespawnCause` is `{Destroyed, Docked, JumpedOut}` and its header says "Wreck-and-salvage and
capture are each one more" (`GameLogic/Universe.h:22, :29-34`); step 4 despawns each dead handle
with `Destroyed` and leaves nothing (`GameLogic/Universe.cpp:1872-1873`);
`Design/Archive/Combat.md:566-567` says the same. Two refuter corrections: the client shatters on every
`Destroyed` departure — "only a death detonates" (`Outpost/UniverseView.cpp:277-281`) — so a decayed
wreck must leave through a cause of its own; and step 4 iterates the death scratch while
`DespawnShip` swap-pops and `SpawnShip` may reallocate `m_ships` (`Universe.cpp:1435`), so wreck
spawns are gathered with the deaths and applied after the loop. The garrison keying
`Design/Archive/Stations.md:640-646` §8.6 asks for is `m_protectors[victim].active`
(`Universe.cpp:1862`). EVE's economy needs the partial return path a wreck gives; Homeworld's
salvage corvettes are a fleet-grain harvest of the same kind.

*Proposal.* Reuse the site table: `HullId::Wreck` (immovable, `collidable = false` so a fight is
not littered with obstacles, indestructible by the `static_assert`) at the dead ship's position
with a `ResourceSite` row whose remaining units are an authored fraction of the dead hull's
`HullCost` and a decay counter advanced by `StepIndustry`. A `MiningTool` works a wreck exactly as
it works a rock. A garrison ship spawns no wreck, and the journal's garrison flag (item 7) is the
same predicate. A wreck whose counter reaches zero leaves through `DespawnCause::Decayed`, which the
client fades rather than detonates. Deterministic because the spawn happens inside the tick in
death order.

*Depends on:* item 3; item 5; item 7; item 6.

### 11. Priced station and gate services: a ledger row that remembers the ship, repair as a sink, dock and hangar fees, jump tolls, consumables [Change] [P1] [both]

The dock capture is `Capture{HandleOf(id), station, hullId, factionId, garrison}` — no entity, no
`hullPoints` (`GameLogic/Universe.cpp:1028-1038`; `:1060-1062` pushes `DockedShip{hullId,
factionId}`) — and `SpawnShip` sets `hullPoints = maxHullPoints` (`Universe.cpp:48`; `:1482` the
launch calls it). Every dock is a free, instant, total repair and the ship's ADR 0047 identity ends
at the airlock — while the jump pass already carries exactly those fields through its own
despawn/spawn (`GameLogic/Universe.h:1201-1207`; `Universe.cpp:1149, :1181`;
`Tests/GameLogicTests/JumpTests.cpp:264`) and no equivalent test exists for docking. The same path
returns a damaged protector to its complement whole ("the hull returns to the complement by simply
stopping being counted", `Universe.cpp:1035-1037, :1345`), so a station's defence can never be
attrited. `Design/Archive/Stations.md:490-494, :836-838` asked that "the ledger must carry whatever
a future ship-identity needs"; `Design/Archive/Combat.md:289-291` says hull points are "never regenerating
— repair is a station-menu design". Beyond repair, everything is free: `IssueDockOrder`'s gates
are live-row, own-ships, standing (`GameLogic/Universe.h:523-534`); `ComposeResult` has no
economic value (`:674-682`); `GateDesc::ownerFaction` is "Read by nothing this phase"
(`Universe.h:195-206`; `Universe.cpp:681-684`); guns need no ammunition and hulls no fuel
(`GameLogic/DeviceSpec.h:37-64`; `Universe.h:173-178`; `Design/Archive/Combat-slice-1.md:435-436`). The
combat panel found the same free repair independently (item C14); it is undeclared and owed today.

*Proposal.* Give the ledger row the `Jumper`'s shape: `DockedShip{entity, hullId, ownerId,
factionId, hullPoints}`, the launch spawns through `SpawnShipAs` at the stored value, and
`UniverseStateTests` pins the wider row. Repair becomes an explicit priced service: a per-point
price per hull class at the station owner's rate, posted as a sink, with `CannotPay` beside the
existing compose and dock results and the quote asked for under ADR 0051; a Homeworld deployment
prices repair at zero in content but keeps the damage. A fee table on `StationDesc` (dock fee per
hull class, hangar upkeep per row per interval on a coarse metronome, launch fee) credited to the
station owner; unpaid upkeep impounds the row rather than deleting it. A toll per hull class on
`GateDesc`, debited at the capture walk before any despawn so the fleet pays as a whole or does not
cross (ADR 0056's atomicity), placed inside the gate-ownership design `Universe.md` names.
Optionally per device row a magazine and charge cost decremented by the fire pass (an empty mount
does not fire, ADR 0052's explainable refusal), refilled at dock, with 0 = infinite as the
Homeworld off-ramp; fuel is a per-jump charge, never per-tick burn. Serves EVE's per-fight and
per-trip sinks; serves Homeworld through persistent damage and the RU-per-jump cost Homeworld 1
charged, and Homeworld does not need the owner-credit half, the upkeep or ammunition.

*Depends on:* item 2; item 1; the station management menu (Stations.md §14, undesigned); item 14
for the toll.

### 12. A station-local market: a per-system economic profile, a station that knows its system, an order book, and NPC seeding [Missing] [P1] [EVE]

The word "market" occurs once in Design/, GameLogic/ and Outpost/ and means a device choice
(`Design/Archive/Combat-slice-5.md:101`); "order book", "toll" and "sovereign" return zero hits. The
reliable lane carries `FleetOrder`, `FleetRoster`, `LedgerRequest/Reply` and `ComposeOrder` and no
price field (`GameLogic/UniverseSnapshot.h:140-197`). Genesis stamps one `StationDesc` on every
`PlanetSite` of every `SystemSite` (`GameLogic/StartingUniverse.cpp:44-58`), `SystemSite` has
nothing economic (`GameLogic/GalaxyLayout.h:47-57`; `GalaxyLayout.cpp:166-173`), and a station does
not know which system it is in — `Station` and `GateDesc` carry no system index and `SystemAt` is
a nearest-star linear scan (`GameLogic/Universe.h:125-144, :197-206`;
`GalaxyLayout.cpp:177-199`). `StationDesc` is owner plus garrison (`Universe.h:105-116`), and the
per-station tick cadence a replenishment metronome copies is `launchEveryTicks` (`Universe.h:112`;
`Universe.cpp:312, :1337`). `Design/Archive/Universe.md:8, :204-207, :412-416` names "54 systems, 164
Vanguard stations" and "routes worth knowing the day anything is traded"; ADR 0015 puts NPC intent
inside the tick; ADR 0055:28-30 makes a profile from the system seed retunable without a reroll.
Player-to-player exchange is EVE's centre; Homeworld has no exchange, though a resource-rich system
that paces a campaign is the same profile at one commodity.

*Proposal.* Derive a per-system economic profile as a pure function of `systemSeed` under ADR
0037/0055: richness per ore grade (item 3's draws), station role (refinery, shipyard, hub, outpost —
item 9's mask) and a security tier by graph distance from the home pin; genesis varies `StationDesc`
per site and writes the `SystemSite` index onto station and gate rows as one codec field. Build
the order book as a station side-table row (ADR 0038's pattern), simulation state in the save.
Buy and sell orders arrive on the reliable lane, issuer from the subscriber, matched at the tick
boundary in a fixed order — station index, then price, then arrival serial — so replay is
byte-equal. Escrow item and currency into the book row on placement so a fill never touches state
outside one Universe's tick; a broker fee and a sales tax post as sinks. Seed each station role
with standing NPC buy and sell orders of bounded depth, replenished on the launch-metronome shape,
as ordinary rows under the owner faction so matching has one path. Read by
`MarketRequest/MarketReply` under ADR 0051's test, never in the header. Contracts (courier, item
exchange, escrowed between two owners) are the later slice on the same rows and the mechanism for a
cross-shard purchase. Land GalaxyMap slice 4's names and designations from the same generator so a
trader can say where.

*Depends on:* item 4; item 2; item 1; item 3.

### 13. Standings as a scalar with a repair path, and a rule for the assets a hostile standing locks away [Change] [P1] [EVE]

`Standing` is `{Neutral, Hostile}` in a `rows[FACTION_LIMIT][FACTION_LIMIT]` table
(`GameLogic/ShipState.h:58-62, :69-74`); `RecordAggression` flips `rows[owner][attackerFaction] =
Hostile` "permanently and empire-wide" (`GameLogic/Universe.cpp:179-192`; `Universe.h:458-464`)
and `StandingOf` is a plain read with no repair path (`Universe.cpp:156-164`). The consequence the
refuter named and every specialist missed: `LedgerFor` answers zeros to a hostile asker and
`ComposeFleet` returns `RefusedStanding` (`Universe.cpp:465-471, :491-492`), the fleet launch is
"the only code that turns a ledger row into a ship" (`Design/Archive/Fleets.md:373-375`), and
`AHostilePortRefusesComposition` pins three rows untouched and unreachable
(`Tests/GameLogicTests/FleetTests.cpp:673-690`) while `StationTests.cpp:145-149` pins that 1000
ticks do not forgive. One attack order against a Vanguard station or one of its garrison ships —
the only victims for which the fire pass calls `RecordAggression` (`Universe.cpp:1856-1866`); a shot
at a Vandal patrol flips nothing — by anyone in `FACTION_PLAYER` closes every Vanguard port in
54 systems to the whole faction for the life of the universe, and every hull the faction has docked
anywhere is confiscated silently, with no record and no recovery. ADR 0039 reserves the widening
("a keyed row and a different lookup, in the same functions"; "no decay, no fines, no amnesty ... a
standings-repair design is where it changes", `Design/Decisions/0039:54-57, :74-76`) and
`Design/Archive/Stations.md:172, :873-874, :900` decision 3 is the rule to reopen. Every standing
read is an equality against `Standing::Hostile` (`Universe.cpp:471, :491, :659, :962, :1026`), so a
threshold-derived bit keeps them reading one byte. EVE's NPC stations never confiscate and its
structures have asset safety; Homeworld's standings are campaign script.

*Proposal.* Widen `Standing` to a signed scalar per (owner, other) where "other" is a faction or an
`OwnerId`, mutated only by stated acts the simulation observes (aggression down; mission
completion, taxes and fines paid up), never by a client message; derive the wire's `hostileMask`
bit from a threshold. Add the one repair path that is also a sink: a fine priced from the journal's
record of what the aggressor destroyed of the owner's (item 7), paid at a neutral third party's
station, flipping the row back across the threshold; record it as superseding ADR 0039's "no fines"
and Stations §15 decision 3, and retarget the StationTests row. State the locked-asset rule
explicitly: rows a hostile standing makes unreachable are impounded, listed in the owner's assets
reply, and released on repair or moved to the nearest station whose owner will take them after a
stated delay — EVE's asset safety in the tree's terms — so "unreachable for the life of the
universe" is never a silent outcome.

*Depends on:* item 1; item 7; missions as a faucet (outside this panel).

### 14. Persistence a persistent economy can survive: input-log recovery, a migration rule, and the wallet's shard home [Change] [P1] [EVE]

The save is one atomic whole-universe file every 1800 ticks (`Outpost/Assets/Server.cfg:31-45`
"how much progress a power cut may cost"; `Outpost/OutpostApp.cpp:612-636, :1017` the only
trigger) with no migration: "there is nothing to migrate from yet" (`GameLogic/UniverseSnapshot.cpp
:117-120`), `Read` refuses an unknown format byte (`GameLogic/UniverseSnapshot.h:519-523`; ADR
0057), and `Design/Archive/MmoScalabilityPlan.md:633` puts "versioning beyond a format byte;
persistence scheduling" out of scope. Six bumps (formats 2 through 7, listed at
`UniverseSnapshot.cpp:120`) landed in three days (2026-08-30 to 2026-09-01) and a wallet and item
schema will move as often. The refuter's correction is load-bearing: a money-only write-ahead journal
replayed onto a stale snapshot leaves the wallet one tick old inside a world thirty seconds old,
the "half a universe" ADR 0057 refuses; the tree's own instrument is byte-exact replay from a
snapshot (`Tests/GameLogicTests/UniverseStateTests.cpp:97, :471`; AGENTS.md:773-775), so the
recovery log is the input log. And a wallet has no home once there are two universes: an owner's
five fleets (`GameLogic/ShipState.h:50`, each crossing whole and independently under ADR 0056) may
be on five shards at once, `Design/CrossShard.md:80-104` §4 carries `Jumper` records "in the outbox
and nowhere else" and forbids a distributed transaction (`:94-96`), §5 lets only owner, slot and
membership survive (`:105-115`), §7 ties one client to one connection (`:128-136`), and a grep of
CrossShard.md for ledger, wallet or docked returns nothing. EVE keeps one cluster-wide ISK wallet;
any per-shard or per-station wallet here is a knowing departure forced by the shard design.
Homeworld's campaign tolerates a 30-second rollback and a fresh start per version.

*Proposal.* Keep ADR 0057 intact and add beside it: the composition root drains every input at its
tick boundary (orders, ledger and market requests, compose, build, refine, admit, handoff applies)
to an append-only log between ticks, and recovery loads the last snapshot and replays the log past
its tick under the byte-equality contract — money, stock and hulls all recover consistently with at
most one tick lost. Add a migration rule: a reader per known older format or an explicit UniverseGen
step that rewrites a file forward. Decide the wallet's home before item 2 lands: a balance is keyed
to the client's one session shard (CrossShard §7) or to a settlement store outside every Universe
reconciled from each shard's journal drain (item 7), with the journal carrying the shard in every
entry; it never rides a fleet handoff, and no tick ever calls a wallet service. Record the choice as
a departure from EVE's single wallet, made for CrossShard's no-distributed-transaction principle.

*Depends on:* item 7; `Design/CrossShard.md` slices 1-5 (networking panel).

### 15. Sovereignty: gate ownership read, player stations, and what a station's death books [Missing] [P2] [EVE]

`Station::ownerFaction` is written only by `MakeStation` (`GameLogic/Universe.h:125-132`;
`Universe.cpp:309`); `NoImmovableHullIsDestructible` pins every immovable hull to `maxHullPoints 0`
"which no design has yet said what to do about" (`GameLogic/HullSpec.h:253-256, :266-274, :279`).
`GateDesc::ownerFaction` is "Read by nothing this phase" (`Universe.h:203-205`; `Universe.cpp:328`
its only write; `:681-684` no standing check on Jump; ADR 0056:67-68 turned gate standings down as
half a design), so the chokepoints ADR 0055 chose the graph to have — "a cut link can genuinely
divide the map" (`GalaxyLayout.h:242-247`) — cannot be held, tolled or closed. When
destructibility arrives, the station's death is the largest single destruction event in the game:
today `manifestCount = 0` when the door is gone (`Universe.cpp:1397-1404`;
`Tests/GameLogicTests/FleetTests.cpp:813` the only path that reaches it;
`Design/Archive/Fleets.md:305-309` "may prefer the manifest to fall back into wreckage or a
refund"), and the ledger's fate is named with options listed and no accounting
(`Design/Archive/Combat.md:310-313, :564-565`; `Design/Archive/Stations.md:871-872`;
`GameLogic/UniverseSnapshot.h:82-83` seven flag bits kept for user and conquerable stations; ADR
0038:35-36). The garrison is bottomless, free and self-repairing (`Universe.h:112-115`;
`Stations.md:557-563`), which forecloses the siege. Homeworld's persistent fleet needs no territory.
P2 because everything here stands behind identity, standings and the market.

*Proposal.* Read `GateDesc::ownerFaction` in the Jump branch the way Dock reads the station owner
once standings are a scalar (item 13), with the toll from item 11 collected at the capture walk. A
deployable station: a hull row immovable once anchored but not indestructible (relaxing
`NoImmovableHullIsDestructible` for that hull and naming the change), made a station by
`MakeStation` under an `OwnerId`, with its role (item 12) deciding what it hosts. Contest as a
stated act the simulation observes — structure hull at zero, or a capture timer while an enemy fleet
holds the doorstep and no defender does. Write the accounting before the first destructible
station: every manifest returns to the ledger first, then each owner's ledger, stock and book rows
resolve through the journal as destruction entries with the killer or move to an owner-keyed
asset-safety holding at the nearest friendly station after a stated delay — never `manifestCount =
0`. Give the garrison a finite stockpile NPC production refills at a metered rate, and let docked
protectors keep their damage (item 11), so a siege can exhaust a station.

*Depends on:* item 13; item 1; item 7; the station destructibility design (combat panel, item C13's
mobile-station record is the same door).

---

## Combat & Mechanics

**The lead's verdict.** The chassis is right for both benchmarks and unusually honest about itself:
a deterministic 60 Hz tick that reads orders before it steps, fleet-grain orders gated in the
simulation, a view record on the wire that withholds intent, two lanes chosen by what a lost message
costs, and a gunnery pass with no dice. What is missing is everything the benchmarks are made of on
top of that chassis: nothing answers an order, nothing predicts, the client has no clock of its own,
the fleet is a wedge only at the endpoints of a move, the roster is an escalation ladder with two
working counters, a device is exactly one verb (subtract hull points), and at a thousand players in
one neighbourhood the publisher sends everything, atomically, from the tick thread, with no rate
governor and no way to resync. None of it contradicts the tree's decisions; most of it is declared
absent, and a handful of items (the LAUNCHING/Jump status-byte collision, the free repair on dock)
are undeclared and owed today.

**Principles the roadmap holds to.** The tick is the simulation's second: `TICK_HZ = 60` stays
fixed, no substepping (ADR 0045; `SimTuning.h:28`); a load governor scales wall time against ticks
and never scales the tick, so replay and every tick-counted cadence (`InterestSet.cpp:22-23`;
`Publisher.h:59-62`) are untouched. Bit-exact replay is the gate: every per-tick counter lives in
`UNIVERSE_STATE_FORMAT` (`UniverseSnapshot.cpp:119, 1432-1448`), the outcome path stays integer
(`ShipState.h:270-278`; `Universe.cpp:1840`), and every effectiveness or resistance is an integer
percent. Orders are inputs at the head of the tick, name a fleet (ADR 0049;
`UniverseSimulation.h:60-66`) and are gated in the simulation (ADR 0014); affordances tell the truth
first and the server refuses anyway (`UniverseView.cpp:1290-1301`); a reply reports the gate and
never moves it. The wire carries a view record (ADR 0009; `UniverseSnapshot.h:40-46`); owner-only
intent is faction-gated per subscriber. Lanes are chosen by whether a later message makes a lost
one right (ADR 0029) and consequences travel as state while instants ride datagrams (ADR 0053).
The publisher is outside the replay contract — it changes what is sent, never what is simulated
(`Publisher.h:33-34`; ADR 0030) — so budgets, priorities, off-thread encoding and per-role
descriptors need no format bump. The ledger idiom (ADR 0051; `Publisher.cpp:206-210`) is reused for
every new request/reply. A miss is geometry the player can see (ADR 0052; `Combat.md:167-169`), and
reversing that argument needs a superseding record. Bytes never renumber (`ShipState.h:214-217`;
`DeviceSpec.h:24`); the ALPN bumps when a record changes width (`Combat.md:379-381`; ADR 0053:83).
Simulation tuning is the replay contract and presentation is free (`SimTuning.h:7-10`;
`ViewTuning.h`). Synchronisation primitives are confined to the two transport files (ADR
0022:41-42) and nothing allocates after `Start` (ADR 0022:72). A change of mind is a new record
naming the one it supersedes; five items below reverse an argued rule and each says which.

### 1. The wire answers an order: fix the LAUNCHING/Jump status collision, re-lay the fleet status block, add OrderReply on the reliable lane, log a dropped send [Change] [P0] [both]

Two facts are owed today. `FLEET_STATUS_LAUNCHING = 6` while `FleetOrderKind::Jump` is also 6
(`GameLogic/UniverseSnapshot.h:212-220`; `ShipState.h:205-218`), and the comments "first value no
FleetOrderKind uses" and "kind > Mine" are both stale; `UniverseSnapshot.cpp:575` writes `status =
manifestCount ? LAUNCHING : orderKind`, so a Jump order with nothing left to launch writes 6, and
both readers draw it as LAUNCHING with no JUMPING case (`Outpost/UniverseView.cpp:567-570`;
`Outpost/FleetSheet.cpp:151`) — a shipping display bug with no `FLEET_STATUS` assertion in
`JumpTests.cpp`. And no order is ever answered: `(void)_universe.IssueFleetOrder(...)`
(`GameLogic/Publisher.cpp:203`) discards eight `FleetOrderResult` values (`GameLogic/Universe.h
:709-719`); the marker plays the moment `SendToSelectedFleets` returns nonzero
(`Outpost/UniverseView.cpp:1256-1277`) and a refused send drops that fleet's order with no log line
(`:1218-1226`; `NeuronCore/QuicTransport.h:46-55, 78-80` `capacityReliableMessages = 32`). The
codec already reserves the order id: "order id, reserved: nothing acknowledges an order yet"
(`GameLogic/UniverseSnapshot.cpp:1749, 1903`). Fire-and-forget was a standing decision —
"Adding an ack here would be adding the first one anywhere"
(`Design/Decisions/0051-the-ledger-is-asked-for-not-broadcast.md:70-75`; `0049:56-58`;
`Design/Archive/Fleets.md:301-303`) — taken when the affordance pre-filtered every refusal from a
one-update-old mask; that premise fails the day standings, ownership and slot occupancy change
server-side (EVE) and the day every new verb can be refused for a reason no mask shows.
`FleetStatus` bits 3-5 are unused and contended (`UniverseSnapshot.h:205-210`).

*Proposal.* Slice 0 (a bug, no design): widen the status block's kind to a byte and move LAUNCHING
to a flag bit, add a JUMPING case to both readers, pin it with a `JumpTests` status-byte assertion;
ALPN bump per Combat.md 9.3. Settle in the same re-lay where a stance byte (item 11) and the
economy's cargo pip (item E4) ride, so `FLEET_STATUS_BYTES` is bumped once. Slice 1: an
`OrderReply{slot, orderId, result, tickApplied}` on the reliable lane, answered at the question in
`ApplyOrders` exactly as `LedgerReply` is, using the reserved order id. The view keeps a marker
pending (dim, not pulsing) until the reply or the status byte confirms, retracts it with a log line
on a refusal, and logs ORDER DROPPED when its own send is refused. Write the record that supersedes
ADR 0051's "first ack anywhere" argument. Homeworld gets the helm-answered cue and the retraction
when an order did not land; EVE gets refusals that can no longer be pre-filtered.

*Depends on:* nothing.

### 2. Owner-only FleetIntent: the fleet's standing order, solved formation, slot offsets and route, stated on change to its owner [Change] [P1] [Homeworld]

ADR 0009 withholds `steerTargetPos`, `orderFacingRad`, `avoidHeadingRad` and the speed cap from
every subscriber including the owner (`GameLogic/UniverseSnapshot.h:40-46`;
`Design/Decisions/0009-a-snapshot-carries-a-view-record.md:22-24, 47-48`); `FleetStatus` is
`{position, status byte, count}` with no order point or referent (`UniverseSnapshot.h:205-210`);
route waypoints are "server-side only" (`GameLogic/Universe.h:843-845`). So the owner cannot draw
where its fleet is going, the route around a station, or the formation it will settle into; the
marker is oriented by re-running `FormationHeading` on stale client positions, one heading for
several fleets, "not a prediction" (`Outpost/UniverseView.cpp:1260-1270`), and
`Design/Archive/Fleets.md:466-471` §6.4 accepts the two halves disagreeing off-screen. No route,
ghost or path line is drawn anywhere in `UniverseView.cpp`. Homeworld's tactical readability is
seeing your own fleet's path and shape before it happens; EVE has no formations and no path display
and needs none of it. This also gates prediction (item 4). Undeclared: neither Fleets.md 14 nor
Combat.md 14 lists it. ADR 0009's anti-cheat reason is about other players' intent, and an
owner-only, faction-gated message preserves it.

*Proposal.* A `FleetIntent` message on the reliable lane, per subscriber and gated by
`ownerFaction`, stated on change like the roster (`PublishRosters`' diff idiom,
`Publisher.cpp:268-304`): order kind, point and referent, solved formation heading, per-member slot
offsets, and the group route's waypoints (item 12's single group route makes this one polyline).
The view draws a destination ghost and the route line for selected fleets, replaces the marker's
client-side heading with the stated one, redraws the standing order after the marker fades, and
hands the steer targets to prediction. Recorded as the deliberate addition ADR 0009:47-48 provides
for; hostile and neutral intent stays withheld.

*Depends on:* nothing.

### 3. A client tick clock with an adaptive interpolation window, correction blending, hull-bounded extrapolation, a wire readout, and the first test of the view under a lagged link [Change] [P1] [both]

The view draws at the in-process `ServerHost`'s tick minus a constant: "Nothing advances a link
clock here any more" and `SetDisplayTime(m_host.Tick() + InterpolationAlpha())` are the only
callers (`Outpost/OutpostApp.cpp:955-969`); `m_displayTick = _tickTime - INTERP_DELAY_TICKS`
(`Outpost/UniverseView.cpp:444-447`) with `INTERP_DELAY_TICKS = 6` and
`INTERP_MAX_EXTRAPOLATE_TICKS = 48`, beside the sentence "a real wire adds its jitter to this"
(`Outpost/ViewTuning.h:175-187`). A second machine cannot read `m_host`; there is no RTT or jitter
estimate anywhere in Outpost/ or NeuronClient/; `QuicTransport` exposes no statistics accessor
(grep rtt, GetStatistics, QUIC_PARAM_CONN_STAT: nothing); the HUD reads TICK, TIME, FPS, MS
(`Outpost/Hud.cpp:258-260`) and `ThrottledTickCount`/`RefusedLeaveCount` are counters "the HUD does
not show yet" (`Outpost/UniverseSimulation.h:97-107`). A new sample replaces from/to with no memory
of the extrapolated pose, so a late sample snaps (`UniverseView.cpp:124-134, 474-481`) — at
`CAMERA_MAX_ZOOM = 900` (`ViewTuning.h:26`) that is a turning ship carried straight for up to ~12
ticks. The instrument exists — `LoopbackTransport` counts latency in ticks "to answer how a
correction feels under lag" (`NeuronCore/LoopbackTransport.h:13-25`) — and no test drives
`UniverseView` through it: Tests/ has no Outpost project and no GameLogicTests file sets
`latencyTicks`; a lost fragment drops the whole update, doubling the gap
(`Tests/GameLogicTests/SnapshotTests.cpp:1158`). Declared as one-process work (AGENTS.md:107-117)
and the prerequisite for every other feel item and for the governor's client half.

*Proposal.* In Outpost/NeuronClient, presentation only: estimate server tick time from the ticks
stamped on arriving fragments and fire messages (smoothed offset, EWMA jitter), hold the display
delay at one update period plus two to three sigma clamped to [6, 30] ticks, slew rather than snap
when it changes, resync when the stream stalls past the extrapolation bound, and multiply the
estimated rate by the governor's r (item 5). When a sample arrives while a ship was extrapolated,
blend from the last displayed pose over an update period capped by a metres-per-tick rate from the
hull's own speed; bound heading extrapolation by the hull's turn rate. Add a `QuicTransport`
statistics accessor (RTT, loss, ring occupancy) and a HUD line with RTT, loss, throttled ticks,
refused leaves, dropped sends. Stand up the missing test: drive `UniverseView` over a
`LoopbackTransport` at `latencyTicks = 6` and `dropOneInN = 50` and pin first-response ticks and
maximum displayed correction — the 100 ms / 2 percent case this lens argues from and has never run.
The composition root stops handing the view the server host.

*Depends on:* nothing.

### 4. Fleet-grain prediction of the owner's own ships: SolveOrder and IntegrateShip run on the client from the tap, reconciled to the sample stream [Missing] [P1] [Homeworld]

An order read on tick T turns the ship on T (`Outpost/UniverseSimulation.h:60-66` ApplyOrders,
Step, Publish), the record leaves on the subscriber's due tick T+d, d in [0,5]
(`GameLogic/Publisher.cpp:246-250`), and the pose is drawn six ticks behind
(`Outpost/ViewTuning.h:186`), so the hull visibly moves 6-11 ticks after receipt and thereafter
100 ms behind truth. The ship is not silent meanwhile — the thruster flare reads `accelSample`
directly (`Outpost/UniverseView.cpp:126-128, 950-956`) — so the plume answers one update period
after the tap; the hull does not answer in a frame or two over any wire. A Carrier at 5 m/s^2
moves 2.5 cm in 100 ms, under the wire's quantum (`GameLogic/HullSpec.h:224-231`;
`UniverseSnapshot.h:32-38`), which is why the gap is invisible in one process and will not be on a
link. Prediction is declared future work (`Design/Archive/Collision-slice-2b.md:145-146`;
`Design/Archive/Collision.md:772-790` option 2: avoidance server-only, client predicts `SolveOrder`
and `IntegrateShip`; ADR 0009:43-46), `SEPARATION_CLAMP_FRACTION` "is the prediction error budget"
(`GameLogic/SimTuning.h:200-204`), and `SolveOrder` is pure over hull and intent
(`GameLogic/Movement.cpp:78-81`), includable by the view as `Formation.h` already is. Homeworld's
bar is exactly this; EVE's ships answer on the server tick and its players accept it.

*Proposal.* Predict only the owner's fleet members: from the moment an order is sent, run
`SolveOrder` and `IntegrateShip` on the client against the `FleetIntent` steer targets (item 2),
draw the predicted pose, and blend it back to the arriving samples over an update period; on an
`OrderReply` refusal (item 1) drop the prediction and revert. Avoidance and separation stay
unpredicted so the worst error is the clamp budget the tree already sized; hostile and neutral
ships stay interpolated. Nothing in GameLogic changes.

*Depends on:* items 1, 2, 3.

### 5. Measure the tick, then govern it: a stated, replicated simulation-rate ratio (time dilation) in place of the silent catch-up drop [Change] [P0] [EVE]

ADR 0045 buys capacity by putting fewer entities in a shard
(`Design/Decisions/0045-the-tick-rate-is-fixed-at-60-hz.md:36-41`; `Design/Archive/Universe.md:429` "one
universe, one thread, 60 Hz"), but a thousand-player fight is one neighbourhood in one shard and
interest management cannot thin it. When Step plus Publish overruns 16.7 ms the only behaviour in
the tree is `maxCatchUpSec = 0.25`: time past it is dropped and "the world runs slow for a moment
rather than freezing" (`NeuronServer/ServerHost.h:20-27`; `ServerHost.cpp:14-29` clamps and counts
steps and times nothing — the file is 40 lines). Corrected from the specialist: tick-counted
cadences already slow with the tick (`GameLogic/InterestSet.cpp:20-24`; `SimTuning.h:435-445`)
and order intake is per tick (`Publisher.h:59-62`), so the machinery survives dilation; what is
missing is the measurement, the statement (the header at `UniverseSnapshot.cpp:64-70` carries no
rate), and a remote client that would otherwise extrapolate into the slowdown. `m_timeScale`
multiplies the host's dt (`Outpost/OutpostApp.cpp:805-814, 958`): a local, unreplicated dilation
knob already proves the host tolerates a scaled dt. A grep for dilat across Design, GameLogic,
NeuronServer, NeuronCore and Outpost has no hits; the only run loop is `OutpostApp::Run` gated on
the swapchain (`OutpostApp.cpp:944-950`). `AStallDoesNotSpiral`
(`Tests/NeuronServerTests/ServerHostTests.cpp`) pins the clamp a governor re-pins. EVE survives its
thousand-player fights because the server states its rate and everyone slows together; a Homeworld
skirmish never overruns and reads r = 1.

*Proposal.* Slice 0: time Step and Publish per tick in `ServerHost`/`UniverseSimulation` (a Stats
struct: step microseconds, publish microseconds, subscribers, records sent) and print a shard-load
line to the event log — without this neither the governor nor the byte budget has an input. Slice
1: keep `TICK_HZ = 60` as the simulation's second and make `ServerHost` a governor: from the
measured cost choose r in [0.1, 1] so one sim tick is scheduled every `TICK_DT / r` of wall time
instead of dropping accumulated time; stamp r as one byte appended to the snapshot header beside
`hostileMask` so it heals every update; the client clock (item 3) multiplies its estimated rate by
r and the HUD states it. Re-pin `AStallDoesNotSpiral` against the governor. A new record amends
ADR 0045's consequences: capacity is still bought by entity count, and a shard that exceeds it
says so. Depends externally on the headless run loop the dedicated-server root would provide
(AGENTS.md:116-117; `MmoScalabilityPlan.md:134-145`), which is no panel's slice yet.

*Depends on:* item 3.

### 6. A session layer beside the Publisher: KIND_JOIN/resync, a lag ceiling, retry of refused reliable sends, reconnect with backoff, and the client side of a shard crossing [Missing] [P1] [EVE]

"Nothing on this seam retries" (`GameLogic/Publisher.cpp:296-304`): a leave refused by a full
ring is counted in `RefusedLeaveCount` and never re-sent — departures never told
(`GameLogic/UniverseSnapshot.h:279-282`) — a refused roster is recorded as sent, and the despawn
and shot logs are trimmed to the minimum over every subscriber's cursor (`Publisher.cpp:255-265`),
so one paused client pins every death and every shot for everyone; ADR 0027:72-75 called the
never-catches-up policy "a session decision". `SnapshotWriter::Write`, the whole-view path, is
declared and defined and called only from Tests/ (`UniverseSnapshot.h:246`;
`UniverseSnapshot.cpp:617`); ADR 0029:53-57 parked the fallen-behind resync as "a different
problem". `OpenQuicLink` dials once and throws (`Outpost/OutpostApp.cpp:324-390`); no reconnect
exists anywhere in Outpost/. `Publisher.h:28-35` says "it is not a session layer" and
`MmoScalabilityPlan.md:225-229` declares authentication, session lifetimes and the dedicated-server
root out of scope. `Design/CrossShard.md:3, 132-138` (not yet agreed) adds the same shape of gap:
one connection to the camera's shard, reconnect on crossing, "watch two at once" deferred, while
`HasAnyFleet`, `IssueFleetOrder` and `PublishRosters` all read the one universe the publisher holds
(`Publisher.cpp:27, 203, 246`) and a joining subscriber starts at the head with empty rosters
(`Publisher.h:67-79`). EVE's one persistent shard is defined by clients that drop, return and
cross; Homeworld in one process never loses a leave but carries a persistent fleet across systems,
which is why the cross-shard half serves both.

*Proposal.* In GameLogic beside the Publisher so a headless root can use it: a subscriber identity
(a token at connect; ADR 0047's serial idiom) so a re-dial resumes the same entry; a per-subscriber
lag ceiling (updates behind, bytes queued) in `Publisher::Desc`; past it, or on
`RefusedLeaveCount` rising, or on request, the subscriber is marked for resync — cursors jump to
the heads, roster memory and interest set are cleared, and a `KIND_JOIN{shard, tick,
drop-everything}` on the reliable lane precedes a full `WriteInterest` of the current
subscription, all five rosters and the status block (ADR 0029's own reserved case). A refused
reliable send marks the subscriber dirty and retries next tick until the ceiling turns it into a
resync; log trims take the minimum over healthy cursors only. Client: a re-dial with backoff and a
receiver reset on `KIND_JOIN`. Cross-shard, folded into CrossShard.md before it is agreed: the
departing shard's status block states "slot s is on shard X" from its outbox and the arriving shard
from its inbox, a tap on such a slot becomes the camera-follows-fleet reconnect, and an order for a
fleet elsewhere is refused with a reply (item 1) that names the shard.

*Depends on:* item 1.

### 7. A per-subscriber budget: relevance-ordered records and fire events, self-applying fragments, a far-tier contact record, an event bonus in the accumulator, and a cell-indexed shot log [Change] [P0] [EVE]

An update carries every entered and every due record in the interest set
(`GameLogic/Publisher.cpp:309-316`), the accumulator's only input is distance
(`GameLogic/InterestSet.cpp:95-110` `weight = clamp(1 - d/R)`), and `INTEREST_RADIUS_METRES 2000`
has no count cap (`GameLogic/SimTuning.h:440-445`). At k = 1000 ships in view that is ~48
datagrams per subscriber per update at 21 records per fragment (`UniverseSnapshot.cpp:462`),
dropped whole if any fragment is lost — "an incomplete update is dropped entire"
(`UniverseSnapshot.h:320-328`; `Design/Archive/Collision-slice-6.md:112-122`) — so (1-p)^48
completes 38 percent of the time at 2 percent loss, a full 256-datagram ring ends the update
(`NeuronCore/QuicTransport.h:46-47`; `UniverseSnapshot.h:237-239`), and a thousand such
subscribers is ~480 MB/s of egress. A ship that just took a hit is no likelier to be sent than
one drifting. Fire events are the same problem in miniature: `MAX_FIRE_EVENTS = 64` chosen by
recency, "past what any battle this envelope holds" (`UniverseSnapshot.h:118-125`;
`UniverseSnapshot.cpp:664-668`), while a fully engaged 500-ship fight produces roughly 100-250
shots per update at 0.13-0.53 shots per ship per six ticks (`GameLogic/DeviceSpec.h:82-89`;
`HullSpec.h:71-88`), and the filter walks every `ShotRecord` since the cursor per subscriber on the
tick thread (`Publisher.cpp:336-347`). `SHIP_FLAG_STATION` and `SHIP_FLAG_GATE` leave six spare
flag bits (`UniverseSnapshot.h:105-106`). `Design/Archive/MmoScalabilityReview.md:147-160` E5
already priced k = 500 against E1's loss amplification; no slice fixed it. EVE's grid works because
each client gets a bounded stream and tolerates stale far ships; Homeworld never has a thousand
hulls on one screen, though its far half of the battlefield benefits from the event bonus.

*Proposal.* Give `Publisher::Desc` a `bytesPerUpdate` budget (Server.cfg, per role) and turn the
accumulator into a send order: sort the due set by accumulated priority (index permutation, handle
order on ties), fill fragments until the budget is spent, carry the remainder's priority forward so
starvation is impossible; add an event bonus (order state, `hullFraction` or fired-recently
changed since last send) fed by a per-tick changed flag on the view table (item 8). Make each
fragment self-applying — an upsert batch stamped with tick and the fleet block it already carries —
so a lost fragment costs only its records. Add a far tier beyond a `nearRadius`: a 16-byte contact
record (entity, quantized position, faction, hullId). For gunfire: choose the 64 by relevance (own
fleet's shots and shots at own ships first, then by distance), index the shot log by the shooter's
`SpatialIndex` cell so a subscriber walks its cells rather than the log, and define a
fired-within-N-ticks bit on the record from a per-ship last-fired tick so a turret slew heals from
state — spending flag bits deliberately against the effect flags of item 10. Records amend
Collision-slice-6 3.5's drop-whole rule and ADR 0053's newest-gunfire rule.

*Depends on:* items 6, 8.

### 8. Publish off the tick thread over a per-tick encoded view table; remove the quadratic loops at the seam; save on a worker [Change] [P0] [EVE]

ApplyOrders, Step and Publish for every subscriber run sequentially on one thread per tick
(`Outpost/UniverseSimulation.h:60-66`; `GameLogic/Publisher.cpp:235-265`). Each `PublishOne` is a
`QueryCircle`, an O(k^2) insertion sort (`GameLogic/InterestSet.cpp:44-60`, ADR 0010's own "first
thing to revisit"; `MmoScalabilityReview.md:234-243` U5 names both loops;
`Outpost/UniverseView.cpp:106-114` the client's linear match), a merge, a shot-log walk and a
serialisation — and `WriteShipRecord` runs inside each subscriber's `WriteInterest`
(`UniverseSnapshot.cpp:466`), so N subscribers sharing one fight do the lattice conversion N times.
`SaveUniverse` serialises the whole universe and writes the file inside the frame loop every 1800
ticks (`Outpost/OutpostApp.cpp:1014-1018, :614-620`; `Design/Archive/Universe-slice-5.md:109-111`
discusses cadence, never the write's cost). Without this the governor (item 5) dilates for egress
rather than for simulation. ADR 0009 already made the wire a view record rather than the ship,
which is exactly the seam that lets publishing read a frozen copy while the next tick runs;
`Publisher.h:33-35` puts it outside the replay contract and `Design/Archive/Universe.md:429`'s "one
thread" is about the simulation. ADR 0022:41-42 confines primitives to the two transport files and
says a fifth file needs a record. A Homeworld skirmish with a handful of subscribers stays
single-threaded, so the worker count is a Server.cfg number.

*Proposal.* After Step, `Universe` writes one dense per-tick view table (the `ShipSnapshot` fields
already quantised to wire form plus handle, a changed flag, the despawn and shot heads, the fleet
rows) into a double buffer; publisher workers read the completed buffer for tick T while the tick
thread runs T+1, subscribers partitioned across workers by slot, and nothing published feeds back.
The encoded-record half needs no thread and lands first. Replace the insertion sort with an
index-permutation `std::sort` and the client's linear match with a handle-sorted vector and
`lower_bound`. Snapshot the save bytes on the tick thread and write the file on a worker. One new
ADR names 0022 and Universe.md 11's "one thread" and confines the new primitives to the
publisher's worker file.

*Depends on:* item 5.

### 9. A leashed, server-anchored interest centre with per-role descriptors (interest, rings, budget), and a sensor tier the simulation owns [Change] [P1] [both]

The interest centre is the client's camera target, pushed into the simulation by the composition
root every tick with no wire message, no bound and no relation to what the faction owns or can
sense: `SetCentre` from `m_viewCentre` each tick, "It is not on the wire", "a dedicated server gets
it from the session instead" (`Outpost/UniverseSimulation.h:60-90`; `Outpost/OutpostApp.cpp:883`);
membership is `QueryCircle(_centre, radius)`, distance only (`GameLogic/InterestSet.cpp:33-43`;
`SimTuning.h:440` no count cap). `Design/Archive/Fleets.md:704-735` §9.1 made the interest set
follow the camera so a tapped distant fleet streams in — a deliberate landed decision that becomes
an exploit at scale: any client parks its centre on a fight it has no ship in and receives every
record at full fidelity, and a thousand spectators each cost a full battle stream. The hook exists —
per-subscriber `InterestSet::Desc` "so a spectator or a distant region can be given a different
one" (`GameLogic/Publisher.h:55-62`) — and the role plumbing does not: listener slots pre-allocate
~1.06 MB of rings each for the life of the listener (`NeuronCore/QuicListener.h:29-37`;
`QuicTransport.h:46-55`) and `Outpost/ServerConfig.h` and `Server.cfg:15-45` carry no ring keys.
ADR 0022:72 forbids allocation after `Start`, which lazy growth would amend. A grep for sensor in
GameLogic and Outpost finds no code; `Design/Archive/Hostiles.md:518-519` declares per-viewer
stealth out. EVE's local and d-scan and Homeworld's sensor ranges both make what you see a
function of what you own.

*Proposal.* A `ViewCentre` message on the datagram lane (state that heals; the latest wins)
replaces the root's direct push, and the publisher derives the effective centre from the
subscriber's own assets (live fleets, stations, the gate just used) accepting the camera only as
an offset inside a per-role leash; a spectator role gets its own `Desc`. Plumb role descriptors
(interest radius, update period, `ordersPerTick`, `bytesPerUpdate`, ring depths) through
Server.cfg keyed by role, server-side inbound rings shallow. Later, with the Combat design: a
per-hull `sensorRadius` in `HullSpec` so the subscribed set is the union of the faction's sensor
circles intersected with the leash, with only the far-tier contact record (item 7) beyond it.
Records amend Fleets.md 9.1 and, if pools grow lazily on the owning thread, ADR 0022:72.

*Depends on:* items 6, 7.

### 10. A device row becomes an effect descriptor, a ship gains an effect table and an EffectiveSpec, the wire gains an act record and effect flags, and the mining tool gets a Resource target class [Change] [P0] [both]

A device is range + cooldown + damage + traverse; `DeviceKind` is `{Gun, MiningTool}` with five
Gun rows and `MiningTool` reserved with no row (`GameLogic/DeviceSpec.h:18-26, 82-89`); the fire
pass skips any non-Gun mount (`GameLogic/Universe.cpp:1778-1783`) and the only write a cycle makes
is a saturating subtract (`:1812-1815`, `:1838-1842`). No per-ship state exists that anything but
the hull spec and the order can change: `ShipState` has no modifier, timer or effect field and its
one speed lever is an order-owned cap cleared at every lowering (`GameLogic/ShipState.h:227-279,
:247-253`); `SolveOrder` caps speed by `_hull.maxSpeedMetresPerSec` (`GameLogic/Movement.cpp
:78-81`); range to skin comes from `_device.rangeMetres` with nothing on the target that shrinks
it (`Universe.cpp:1694-1702`). `Mine` returns `Unsupported` and `FleetCommand` has no resource
referent (`Universe.cpp:686-691`; `Universe.h:723-732`). On the wire a `FireEvent` is
`{shooter, target, mount}` and no record state says webbed, jammed, cloaked or repairing
(`GameLogic/UniverseSnapshot.h:113-125, :105-106`); ADR 0053:23-33, 73-83 puts consequences in
state and instants on datagrams. `Design/Archive/Combat.md:460-481` §12 already prescribes the rock as a
hull row with a side table and the Miner's tool mounts — "Nothing in 16's slices touches any of
that" — but a rock-as-ship carries a `FactionId` that `MountTargetStands`' own-faction test would
refuse, so the framework must add a target class. Every ability both benchmarks need — repair,
tackle, jam, burst, cloak, harvest, hangar launch — is a device cycling on a mount that none of
this can state; eight other findings in this lens depend on it. Declared in `DeviceSpec.h:24-26`
and Combat.md 12/14 and scheduled by nothing.

*Proposal.* Widen `DeviceSpec` into an authored effect descriptor keeping ticks and integers
(`ShipState.h:270-278`'s argument): `DeviceKind` appended `{Gun, MiningTool, Repair, Web,
GateLock, Jam, Burst, Cloak, Hangar}`; `TargetClass{HostileShip, FriendlyShip, Self, Resource,
Point}`; `u32 magnitude`, `u32 durationTicks`, a `Channel` bit. Self-class devices skip the arc
test; Resource-class targets bypass the faction and `HoldsHostile` tests. Add `ShipEffects`
parallel to `m_ships` (fixed capacity like `ShipMounts`: kind, source, remainingTicks, magnitude)
decremented in the standing-intent slot and written from Effect captures in `StepMounts`' apply
step; consumers read one `EffectiveSpec(ship)` view (speed, acceleration and turn permille, sensor
permille, gate-lock and cloak flags) in `SolveOrder`, the pursuit stand-off, `MountTargetStands`
and `StepJumps`, so Burst can exceed hull max and Web can cut it. Wire: activation is a
`FleetOrder` (item 11); the instant rides the datagram lane as `ActRecord{source, target, mount,
effectKind, phase}` re-laid under an ALPN bump; the effect is state in the record's spare flag bits
(WEBBED, JAMMED, CLOAKED, LOCKED, REPAIRING — budgeted against item 7's fired bit) so a lost
datagram heals. Every remaining-ticks counter enters `UNIVERSE_STATE_FORMAT`. First non-gun
device: the `MiningTool` row with `TargetClass Resource`, the rock as an immovable hull row with a
side table placed by the layout, and a Mine order with a resource referent — item E5 is that
row, and what a full cycle writes (cargo, unload, ledger) is items E3-E5.

*Depends on:* item 1.

### 11. The order grammar grows a verb-and-parameter shape and a settings axis: Guard and Activate verbs, a Mine referent, a range parameter, a FleetSetting message writing stance and formation on the fleet row, and later an order queue [Change] [P1] [both]

An order is one of seven kinds carrying a point, a station, a target or a gate
(`GameLogic/ShipState.h:205-218`); the codec refuses any kind above Jump both ways
(`GameLogic/UniverseSnapshot.cpp:1743, 1776`); `FleetCommand` has no range, device, stance or
formation parameter and the standing order is assigned whole on every order (`GameLogic/Universe.h
:723-732`; `Universe.cpp:715-721`); the `Fleet` row has no stance or formation
(`Universe.h:245-266`). The fleet has exactly one posture by design: priority-4 acquisition is
unconditional for every mount (`Universe.cpp:1749-1756`) and ADR 0050:34-36, 47-49 refused "a
sense". `ENGAGE_STANDOFF_FRACTION` is the only range a pursuit knows (`SimTuning.h:419-422`).
Hold-fire, withdraw and escort doctrine are deferred to an RoE design that has no file
(`Design/Archive/Combat.md:454-458, 556-561`); queues, formation choice and stances are deferred
(`Design/Archive/Fleets.md:1019-1030`). Homeworld needs Guard/Escort, a harvest referent,
evasive/neutral/aggressive tactics and a formation choice that survive orders, plus queued
waypoints; EVE needs orbit and keep-at-range on Attack, activate-device-on-target, and hold-fire
and aggression safeties. A stance is not an order — it survives orders — so it needs a row field
and a message that does not disturb the standing order.

*Proposal.* Verbs, appended so no byte renumbers: Guard (referent = a friendly entity or own
fleet slot; combatants hold their stand-off between protectees and the threat anchor, mounts gain
a priority above "nearest hostile" for any hostile inside a protectee's envelope — the mechanism
behind Fleets.md's "2 Miners + a Hauler + 5 escorts"), Activate (device kind + referent, lowered
to a per-mount stated target at priority 2), Mine with a resource referent (item 10). Parameter:
`u32 rangeMetres` on Attack/Guard, 0 meaning the hull's stand-off. Settings: a
`FleetSetting{slot, stance, formation}` message on the reliable lane writing `u8 stance{Neutral,
Aggressive, Evasive, HoldFire}` and `u8 formation` on the `Fleet` row, saved with it; HoldFire
disables priority-4 acquisition, Evasive keeps combatants on the standing order firing only what
bears (Combat.md 5.4's "guns yes, course no"), Aggressive lets combatants pursue the nearest
standing-hostile without a stated act — the sense ADR 0041/0050 refused, admitted as an explicit
player choice and recorded as amending both. Stance rides the re-laid status block (item 1).
Later slice: a short ring of standing orders with a queue flag, completed by the same arrival and
dock-capture events, stated whole in `FleetIntent` (item 2) — after arguing with the patience rule
as Fleets.md 14 asks; item E8's harvest cycle and route-to-dock are the first two queued shapes.
Raise the codec gates; `UNIVERSE_STATE_FORMAT` and ALPN bump.

*Depends on:* items 1, 10.

### 12. Formation becomes fleet state that combat reads: shape on the row, en-route station keeping about a moving centroid, one route for the group, and formation pursuit under Attack and Guard [Change] [P1] [Homeworld]

Four formation shapes exist as pure functions (`GameLogic/Formation.h:12-18`) and one
compile-time constant in the replay contract picks the Wedge for every order (`FORMATION_SHAPE =
1`, `GameLogic/SimTuning.h:7-10, 297-300`; cast at lowering, `Universe.cpp:2355-2368`);
`FleetOrder` carries no shape or spacing (`UniverseSnapshot.h:140-150`). "Slots are solved at
the destination, not maintained en route" (`Design/Archive/Fleets.md:457-466`; `:1025-1026`
formation choice deferred; `Design/Archive/Combat.md:337-339` formation combat deferred). Patience
re-plans to the member's own route destination, "never to a re-solved formation"
(`Universe.cpp:1654-1662`); there is one group clearance but one `PlanRoute` per member and no
leader (`:2362-2368`); Attack lowers to per-ship `PursueTarget` for every combatant
(`:621-638`). The separation solver's own comment warns that a parallel pack relaxes as the square
of N (`SimTuning.h:159-163`), which is what naive per-tick slot keeping would provoke, and
`Fleets.md:212`'s 247 m Carrier spacing is the number the fleet-size argument rests on.
Homeworld's formations matter because they shape the fight en route and on contact — which guns
bear, who screens whom; a fleet that arrives in a wedge but flies as a string and fights as eight
chases cannot deliver that. EVE has no formations and needs none of this.

*Proposal.* Move the shape off `SimTuning.h` onto the `Fleet` row (item 11's `FleetSetting` writes
it, the codec saves it — a state-format bump that takes `FORMATION_SHAPE` out of the contract).
En-route mode: each member's steer target is its slot offset from the fleet's solved centroid and
heading, re-solved on a deviation threshold (`PATH_REPLAN_DEVIATION_METRES`' idiom, never per
tick), the slowest-member cap keeping the shape closed and spacing staying the largest-hull rule so
separation seldom binds. Route once for the group leader with the group clearance the code already
computes and derive member waypoints from it, handling ADR 0042's into-a-wall case per member.
Under Attack and Guard, Circle/Box fleets pursue as a formation about the centroid so a
capital-and-escort fleet screens; this reopens Combat-slice-5's 81.6 s measurement, which item
13's matrix re-pins. `FleetIntent` (item 2) states the shape and the group route; ADR 0049 is
preserved because the order still names a fleet.

*Depends on:* items 2, 11.

### 13. Author the counter graph and pin it: a matchup matrix test first, then class effectiveness, a per-hull engage range, a real blind spot astern, a speed spread, the Battleship's lights, and a Carrier with a job [Change] [P0] [both]

The counter graph is two edges short rather than absent. Per-hull dps 6/15/13.3/24/43.3/26.7 and
hull points 60/150/240/520/3800/5200 both rise with size and speeds span only 34 to 20 m/s
(`GameLogic/DeviceSpec.h:82-89`; `GameLogic/HullSpec.h:217-234`): the Bomber beats the line and
the LightTurret is "the escort's tool" (`Design/Archive/Combat.md:490-503, 511`), but the Interceptor
kills nothing armed — "Two Interceptors cannot harass a Frigate; they die first"
(`Design/Archive/Combat-slice-5.md:56-63, 74-80`) — and `LOADOUT_BATTLESHIP` carries two LightTurrets of
its own (`HullSpec.h:71-88`), so an escort adds nothing a capital does not bring. Every turret
hull's +/-150 degree arcs union to 360 degrees against `HullSpec.h:61-63`'s promised "blind spot
astern". `EngageStandoffMetres = 0.8 x shortest traversing mount` holds the Battleship and Carrier
at 144 m and throws away the 420 m their heavies were authored for (`HullSpec.h:163-178, 286-292`;
`SimTuning.h:419-422` argued from Corvette and Frigate only; `Design/Archive/Combat.md:321-328` chose the
rule deliberately). `LOADOUT_CARRIER` is four LightTurrets on 5200 points (`HullSpec.h:84-88,
231`) and "Carrier wings" is deferred to a hangar design with no page (`Combat.md:570`; ADR
0038:33-37, 60-62 anticipates non-Structure stations and "stations do not despawn" is what a
mobile one breaks). No cost column exists (`HullSpec.h:90-141`; `Design/Archive/Fleets.md:217-220`
declined a hull-weighted budget). Slice 5 measured all of this with "a standalone harness"
(`Combat-slice-5.md:30-31`) that is in no file under Tests/ or Tools/, found a window narrow
enough that 3800 versus 4000 points flips who wins, and `CombatTests.cpp:67, 80, 267` pins one
pacing assertion, one determinism test and one tracking counter. ADR 0052:52-57 — "a
tracking-versus-signature number is not [an answer the player can see]" — is the record an
effectiveness table reopens. Homeworld's class system is that every hull has something it kills
cheaply and something that kills it cheaply; EVE reaches the same result through signature and
tracking (item 15), so the class table is the deterministic stand-in.

*Proposal.* Slice 0, before any number moves: bring the slice-5 harness into
`Tests/GameLogicTests` as a matchup matrix — every combatant hull against every other, one-on-one
and eight-on-one, asserting the sign of the outcome and a time band, with the intended counter
graph written as the expected matrix; the universe is deterministic so it is cheap and exact, and
a retune that flips a counter fails a test rather than a play session. Then, in one retune
measured by that matrix: a hull size class `{Fighter, Escort, Line, Capital}` on `HullSpec` and an
integer effectiveness percent per (device x class) on `DeviceSpec` (`damage = spec.damage * effect
/ 100`, floored), printed as a class label on the target bar — recorded as amending Combat.md 15
decision 1 and ADR 0052's opacity argument; strip or halve the Battleship's LightTurrets so a
capital is naked without Corvettes; an authored `engageRangeMetres` per hull so a Battleship holds
where its heavies bear; mount arcs that leave a true astern gap on the line and capital hulls,
pinned by a coverage test; a speed spread that lets a fighter disengage; a cost column on
`HullSpec` (the number item E6 prices). Carrier: a `Station` side-table row on a mobile hull (ADR
0038's own next case) with a Hangar device (item 10) launching a complement on the manifest
metronome, fighters below a hull fraction docking back to it, no line armament so it needs its
escorts; a new record names the mobile, destructible station ADR 0038:61 and `Universe.h:86-88`
assume away — the same door item E15's deployable station walks through.

*Depends on:* item 10.

### 14. A ship becomes a record: damage carried through the ledger (decide the free dock repair), a shard-scoped id, fitting slots on a hull's mounts, and hull and device catalogues as authored, versioned data [Missing] [P1] [EVE]

A ledger row is `{hullId, factionId}` — "Hull and faction is the whole of what a ship is today"
(`GameLogic/Universe.h:92-100`; `:1135-1141` `Capture` carries no `hullPoints`); the row is
pushed without damage and `SpawnShip` starts at `maxHullPoints` (`GameLogic/Universe.cpp
:1056-1063, :46-49, :1480-1483`) while a jump carries `hullPoints` (`:1146-1150, 1179-1182`).
`ComposeFleet` is the undock path (`Universe.cpp:481-486`), so `Design/Archive/Stations.md
:863-867`'s "no undocking by any path" predates it, and dock-and-recompose is a free, silent,
total repair contradicting `Design/Archive/Combat.md:288-294`'s "never regenerating" — the economy panel's
item E11 found the same fact from the other side. There is nowhere to hang a fitting, a cargo hold,
a kill count, a repair bill or an insurance record; `MountLoadout` is a `constexpr` member of the
hull row and `DeviceId` is a closed enum of five (`HullSpec.h:55-59, 71-88`; `DeviceSpec.h:27-36`;
`Fleets.md:286` `ComposeOrder` is `hullCounts[HULL_COUNT]`); `HullId` is closed with
`static_asserts` and compile-time derivations (`HullSpec.h:20-34, 277-279, 307-375`) and
`DeviceSpec.h:69-70` says "change a row and a recorded battle ends on a different tick" with no
table version. AGENTS.md:96-99 and README.md:96-98 make constexpr tuning the rule; ADR 0057 and
0058 are the precedent for authored content as versioned data. Three of eight flyable hulls are
reachable by a player today (`StartingUniverse.h:66`; `StartingUniverse.cpp:17-20`); `EntityId`
already survives a jump (`ShipState.h:134-153`; ADR 0047). EVE needs every one of these; Homeworld
needs only persistent damage and veterancy, which the same record gives for free.

*Proposal.* First, one decision on the ledger, shared with item E11: the `DockedShip` row carries
`hullPoints` (and item 15's layers) and the launch spawns from the row, so docking preserves damage
and repair becomes a station action priced by the economy panel — record it as the resolution of
Combat.md 7.1 versus the code. Then promote the row to a ship record: `EntityId`, hull, faction,
points, and a fit array of `DeviceId` per mount; split `HullSpec`'s loadout into mounts (bearing,
arc, size class — geometry, stays authored) and the record's fit, so the fire pass changes one
indirection (`Universe.cpp:1778`) and `ComposeOrder` names records rather than hull counts. Move
the hull and device catalogues into data the UniverseGen tool authors and the save carries (ADR
0058's pattern, ADR 0057's versioning) with a table hash in the save header so a replay against a
different table refuses; compile-time derivations become boot-time derivations and
`static_asserts` become refusals under ADR 0057's "a refused file stops the boot". A record
explains why the class catalogue is content while the physics constants stay code.

*Depends on:* item 13.

### 15. A second quantity in combat: shield, armour and resistances with a damage kind, a continuous tracking-versus-signature race, a capacitor with device costs, and the Repair, Web, GateLock, Jam and Cloak kinds that spend them [Missing] [P1] [EVE]

Every hull has one `u32` and every device one damage figure (`GameLogic/ShipState.h:270-278`,
whose comment names a fractional resist as the float trap; `DeviceSpec.h:38-64`;
`HullSpec.h:90-141` no energy pool, no signature); nothing regenerates, resists or distinguishes a
90-point alpha from ninety 1-point shots, which is why the Battleship matchup is bimodal on a
single dial (`Design/Archive/Combat-slice-5.md:56-57`). Aim is a binary gate followed by full damage
(`Universe.cpp:1694-1702, 1812-1815, 1838-1842`): the LightTurret "tracks anything"
(`DeviceSpec.h:86`) and the target's bounding radius, read for range-to-skin, is never read for hit
weight, so speed and size buy nothing; `CombatTests.cpp:267` pins the one tracking counter.
Nothing can hold, slow, blind or heal another ship: own-faction targets are refused and every
priority is hostile (`Universe.cpp:1694-1698, 1704-1756`), `StepJumps` is whole-or-not-at-all with
no per-ship inhibit (`Universe.h:907-910`; ADR 0056), speed comes from the hull alone
(`Movement.cpp:78-81`), and radius is the only visibility rule (`SimTuning.h:440`;
`Publisher.h:33-35`; `Publisher.cpp:203` has `subscriber.faction` at hand). The HUD draws a SHIELD
bar at a hard-coded 1.0 (`Outpost/Hud.h:92-94`; `Hud.cpp:695-697`; `Combat.md:433-435` calls it
scaffolding). `Combat.md:288-294, 556-558`'s stated reason for refusing a second quantity —
"before the first fight has ever been measured" — expired when slice 5 landed. ADR 0052:52-57 and
`Combat.md:167-169` are the reversal this item records. Tackle and ewar are undeclared gaps
(`Hostiles.md:518-519` names only stealth). EVE's fights are shield/armour/hull with resist
profiles, signature against tracking, capacitor warfare, logistics and tackle; Homeworld needs at
most the class table of item 13, a repair corvette and an EMP-style jam, so this item is EVE's with
Repair and Jam serving both.

*Proposal.* On the ship record (item 14): integer `shieldPoints` regenerating N whole points every
M ticks and `armourPoints` with no regen ahead of `hullPoints`, a `DamageKind` byte on
`DeviceSpec` and an integer resistance percent per (layer x kind) on `HullSpec`, applied layer by
layer in whole points; one fraction byte per layer fills the SHIELD placeholder. Author the first
pass so alpha breaks shields and sustained light fire is what shields eat, giving the Bomber and
the Interceptor different jobs against the same Frigate. Make the tracking race continuous but
dice-free: hit weight is an integer step function of the target's angular rate over device
traverse and of `BoundingRadiusMetres` over a per-device resolution radius — geometry the target
bar can print, recorded as superseding ADR 0052's alternatives paragraph and Combat.md 15 decision
1. Capacitor: `u32 energy` on the record, `maxEnergy` and `energyPerTick` on `HullSpec`,
`energyCost` on `DeviceSpec`, a fifth gate in the fire pass. Device kinds from item 10's
vocabulary: Repair (FriendlyShip, channelled, ordered by (`hullPoints`, `ShipId`) then the
Activate-stated ally), Web (speed permille via `EffectiveSpec`), GateLock (`StepJumps` skips a
fleet any live member of which is locked, preserving ADR 0056), Jam (sensor permille and a dropped
held target), Cloak (Self, energy-costed; the publisher excludes it from non-owner subscribers so
it leaves by the ordinary leave run). All stated as acts so a web rouses a fleet through
`RecordHostileAct` unchanged. `UNIVERSE_STATE_FORMAT` and ALPN bump; item 13's matrix re-pins
every number.

*Depends on:* items 10, 13, 14.

---

## Cross-cutting

Both panels arrived, independently, at the same six seams, and in three places their proposals
pull on the same bytes. This section names each seam, says which panel owns it, and resolves the
conflicts so the order of work below has one answer per line.

**The ledger row is one decision, not two.** The economy panel (E11) wants `DockedShip{entity,
hullId, ownerId, factionId, hullPoints}` so a dock stops being a free repair and a fee can be
charged; the combat panel (C14) wants the same row to carry `hullPoints`, an `EntityId` and a fit
so a ship is a record. Both cite the same lines (`GameLogic/Universe.cpp:1028-1038`;
`Universe.cpp:48`; `Universe.h:1201-1207`) and the same contradiction with `Design/Archive/Combat.md
:288-294`. Resolved: one design, `Design/ShipRecord.md`, owned by the combat panel, lands the row
with `entity`, `ownerId`, `hullPoints` and a fit array in one format bump, and the economy panel's
repair price, fee table and impound rule are sections of that document rather than a second row
change. The row carries damage from the first slice even though the price arrives later — the
combat panel's own risk list names the punishment this causes a damaged fleet, and the answer is
that repair is priced at zero in content (ADR 0043's property) until the wallet exists, which is
also the Homeworld deployment's permanent setting.

**The owner key is the economy panel's and the combat panel inherits it.** `OwnerId` (E1) is the
key of the wallet, the stock, the book, the ship record's owner and — in C6 — the subscriber's
identity that survives a re-dial. Neither panel builds the login; both depend on it. The login is
the one dependency in this review that belongs to no roadmap here, and until it exists every
EVE-tagged item is keyed on a placeholder owner. The Homeworld path does not need it and that is
the temptation the economy lead names: keying stock, wallet and build queues on the faction and
inheriting a shared hangar for every player. The rule this review sets is that no P0 economy item
lands keyed on `FactionId` where an `OwnerId` field can be added with a single owner, even before
the login exists.

**The wire grows in one re-lay, not five.** Three proposals claim the same bits. The `FleetStatus`
status byte's bits 3-5 (`GameLogic/UniverseSnapshot.h:205-210`) are wanted by C1 (a byte-wide kind
and a LAUNCHING flag), C11 (a stance) and E4 (a cargo pip). The `ShipSnapshot`'s six spare flag
bits (`UniverseSnapshot.h:105-106`) are wanted by C10 (five effect flags), C7 (a fired-recently
bit), E3 (a resource-site flag) and E4 (nothing — `cargoFraction` is a byte, not a bit). Resolved:
C1's slice 0 re-lays the fleet status block once, with a kind byte, a flags byte (LAUNCHING,
JUMPING, cargo-full) and a stance byte, so `FLEET_STATUS_BYTES` moves once; and the ship record
gains a second flags byte in the same ALPN bump, carrying the site flag, the fired bit and the five
effect flags, rather than rationing six bits three ways. One ALPN bump, one `SnapshotTests` update,
recorded in one amendment to `Design/Archive/Combat.md` 9.3. The interest-update header grows twice and
both are one field: E2's `I64` balance and C5's rate byte, and they should land as one header
change.

**The save format moves as often as the economy does, and only E14 gives it a migration path.**
Every table above is a `UNIVERSE_STATE_FORMAT` bump — a wallet, items and stock, sites, the
journal, a build queue, a book, the ship record, effects, formation on the fleet row, shields and
energy — and ADR 0057 refuses an unknown format outright. Six bumps landed in three days
(`GameLogic/UniverseSnapshot.cpp:119-120`). The economy panel's migration rule and input-log
recovery (E14) is therefore not an EVE-only item in practice: it is what lets the combat panel's
P0 items ship to a live shard without deleting it. Resolved: E14's migration rule is pulled forward
to land immediately after the first P0 table (the wallet), and both panels' format bumps carry a
reader for the previous format from then on. Homeworld's fresh-start-per-version tolerance is real
and is why the rule is a migration rule and not a compatibility guarantee.

**Telemetry has no home and both panels need it first.** C5's slice 0 (time Step and Publish,
print a shard-load line) and E7's `EconomyCounters` (issued and sunk per category, sampled on the
save cadence) are the same instrument: a per-tick stats block the composition root samples and
writes beside `Universe.sav`. Neither the governor, the byte budget, the inflation report nor the
matchup matrix's time bands has an input without it. Resolved: one `Design/Telemetry.md`, owned
by the combat panel because the tick timing lands first, with the economy counters as a section;
the HUD's `creditsPerMin` and the RTT/loss line are two readers of one block.

**Client-side prediction is the one place the panels' principles could collide, and they do
not.** The economy panel's second principle is that nothing outside GameLogic writes a balance, a
stock or a row; the combat panel's C4 runs `SolveOrder` and `IntegrateShip` on the client. These
are compatible because prediction writes a pose the client draws and never a value the server
reads — the economy's rule is about authority, the combat panel's about display — and because C10's
`EffectiveSpec` (webs, bursts) is state on the record the client reads, not intent it derives. The
cost is that an owner-only `FleetIntent` (C2) hands a client its own steer targets; the tree's
threat model tolerates that because it changes nothing the server decides, and the review records
that the same rule forbids ever putting a price, a stock or a balance on the datagram lane where a
predicted value could be confused with a stated one.

**Where the panels conflict on a mechanism, resolved.** Three cases. First, the Mine order: E5
specifies `MiningLaser` as a `DeviceKind::MiningTool` row with its own stands rule in
`StepMounts`, and C10 specifies the same row as the first instance of a `TargetClass::Resource`
in a widened `DeviceSpec`. Resolved in C10's favour for the table shape (an effect descriptor with
a target class, since seven more kinds follow) and E5's favour for the order, the cargo transfer
and the site table; E5 lands as C10's first row and does not wait for C10's other kinds. Second,
the Carrier and the deployable station: C13 makes the Carrier a mobile, destructible station
through ADR 0038's door; E15 makes a player station an anchored, destructible hull through the
same door. Resolved: one record, written with C13, relaxes `NoImmovableHullIsDestructible` for
named hulls and states what a destroyed station's ledger, manifest and stock do (E15's
accounting), so the Carrier's death and the station's death are one rule. Third, the standing
scalar: E13 widens `Standing` to a signed scalar per (owner, other) and C11's Aggressive stance
lets a fleet pursue "the nearest standing-hostile"; the threshold bit E13 derives is what C11's
priority-4 acquisition reads, so neither changes the other's function, and the design that lands
the scalar owns the hostileMask derivation.

---

## The order of work

One dependency-ordered list across both disciplines, P0 first, each line naming the design
document it wants. E-n is an Economy & Systems item, C-n a Combat & Mechanics item. Where two
items can proceed in parallel because they share no files, they sit on adjacent lines with the
same number.

1. **C1 slice 0 — the LAUNCHING/Jump collision and the one status-block re-lay** (P0, both).
   A work order, no design: `Design/Archive/FleetStatus-work-order.md`, budgeting the kind byte, the flags
   byte and the stance byte at once, amending `Design/Archive/Combat.md` 9.3 for the ALPN bump.
2. **C5 slice 0 and E7's counters — telemetry** (P0, both). New `Design/Telemetry.md`: per-tick
   Step and Publish timing, subscriber and record counts, economy counters, one log beside
   `Universe.sav`. Nothing below can be measured without it.
2. **C13 slice 0 — the matchup matrix test** (P0, both). A work order under `Design/Archive/Combat.md`
   16: the slice-5 harness brought into `Tests/GameLogicTests` with the intended counter graph as
   the expected matrix. Lands before any hull or device number moves.
3. **E1 — OwnerId and AdmitOwner** (P0, EVE). New `Design/Ownership.md`: the second key beside
   `FactionId`, the slot table per owner, `AdmitOwner`, and the placeholder-owner rule until the
   login exists. Amends ADR 0013's consequences and names ADR 0047.
3. **C1 slice 1 — OrderReply** (P0, both). Amendment to `Design/Archive/Combat.md` (a new section on the
   reliable-lane reply) and a record superseding ADR 0051's "first ack anywhere" argument.
4. **E2 and E14's wallet-home decision — the wallet** (P0, both). New `Design/Economy.md` §1-2:
   the money discipline, `Post`, the balance on the header, and the shard-home decision recorded as
   a departure from EVE's single wallet. Lands with the E14 migration rule (below) in the same
   format bump.
4. **E14 (migration rule half) — a reader per older format** (pulled forward to P0 in effect).
   Amendment to ADR 0057's consequences and to `Design/Archive/Universe.md` §11; the input-log recovery
   half stays P1.
5. **E3 — resource sites and geography** (P0, both). New `Design/Mining.md` §1-3 (the design
   `Combat.md` 12 and `Fleets.md` 6.6 both owe): `HullId::Asteroid`, the ownerless-record rule,
   `HULL_COUNT`'s exclusion from the ledger, appended draws after the planet loop, a per-system
   cap. A record reopening ADR 0016 by its clause.
5. **C10 — the effect descriptor and EffectiveSpec** (P0, both). New `Design/Devices.md`:
   `DeviceKind` and `TargetClass`, `ShipEffects`, `EffectiveSpec` into `Movement`, `ActRecord` and
   the second flags byte. Names ADR 0053.
6. **E4 — items, cargo and stock** (P0, both). `Design/Economy.md` §3: the `ItemId` table,
   `cargoKind`/`cargoUnits`, `holdUnits`, `ItemStack`, the widened ledger reply and the assets
   reply; amends `Design/Archive/Universe.md` 6.3 and `Design/CrossShard.md` §5 so cargo crosses in the
   `Jumper`.
7. **E5 — the Mine order** (P0, both). `Design/Mining.md` §4-6 as C10's first row: `MiningLaser`,
   `LOADOUT_MINER`, the Mine branch, the transfer in `StepMounts`, MINE on the sheet.
8. **E6 — HullCost, BuildOrder, StepIndustry** (P0, both). `Design/Economy.md` §4: the cost table
   (C13's cost column is this table's credits entry), the build queue, the industry metronome.
9. **C5 slice 1 — the governor** (P0, EVE). New `Design/Governor.md`: r on the header, the client
   clock's use of it, `AStallDoesNotSpiral` re-pinned; a record amending ADR 0045's consequences.
   Waits on the headless run loop, which this review names as an external dependency and which
   `Design/CrossShard.md` or a `Design/DedicatedServer.md` must own.
9. **C13 — the counter graph retune** (P0, both). Amendment to `Design/Archive/Combat.md` §8 and §15
   (class effectiveness, engage range, arcs, speeds, the Battleship's lights) and a new record on
   ADR 0052's opacity argument; the Carrier's hangar as its own section naming ADR 0038:61.
10. **C8 — publish off the tick thread, the quadratic loops, save on a worker** (P0, EVE). New
    `Design/PublisherWorkers.md` with a record naming ADR 0022 and `Design/Archive/Universe.md` 11.
11. **C7 — the per-subscriber budget** (P0, EVE). Amendment to `Design/Archive/Collision-slice-6`
    3.5's drop-whole rule (as a new record, since the design is archived) and to ADR 0053's
    newest-gunfire rule; `bytesPerUpdate` in Server.cfg.
12. **C3 — the client clock and the first test under lag** (P1, both). A work order under
    `Design/Archive/Combat.md` 9 or a new `Design/ClientClock.md`; presentation only, plus the
    `QuicTransport` statistics accessor.
12. **E7 — the journal, the garrison invariant, F4 fenced** (P1, EVE). `Design/Economy.md` §5;
    the F4 fence is a one-line `ServerConfig` key and lands with the first slice.
13. **C2 — FleetIntent** (P1, Homeworld). Amendment to `Design/Archive/Combat.md` (a section on
    owner-only intent) recorded as the deliberate addition ADR 0009:47-48 provides for.
13. **E11 and C14 — the ship record: damage through the ledger, priced repair, fees, tolls**
    (P1, both). New `Design/ShipRecord.md` (the resolution of `Combat.md` 7.1 versus the code) with
    the economy's price table as a section; the toll waits for E15's gate-ownership design.
14. **C11 — verbs, parameters, FleetSetting** (P1, both) and **E8 — the harvest cycle and the
    route to a dock** (P1, both). One new `Design/Orders.md` (the RoE design `Combat.md` 14 owes):
    Guard, Activate, the range parameter, stance and formation on the row, the Mine phases, the
    route's terminal Dock, and the queue as the last section arguing with the patience rule; agrees
    and lands `Design/GalaxyMap.md` slice 3 on the way. A record amending ADR 0041 and 0050.
15. **C4 — prediction** (P1, Homeworld). Amendment to `Design/Archive/Collision.md` §10 option 2
    as a new `Design/Prediction.md`, since the design is archived.
15. **C6 — the session layer** (P1, EVE). New `Design/Session.md` beside `Design/CrossShard.md`,
    folding the cross-shard client half into CrossShard.md before it is agreed.
16. **C12 — formation as fleet state** (P1, Homeworld). Amendment to `Design/Archive/Combat.md` §8 and a
    record taking `FORMATION_SHAPE` out of the replay contract.
16. **E9 — refining** and **E10 — wrecks** (P1). `Design/Mining.md` §7-8 and `Design/Economy.md`
    §6; `DespawnCause::Decayed` through ADR 0040's door.
17. **C9 — the leashed interest centre and per-role descriptors** (P1, both). New
    `Design/Interest.md` amending `Design/Archive/Fleets.md` 9.1 by record; the sensor tier as a
    later section of `Design/Archive/Combat.md`.
17. **E12 — the market** (P1, EVE). New `Design/Market.md`: the system profile, the station's
    system index, the book, NPC seeding, `MarketRequest/Reply`, contracts as the last section.
18. **E13 — standings as a scalar** (P1, EVE). New `Design/Standings.md`, the standings-repair
    design ADR 0039:74-76 names, superseding Stations §15 decision 3 by record.
18. **C15 — shields, armour, tracking, capacitor, the support kinds** (P1, EVE). Amendment to
    `Design/Archive/Combat.md` §7 and a record superseding ADR 0052's alternatives paragraph.
19. **E14 (input-log half) — recovery from the last snapshot plus the input log** (P1, EVE).
    `Design/Persistence.md`, naming ADR 0057 and `Design/CrossShard.md` §4.
20. **E15 — sovereignty** (P2, EVE). New `Design/Sovereignty.md`: gate ownership read, the
    deployable station through C13's record, the station's death accounting, the finite garrison.

---

## Risks

Where the goal pulls against a standing decision, named, so the record that reverses it is written
on purpose rather than discovered in a diff.

- **ADR 0045 (fixed 60 Hz, capacity by entity count) versus the thousand-player fight.** The
  governor (C5) keeps the tick and stretches wall time, so under load the Homeworld bar and EVE's
  survival pull apart on the same server; the design must say that r < 1 is an EVE mode a
  skirmish server never enters. Static records — sites, wrecks, deployable stations (E3, E10,
  E15) — spend the same envelope, measured at forty ships (`ShipState.h:52-54`), and are uncapped
  until E3's per-system cap lands.
- **ADR 0051's "first ack anywhere" is a stated rule.** C1 reverses it and every later verb (C11),
  every EVE-dynamic refusal (E13) and the cross-shard refusal (C6) depend on the reversal. If the
  owner keeps the rule, those refusals stay silent by design.
- **ADR 0052's opacity argument** is the tree's best combat decision, and C13's class table and
  C15's tracking race both reopen it. Integer steps printed as labels are legible but are still a
  number; the records must say where "a miss is geometry" ends.
- **ADR 0009 withholds intent from everyone**; C2 and C4 hand a client its own steer targets. The
  threat model tolerates it because it changes nothing the server decides; the cross-cutting rule
  above keeps prices and balances off the same lane.
- **ADR 0013, 0039 and 0047 rule that players are not factions**, and every P0 economy item can
  ship on `FactionId` with a single player. That is the temptation this review forbids: the
  `OwnerId` field lands with the wallet even while the login does not exist.
- **ADR 0057 refuses an unknown format and never falls to genesis**, which is right, and every
  table here is a bump. Without E14's migration rule, pulled forward above, each P0 stops the live
  shard's boot.
- **ADR 0022's confinement of primitives and Universe.md 11's "one thread"** are both read against
  by C8; the frozen view table is the argument and the save-on-worker path rides the same record.
- **ADR 0041 and 0050 refused a sense twice**; C11's Aggressive stance admits it as a player
  choice, and the protectors and NPC fleets that share the machinery inherit whatever default the
  row carries.
- **ADR 0039's "no decay, no fines, no amnesty" and Stations §15 decision 3** make one attack order
  against a Vanguard station or its garrison a permanent, silent, empire-wide confiscation
  (`Universe.cpp:179-192`, `:1856-1866`;
  `FleetTests.cpp:673-690`). E13 supersedes it by record; at MMO scale the current rule is a churn
  engine.
- **ADR 0016 ("bodies are presentation") and ADR 0038 ("stations do not despawn")** are each
  reopened by their own clause — E3 for the rock, C13 and E15 for the mobile and destructible
  station — and `NoImmovableHullIsDestructible` (`HullSpec.h:279`) is the assert that names the
  design it is waiting for.
- **AGENTS.md's constexpr-tuning rule** is contradicted by C14's authored, versioned hull and device
  catalogues; a table hash in the save header keeps replays honest, and every recorded battle
  before the change is orphaned.
- **The dock is Homeworld's own behaviour and EVE's largest free repair.** Both panels resolve it
  the same way — carry the damage always, price repair in content — which asks one build to hold
  both benchmarks through a per-deployment economy switch the tree does not have; ADR 0043 is
  where it would live.
- **Three existing behaviours become farms the day any faucet keys off a kill**: free repair, the
  bottomless garrison whose "drops nothing" rule is prose in four places and code in none, and F4
  as a real host-side `Destroyed` despawn with no shooter (`OutpostApp.cpp:764-787`). E7's
  invariant and fence must land before the first bounty, wreck or insurance payout.
- **Combat-slice-5's window is narrow** (3800 versus 4000 hull points flips a fight) and its harness
  is not in the tree; C12, C13 and C15 all change the numbers, and C13's matrix test is the only
  thing that would notice, so it lands first.
- **The headless run loop, the dedicated-server root and the login** belong to no panel and no
  slice (AGENTS.md:116-117; `MmoScalabilityPlan.md:225-229`; `Publisher.h:28-35`). C5, C6, C8, C9
  and E1 depend on them; the fixed-tick-under-load story cannot be measured until a shard process
  exists, and every EVE item is keyed on a placeholder owner until a session says who is asking.
- **Everything the economy panel found is declared absent by the tree itself** (README.md:92;
  AGENTS.md:92-93; `Design/Archive/Universe.md:425-426`; `Design/Archive/Fleets.md:1018-1020`;
  `Design/Archive/Combat.md:474-480`) and handed to a mining design and a station-menu design that do not
  exist under `Design/`. The gap is honest; the risk is that each panel keeps deferring to the
  other's unwritten design while the one seam they share — an owner, a wallet, an item — has no
  owner. The order of work above gives it one.

---

## What this review may still miss

A completeness pass against both benchmarks, listing what a Homeworld or an EVE designer would ask
about that no item above names. Each line says where the tree was checked.

- **The simulation is a plane and Homeworld is 3D.** Every position is `y = 0` (AGENTS.md:76-77;
  `Design/Archive/MmoScalabilityPlan.md:140` "No 3D simulation: the plane is a product decision";
  ADR 0025) and nothing above mentions it; Homeworld's movement disc and vertical flanking are its
  signature tactical axis. EVE's fights are range and transversal and do not need it.
- **Progression and skills.** No skill, research or unlock exists (grep skill, progression: nothing
  in the tree; research is the HUD rail icon `Outpost/Hud.h:64` and E6's one line on blueprints).
  EVE's real-time skill queue is what makes a character persist independently of its assets;
  Homeworld's research tree paces which hulls a campaign can build.
- **PvE content: missions, NPC spawners, an AI opponent.** The only hostiles are the Vandal base's
  boot-time patrols (`GameLogic/StartingUniverse.cpp:99-113`); spawners and hostile production are
  declared out (`Design/Archive/Hostiles.md:522-523`) and NPC helm behaviour is deferred
  (`Design/Archive/Combat.md:562-563`). E7 names bounties and E13 names missions as faucets with nothing to
  pay them on. EVE's rats and agents are its faucet; Homeworld's scripted enemy fleets are its game.
- **Homeworld's campaign structure.** Scripted missions, level flow, the fleet carried between
  missions, per-mission save and difficulty that scales to the surviving fleet: "campaign" appears
  above only as a pacing word, and the tree has no such file (grep campaign, mission: nothing).
- **Corporations and social structures.** "Corporation" above is an `OwnerId` namespace (E1);
  shared wallets and hangars, roles, alliances and chat are unmentioned and absent (grep chat,
  alliance, guild: nothing). EVE's local chat is an intel tool and its corporation is the unit of
  sovereignty; Homeworld needs none of it.
- **New-player experience.** E1's `AdmitOwner` grants a starting fleet; nothing above or in the tree
  says how a player learns the game (grep tutorial, onboarding: nothing). Homeworld's campaign is its
  tutorial; EVE's NPE is its retention.
- **The floor under total loss.** What a player with zero hulls and zero credits does — EVE's free
  rookie ship, Homeworld's mission restart — is unstated; `AdmitOwner` grants once.
- **Anti-cheat beyond intent withholding.** Botting and macro mining once a Mine order runs
  unattended (E5), multiboxing and RMT are never named; the only rate limit is `ordersPerTick`
  (`GameLogic/Publisher.h:59-62`), and the byte-exact replay is never named as the audit instrument
  it is. EVE's problem; Homeworld's single process has no one to cheat.
- **Moderation.** Names, chat, reporting and bans: nothing in the tree or above (grep moderat: nothing).
- **Live-ops and content cadence.** E14 migrates the save; nothing covers how a live shard takes a
  client ALPN bump, a new hull table or a content release without a wipe or a downtime window.
- **The offline player.** Login is named as an external dependency, but no item says what a player
  record holds (home station, last shard, wallet) or what a disconnected player's fleets do in space
  (EVE's log-off timer); C6 resumes a subscriber within a session only.
- **Homeworld multiplayer.** Skirmish lobbies, matchmaking and a match's end condition are absent
  from the tree and unmentioned; the review assumes one persistent server for both benchmarks.

---

## Out of scope

This review did not judge rendering (the D3D12 pipeline, the planet and sky renderers, the NMO
content path, culling and instancing, which `Design/Archive/MmoScalabilityReview.md` covered),
audio (declared absent and irrelevant to either benchmark's mechanics), code quality and
conformance against AGENTS.md (the build guards and the rulebook's own gates cover it), UX polish
(the long-press-on-release and touch pre-highlight finding the combat lead dropped below the cap,
the HUD's typography, the minimap's readability), the content pipeline and tooling beyond
`UniverseGen`'s census line, the choice of MsQuic and the transport's internals below the
statistics accessor C3 asks for, and the account, login and dedicated-server story — which it names
as a dependency wherever an item stands behind it and leaves to the panel that owns it. It also did
not weigh the two benchmarks against each other: where an item serves one and not the other it says
so, and the choice between building the Homeworld loop first (the shorter path: E3, E4, E5, E6,
C1, C13) or the EVE loop first (the same six plus E1, E2, E7 and the governor) is the owner's.
