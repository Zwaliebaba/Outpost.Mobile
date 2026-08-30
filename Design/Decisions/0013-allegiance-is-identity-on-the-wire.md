# 0013 — Allegiance is identity on the wire, not a relation

Status: accepted
Date: 2026-08-29

## Context

The first ship that is not the player's forces the tree to answer whose a ship is, and the answer
travels: `WorldView` draws from a snapshot and has no other source of truth, so whatever the overview
colors red has to be in the record or derivable from it. Two shapes were available and they are not
interchangeable. **Identity** is "this ship belongs to faction 1", one byte the server states about
the ship. **Relation** is "this ship is hostile to you", a fact about a pair — the ship and whoever
is looking.

The distinction is invisible with one subscriber and load-bearing with many, which is exactly the
kind of decision Design/Archive/Hostiles.md exists to take while it is still one field.

## Decision

`ShipState` and `ShipSnapshot` carry `FactionId factionId`, a `std::uint8_t` with `FACTION_PLAYER`
and `FACTION_HOSTILE` beside it. The server states identity; each client maps identity to a relation
of its own. Nothing on the wire says "hostile", and nothing says whether a ship is flown by a person
or by the simulation.

## Alternatives considered

- **A per-subscriber friend/foe bit.** The direct answer to what the overview actually needs, and it
  reads well: the server already knows who is asking. Rejected because it forks record contents per
  viewer — the same ship is two different records depending on who receives it — for no gameplay the
  design has today. Interest management already varies *which* records a subscriber gets; varying
  what is *in* one is a different and much larger promise, and it would have to be unmade to add
  standings later.
- **Inferring allegiance from the hull.** Free: the record already carries `hullId`, and the patrol
  flies Interceptors the fleet does not. Rejected as presentation guessing baked into a protocol. It
  is wrong the first day both sides fly the same hull, and it fails silently rather than loudly —
  the ships simply draw the wrong color.
- **An `isNpc` flag beside the faction.** Tempting because the client could then treat NPCs
  differently. Rejected: a client that can tell a player from an NPC knows something an MMO client
  should not, and the day players fly beside NPCs in one faction the flag has to be taken away
  again, which is a protocol change made under pressure.

## Consequences

- `SHIP_RECORD_BYTES` grows 81 → 82; `ShipsPerSnapshotFragment()` follows it without being touched.
- Standings, diplomacy and per-viewer IFF stay expressible: they become a client-side mapping from
  identity to relation, plus whatever the server later chooses to *write* for a given viewer.
  Deception — a spoofed transponder, stealth — stays expressible for the same reason, because the
  field means "what the server says the faction is" and not "what the faction is".
- Ownership finer than faction is not addressed. Two players in one faction can still command each
  other's ships (ADR 0014), and closing that is one more field and a different comparison.
- The HUD needs the viewer's own faction to compare against, which is session identity no library
  below the composition root can know. It is injected by the root today and arrives with a login the
  day one exists.
