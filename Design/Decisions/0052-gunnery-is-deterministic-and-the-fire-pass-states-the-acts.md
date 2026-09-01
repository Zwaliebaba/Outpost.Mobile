# 0052 — Gunnery is deterministic, and the fire pass states the acts

Status: accepted
Date: 2026-09-01

## Context

Two records end with an instruction to a design that did not exist. ADR 0041 closed with "the combat
design meets this at two named sockets and needs nothing else: it calls `RecordAggression` on the
first hostile act against a station, and it gives the shadowing protector its guns." ADR 0050 said
the same at fleet grain and left "two questions this record deliberately leaves open: what friendly
fire means, and whether a fleet should ever run." `Design/Archive/Stations.md` §8.4 assigned senses
— aggro radii, threat assessment, target switching — to that design by name, and §8.5 left it a
standing rule to implement: a Vanguard station's damage is discarded.

Everything around the hole was already built. Fleets pursue, shadow, rouse and stand down; stations
scramble garrisons; a departure carries a cause (ADR 0040) and the client answers `Destroyed` with a
shatter and a shock ring. What was missing was the middle: a gun that fires, a number that falls,
and a death caused inside the simulation rather than behind a debug key.

Three things forced the choices below. The one generator in this tree is `Neuron::Pcg32` and
nothing inside `Step` draws from it, so a resolution model with dice would be the first thing in the
replay contract to need one. The tick is fixed at 60 Hz with no swept collision test (ADR 0045), so
a fast projectile as an entity re-opens that record. And the game is played at fleet grain (ADR
0049), so nobody is steering the one ship that dodging would reward.

## Decision

**Gunnery resolves deterministically, and the fire pass is what states an act.**

A shot happens on the tick where four things hold at once: the target is inside the device's range,
inside the mount's authored arc, the aim has traversed to within `FIRE_ALIGN_RAD` of it, and the
cooldown is spent. There is no to-hit roll and no damage roll. A miss is geometry — out of range,
out of arc, or a turret that lost the traverse race — so every outcome has a cause a player can see
at RTS zoom. A `HeavyTurret` at 18°/s holds a 34 m/s fighter beyond 108 m and cannot hold it inside
that, which makes closing under the guns of a capital a real tactic produced by three authored
numbers and no mechanism built for it.

`World::StepMounts` runs last in the tick and states, from its own hits, the acts the two records
above were waiting for: `RecordHostileAct` on every hit against a ship, and `RecordAggression` where
the victim is a station's structure or a garrison ship on duty. `World.h`'s "nothing inside Step
states an act" was true and is now false, and changes in this commit. What ADR 0041 actually forbids
is untouched and load-bearing: **no client message states an act, and there never will be one.** A
client sends orders; the simulation fires, observes itself firing, and judges.

The two questions ADR 0050 left open are answered. **Friendly fire: none, structurally** — a
hitscan shot lands on its acquired target and nowhere else, no target priority may resolve to a ship
of the shooter's own faction, and `IssueFleetOrder` returns a new `RefusedFriendly` for an attack
naming one. **Fleeing: no** — the posture ships exactly as ADR 0050 wrote it, and hold-fire,
withdrawal thresholds and escort doctrine belong to a rules-of-engagement design.

## Alternatives considered

**Seeded dice.** Rejected for opacity rather than for determinism: seeded rolls replay perfectly
well, and explain nothing. With forty ships on screen, "why did I miss?" must have an answer the
player can see, and a tracking-versus-signature number is not one.

**Live projectiles as entities.** Genuinely richer, and rejected on cost: every round in flight
joins the spatial index, the interest sets and the wire; a fast shell at 60 Hz needs the swept test
ADR 0045 deliberately does not have; and fleet-grain command gives the player no way to use
dodgeable fire. The door is not shut — a slow, killable torpedo is the Bomber's long-term identity
and one more design, and it will co-exist with hitscan the way the capsule co-exists with the mesh.

**Positional friendly fire**, where a shot strikes the first hull crossing its line. Rejected for
the same swept-test reason, and because it punishes a formation solver the player does not steer.

**Deriving the armament from the meshes.** The shipped Battleship already carries three turret
submeshes and the NMO format defines a `Gun` marker with a muzzle direction. Rejected under ADR
0002: what the simulation needs of a hardpoint arrives as authored numbers in `GameLogic`, because a
headless server has no content, and the capsule table already made this argument about size.

**Running the fire pass beside the other standing behaviours**, at the top of the tick. This is what
the work order specified and it is wrong, for a reason only implementation surfaced: opportunistic
acquisition reads the neighbour list, a `Neighbour` names a `ShipId`, and a `ShipId` is an array
index that every despawn moves (ADR 0005). The list is trustworthy only between the gather that
built it and the next despawn. Running the pass last is what puts the whole of it inside that
window; it also means a mount fires on where the ships ended the tick rather than where they began
it, which is the more honest of the two readings.

## Consequences

**A fifth table joins the despawn repair**, and `WORLD_STATE_FORMAT` goes 5 → 6 for the mount state,
`ShipState::hullPoints` and `Route::pursuitAimedAt`. Hull points are an unsigned integer and every
damage number is whole, so the damage path holds no float at all and is bit-exact under every
summation order — a determinism result rather than a taste.

**`maxHullPoints == 0` means indestructible**, which is how Stations §8.5's rule is implemented: a
property of the hull rather than of the faction, with no station special case in the fire pass. Two
`static_assert`s keep the immovable rows unarmed and indestructible.

**A pursuit now stops short.** `PursueTarget` aims `ENGAGE_STANDOFF_FRACTION` of the hull's shortest
*traversing* mount's range from its target, clamped so a stand-off can only ever shorten a chase and
never turn one into a withdrawal. A hull whose mounts are all fixed takes none and is sent at the
target itself, which is what makes a fighter fly attack runs with no code written for attack runs.

**One premise expired and is recorded rather than worked around.**
`Design/Archive/Fleets-slice-4.md` §2.3 refused a stored aim point because "the point last aimed at
is already `m_routes[member].destination`". A stand-off puts those up to 224 m apart, so the drift
test would read a constant offset as movement and re-plan an A* sixty times a second. `Route` now
carries `pursuitAimedAt`, and `World::PursuitAimedAt` exposes it because it is the only place that
quantity now lives.

**Two rows of `FleetTests` changed their setup, not their meaning.** Both were written when nothing
could die. One read "the combatant turns on the attacker" as distance closed, which a Corvette
already inside its own turret range correctly declines to do; it now reads it as the attacker losing
hull points. The other isolated the alert as the only bound on an engagement, which stopped being
true the moment a roused fleet could kill its threat; it now uses a fleet with nothing armed in it.

**The world is lethal in tests and nowhere else yet.** No snapshot record field, no fire event, no
muzzle flash and no turret slew: a client learns that a ship died and nothing about why. `F6` and
`F7` still stand in for acts they can now cause for real, and the sentences in `AGENTS.md` and
`README.md` that say there is no combat stay as they are until slice 4, when the game a player boots
is the one they describe.
