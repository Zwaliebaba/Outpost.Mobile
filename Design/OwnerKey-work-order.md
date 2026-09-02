# Work order — an owner key beside the faction

Slice 3 of [`GameDesignPlan.md`](GameDesignPlan.md), phase 0, round three. One slice, `GameLogic`
with the composition root following, and it waits for slices 1 and 2 because it bumps the state
format the first repaired and edits the codec the second re-laid.

The finding it retires is the key half of E1 in [`GameDesignReview.md`](GameDesignReview.md)
§Economy 1. Every ownership fact in the tree is a `FactionId`: the ledger row, the fleet's owner and
its five slots, the subscriber, the standing gate — and `FactionId` is a `u8` with `FACTION_LIMIT`
of 8, tied to the wire's `hostileMask`. So a second player would be a second faction or would share
one wallet, one ledger and five fleets with the first. The tree has ruled three times that players
are not factions (ADR 0013, 0039, 0047) and left nothing in their place.

This slice puts the key in place with exactly one owner in it. It adds no login, no `AdmitOwner`, no
wallet and no second player. It is the door every P0 economy item walks through, and the review's
own rule is that none of them may land keyed on a faction where an owner field would do.

## 1. Scope

1. **`GameLogic/ShipState.h` — the key, and who is asking.**

   `using OwnerId = std::uint64_t;` — a global serial in a different namespace from ADR 0047's
   shard-scoped `EntityId`, because an account outlives any one shard and an entity does not.
   `OWNER_NOBODY = 0` for everything the government and the Vandals hold, and `OWNER_LOCAL = 1` for
   the one player this build has until a login exists.

   And an `Issuer{owner, faction}` pair, because every authority call needs both and they answer
   different questions: the owner says whether you may, the faction says how the world holds you.
   A pair rather than two parameters, since `OwnerId` is a `u64` and `FactionId` a `u8` and two
   loose ones would swap silently through an implicit conversion.

2. **`GameLogic/Universe.h`, `.cpp` — the rows and the gates.**

   | Row | Gains |
   |---|---|
   | `DockedShip` | `OwnerId owner` beside `factionId` |
   | `Fleet` | `OwnerId owner` beside `ownerFaction` |

   | Gate | Was keyed on | Is keyed on | Because |
   |---|---|---|---|
   | `LedgerFor` | `FactionId _asker` | `OwnerId` | a ledger row belongs to whoever docked it |
   | `ComposeFleet` | `FactionId _issuerFaction` | `Issuer` | draws from the owner's rows into the owner's slot |
   | `FormFleet` | `FactionId _ownerFaction` | `Issuer` | the fleet is the owner's; its ships are the faction's |
   | `FleetInSlot`, `CanTakeSlot` | `FactionId` | `OwnerId` | five slots per player, not per faction |
   | `IssueFleetOrder` | `FactionId _issuerFaction` | `Issuer` | authority is ownership; the Dock refusal is still standing |
   | `IssueDockOrder`, `IssueMoveOrder` | `FactionId` | **unchanged** | they gate on whose *ships* these are, and a ship has a faction and no owner (ADR 0013) |

   That last row is the whole shape of this slice: **ships are not owned, fleets and ledger rows
   are.** A ship's faction is its identity on the wire and stays exactly what ADR 0013 made it.

3. **The state codec bumps to format 8**, and it is the first migration slice 1's reader was built
   for. Both new fields are read behind a gate on the format the file carries:
   `owner = (format >= 8) ? in.U64() : (faction == FACTION_PLAYER ? OWNER_LOCAL : OWNER_NOBODY)`.
   A format-7 file therefore comes back with the player's fleets and rows owned by the one player
   and everyone else's unowned, which is what those files meant. The format-7 fixture slice 1
   committed is unchanged and becomes the thing that proves the gate.

4. **`Publisher::Desc` carries an owner** beside the faction, and `ApplyOrders` issues with the
   subscriber's `Issuer` rather than its faction. `UniverseSimulation::Connect` supplies
   `OWNER_LOCAL` — the placeholder the login will replace, named as one.

5. **Prose in the same commit**: ADR 0013's consequences gain a sentence naming this record; the
   rulebook's "what is actually here" says a fleet has an owner; `Design/Universe.md` §8 needs no
   change beyond the format number, which its own bullet already covers.

6. **A decision record**: an owner is not a faction and not an entity.

## 2. Out of scope

- **The login, the account, `AdmitOwner`, a second player.** There is one owner and the root
  supplies it. Every EVE-tagged item behind this stays behind it.
- **A wallet, stock, items, a market.** Phase 1 and 3; this is the key they will hang on.
- **The ship record.** A ship gains nothing. Ships are the faction's.
- **Standings.** Still faction to faction, and item E13 is the design that changes that.
- **The wire's ship and status records.** No ALPN bump: nothing a client is told changes.

## 3. What to build on

- `ADR 0013`'s split between identity and relation, which is the argument this slice completes
  rather than reopens: it said a faction is an identity every client maps to a relation, and never
  said an *account* was one.
- Slice 1's format window and its gated-read shape, which exists for exactly this.
- `StartingUniverseTests`' census, which pins that genesis still builds the same universe.

## 4. How it must behave

1. Two owners in one faction hold separate ledgers and separate five-slot tables in one universe.
2. An order from the wrong owner is refused even when the faction matches.
3. A format-7 file loads, its player fleets and rows come back owned by `OWNER_LOCAL`, and it
   replays.
4. Genesis builds the same census as before, with the starting fleet owned by `OWNER_LOCAL`.
5. Every existing suite passes with the single owner: this slice changes no outcome.

## 5. Acceptance

- **A new `FleetTests` row**: two owners in `FACTION_PLAYER`, each composing into slot 0 at the same
  station, each seeing only their own hulls in `LedgerFor`, and neither able to order the other's
  fleet.
- `UniverseStateTests`' fixture row now exercises a real migration, and stays green.
- Both replay gates and the whole suite set green; `CheckProjectFiles.py`, `CheckFormat.py`,
  clang-tidy over `GameLogic`.
- The decision record is written and indexed.

---

## 6. What changed on contact, and what is deliberately not here

**A docking had to carry the owner, which §1 did not foresee.** The plan was that a ledger row takes
its owner from the fleet that docked. It cannot: by the time a ship is captured it may have been
pruned from its fleet, and the ship-list `IssueDockOrder` path — which NPC intent and every test use
— has no fleet at all. So `Universe::Docking` carries an `OwnerId`, written by the order that asked
and read by the capture. That also made `IssueDockOrder` take an `Issuer` after all, which §1's
table said it would not: its *gate* is still the faction ("are these your ships"), and only the row
it eventually writes is the owner's. The table's line is right about the gate and was wrong to
conclude the signature could stay.

**`LedgerFor` kept its standing gate and therefore takes an `Issuer`, not an `OwnerId`.** §1's table
said owner only. Written that way, a player whose faction a station holds hostile would have been
handed their ledger — a behaviour change, in a slice whose whole claim is that it changes none. The
row filter is the owner's and the refusal is the faction's, and both belong here because both decide
what a ledger answers.

**The codec's fleet-count bound had to change.** It was `FACTION_LIMIT * FLEET_SLOTS`, exact while a
fleet's owner was a faction and wrong the moment owners are unbounded. It is now `in.Remaining()`,
which is the bound every other count in that codec uses. The duplicate-slot check went the same way:
an array indexed by faction cannot be indexed by a `u64`, so the (owner, slot) pairs are packed into
a `u64` — the slot in the low three bits, guarded by a `static_assert` — sorted, and swept for
adjacent equals. O(n log n) rather than the O(n²) a pairwise sweep would cost on a corrupt file.

**Not here:** no login, no `AdmitOwner`, no wallet, no second player in any shipped universe, and
nothing new on the wire. `OWNER_LOCAL` is the one owner and every place that assumes it is greppable.

## 7. What was verified, and how — and the honest gap

**Compiled and run here, on Linux with clang behind the shim:**

- Every `GameLogic` source, and every `GameLogicTests` file syntax-checked behind a stub of the test
  framework.
- **The matchup matrix is byte-identical to before this slice** — all thirty-six duels and all five
  group rows, same outcomes, same ticks. That is the claim "this slice changes no simulated
  outcome", measured rather than asserted, on the instrument slice 5 committed two hours earlier.
- **The migration works end to end, which is slice 1's payoff collected.** The committed format-7
  fixture loads through the format-8 reader; its census is unchanged at 307 ships, 136 gates, 165
  stations and one fleet; the player's fleet comes back owned by `OWNER_LOCAL` in slot 0 with its
  three members; it re-saves at format 8, is idempotent on a second pass, and replays to byte
  equality against its own reload over 600 ticks. The file grew from 124,438 bytes to 126,902,
  which is 307 dockings and one fleet at eight bytes each and accounts for every one of them.
- `python Build/CheckFormat.py` and `python Build/CheckProjectFiles.py` pass.

**Run by CI and by nobody here:** the four suites, `Outpost`, and clang-tidy. The row that matters
is the new `TwoOwnersInOneFactionAreTwoPlayers`, which is the door itself; `UniverseStateTests`'
fixture row now exercises a real migration for the first time, and
`TheNewestFixtureIsTheToolsOutput` correctly stands down, because the newest fixture is a format
behind and that row is written to skip exactly then. CI-green is the gate the owner set on
2026-09-02.

---

## 8. What CI caught that nothing here did

**The jump pass looked its fleet up by the faction, and the compiler was happy.** `Jumper` — the
record a crossing carries from the near side to the far one — stored `ownerFaction` and nothing
else about who owned the fleet, so `FleetInSlot(jumper.ownerFaction, jumper.slot)` passed a
`FactionId` where an `OwnerId` was wanted. `FACTION_PLAYER` is 0 and `OWNER_LOCAL` is 1, so the
lookup found nothing, the arriving members were never written back into their row, `StepFleets`
pruned every handle at the end of the same tick, and the fleet retired on the tick it arrived. Two
`JumpTests` rows said so — `AFleetJumpsWholeOrNotAtAll` and `AJumpClearsIntentAndTheAlert` — out of
551 tests, and everything else passed.

**This is the exact hazard ADR 0062 names as the reason for the `Issuer` pair, and the pair did not
cover it.** The pair guards a *call* that needs both halves; this call site needed one half, read
out of a stored record, and a `u8` converts to a `u64` in silence. So the fix is in two parts:
`Jumper` carries the owner — which cross-shard needs anyway, since a handoff has to tell the
arriving universe whose slot to find — and `FleetInSlot` and `CanTakeSlot` each gain a **deleted
`FactionId` overload**, so that passing a faction where an owner belongs is now a compile error
rather than a lookup that quietly answers nothing.

**What this says about the local checks.** Everything in §7 passed both before and after the bug:
the library compiled, the matchup matrix was unchanged, the migration round-tripped, every test
file syntax-checked. None of them exercises a gate crossing, and the one instrument that would have
is the suite this container cannot run. The fix was verified here by writing the failing row out as
a standalone program against the same `GameLogic` — a fleet of three, one straggler outside the
gate, ordered through it — which now crosses whole at tick 1,828 with all three members and its
row alive. That is the row CI failed, reproduced and passing, rather than an argument that it
should.
