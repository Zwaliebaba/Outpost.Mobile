# 0060 — The game is its own benchmark: one persistent plane, at fleet grain, with an economy under it

Status: accepted
Date: 2026-09-02

## Context

`Design/GameDesignReview.md` (tree at `caf9814`) measured the tree against two games at once,
Homeworld and EVE Online, because that was the brief. It found thirty items, and about half of them
serve one benchmark and not the other. A roadmap read against two proxies drifts toward whichever
one argued loudest in the slice at hand, and the drift is not visible in a diff: a dock that repairs
for free is Homeworld's dock and EVE's largest exploit, and either reading passes review.

The owner's answer, on 2026-09-02, was that the game is neither. It takes parts from both and drops
parts from both, and the drops have to be written down or every future slice re-argues them. Some
of the parts exclude each other, so "the best of both" is not a direction until each conflict has a
winner. This record is the list of winners.

## Decision

**Outpost: Frontier is its own benchmark, and later reviews measure against this record rather than
against Homeworld or EVE.** The pillars, each taken on 2026-09-02:

- **One persistent shared universe.** Every player is in the same saved galaxy, across shards. No
  lobbies, no match, no end condition. A skirmish mode is not planned.
- **The simulation is a plane.** Every position is `y = 0` (ADR 0025), the camera and the ship
  interaction the tree has today are the product, and Homeworld's third axis is dropped, not
  deferred.
- **The fleet is the unit of command** (ADR 0049), and stays so: no single ship is ever selected,
  ordered or fitted in the field. What a hull carries — its fit, its hold, its damage — is a
  **record per hull**, and every act on it (an order, a cargo transfer, a sale) is addressed to the
  fleet. Neither benchmark has this shape; it is designed here, not borrowed.
- **Damage persists through a dock.** The ledger row carries hull points, and repair is a station
  service with a price. Until a wallet exists the price is zero in content (ADR 0043), which is the
  same behaviour the game has today, stated instead of accidental.
- **Under load the tick is stated, not dropped.** The simulation stays at a fixed 60 Hz (ADR 0045);
  when a fight outruns the server, the server stretches wall time and tells every client the ratio.
  Nobody skips, and everybody sees the same fight, slower.
- **Loss is real and has a floor.** A destroyed hull leaves the ledger and is not recovered by
  insurance or a checkpoint. A player at zero is always granted a starter hull at a home station, so
  loss is meaningful and never terminal.
- **The economy is seeded by NPCs and driven by players.** Stations buy and sell at seeded prices as
  a floor; players mine, build and trade on top and may out-compete the seed.
- **Progression is assets and standings, and nothing else.** No skill queue and no research tree:
  what a player can do is what they own and how the factions hold them.
- **Dropped: corporations and every social layer** — shared hangars, alliances, chat — for now.
  Sovereignty and player-anchored structures are **not** dropped; they are later, and every slice
  before them leaves the door ADR 0038 already cut.

## Alternatives considered

- **Pick one benchmark.** Cleanest; half the review falls away. Rejected because the owner wants
  parts of each, and the parts wanted from EVE (the economy, the persistent shard, loss that counts)
  and from Homeworld (the plane, fleet grain, formations, immediacy) are not the halves a single
  benchmark would keep.
- **Homeworld's dock.** Free full repair. Rejected: it cannot be a sink later without changing the
  player-facing rule, and it is a farm the day any faucet keys off a kill (review, Risks).
- **Never dilate; cap the fight instead.** Keeps immediacy absolute at the cost of scale. Rejected:
  a cap is a rule players meet as a wall, and a stated rate is one they meet as a slowdown.
- **Drop ticks silently under load.** What the tree does today. Rejected: it hides the one thing a
  player in a large fight most needs to know.
- **Loss without a floor.** Pure EVE. Rejected as a churn engine for a game with no corporations to
  catch a player who has nothing.
- **A fully player-driven economy.** Rejected for the first years: a market with no seed is dead
  until there are enough players to make one.
- **A skill queue or a research tree.** Each is a second progression axis with its own table, its
  own time base and its own balance surface. Rejected to keep the surface small; hulls gate on cost
  and blueprints instead.
- **Ship-grain command.** EVE's shape, and what per-hull fitting seems to ask for. Rejected: it
  reverses ADR 0049 and the game the owner likes. Per-hull records commanded per fleet is the price.
- **Per-fleet items only.** Simplest to build. Rejected: it rules out fitting a hull, which is the
  part of EVE's depth the owner wants.

## Consequences

- **`Design/GameDesignPlan.md` is the review filtered through this record.** Items serving a dropped
  pillar are not deferred, they are out. Items that survive are ordered so that shape decisions land
  before content.
- **The review workflow is re-run against this record**, passed as its focus, rather than against
  the two benchmarks. The benchmarks stay as vocabulary; they stop being the bar.
- **Two ADRs will be reopened by name when their slice arrives**, not now: ADR 0016 (bodies are
  presentation) by a minable site, and ADR 0051 ("first ack anywhere") if the owner takes the
  plan's decision to answer an order. Neither is decided here.
- **The plane is no longer a scoping decision** (`Design/Archive/MmoScalabilityPlan.md` §5) but a
  product one. A proposal for a third axis is a proposal to supersede this record.
- **Three placeholders become rules.** The free repair is a zero price; the dropped tick is a stated
  ratio the code does not yet publish; the single player is a single owner the code does not yet
  key on. Each is a slice in the plan and each is owed before its feature, not after.
- What this record does not do: it does not schedule anything. The plan does.
