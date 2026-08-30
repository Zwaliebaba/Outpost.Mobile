# Work order — the publisher

Slice 3 of [`MmoScalabilityPlan.md`](../MmoScalabilityPlan.md), `GameLogic`. It depends on
[slice 2](DespawnCursors-work-order.md), which made the despawn log readable by more than one
reader, and it is what the plan's §2 calls the biggest MMO-shaped change in the tree: the seam stops
serving one subscriber because there is one of everything, and starts serving N because there is a
table of them.

It retires three review findings at once — E2 (fan-out is single-subscriber by construction), E4
(every subscriber's interest update lands on the same tick), and E6 (order intake is unthrottled).
All three only exist as *defects* once a second subscriber does, which is why they land together
with the thing that creates one.

## Scope

1. **`GameLogic/Publisher.h/.cpp`** — a new type owning N subscribers. One entry is:

   ```
   Transport&        the wire to that subscriber
   InterestSet       its own set, with its own radius if it wants one
   SnapshotWriter    its own writer, so snapshot ids do not collide
   FactionId         whose orders it may give
   std::uint32_t     its phase: which tick within the update period it is due on
   std::uint32_t     its order budget, per tick
   std::uint64_t     its despawn cursor (slice 2)
   WorldPos          where it is looking
   ```

   The API is small on purpose: `Add(const Subscriber::Desc&)` returning a handle, `Remove(handle)`,
   `SetCentre(handle, pos)`, and `Publish(World&)` plus `ApplyOrders(World&)` called once per tick.
   Everything else is a consequence.

2. **Phasing (E4).** A subscriber's phase is assigned on `Add` as `index % updateEveryTicks`, and
   `InterestSet::IsDueOn` gains the phase as an argument rather than assuming zero. With six ticks
   between updates and six subscribers, one is due on every tick instead of six being due on every
   sixth — the same total work, spread, and no tick carrying the whole world's egress.

3. **Order budgets (E6).** Each entry drains its own transport, both lanes, and stops after
   `ordersPerTick`. **Implementation note:** the order says "dropped exactly as a full queue already
   is", and what landed defers instead — what is over budget stays in the transport's queue and is
   read next tick, and the *tick* is counted, not an order. Dropping would discard a click the player
   made, while deferring costs nothing and lets the transport's own backpressure be the thing that
   drops when a client truly never stops. The faction gate stays where it is — in the simulation, per
   ADR 0014 — and this adds a rate limit, not a second authority check.

4. **Despawn delivery (E2).** Each entry carries its own cursor. The publisher reads
   `DespawnsSince(cursor)` per subscriber, advances that entry's cursor, and trims once at the end
   of the tick with the minimum cursor across every entry. That last part is the whole reason slice
   2 exists.

5. **`Outpost/WorldSimulation.h`** — becomes a `Publisher` with one entry. Its behavior does not
   change; its shape does. `SubscriberCentre()`'s fleet-centroid scan goes: the centre is a field on
   the entry, and the composition root sets it from the same fleet centroid it computes today.

6. **Tests**, in `GameLogicTests`, over paired `LoopbackTransport`s.

## Out of scope

- **Sessions.** No login, no identity, no lifetime beyond `Add`/`Remove`. A subscriber is a
  transport and a faction that someone hands in; who decides that is the composition root's, and for
  a dedicated server it is the unresolved config question in `MmoScalabilityPlan.md` §4 decision 3.
- **Threading.** The publisher runs on the ticking thread, like everything else in `GameLogic`.
  Per-subscriber updates are legally parallel — interest is outside the replay contract — but ADR
  0022's confinement rule means threading them is a decision record of its own, not a side effect
  of this slice.
- **A policy for a subscriber that never reads.** Its lane fills, `RefusedLeaveCount` rises, and
  that is visible. What to *do* about it — disconnect, snapshot-resync, drop to a lower rate — is a
  session decision and needs the sessions this slice does not build.
- **Per-region interest tuning.** The `Desc` allows a different radius per subscriber because the
  type costs nothing to allow it; nothing in this slice sets one.

## What to build on

`InterestSet`, unchanged except for the phase argument. `SnapshotWriter`, unchanged — note it
already carries per-writer `m_nextSnapshotId`, which is why one writer per subscriber is the right
grain and a shared one would collide. `World::DespawnsSince`/`DespawnHead`/`TrimDespawnsBefore` from
slice 2. `WorldSimulation::SplitTheLost`, whose binary-search-into-`Left()` moves into the publisher
essentially as written.

`ADR 0008`'s elimination is the argument for where this lives, and it is worth re-running rather
than citing: `NeuronServer` may not name `InterestSet`; `Outpost` owns the table today and a second
executable would strand it; `GameLogic` owns both tabled types and may include `Transport.h`.

## Acceptance

- `GameLogicTests` rows:
  - **two subscribers with different radii get different bytes**, and neither sees the other's;
  - **no two of six subscribers are due on the same tick**, and each is due exactly once per period;
  - **every subscriber hears every death exactly once**, including one added mid-match, which hears
    only deaths after it joined;
  - **the order after the budget waits rather than being lost, and the tick is counted**, and the
    budget refills next tick;
  - **a subscriber removed mid-tick does not strand the trim** — the log still trims to the minimum
    of those that remain.
- `TheSameOrderProducesTheSameRun` and the permutation test unchanged and green: none of this is
  simulated. If either moves, the publisher has reached into the tick and the slice is wrong.
- Every suite green; Debug|x64 builds; both `Build/` checks pass; the new files are in the
  `.vcxproj` **and** the `.filters`.
- A decision record: GameLogic gains the session-fan-out responsibility, with ADR 0008's elimination
  re-run and the alternatives (NeuronServer, the executable, a new library) stated with why each
  lost.
