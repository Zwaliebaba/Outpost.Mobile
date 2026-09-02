# The game design plan — the review as slices, under the pillars

**Status: drafted 2026-09-02. Phase 0 is complete and its work orders are in `Archive/` — all five slices landed on
`claude/multi-agent-game-design-review-8qu6h8` the same day and are CI-green (Debug|x64, run 240),
under the owner's decision of 2026-09-02 that CI-green is the gate and the Windows checks each order
names are waived.** Nothing in phase 1 is cut, and the four decisions in §4 are open.

Two of the five came back red first and both are recorded where they happened: the matchup table was
drafted on the wrong toolchain and CI's log carried the right numbers
([`MatchupMatrix-work-order.md`](Archive/MatchupMatrix-work-order.md) §7-8), and the owner key's first
version lost a fleet through a gate because a faction converted silently to an owner
([`OwnerKey-work-order.md`](Archive/OwnerKey-work-order.md) §8). Slice 1's fixture row and slice 5's matrix
each did the job they were built for on the slice that came after them. This plan converts
[`GameDesignReview.md`](GameDesignReview.md) (tree at `caf9814`) into ordered work in the shape
`Design/README.md` defines: one slice, one branch, one pull request. It is filtered through
[ADR 0060](Decisions/0060-the-game-is-its-own-benchmark.md), the owner's pillars of 2026-09-02, so
it is shorter than the review: items that served a pillar the owner dropped are not deferred, they
are out (§2). The review is the evidence; this document is the work.

The owner's brief for this plan, stated so nobody has to infer it: **limit new features as far as
possible, so the game does not have to be restarted, and land now only what a later feature could
not be given without a rebuild.** Phase 0 is the whole of that. It adds no player-visible feature.
Every later phase is listed so the doors phase 0 leaves are known, and none of it is cut until the
owner says a slice is next.

Work orders are cut from §6 one slice at a time, when a slice is actually next, not in advance,
because every slice that lands changes the ground the next order stands on.

---

## 1. Problem

The review's verdict: the chassis is right and the game is built on it for neither benchmark. Its
thirty items divide, under the pillars, into three kinds. **Shape decisions** are keys, bytes and
rules that every later table inherits, cheap now and a rebuild later: an owner key, a save that can
be migrated, a wire laid once, a tick that is measured, a balance that is pinned. **Content** is the
loop itself: sites, items, mining, building, a wallet, a market. **Depth** is what makes the loop a
game: verbs, formations, shields, prediction, the governor, the publisher at scale. The owner's
brief says: shape now, content when cut, depth after content.

## 2. What the pillars remove from the review

Out, not deferred, with the review item that carried it:

- **A third axis.** The critic's first gap. The plane is a pillar.
- **Skirmish, lobbies, matchmaking, a match end.** The critic's last gap. One persistent universe
  is a pillar.
- **A skill queue and a research tree.** The critic's progression gap. Assets and standings only.
- **Corporations, alliances, shared hangars, chat.** The critic's social gap, and the corporation
  namespace in E1's `OwnerId`: an owner is an account, and the namespace stays open for a
  corporation the day the pillar changes.
- **Ship-grain selection.** Named nowhere in the review as an item, but implied by C14's fitting
  slots and E4's per-hull hold; the pillar says per-hull records commanded per fleet, so both
  items survive with that shape and no selection change.
- **Insurance and checkpoint recovery** as faucets in E7 and E10: loss is real. The floor survives
  as a starter grant.

Kept open, not scheduled: sovereignty and player stations (E15), which the owner did not drop.

## 3. Shape — the decisions the pillars take for the slices

Taken in ADR 0060 and restated here so a slice can be cut against them: damage persists through a
dock and repair is priced (zero in content until a wallet); the tick is stated under load, not
dropped; a destroyed hull leaves the ledger and a player at zero gets a starter grant; items, fits
and holds are records per hull addressed per fleet; the economy is NPC-seeded and player-driven.

## 4. Decisions this plan puts to the owner

Each is needed before the slice that names it, and not before.

1. **Is an order answered?** C1 slice 1 adds `OrderReply` on the reliable lane and reverses ADR
   0051's "first ack anywhere". Needed before the first verb that can be refused for a reason no
   mask shows (a Mine order on an exhausted site, a dock a fee cannot be paid for). Phase 0's wire
   re-lay leaves the room; the reply itself waits for this answer.
2. **Where does a wallet live once there are two shards?** Per the client's session shard, or a
   settlement store outside every Universe reconciled from each shard's journal (E14). Needed
   before E2. One sentence goes into `Design/CrossShard.md` now, while it is still unagreed: a
   balance never rides a `Jumper`.
3. **Does a Vanguard station's aggression ever repair?** ADR 0039 says never; E13 makes standing a
   scalar with a repair path. Needed before any player meets a second player, because today one
   attack on a station or its garrison is a permanent empire-wide confiscation.
4. **Sovereignty: when, and whether at all.** E15 stays open. The Carrier-as-station rule in C13
   and the destructible-station record are shared with it, so the record that relaxes
   `NoImmovableHullIsDestructible` should be written once, for both.

## 5. Deliberately left out

No feature in phase 0. No login, no account, no dedicated-server process: the review names them as
the one dependency no panel owns, and this plan does the same; every EVE-shaped item after phase 0
is keyed on a placeholder owner until a session says who is asking. No audio. No rendering work,
which `Design/Archive/MmoScalabilityPlan.md` covered. No retune of any hull or device number before
the matrix test that would notice it has landed.

---

## 6. Slices

Sizes: **S** is a sitting, **M** is a normal work order, **L** needs its own design before it can
be ordered. "ADR" marks a decision record due in the slice's commit. Items reference
`GameDesignReview.md` by their E-n and C-n numbers.

### Phase 0 — the shape, and nothing else (slices 1 to 5)

Cut now. Five slices, no feature, each leaving one door.

| # | Slice | Layer | Size | Depends on | Items | ADR |
|---|---|---|---|---|---|---|
| 1 | [The save migrates forward](Archive/SaveMigration-work-order.md) | `GameLogic` + `Outpost` | M | — | E14 (migration half) | [ADR 0061](Decisions/0061-the-save-is-migrated-on-read.md) — **landed** |
| 2 | [The wire is laid once: the status collision, a flags byte, a reserved byte, a dropped-send log line](Archive/FleetStatus-work-order.md) | `GameLogic` + `Outpost` | M | 1 | C1 slice 0, cross-cutting | — **landed** |
| 3 | [An owner key beside the faction, with one owner](Archive/OwnerKey-work-order.md) | `GameLogic` + `Outpost` | M | 1, 2 | E1 (key half) | [ADR 0062](Decisions/0062-an-owner-is-not-a-faction-and-not-an-entity.md) — **landed** |
| 4 | [The tick is measured: a per-tick statistics block beside the save](Archive/TickTelemetry-work-order.md) | `Outpost` | S | — | C5 slice 0, E7 counters | — **landed** |
| 5 | [The matchup matrix test pins the balance](Archive/MatchupMatrix-work-order.md) | `Tests/GameLogicTests` | S | — | C13 slice 0 | — **landed** |

Three rounds, serial between them. Round one is slices 1 and 5: the codec and a test file that
touches nothing else. Round two is slice 2 alone, because the state reader and the wire's fleet
status block live in the same two codec files and slice 1 has to land first. Round three is
slices 3 and 4, which share no files; 3 waits for 1 because it bumps the save format and is the
first bump the migration rule must carry, and for 2 because it edits the same codec.

**Slice 1 — the save migrates forward.** ADR 0057 refuses an unknown format and never falls to
genesis, and that stays. This slice adds beside it a window of accepted formats in
`ReadUniverseState`, every later field read behind a gate on the format the reader took, a fixture
per retired format written by `UniverseGen` at the commit before the bump, and a sidecar copy of
the file a boot migrated from. The work order is
[`SaveMigration-work-order.md`](Archive/SaveMigration-work-order.md); the tool turned out to change nothing
and the root gains two lines, so the layer is `GameLogic` + `Outpost`. This is the
highest-leverage slice in the plan: without it every table after it deletes the live universe.

**Slice 2 — the wire is laid once.** `FLEET_STATUS_LAUNCHING = 6` collided with
`FleetOrderKind::Jump = 6`, so a fleet holding a jump order drew as LAUNCHING on both readers. The
status byte became a kind byte, a flags byte (launching, engaged, under attack, five bits reserved)
and a reserved stance byte; both readers gained the `JUMPING` case they never had, the view's one
status accessor split in two, a refused reliable send logs `ORDER DROPPED`, and the ALPN moved to
`outpost-6` with no state-format bump. Two things the sketch asked for did not land, and the order
argues both: no second flags byte on the ship record (six bits free, seven wanted, the shortage a
phase-4 item's, against a cost paid per ship per update), and `JUMPING` is a kind rather than a
flag because a jump has no state in between. The work order is
[`Archive/FleetStatus-work-order.md`](Archive/FleetStatus-work-order.md).

**Slice 3 — an owner key beside the faction.** `OwnerId`, a `u64` in its own namespace, on the
fleet row, the ledger row and — found on contact — the docking in flight, since a ship has no owner
and the order that asked is the only place one was ever written. Authority calls take an
`Issuer{owner, faction}` pair, ownership gates compare the owner, standing gates keep the faction,
and `IssueDockOrder` takes the pair too (its gate is still the faction; the row it writes is the
owner's). The state format moved to 8 and the two fields read behind a gate on the file's byte,
which was slice 1's reader's first real use: the format-7 fixture loads, keeps its census, comes
back owned by `OWNER_LOCAL`, and replays. CI caught the one thing the local checks could not — the
jump pass looked its fleet up by the faction, and a `u8` converts to a `u64` in silence — so the
jumper carries the owner and a faction may no longer stand in for one at compile time.
[ADR 0062](Decisions/0062-an-owner-is-not-a-faction-and-not-an-entity.md); the work order is
[`Archive/OwnerKey-work-order.md`](Archive/OwnerKey-work-order.md).

**Slice 4 — the tick is measured.** `TickStats` in `Outpost` — and only `Outpost`, since a wall
clock is the first thing `GameLogic`'s determinism list forbids and the tick's two halves are run by
the adapter in the executable, not by `ServerHost`. The step and the publish are timed separately,
the window's means and worst go to `Universe.stats` beside the save on their own `statsEveryTicks`
cadence and reset, and the `F1` readout gained the line. No counter went into the publisher: records
and fire events sent are numbers item C7's byte budget will decide with, and it adds them where it
reads them. The work order is
[`Archive/TickTelemetry-work-order.md`](Archive/TickTelemetry-work-order.md).

**Slice 5 — the matchup matrix test.** Combat slice 5's harness, which is not in the tree, brought
into `GameLogicTests` as a table: hull against hull at the review's range bands, expected outcome
and time band per cell, pinned to what the shipped numbers do today. No number moves. The test is
what makes C12, C13 and C15 safe to take later, and it is also what turns "do not break the current
game" from an intention into a gate.

### Phase 1 — the loop (not cut)

The shortest path from a rock to a launched hull, in the review's dependency order, keyed on the
owner from slice 3 and migrated by slice 1. Listed so the doors are known; cut one at a time.

| # | Slice | Layer | Size | Depends on | Items | ADR |
|---|---|---|---|---|---|---|
| 6 | Resource sites as records; the rock reopens ADR 0016 by its clause | `GameLogic` + `Tools` | L | 1, 2 | E3 | ADR |
| 7 | Items, holds and stock: per hull, addressed per fleet | `GameLogic` | L | 3, 6 | E4, C14 (record half) | |
| 8 | The Mine order and the first `MiningTool` row, as the first effect descriptor | `GameLogic` + `Outpost` | L | 7 | E5, C10 (first row) | ADR |
| 9 | Hull cost, a build order and an industry pass feeding the existing ledger | `GameLogic` + `Outpost` | M | 7 | E6 | |
| 10 | The wallet, priced repair, and the ship record's damage through the dock | `GameLogic` + `Outpost` | M | 3, §4.2 | E2, E11, C14 | ADR |
| 11 | The journal, the starter grant, the garrison invariant, F4 fenced | `GameLogic` + `Outpost` | M | 10 | E7, E1 (`AdmitOwner`) | |
| 12 | `OrderReply` on the reliable lane | `GameLogic` + `Outpost` | M | 2, §4.1 | C1 slice 1 | ADR |

Slices 6, 7 and 8 want one design, `Design/Mining.md`, which Combat.md 12 and Fleets.md 6.6 both
owe; 9 to 11 want `Design/Economy.md`. Neither exists, and the first work order cut from this
phase is the one that writes the design.

### Phase 2 — the shard (not cut)

What a second player and a second machine need. Every slice here waits on the headless run loop
and the dedicated-server root, which belong to no plan yet.

| # | Slice | Layer | Size | Depends on | Items | ADR |
|---|---|---|---|---|---|---|
| 13 | The governor: a stated rate on the header, the client clock reads it | `GameLogic` + `NeuronServer` + `Outpost` | L | 4 | C5 | ADR |
| 14 | A client tick clock, adaptive interpolation, the first test under lag | `Outpost` + `NeuronCore` | M | 4 | C3 | |
| 15 | Publish off the tick thread; the quadratic loops; save on a worker | `GameLogic` + `NeuronServer` | L | 4 | C8 | ADR |
| 16 | The per-subscriber byte budget | `GameLogic` | L | 15 | C7 | ADR |
| 17 | The session layer: join, resync, reconnect, the client half of a crossing | `GameLogic` + `Outpost` | L | 3 | C6 | |
| 18 | Standings as a scalar with a repair path | `GameLogic` | M | §4.3 | E13 | ADR |
| 19 | Recovery from the last snapshot plus the input log | `GameLogic` + `Outpost` | L | 1, 11 | E14 (log half) | |

### Phase 3 — the economy (not cut)

| # | Slice | Layer | Size | Depends on | Items | ADR |
|---|---|---|---|---|---|---|
| 20 | Refining | `GameLogic` | M | 7 | E9 | |
| 21 | Wrecks and salvage through their own `DespawnCause` | `GameLogic` | M | 11 | E10 | |
| 22 | Multi-leg standing orders: harvest and unload, a route that ends in a dock | `GameLogic` + `Outpost` | L | 8 | E8, C11 (Mine phases) | |
| 23 | A station-local market, NPC-seeded | `GameLogic` + `Outpost` | L | 10, 20 | E12 | |

### Phase 4 — depth (not cut)

| # | Slice | Layer | Size | Depends on | Items | ADR |
|---|---|---|---|---|---|---|
| 24 | The counter graph retune | `GameLogic` | M | 5 | C13 | ADR |
| 25 | Owner-only fleet intent on the wire | `GameLogic` + `Outpost` | M | 2 | C2 | ADR |
| 26 | Verbs, parameters and a fleet setting: Guard, Activate, stance, formation on the row | `GameLogic` + `Outpost` | L | 12 | C11 | ADR |
| 27 | Formation as fleet state combat reads | `GameLogic` | L | 26 | C12 | ADR |
| 28 | Fleet-grain prediction of the owner's own ships | `Outpost` | L | 14, 25 | C4 | |
| 29 | A second combat quantity: shields, armour, tracking, capacitor, the support kinds | `GameLogic` | L | 8, 24 | C15 | ADR |
| 30 | A leashed interest centre and per-role descriptors | `GameLogic` | M | 16 | C9 | |
| 31 | Fitting slots on a hull's mounts; catalogues as authored data | `GameLogic` + `Tools` | L | 7, 29 | C14 (fit half) | ADR |

### Later, kept open

| # | Slice | Layer | Size | Depends on | Items | ADR |
|---|---|---|---|---|---|---|
| 32 | Sovereignty: gate ownership read, a player station, what its death books | `GameLogic` + `Outpost` | L | 23, §4.4 | E15 | ADR |

---

## 7. What every phase-0 slice hands back

The rulebook's checklist, plus: the format bump it made, if any, and the fixture that proves slice
1 carries it; the ALPN bump, if any, with `SnapshotTests` green; the door it leaves, named in the
work order's out-of-scope so the next slice does not reinvent it; and a line in this document's
status block.
