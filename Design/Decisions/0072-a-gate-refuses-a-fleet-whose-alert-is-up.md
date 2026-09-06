# 0072 — A gate refuses a fleet whose alert is up

Status: accepted
Date: 2026-09-06

Supersedes, in part, [`Design/Archive/Universe.md`](../Archive/Universe.md) §6.2's last paragraph —
"fleeing through a gate is escape" — and the half of its §12 decision 7 that made the crossing
unconditional. The archived text stays as written; this record names it.

## Context

[`SystemLayout.md`](../SystemLayout.md) §5 makes warp interruptible: a fleet under fire does not
align, so a fight ends when it is won and not when the loser says so. It then asked whether the gate
obeys the same rule, because today it does not. `StepJumps` crosses a fleet the tick every member is
inside the gate's range whatever is shooting at it, and clears its threat and alert on the far side.
A camp could fire on a fleet at the doorstep and watch it leave, which is the opposite of the camp
[ADR 0071](0071-looking-at-a-system-costs-presence.md) was written to protect.

The owner took the design's decision 4 on 2026-09-06: one rule for both.

The design said the rule's primitive was *engaged*, the judgement the tick already computes from the
row's threat. Writing it found that it cannot be. `Universe::IssueFleetOrder` clears `threat` on
every order it accepts — a Move means leave, and the combatants' chase ends with the threat that
started it — so a gate that read the threat would open the moment the player tapped the gate again.
The alert is the other half of the same row: `alertTicks`, set to `FLEET_ALERT_TICKS` by
`RecordHostileAct` on every landed hit, decremented by the tick, and touched by nothing a client
sends. It is the server's own memory of having been shot, and it is what pulses the fleet button
red.

## Decision

`StepJumps` does not cross a fleet whose `alertTicks` is above zero. The approach order keeps the
members at the gate; the alert lapses ten seconds after the last landed hit, or is renewed by the
next one; the door opens on the tick after it lapses. Nothing else about the crossing changes, and
the far side still clears what it cleared — now vacuously, since a fleet that crosses has nothing to
clear.

The same primitive is the warp's, when slice 2 lands it: a fleet does not align while its alert is
up. One row, one rule, one red pulse on the bar that means the same thing at a gate and in open
space.

## Alternatives considered

- **EVE's split** — a scrambler stops warp, a gate is still a door, and an aggression timer holds
  only the aggressor. Three rules for one question, and the third needs a notion of aggressor this
  simulation does not carry: `RecordHostileAct` records the act against the victim's fleet and
  nothing against the attacker's. It also leaves the camp toothless: the fleet it catches at the
  gate is the victim, and the victim is the one EVE lets through.
- **The threat as the primitive.** The design's first wording, and what the tick calls engaged. It
  is one order away from clear, so the door would be opened by re-issuing the jump — a hole a
  player finds in the first minute. `AGateHoldsAFleetWhoseAlertIsUp` re-issues the order mid-alert
  and asserts the door stays shut, so the hole cannot come back unnoticed.
- **Stop orders from clearing the threat instead.** Closes the hole at the cost of the chase: a
  Move that no longer ends the defense turns "Move means leave" into "Move means leave while still
  turning to fight", which `CombatTests::TheDefenseDoesNotSuspendATravelOrder` and ADR 0050 both
  argue against. The alert costs nothing that is already decided.
- **Hold the crossing only while the attacker is in range**, the leash rule the threat carries.
  Right in spirit and the same hole in practice, since the range is measured from a threat an order
  can drop. Ten seconds from the last hit is the leash, stated as time.

## Consequences

- A camp holds what it catches. A fleet that reaches a camped gate under fire stays at the gate
  under fire, and the ways through are to outlast the alert, to kill what is shooting, or to be the
  hull that dies last. That is the cost the design stated and the owner accepted.
- The `UNDER_ATTACK` pulse on the fleet button is now also the affordance for "you cannot cross",
  with no new state and no new drawing: the bit that lights it is the bit that holds the door.
- A voyage under fire waits at its door and resumes when the alert lapses; `StepVoyages` needs no
  change, because a held fleet still holds its gate and the pass is already patient.
- The crossing reads last tick's alert, since `StepFleets` decrements after `StepJumps`. A fleet hit
  on the very tick it would have crossed is held one tick late, which is the direction to be wrong
  in.
- `JumpTests::AJumpClearsIntentAndTheAlert` used to prove that a roused fleet crosses and leaves its
  alert behind; it now waits the alert out first, so what it proves is that a crossing never carries
  one. `AGateHoldsAFleetWhoseAlertIsUp` is the rule itself, including the re-order hole.
- Landed ahead of `SystemLayout.md` slice 2 as that design's slice 0, because it changes shipped
  behaviour on its own and depends on nothing in slice 1.
