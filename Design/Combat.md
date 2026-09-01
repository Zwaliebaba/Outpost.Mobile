# Combat — mounts, gunnery, and the acts a shot states

**Status: agreed with the owner on 2026-08-31 — the six decisions in §15 were put and taken the
same day, each the recommended option.** Slices 1 to 5 landed on 2026-09-01 and are in review
([`Combat-slice-1.md`](Combat-slice-1.md),
[ADR 0052](Decisions/0052-gunnery-is-deterministic-and-the-fire-pass-states-the-acts.md)); the
world is lethal, says so on the wire, draws it — muzzle flashes, tracers and impacts in the shooter's
colours, condition pips on the fleet sheet, and F6/F7 retired because the simulation states its own
acts now — and has been measured against §13's pacing targets and retuned twice on what that found
(slice 5). What is left is slice 6: the turret geometry turning, and the `Gun` markers and their
consistency check, all three of which wait on the same binding between a mount and the part that
carries it. **The screenshots slice 4 is accepted by are owed**, and slice 6 pays them.

**A factual correction, taken on 2026-09-01:** §3.1 and §10.1 describe the shipped Battleship as
carrying "three turret submeshes with barrel bones" and plan for `MeshData` to grow bones and clips.
**The bones are not there.** Every hull in the game is named rigid submeshes with no skeleton, no
clip and no skin buffer; the only file in the tree with a rig is the golden fixture. The turrets are
real and so are the barrels — they are separate named submeshes — so a turret still turns, about its
own bind-pose centre rather than about a bone. `Combat-slice-3.md` §2.6 has the table and the
consequence, and the sections below keep their argument unamended, which is what this file does with
a claim a slice overtakes.

**Two more sentences the slices overtook, recorded the same way.** §1 and §9.2 call the fire
events a "block"; they shipped as their own datagram message instead, because the fleet status block
rides *every* fragment so that it heals, and a list of events stamped on every fragment would draw
every tracer once per fragment ([`Combat-slice-2.md`](Combat-slice-2.md) §2.3). Only the word is
wrong — the lane argument §9.2 makes is the one ADR 0053 records, untouched. And §1's "turrets slew
and flash at authored `Gun` markers" is slice 6's sentence rather than slice 4's: the slew needs a
submesh-range draw `SceneRenderer` does not have and a screenshot to accept it
([`Combat-slice-4.md`](Combat-slice-4.md) §2.7), and the markers wait on the binding that would read
one ([`Combat-slice-5.md`](Combat-slice-5.md) §3). What ships today is §3.1's own fallback rule — a
mount without an authored marker draws its effects from the hull's origin — and §2's socket row
naming "per-submesh skeletons and clips" retires with the bones above: what §10 will ride on is
named submeshes and a client-side table.

**One correction this document has already taken:** §8's stand-off reads the shortest range among
a hull's *traversing* mounts rather than among all of them, so that a bow-fixed hull takes no
stand-off and flies the attack runs the same section describes. The first reading contradicted its
own next sentence, and the work order caught it before a line was written.

This is the design the rest of the tree has been writing IOUs against. ADR 0041 closed with "the
combat design meets this at two named sockets and needs nothing else: it calls `RecordAggression`
on the first hostile act against a station, and it gives the shadowing protector its guns." ADR
0050 said the same at fleet grain — "it calls `RecordHostileAct` on the first hostile act and
gives the shadowing combatants their guns" — and left it "two questions this record deliberately
leaves open: what friendly fire means, and whether a fleet should ever run." Stations §8.4
assigned senses — aggro radii, threat assessment, target switching — to combat by name. Hostiles
§12 deferred target brackets and health bars to it. Fleets §14 parked a damage column and a pip
row on the sheet that read nothing until it exists. This design pays those debts and, deliberately,
no other.

The player-facing sentence: **the attack order stops being a threat and starts being one.** Fleets
already pursue, shadow, rouse and stand down; protectors already hunt; the wire already says
*destroyed* and the client already answers with a shatter, a shock ring and `SHIP LOST`. What is
missing is the middle of the sentence — a gun that fires, a number that falls, a death that has a
cause inside the simulation instead of behind the F4 key. And because the instrument of all of it
is the **mount** — a hardpoint that carries a device — this design shapes that seam so the mining
design can later hang a different tool on the same arm (§12) without reopening anything here.

---

## 1. What is being built

- **A device table and a mount table** (§3): authored, `constexpr`, simulation-side content beside
  `HULL_SPECS` — what a gun is (range, cooldown, traverse, damage) and where each hull carries one
  (bearing, arc, device). In the replay contract like every column of `HullSpec`.
- **Per-mount live state** (§3.2): an aim bearing, a cooldown, a held target — one small table
  parallel to `m_ships`, in the state codec, withheld from the wire as the intent it is.
- **Deterministic gunnery** (§4): a shot happens exactly when geometry allows — in range, in arc,
  aim settled, cooldown spent. No dice, no generator in the contract; a miss is a turret losing a
  crossing target, which is a tactic, not a roll.
- **The fire pass** (§5): `StepMounts`, one more standing-behavior pass inside `World::Step`
  (ADR 0015), gather-then-apply like the dock captures and the launches. Senses live in the
  mounts, not the helm: no hull changes course because of what it sees.
- **A shot is a stated act** (§6): the fire pass is the caller ADR 0041 and ADR 0050 were waiting
  for. `RecordHostileAct` on a hit against a ship; `RecordAggression` on a hit against a station
  or its garrison. No client message states an act — the simulation observes its own shots.
- **Damage and death** (§7): one number, `hullPoints`; at zero the ship leaves through
  `DespawnShip(handle, DespawnCause::Destroyed)` — ADR 0040's door, already wired to the shatter,
  the shock ring, the departure runs and the fleet prune. Immovable hulls discard damage this
  design; the act still counts.
- **Engagement motion** (§8): pursuit gains a stand-off, so a gunship holds where its guns bear
  instead of ramming its quarry; fixed-gun hulls keep flying attack runs, which their arc gives
  them for free.
- **The wire** (§9): one byte of hull fraction in the ship record, self-healing like everything
  else in it; one loss-tolerant fire block on the datagram lane for the view's flashes and
  tracers. Death already rides the reliable lane and keeps doing so.
- **The view** (§10): the reader stops discarding the rigs it already validates; turrets slew and
  flash at authored `Gun` markers; the fleet sheet's pip row reads the number it was placed for;
  the event log stays edge-triggered.
- **The answers owed** (§11): friendly fire and fleeing — answered, not deferred again.

## 2. What this design builds on, and the sentences it retires

The sockets, by name, all verified in the tree as it stands:

| Socket | Where | What this design does with it |
|---|---|---|
| `RecordHostileAct(attacker, victim)` | `World.h`, Fleets §7.1 | called by the fire pass on every landed hit against a ship |
| `RecordAggression(attacker, station)` | `World.h`, Stations §8.1 | called on every landed hit against a station or its garrison ship |
| `PursueTarget` — "one function with two masters" | `World.cpp`, ADR 0050 | gains a stand-off (§8); stays the only chase |
| `HullSpec::combatant` | `HullSpec.h`, ADR 0050 | unchanged; decides who *moves* to a fight, while mounts decide who *shoots* (§5.3) |
| `Fleet::threat` / `threatAnchorPos` / `alertTicks`, leash + alert | `World.h`, Fleets §7 | unchanged; becomes lethal because the pursuers now carry working guns |
| `Station::targets`, protector launch metronome | `World.h`, Stations §8.2–8.3 | unchanged; a protector's duty target is its mounts' priority |
| `DespawnCause::Destroyed` → leave runs → `ExplodeTheLost` | ADR 0040, `WorldView.cpp` | the fire pass becomes the first in-simulation caller |
| `FleetOrderKind::Attack`, armed and flowing end to end | Fleets §6.5, `FleetSheet.cpp` | unchanged on the wire; finally means what it says |
| `Gun` markers, turret submeshes, per-submesh skeletons and clips | NmoFormat §5.10, §7 | the view's anchor for muzzle, tracer and slew (§10) |
| Fleet status bits 6–7, the red pulse, `SHIP LOST`, the pip row | Fleets §8.2, §9.3 | the pips and the target bar start reading (§10.3) |

Truth maintenance, because the rulebook demands it in the same commits: "there is still no combat"
and its siblings in `README.md` and `AGENTS.md`; `World.h`'s "nothing inside Step states an act"
(§6 changes that sentence on purpose); `OutpostApp.cpp`'s F4 comment "nothing in the game can
destroy a ship"; the `FleetOrderKind` comment in `ShipState.h` describing what Attack waits for.
Each slice retargets the sentences its change falsifies.

## 3. The mount and the device

### 3.1 Authored simulation content, never read from a mesh

A **device** is what fires: range, cooldown in ticks (ADR 0045 — never seconds off a clock),
traverse rate, aim tolerance, damage. A **mount** is where a hull carries one: a bearing in hull
frame, an arc it may bear through, and the device id. `GameLogic/DeviceSpec.h` holds the device
table; `HullSpec` grows `mounts[MAX_MOUNTS]` and `mountCount`, `constexpr` beside every other
column, because every field changes recorded outcomes and belongs in the same contract by the same
route (`SimTuning.h`'s own words).

The meshes already speak turret — the shipped Battleship carries three turret submeshes with
barrel bones, the Corvette two, and the format defines the `Gun` marker with "+Z = muzzle" — but
the simulation reads none of it, for NmoFormat §9's stated boundary (ADR 0002): what the
simulation needs of a hardpoint arrives as authored numbers in `GameLogic`, generated or checked
offline by a `Tools/` script at most, never read from content at runtime. The capsule table
already made this exact argument about size: the mesh fit is where the numbers start, not what
they are. A mount without an authored marker draws its effects from the hull's origin — content is
a diagnostic, never a crash — and the `Tools/` check (§16, slice 5) is what keeps the two tables
from quietly disagreeing. No shipped hull carries a `Gun` marker today; that is authoring work the
slice plan owns, not a blocker for the simulation half.

### 3.2 Per-mount state

Three fields per mount: `aimBearingRad` (hull frame — bounded, and a heading change costs the
turret nothing it should not pay), `cooldownTicks`, and a held `ShipHandle target`. A dense table
parallel to `m_ships`, `MAX_MOUNTS` per ship, swap-and-popped with the other parallel tables, in
the state codec with a `WORLD_STATE_FORMAT` bump.

The held target is `avoidHeadingRad`'s argument at the gunnery scale: without it a mount flickers
between two targets that score within noise of each other, resetting its traverse each time. A
held target is re-validated every tick — live, still valid under §5.3's rules, still inside the
envelope — and dropped the moment it is not; holding is a tie-break, never a commitment.

Aim state is intent, and the snapshot exists to withhold intent: none of this table reaches the
wire. The view slews its turrets from the fire block (§9.2) and its own clock, which may drift a
degree from the simulation's aim and is allowed to — presentation, ADR 0016's side of the line.

## 4. Gunnery is deterministic mechanics

A mount with a target fires on the tick where all four hold:

1. **Range** — centre distance minus the target's bounding radius is within the device's range;
2. **Arc** — the bearing to the target lies inside the mount's authored arc;
3. **Aim** — the mount has traversed to within `FIRE_ALIGN_RAD` of that bearing, at its device's
   traverse rate, which is the only thing aim state does between shots;
4. **Cooldown** — spent, and reset by firing.

A shot that fires, hits, and lands its device's damage number. There is no to-hit roll and no
damage roll, and that is a decision, not an omission (§15, decision 1): the one generator in this
tree is `Neuron::Pcg32` and nothing in the replay contract draws from it at tick time — combat
would be the first, and it would buy opacity with the noise. Forty ships fight on screen at once;
"why did I miss?" must have an answer the player can see. Here the answer is always geometry: out
of range, out of arc, or the turret lost the traverse race. A heavy turret at 18°/s cannot hold a
fighter crossing at 20°/s point-blank — the same fighter at 300 m subtends 6.5°/s and is tracked
and hit. Closing in *under the guns* versus standing off in their envelope becomes the tactical
sentence of every engagement, and it falls out of three authored numbers per device with no
mechanism built for it.

**What lost.** *Dice* (EVE's model): adds a generator to the contract and answers "why did I miss"
with a number the player never sees; rejected for opacity, not for determinism — seeded dice
replay fine, they just explain nothing. *Live projectiles* (simulated shells as entities):
dodgeable fire is genuinely richer, but every round in flight is an entity in the index, the
interest sets and the wire; ADR 0045 has no swept test and a fast shell at 60 Hz re-opens it; and
the fleet-grain game gives the player no way to *use* dodgeable rounds — nobody is steering one
ship. The door is not nailed shut: a slow, killable torpedo as a real entity is the Bomber's
long-term identity and one more design (§14), and it will co-exist with hitscan guns the way the
capsule co-exists with the mesh. This design ships hitscan resolved in the fire pass, with the
projectile drawn by the view as long as it likes (§10.2).

## 5. The fire pass

### 5.1 Position and idiom

`StepMounts`, last in the standing-intent slot, after `StepFleets` — so a mount reads the fleet
posture decided *this* tick (the threat taken, the chase re-lowered), and every geometric read is
end-of-last-tick state like every other pass, which keeps the answer independent of array order
(Collision §6's property, restated once more because it is the property).

Gather-then-apply, the dock pass's idiom: the walk chooses targets and fires, accumulating damage
into a scratch indexed by ship, hits into an act list, and nothing mutates mid-walk. After the
walk, in order: damage lands (immovable hulls discard theirs, §7.2), acts are stated off the hit
list (§6), and ships at zero are despawned through the capture idiom, exactly as if the deaths had
arrived from outside the tick — which is a case every table already survives. Two ships that kill
each other on the same tick both die, in either array order, and the test suite says so.

### 5.2 Target priority

A mount evaluates candidates in one fixed order and takes the first that stands:

1. the fleet's **threat**, while the fleet is engaged (defense outranks everything, Fleets §7.4);
2. the fleet's **ordered attack target** (the standing order, doing its job);
3. the ship's **protector duty target** (a protector is in no fleet; this is its whole life);
4. **opportunistic**: the nearest ship in the neighbour list whose faction the mount's owner holds
   `Standing::Hostile`, inside the device's envelope — nearest by `(proximityMetres, ShipId)`,
   which is already the sense pass's own deterministic order.

Priorities 1–3 are stated handles; only priority 4 is a sense, and it is the sense Stations §8.4
assigned to this design. It reads **only the neighbour list the sense pass already built** — no
new query, no cadence, no scan. The honest consequence of the neighbour cap comes with that: a
hull whose K nearest are all friends does not see the enemy K+1 away, holds its fire, gets shot,
and the *stated act* takes over from there — the failure self-corrects through the defense, and a
combatant's `neighbourCap` is one authored column away if measurement says it matters. The gather
radius grows a third term so the list can contain what the guns can reach: `QueryRadiusMetres`
takes the max of avoid, separate, and now `ownRadius + longest mounted range + margin`. For
capitals the avoidance term still dominates (a Battleship's 8 s horizon out-reaches its own
guns); for a fighter the gunnery term adds tens of metres to a query that narrowed below its gun
range the day the extent narrowed. Measured before believed, like the margin that section already
carries.

### 5.3 What a mount may target

Live, not itself, not its own faction — ever (§11). Priorities 1–3 shoot the stated handle
whatever the standing says, because an *ordered* attack on a neutral is the player spending their
own standing (that is F6 becoming gameplay); priority 4 fires only on `Standing::Hostile`, so no
one drifts into a war by parking near it. No radius makes anyone a criminal (ADR 0041) — and no
radius starts shooting at anyone the shooter's faction has not already judged or been ordered at.

### 5.4 Senses live in the mounts, not the helm

Nothing in this pass steers. The Vandal patrol keeps its metronome exactly as Hostiles §5.5
promised — and now shoots back over its shoulder as it circles, because its mounts acquire
opportunistically while its helm reacts to nothing. A loose ship defends itself precisely that
far: guns yes, course no. The first helm that *reacts* — breaks off, kites, flees — is the NPC
behaviors design (§14), and the fleet defense already covers the player's ships. This is the
narrowest reading of "senses and thresholds" that makes the world dangerous, and it is deliberate:
approach the Vandal ring today and the first shot, the stated act, the roused fleet and the fight
all fall out of machinery that already exists.

## 6. A shot is a stated act

The fire pass states, from its hit list, after damage lands:

- a hit on a **ship** → `RecordHostileAct(shooter, victim)` — every hit, because "any act on any
  member resets it" is Fleets §7.3's definition of the alert, and the latest act winning the
  threat slot is `RecordHostileAct`'s own semantics already;
- a hit on a **station's structure** or on a **garrison ship** with a live duty →
  `RecordAggression(shooter, thatStation)` — Stations §8.1's sentence "on the first hostile act
  against a station or its garrison", idempotent because the standing flip and the target list
  both already tolerate repetition.

`World.h`'s "nothing inside Step states an act" was true and dated the day it was written — the
acts were always going to come from the one pass that can observe a shot. What ADR 0041 actually
forbids is unchanged and load-bearing: **no client message states an act, and there never will be
one.** A client sends orders; the simulation fires, observes itself firing, and judges. The ADR
this design owes (§16, slice 1) records that completion rather than leaving the sentence's change
to be discovered in a diff.

F6 and F7 retire in the last slice, replaced by the acts they were standing in for — an attack
order on a Vanguard asset *is* F6 now, and any landed hit *is* F7. F4 stays: a tuning hook for the
explosion is still a tuning hook.

## 7. Damage and death

### 7.1 One number

`ShipState` gains `hullPoints`, spawned at the hull's authored `maxHullPoints`, clamped at zero,
never regenerating — repair is a station-menu design (§14). One number, no shields, no armor
types, no facing modifiers: every one of those is a fine later design that would today multiply
the tuning surface before the first fight has ever been measured. The snapshot carries its
fraction (§9.1); the exact number stays server-side.

### 7.2 Death, and who may not die

Zero despawns through `DespawnShip(handle, DespawnCause::Destroyed)` — the same door F4 uses, so
the shatter, the shock ring, the camera shake, `SHIP LOST`, the fleet prune, the protector
stand-down and the departure runs all answer without a line of new code. That chain is the reason
this design is smaller than it looks.

**Immovable hulls discard damage this design.** For a Vanguard station that is Stations §8.5's
standing rule ("however it models damage, a Vanguard station's is discarded") — implemented here,
permanently. For the Vandal base and the Stargate it is scope: a station's death drags a ledger,
a garrison, a docked fleet's manifest and a layout mark behind it, and that is the
station-destructibility design's page, not a side effect of the first gun (§14). The act is still
stated — shooting a station makes an enemy, it just does not yet make a wreck. A protector that
dies drops nothing, which Stations §8.6 already ruled and a game with no loot satisfies for free;
the rule is restated here so the loot design finds it, keyed to the garrison, not the faction.

## 8. Engagement motion

`PursueTarget` today plans to the target's own position, which was right for shadowing and is
wrong for gunnery: eight ships aimed at one point arrive as a scrum with their turrets parked on
their neighbours' hulls. The chase keeps its chassis — re-plan on `PURSUIT_REPLAN_METRES` of
drift, never per tick — and gains a stand-off: the destination becomes the point
`ENGAGE_STANDOFF_FRACTION` of the hull's shortest **traversing** mount's range along the bearing
from target to pursuer, so a gunship holds where *all* its turrets bear and the separation solver
stops being the thing that ends every fight. A hull whose mounts are all bow-fixed takes no
stand-off at all and is sent at the target itself, because its behavior comes from its arc: to
satisfy §4's gate it must point at its quarry, so a fighter flies attack runs and overshoots
without one line written for attack runs. Unarmed combatants — there are none in the table today,
but the Q-ship comment in `HullSpec.h` insists one stays expressible — shadow at the old range,
which is the degenerate case of the same arithmetic.

The furball is accepted, named, and bounded: fighters orbiting inside each other's envelopes read
as a dogfight and the avoidance table already keeps them apart; capitals hold painted stand-off
rings by construction. What this design does not add is formation combat — line abreast, focus
doctrine, screening assignments are RoE work (§14).

## 9. The wire

### 9.1 Hull fraction in the record

One byte, `hullFraction` (255 = whole), in the ship record beside `flags`: 47 → 48 bytes, and the
fragment capacity re-derives itself the way `WorldSnapshot.h` promises. It is state that heals —
the next update corrects a lost one — so it belongs in the record, ADR 0029's own test answered
the record's way. Every subscribed ship carries it, hostiles included, which is what target bars
read (§10.3). The exact points, the attacker, and every mount's state stay withheld with the rest
of the intent (Fleets §8.3 keeps withholding the threat's identity).

### 9.2 The fire block

A per-update datagram block — shots since the subscriber's last update where shooter or target is
in its interest set: shooter entity, target entity, mount index; capped, oldest dropped first.
Loss-tolerant on purpose, and the lane argument is worth recording (§16, slice 2 owes the ADR):
ADR 0029 asks *"if this message is lost, does a later one make it right?"* — and a fire event's
only consumers are a muzzle flash, a tracer and a turret slew. The authoritative consequences of
the shot travel elsewhere and reliably: the fraction in the record, the death in the leave run. A
lost flash is not a lie, so it takes the lane where late is worse than lost. Kill attribution on
the wire — *who* destroyed you — is deliberately absent this design (§14); the leave run already
says *that* you were destroyed, and attribution is a UI debt the cause door can carry later.

### 9.3 Format discipline

`WORLD_STATE_FORMAT` bumps for the codec fields (§3.2, §7.1); the ALPN string bumps with the
record and block changes so two builds disagree at the handshake, not in a misparse — both rules
already written, both merely obeyed.

## 10. The view

### 10.1 The rig finally pays

`NmoReader` validates submesh skeletons, clips and bone-parented markers and then flattens them
away; `MeshData` grows what it already proved — named submeshes, bones, clips, and `parentBone`
on markers — and the pose evaluator takes NmoFormat §7's licensed shortcut: the common turret is
one rigid submesh riding one bone, a per-submesh world transform, no skinning. The Battleship's
three turrets and the Corvette's two start turning the day this lands; hulls whose art has no
turret submesh lose nothing.

### 10.2 What a shot looks like

From the fire block: muzzle flash at the mount's `Gun` marker (hull origin when unauthored, §3.1),
a tracer or bolt to the target — drawn as long as the view likes, because the hit already
happened and presentation owns time on its side of the wire — an impact flash on the target, and
the turret submesh slewing toward the last target its mount was seen firing at, at the device's
traverse rate for honesty and at nobody's expense when it drifts. All of it on the existing FX
pipelines; nothing new in the renderer's contract.

### 10.3 Readouts, with restraint

The fleet sheet's pip row reads each member's `hullFraction` — the column was placed for this
(Fleets §9.3). A thin bar joins the selection bracket of the **ordered target only**: the player
reads their own fleet on the sheet and their quarry on its bracket, and nobody else grows UI. The
event log stays edge-triggered — `FLEET %d UNDER ATTACK` and `SHIP LOST` already exist; slice 4
adds at most a completion edge (`TARGET DESTROYED`) and refuses per-hit lines, because an 8-entry
ring in a firefight is exactly why the rising-edge rule exists.

## 11. The two answers owed to ADR 0050

**Friendly fire: none, structurally.** A shot resolves against its acquired target and only ever
lands there — hitscan has no bystanders — and §5.3 makes an own-faction target unacquirable at
every priority, while `IssueFleetOrder` refuses an attack order naming the issuer's own ship the
way it refuses a slot that is not theirs (ADR 0014: the simulation refusing is a property). The
day a splash weapon exists, *it* reopens this answer; a design with none does not pre-pay that
bill.

**Fleeing: no.** The defense posture stays exactly ADR 0050's — combatants engage inside alert
and leash, everyone else flies their orders, and nobody runs, because "where safety is" is a
judgment this game has not yet given anything the senses to make. Hold-fire, withdraw thresholds
and escort doctrine arrive as the RoE design (Fleets §14 reserved them), on top of stances this
design deliberately does not add.

## 12. Mining and the mounts

The mining design owns everything about mining except the arm it hangs its tool on, and that arm
is why devices are devices and not weapons. What this design guarantees it, and no more:

- **The device table is tool-shaped.** A device kind byte (`Gun` now, `MiningTool` reserved the
  way `FleetOrderKind::Mine` reserved its byte) and stats that read as "cycle" as naturally as
  "cooldown". A mining cycle is range + cooldown + yield where a shot is range + cooldown +
  damage; the fire pass's envelope, traverse and hold-a-target machinery is the extraction
  machinery, unchanged.
- **The mount table already dresses every hull**, so giving the Miner two tool mounts is a table
  edit, not a schema change — `combatant` stays honestly `false` on it (ADR 0050's Q-ship
  argument, run the other way).
- **What it still needs, and must bring itself:** something to mine that the simulation can see.
  ADR 0016 named its own reversal — "when a body first has to be flown around, it becomes a hull
  row" — and Stations §6.1's pattern fits it exactly: a minable rock is a ship on an immovable
  hull with a side-table row (resource, remaining units), placed by `LayOutSystem` so both halves
  agree where it is, which today they do not (the six rendered rocks are seeded per client and
  reseed on F5). Cargo, unloading into a ledger, and the meaning of a full hold are the same
  design's, beside `FleetOrderKind::Mine` finally leaving `Unsupported`.

Nothing in §16's slices touches any of that; this section exists so the mining designer reads one
page and knows which half is already theirs.

## 13. Numbers — a starting table, and the measurements that will replace it

Every number below is a starting point written to be measured against the targets beside it,
`HullSpec.h`'s own discipline: the mesh fit is where they start, not what they are.

**Pacing targets** (the contract; the tables serve them):
- one fighter under one peer's gun: dead in ~10 s — long enough to disengage, short enough to matter;
- one fighter under a full fleet's focus: ~1.5 s — focus is lethal, screening is a job;
- a Frigate under two fighters: ~40 s — fighters harass capitals, Bombers kill them;
- a Battleship under a mixed eight-fleet: ~1.5 min — a capital is an event, not a target;
- every range inside the leash (1000 m) and well inside interest (2000 m), so a fight never
  out-ranges the machinery that bounds it — the biggest gun at 420 m leaves the leash room to mean
  something.

**Devices** (cooldowns in ticks, ADR 0045):

| Device | Range m | Cooldown | Damage | Traverse °/s | Notes |
|---|---|---|---|---|---|
| LightGun (fixed) | 160 | 30 (0.5 s) | 3 | hull's own turn | fighter bow gun; arc ±20° |
| StrikeCannon (fixed) | 240 | 360 (6 s) | 90 | hull's own turn | Bomber's argument; arc ±10° |
| LightTurret | 180 | 45 (0.75 s) | 5 | 180 | tracks anything; the escort's tool |
| MediumTurret | 280 | 90 (1.5 s) | 18 | 60 | the line weapon |
| HeavyTurret | 420 | 240 (4 s) | **40** | 18 | loses a crossing fighter inside ~110 m by §4's arithmetic — on purpose |

**Loadouts and hull points** (mounts the shipped art can mostly already wear — the Battleship's
three turret submeshes and the Corvette's two are authored today):

| Hull | Mounts | maxHullPoints |
|---|---|---|
| Interceptor | 1 × LightGun, bow | 60 |
| Bomber | 1 × StrikeCannon, bow | 150 |
| Corvette | 2 × LightTurret | 240 |
| Miner | — (the mining design arms it with tools) | 200 |
| Frigate | 2 × MediumTurret | 520 |
| Hauler | — | 420 |
| Battleship | 3 × HeavyTurret, 2 × LightTurret | **3800** |
| Carrier | 4 × LightTurret | 5200 |
| Stargate / Structure | — | damage discarded (§7.2) |

Sanity against the targets: 60/6 dps = 10 s; 60 under eight fighters ≈ 1.25 s; 520/12 ≈ 43 s;
a Bomber pair puts 30 dps on a Frigate ≈ 17 s, which is the Bomber's job description; the
Battleship's 2400 under ~100 mixed-fleet dps ≈ 24 s — short of the 1.5 min target, so either its
points rise toward 6000 or the target softens, and *that decision is taken by playing it*, slice 5's
hand-back, not by this table.

**Slice 5 played it, and the two numbers in bold above are what it came back with**
([`Combat-slice-5.md`](Combat-slice-5.md) §2). The Battleship's points did not rise to 6,000 and
could not: the matchup is bimodal, so raising them alone walks from a 45 s kill straight to the
Battleship wiping all eight. Its *output* had to come down first — 70 damage a heavy turret killed a
Corvette in 3.6 s, so the fleet lost its damage faster than it could spend it — and at 40 damage with
3,800 points a mixed eight grinds it down in **81.6 s** against the 90 s target.

**Two of the five targets are corrected rather than met**, and this is the correction. *A fighter
under a fleet's focus* cannot be 1.5 s while *a fighter under one peer* is 10 s: the ratio between
one Interceptor's gun and eight Corvettes' is more than six to one, so the same hull cannot do both.
Focus fire deletes a fighter in under a second, which was the intent; the number was aspirational.
And *a Frigate under two fighters* is not 40 s but a fight the Frigate wins — its 24 dps kills each
fighter in 2.5 s, well before their 43 s of throughput lands. The sentence beside the target already
said "fighters harass capitals, **Bombers kill them**", and the second half measures at 17.3 s; the
first half does not survive contact, and making it true would need a fighter that can trade with a
Frigate, which breaks the first target. New `SimTuning.h` constants: `FIRE_ALIGN_RAD`,
`ENGAGE_STANDOFF_FRACTION`, the fire-block cap, the gather's gunnery margin — each with the
argued comment the file demands.

## 14. Deliberately left out, so nobody goes looking

- **Shields, armor classes, resists, facing damage** — one number first (§7.1).
- **Live projectiles and the torpedo** — the Bomber's future identity, its own design (§4).
- **Stances and rules of engagement** — hold-fire, withdraw, focus doctrine, escort assignment
  (Fleets §14 reserved them; §11 keeps them reserved).
- **NPC helm behavior** — breaking off, kiting, aggro-driven course changes; guns react this
  design, helms in the next (§5.4).
- **Station destructibility** — the ledger, garrison, manifest and layout questions it drags
  (§7.2); user-owned stations inherit the counter-rule when their design arrives.
- **Wrecks, loot, salvage, capture** — each is one more `DespawnCause` through ADR 0040's door,
  none is this design's.
- **Kill attribution to the player** — the wire deliberately does not say who (§9.2).
- **Repair and resupply** — the station-menu phase Stations §14 already owns.
- **Carrier wings** — a Carrier fields point defense today and a hangar design later.
- **Mining itself** — §12 is the whole of this design's obligation to it.

## 15. Decisions taken by the owner

Six decisions were put to the owner on 2026-08-31 and taken the same day; each was the
recommended option. What lost is recorded beside what won, so the next proposal of a loser
starts from why it lost rather than from silence.

1. **Gunnery resolves deterministically** (§4). Seeded dice lost for opacity, not for
   determinism — they replay fine and explain nothing at forty ships on screen. Live projectiles
   lost to their cost — every round an entity in the index, the interest sets and the wire, and a
   fast shell reopening ADR 0045 — and to fleet-grain command's inability to cash what dodgeable
   rounds buy. The slow, killable torpedo stays a named future design (§14), not a casualty.
2. **Opportunistic fire ships in slice 1** (§5.2–5.4). Mounts acquire standing-hostiles from the
   neighbour list on their own, so the Vandal ring is dangerous from the first lethal build. The
   quieter stated-targets-only opening lost: it defers exactly the emergent first fight this
   design exists to produce, and the acquisition rule costs the same few lines either way.
3. **The pacing band stands as §13 states it** — fighter ~10 s under a peer and ~1.5 s under
   focus, a Frigate ~40 s under two fighters, a Battleship ~1.5 min under a mixed eight. The
   faster band lost for starving command of reading time; the slower one for making focus fire
   feel like nothing. Every table in §13 serves these targets and retunes with them, never
   against them.
4. **Friendly fire: none, structurally** (§11). Own-faction attack orders (the scuttling case)
   lost — nothing in the game rewards one and a touch screen invites the misclick. Positional
   friendly fire lost to the swept tests the tick deliberately lacks (ADR 0045) and to punishing
   a formation solver the player does not steer. A future splash weapon's design reopens this
   answer; nothing else does.
5. **Nobody flees** (§11). The posture ships exactly as ADR 0050 wrote it. Break-off-when-hurt
   lost as a tuning surface bought before the first fight was ever measured; fleeing
   non-combatants lost because "where safety is" is a judgment that record deliberately refused
   to give the simulation. Both belong to the rules-of-engagement design (§14).
6. **F6 and F7 retire in slice 4** (§6, §16). An attack order on a Vanguard asset *is* F6 now,
   and any landed hit *is* F7; keeping hooks that state acts the simulation never observed lost
   to exactly that sentence. F4 stays — a tuning hook for the explosion is still a tuning hook.

## 16. Slices

One agent per slice, one slice per layer at a time; each retargets the sentences it falsifies
(§2) and lands its ADRs with the change they explain.

1. **The fire pass** (`GameLogic`) — `DeviceSpec.h`, `HullSpec` mounts, the mount-state table and
   codec bump, `StepMounts` with §5's priorities and §6's acts, `hullPoints` and death through
   the despawn door, the stand-off in `PursueTarget`, the gather's gunnery term. Tests: same seed
   same battle to byte equality; fire results invariant under array order; mutual kill; the
   leash-and-alert dance with real guns; a protector killing its aggressor and docking home; an
   attack order on a Vanguard station flipping the law and launching the garrison; immovable
   damage discarded while the act states. ADR due: *combat resolves deterministically, and the
   fire pass states the acts* (completing 0041/0050).
   Work order: [`Combat-slice-1.md`](Combat-slice-1.md). **Landed and in review 2026-09-01**, with
   [ADR 0052](Decisions/0052-gunnery-is-deterministic-and-the-fire-pass-states-the-acts.md).
2. **The combat wire** (`GameLogic` seam) — `hullFraction` in the record, the fire block,
   receiver accessors, ALPN and format bumps. Tests beside the existing snapshot suite; ADR due:
   *fire events ride the datagram lane* (ADR 0029 applied).
   Work order: [`Combat-slice-2.md`](Combat-slice-2.md). **Landed and in review 2026-09-01**, with
   [ADR 0053](Decisions/0053-fire-events-ride-the-datagram-lane.md).
3. **The rig** (`NeuronClient`) — `MeshData` grows submeshes, bones, clips and marker
   `parentBone`; the per-submesh pose shortcut; reader tests against the golden fixture, whose
   turret has been waiting for exactly this slice. Parallel-safe with slice 4 by layer.
   Work order: [`Combat-slice-3.md`](Combat-slice-3.md). **Landed and in review 2026-09-01**, and
   narrower than this line reads: submeshes and marker `parentBone` landed, bones and clips did not,
   because no shipped hull has one to pose (§2.6 there, and the correction at the top of this file).
4. **The look and the readouts** (`Outpost`) — muzzle, tracer, impact off the fire block; turret
   slew; pips and the target bar; the completion edge in the log; F6/F7 retire per decision 6.
   Screenshots at two sizes, a fight and a quiet frame.
   Work order: [`Combat-slice-4.md`](Combat-slice-4.md). **Landed and in review 2026-09-01, less the
   turret slew**, which is cut out as slice 6 below: it needs a renderer entry point that does not
   exist, and a screenshot is the only thing that can accept it. **The screenshots are owed**, and
   are recorded as owed for the reason `Fleets.md` records its own three.
5. **Content and the check** (`Tools/` + assets + tuning) — `Gun` markers authored (scriptable
   offline), the mount-vs-marker consistency check beside `CheckProjectFiles.py`, and the
   measured hand-back against §13's targets with the numbers that actually shipped.
   Work order: [`Combat-slice-5.md`](Combat-slice-5.md). **Landed and in review 2026-09-01, as the
   measurement and the retune alone**: the marker authoring and the cross-check move to slice 6,
   because slice 3 found the position a `Gun` marker would carry is already exact in the art, and
   the thing that would read one is slice 6's binding (§3 there).

6. **The turret turns, and the content it needs** (`NeuronClient` + `Outpost` + `Tools/`) — a
   submesh-range draw on `SceneRenderer`, the hull drawn as its own complement, and a client-side
   table binding a hull's mounts to the parts that carry them. It is a slice of its own because it
   is the only piece of this feature that reaches into the D3D12 command list, and because slice 3
   found the shipped hulls carry no rig: the binding is a client table read off submesh names, which
   is where ADR 0002 would have put it. A decision record is likely due for that table.
   It also inherits the two content items slice 5 lists beside its measurement and deliberately did
   not do ([`Combat-slice-5.md`](Combat-slice-5.md) §3), because both wait on that same binding:
   **the `Gun` markers**, authored where one table will read them rather than as a third copy of a
   position the art and `HullSpec` already carry; and **the mount-versus-marker consistency check**,
   which needs a place that can see the simulation's table and the game's art at once and has to
   choose between a generated table and a parse of `HullSpec.h`. `Tools/NmoShippedArtTest.py`
   (slice 3) already checks everything about the art that can be checked without the table.
   **The screenshots slice 4 is accepted by are owed and are this slice's to pay**, since it is the
   one that changes what the same frames show.

Dependencies: 1 → 2 → {3, 4} → 5, with 3 and 4 concurrent, and 6 after 3 and 4. After slice 1 the world is lethal in
tests and under F-keys; after 2 a client knows; after 4 a player sees; after 5 the numbers are
measured rather than guessed; after 6 the guns are geometry and content rather than scaffolding.
