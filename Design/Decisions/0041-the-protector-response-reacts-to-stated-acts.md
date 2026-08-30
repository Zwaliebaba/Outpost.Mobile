# 0041 — The protector response reacts to stated acts, not senses

Status: accepted
Date: 2026-08-30

## Context

`Design/Archive/Hostiles.md` §5.5 built the first standing NPC intent in this tree — a patrol that
walks a ring — and was explicit that it reacts to nothing: "the first behavior that responds to what
it sees is a different design with senses and thresholds." `Design/Archive/Stations.md` §8 is that design.
An attacked station scrambles its garrison, and the protectors pursue the attacker until it is dead.

The obvious way to build that is the way every game builds it: give the station an aggro radius and
the protectors a threat scan, and let them notice things. This record is about not doing that, and
about the two smaller choices that follow from not doing it.

## Decision

**The response starts from a stated act.** `World::RecordAggression(attacker, station)` is the only
way a target list ever gains an entry. No station scans for enemies, no protector picks its own
quarry, no radius makes anyone a criminal. A Vandal flying past a Vanguard station is unmolested,
however hostile the standings table says it is.

The pursuit itself reads exactly two things it did not write: its target's position, and whether its
target is still alive. Everything else is the order machinery a player's click already exercises —
the reaction is *choosing the point*, and `PlanRoute`, steering, avoidance and separation do the
rest.

Two consequences of that shape are decisions in their own right:

**`launchedCount` is derived, not stored.** `Design/Archive/Stations.md` §8.2 gives `Station` a field for the
protectors currently in space. This implementation counts the active duties whose home is that
station instead. Storing it needs a repair path on *death* as well as on docking — or "losses are
replaced by the same metronome" never fires — and `DespawnShip` has no business knowing what a
protector is.

**A duty stays active while its ship flies home.** `ProtectorDuty::active` means "this ship is a
garrison ship of `home`, and it is in space", not "it is hunting". §8.3's wording — "the duty ends
and the ship goes home" — would lose that, and three things need it.

## Alternatives considered

- **Aggro radii and threat scans.** What the genre does, and what makes an NPC feel alive without
  anyone having to trigger it. Rejected as the combat design's, not this one's: a radius that makes
  you a criminal is a *judgment*, and every judgment this phase makes is a stated act with a
  recorded consequence. Senses also cost a neighbour query per station per tick against a
  neighbourhood the interest system is built to keep small, and they are the thing that would make
  the response untestable — a test would have to arrange a geometry rather than call a function.
- **A client message declaring an aggression.** It would make F6 trivial and the combat design's job
  smaller. Rejected outright, and this record exists partly so nobody adds one: a client that can
  declare an aggression can make anybody a criminal.
- **A pre-existing wing docked at each station, patrolling.** More natural-looking, and no spawn
  inside a tick. Rejected on cost: three NPCs per station forever, animating a courtesy nobody
  attacked. Spawning on aggression costs nothing until the player buys trouble.
- **A return-behavior of its own for standing down.** A `GoingHome` state with its own steering.
  Rejected because docking already does it: standing down is one intent write into the table slice 3
  built, and the dock pass flies and captures a protector exactly as it does a visitor. The one
  difference — a garrison is not a guest, so no ledger row — is a branch at the capture, not a
  system.
- **Storing `launchedCount`, as the design says.** Rejected above. The cost of deriving it is a walk
  of the ship array per station per tick, at single digits of stations and a fleet that fits in a
  datagram; the cost of storing it is a repair path in `DespawnShip` that would be wrong the first
  time somebody added a third way for a ship to leave.
- **Ending the duty when the target dies**, as §8.3 words it. Rejected because a protector flying
  home is still out: it still counts against the complement, so a station would relaunch behind it;
  it still has to be told from a visitor at the door, or it writes a ledger row; and a new
  aggression should turn it round rather than let it dock and be relaunched a tick later. All three
  fall out of `active` meaning "in space".

## Consequences

- The combat design meets this at two named sockets and needs nothing else: it calls
  `RecordAggression` on the first hostile act against a station, and it gives the shadowing
  protector its guns. What a protector does on arrival today *is* shadowing — avoidance and
  separation hold it off the target's hull at about 19 m, measured, and it re-aims as the target
  moves — which is exactly "a ship that is always in weapons range of its target".
- The pursuit never gives up, by range or by time, and the server simulates beyond every interest
  radius, so flight is postponement. Nothing about the behavior is keyed on whether anybody is
  watching.
- The reserve is bottomless while a target lives. That is safe only because a protector drops
  nothing when destroyed (`Design/Archive/Stations.md` §8.6) — the rule is recorded for the loot design and
  is not code here — and it is what makes an infinite response farm-proof rather than an exploit.
- `StepProtectors` both spawns and despawns inside a tick, the first pass to do both. Launches and
  captures are each collected during a walk and applied after it, because both append to or
  swap-and-pop the very tables the walk is iterating.
- A fourth parallel table joins routes, patrols and dockings in the despawn repair. That is the
  fourth time this pattern has been paid for, and the day there is a fifth it is worth a structure
  rather than a fourth line.
- The target list is capped per station and drops the *newest* when full. The standing flip has
  already happened by then, which is the part that matters: a criminal the garrison is too busy to
  chase is still a criminal, and still refused at every dock.
