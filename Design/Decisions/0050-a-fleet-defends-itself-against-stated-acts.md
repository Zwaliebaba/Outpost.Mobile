# 0050 — A fleet defends itself against stated acts, at fleet grain

Status: accepted
Date: 2026-08-31

Extends [ADR 0041](0041-the-protector-response-reacts-to-stated-acts.md), which established that a
reaction in this tree starts from an act the server observed rather than from anything a ship
senses. Nothing in that record is reversed; this one takes the same rule down a level, from a
station's garrison to a player's fleet, and settles the three things that are different when the
thing reacting is eight ships instead of one.

## Context

[`Design/Fleets.md`](../Fleets.md) §7 says a fleet defends itself when it is attacked. There is
still no combat, so — exactly as with the protector response — what lands is the whole framework
around an absent act, driven by `RecordHostileAct` and a debug key.

Three questions have no answer in ADR 0041, because a protector is one ship with one duty:

- **Who reacts.** A fleet is a mix. A Miner and a Hauler answering an attack by giving chase is not
  a defense, it is a fleet losing its cargo ships in a fight it cannot win.
- **For how long.** A protector pursues until its target is dead, because the Vanguard's response is
  a sentence rather than a skirmish. A fleet's escorts have somewhere else to be.
- **What they go back to.** A protector has a home to dock at. A fleet has an order it was in the
  middle of.

## Decision

`World::RecordHostileAct(attacker, victim)` — server-judged, no client message, arriving from
outside the tick — rouses the victim's fleet: the attacker becomes its **threat**, the victim's
position becomes the **anchor**, and the **alert** is filled to `FLEET_ALERT_TICKS`.

- **Who reacts** is `HullSpec::combatant`, one authored `bool` per hull. Combatants pursue; everyone
  else carries on with the order it was given, unmoved. They do not flee — fleeing is a judgment
  about where safety is, which is a sense.
- **For how long** is two bounds at once, and a fleet is engaged only while both hold: the alert is
  still burning, and the threat is within `FLEET_ENGAGE_RANGE_METRES` of the anchor. Losing either
  stands the fleet down.
- **What they go back to** is the standing order, re-lowered over every member.

The pursuit itself is `PursueTarget`, one function that the protector duty and the fleet posture
both call, so the two cannot drift apart.

## Alternatives considered

- **A sense: react to anything hostile within a radius.** The thing ADR 0041 refused, refused again
  and for the same reason. It would also make the defense fire without anybody having done anything,
  which is a different feature wearing this one's name.
- **Every member reacts.** Simpler, and wrong at the first mixed fleet: the design's whole point is
  that a fleet is a composition, and a composition that behaves uniformly under fire is a stack of
  identical ships.
- **`combatant` derived from a future weapon table.** Rejected for `avoidanceAuthority`'s reason: a
  hull that is armed but precious — a Q-ship, an armed hauler — must stay expressible, and a flag
  inferred from a table that does not exist is a guess nothing can disagree with.
- **A threat per member rather than per fleet.** Eight postures where the design asks for one, eight
  anchors to reason about, and a wire that carries one bit. A fleet attacked at both ends takes the
  latest act, which is the same choice the station's target list makes.
- **A leash measured from the fleet, or from the pursuers.** It would never release: chasing keeps
  the distance small. Anchoring on the ground that was struck is what makes hit-and-run a tactic
  rather than a way to drag five fleets around by their tempers.
- **Letting patience carry the combatants home.** It cannot: pursuit overwrites a member's route
  destination with the target's position, so patience — which re-issues a member to its own route
  destination — would send it back to where its quarry used to be. The stand-down re-lowers the
  standing order instead.

## Consequences

- **The two bounds bind in different situations, and neither is redundant.** No hull in the table
  covers a kilometre in the ten seconds one act buys, so a hit-and-run attacker is released by the
  **alert**; a sustained fight, whose acts keep refilling the alert, is released by the **leash**
  when it drifts off the ground it started on. `Design/Fleets.md` §7.2 reads as though the leash
  does both, and an amendment there records that it does not.
- **The alert outlives the engagement**, which is what makes the wire's two bits differ: a fleet can
  be *under attack* and no longer *engaged*. An explicit order clears the threat and leaves the alert
  burning, because the button should not stop glowing because the player gave an order.
- **`combatant` is in the replay contract**, like every other column of the hull table.
- **An act against a ship in no fleet is recorded and ignored.** A loose NPC has no response of its
  own until some design gives it one; nothing is silently half-built.
- **The attacker's liveness is not checked.** Being shot by something that then died is still being
  shot: the alert lights, and the posture finds nothing to pursue and stands down on its own.
- **The combat design inherits a socket, not a rewrite.** It calls `RecordHostileAct` on the first
  hostile act and gives the shadowing combatants their guns. It also inherits two questions this
  record deliberately leaves open: what friendly fire means, and whether a fleet should ever run.
