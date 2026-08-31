# 0048 — Fleets are simulation state at fleet grain

Status: accepted
Date: 2026-08-31

## Context

[`Design/Archive/Fleets.md`](../Archive/Fleets.md) makes the fleet the unit of command: at most
five per faction, at most eight ships each, composed at a station, ordered as one thing, defending
itself when attacked.
The tree already has something that looks like the answer — `WorldView::CONTROL_GROUPS`, five
remembered selections on the same five HUD buttons — and the cheapest reading of the brief is that a
fleet is a control group that got stricter.

It is not, and the difference is worth a record because it is a shape four later slices bind to and
because "just widen the control group" is a proposal somebody will make again.

## Decision

A fleet is a row in a dense table in `World`, in the replay contract and in the save format, walked
by a pass in the standing-intent slot. `FLEET_SLOTS` and `MAX_FLEET_SHIPS` sit beside
`FACTION_LIMIT` in `ShipState.h` as contract constants. A row names its members by `ShipHandle`; its
name outside the table is the pair `(ownerFaction, slot)`, never its index.

Four things forced it, and each is one the client could not have done:

- **The caps are rules.** Five fleets and eight ships decide which orders are accepted, so the gate
  belongs in the simulation or it is a convention an adapter can talk its way past (ADR 0014).
- **The defense is behavior.** A fleet defends itself whether or not anybody is subscribed to it,
  beyond every interest radius, exactly as a protector pursues — and behavior lives in `GameLogic`,
  inside the tick (ADR 0015, ADR 0041).
- **Orders name it.** An order at fleet grain needs a server-side referent to gate and to lower onto
  the per-ship machinery.
- **A spectator would need it**, which is AGENTS.md §5's own test for simulation state: which ships
  are one fleet changes what the buttons, the wire and a replay say.

## Alternatives considered

- **Keep it in the client — a control group with stricter rules.** The cheapest answer, and it fails
  all four tests above at once: the caps become a convention, the defense cannot run for a fleet
  nobody is watching, an order still has to name every ship, and a replay of the same input produces
  a different game because the grouping was never in it.
- **A membership field on `ShipState` — one `fleetSlot` byte.** O(1) in both directions and tempting.
  Rejected for `ShipState`'s own promise: nothing in it that a snapshot could not carry, so the byte
  would ride in every record ten times a second to state something that changes at human speed. It
  also has nowhere to put what a fleet actually is — the launch manifest, the standing order, the
  threat and the alert are the fleet's state, not any ship's, and a per-ship byte would leave them
  homeless.
- **A table parallel to `m_ships`, like `m_patrols` and `m_dockings`.** Wrong shape: a patrol is
  something a ship *has*, a fleet is something ships *belong to*. It would spread one fleet's state
  across eight rows, give the despawn repair a fifth table to keep in step, and still need somewhere
  else for the manifest and the order.
- **A generational `FleetHandle`, the way `ShipHandle` works.** Machinery for a reference nobody
  holds. `(faction, slot)` is what the wire states, what the button shows and what the player
  tracks, and it survives a retirement in a way an index cannot — so nothing needs to hold an index
  across a tick and there is nothing for a generation to protect. Ships need handles because
  thousands of them churn; five rows per faction do not.
- **A `std::map` keyed by slot.** AGENTS.md §5: no maps, and no iteration order that is not
  dense-array order. A map's order would enter the replay contract the moment the pass walked it.

## Consequences

- **The row is in the save format.** `WORLD_STATE_FORMAT` moved from 1 to 2, and it moves again with
  each slice that adds a field to the row — the manifest with slice 2, the standing order with slice
  3, the threat and the alert with slice 4. That is deliberate: a field, its codec lines and the test
  that exercises them land together, so no line of the format is ever written that no test can
  reach. There is no migration to pay for, because a mismatched format byte is a refusal and nothing
  in this tree keeps a saved world across a build.
- **`FLEET_SLOTS` and `MAX_FLEET_SHIPS` are contract constants.** Changing either changes which
  fleets can be formed, which changes recorded outcomes. `Design/Archive/Fleets.md` §4.2 argues the
  eight against formation span, the separation measurements and the interest radius.
- **Members are handles, so the despawn path gains nothing.** A member that docked or died stops
  resolving and the fleet pass prunes it in place; there is no fifth parallel table for `DespawnShip`
  to repair, and a ship whose `ShipId` moved by swap-and-pop is still the same member.
- **`FleetInSlot` and `FleetAt` are linear scans**, over at most `FACTION_LIMIT × FLEET_SLOTS` rows
  and `× MAX_FLEET_SHIPS` handles. Free at these counts, and an index the day something makes it
  matter — the station table's own trade (ADR 0038).
- **The table is faction-generic.** Nothing in the row or the gates assumes `FACTION_PLAYER`, so an
  NPC raid wing is content rather than a redesign; nothing composes one today.
- **The client's control groups become redundant** and retire with slice 6, when the five buttons
  are rebound to the slots. Until then both exist and only the group is wired to a button.
