# 0062 — An owner is not a faction and not an entity

Status: accepted
Date: 2026-09-02

## Context

Every ownership fact in this tree was a `FactionId`: the ledger row, the fleet and its five slots,
the subscriber, and the gate on every order. `FactionId` is a `u8` with `FACTION_LIMIT` of 8, tied
to the wire's `hostileMask`, which is a byte.

So a second player had two possible shapes and both were wrong. Give them their own faction, and
eight players exhaust the game while every ally pair needs a standings row — the table is quadratic
in its own size, which is the objection ADR 0013 raised and ADR 0047 repeated. Share a faction, and
they share one ledger, one wallet and five fleets between them.

Three records have now ruled that a player is not a faction (ADR 0013, 0039, 0047) and none of them
put anything in its place, because nothing needed one yet. `Design/GameDesignPlan.md` schedules a
wallet, an item table, station stock, a build queue and a market, and every one of them is keyed on
whoever owns the thing. The review's rule is that none may land keyed on a faction where an owner
field would do, because the field is cheap now and a migration of five tables is not.

## Decision

**An owner is a `u64` in a namespace of its own, and it is what owns a fleet and a ledger row.**

- `OwnerId`, with `OWNER_NOBODY = 0` for everything no player holds and `OWNER_LOCAL = 1` for the
  single player this build has until a login exists.
- `Universe::Fleet` and `Universe::DockedShip` each carry one beside the faction they already had;
  `Universe::Docking` carries one so the order that asked is what the ledger row records.
- **Ownership gates compare the owner; standing gates keep comparing the faction.** `FleetInSlot`,
  `CanTakeSlot` and the ledger's row filter are ownership. A station's refusal to take your dock, a
  mount's refusal to shoot a friend, and the header's `hostileMask` are standing.
- **A ship has no owner.** A ship's faction is its identity on the wire and nothing else was added
  to it, so "are these your ships" stays exactly the question ADR 0013 made it.
- An `Issuer{owner, faction}` pair is what authority calls take, because every one of them needs
  both, and because two loose parameters of `u64` and `u8` would swap silently and compile.
- `UNIVERSE_STATE_FORMAT` moves to 8, and the fields are read behind a gate on the file's own byte
  (ADR 0061): a format-7 file comes back with the player's fleets and rows owned by `OWNER_LOCAL`
  and everyone else's unowned, which is what those files meant.

## Alternatives considered

- **Widen `FactionId` and let players be factions.** Rejected for the reason ADR 0013 gave and ADR
  0047 repeated: standings are a relation between factions and the table is quadratic, so ten
  thousand players is a hundred million rows describing nothing anybody asked about. It also puts
  an account on the wire, where `hostileMask` would have to carry it.
- **Key the economy on the faction and sort it out when the login lands.** The cheap option, and
  the one the review names as the temptation. Rejected because by then it is five tables, a save
  format, a wire message and every gate that reads them, and because a single-player deployment
  would have shipped in the meantime with the wrong key baked into its save files.
- **Reuse `EntityId` (ADR 0047).** Tempting: it is already a 64-bit global identity. Rejected
  because it is shard-scoped by construction — `{shard:16, serial:48}` — and an account outlives
  every shard it has ever had a ship on. Two namespaces that mean different lifetimes must not be
  the same type, or a cross-shard handoff starts comparing them.
- **Put the owner on the ship.** It would have made the dock capture trivial. Rejected because it
  contradicts ADR 0013's whole argument — a client is told a ship's faction and would then be told,
  or be able to infer, who is flying it — and because the fleet is the unit of command (ADR 0049),
  so ownership at ship grain is a key nothing would read.
- **Two parameters instead of a pair.** Rejected on the implicit conversion: `IssueFleetOrder(3, 0,
  command)` would compile with the faction in the owner's place and refuse every order at run time.

## Consequences

- **Two players in one faction are now representable**, which is what the pillars' persistent shared
  universe needs and what a corporation would have needed later. `FleetTests` pins it.
- **`FACTION_LIMIT` stops being a player ceiling.** It is what it always claimed to be: how many
  sides the wire can name. The fleet table's read bound in the codec had to stop being
  `FACTION_LIMIT * FLEET_SLOTS` for the same reason, and is now the buffer, like every other count.
- **`OWNER_LOCAL` is a placeholder and is named as one.** A grep for it finds every place that
  assumes a single player: genesis, the composition root's subscriber, and the default on the
  snapshot writer's viewer. The login replaces exactly those.
- **A ledger row docked by nobody is unowned and nobody can draw it out.** That is correct for an
  NPC and unreachable for a player, whose ships dock through a fleet or through an order that names
  them.
- **The save grew by eight bytes per fleet, per docked row and per docking in flight.** The shipped
  universe went from 124,438 bytes to 126,902 on the first migration, which is 307 dockings and one
  fleet, and it is the price of the key.
- Nothing on the wire changed and no ALPN moved: a client is told about fleets in *its own* slots
  and was never told whose they are.
