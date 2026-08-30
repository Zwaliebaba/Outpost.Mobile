# 0045 — The tick rate is fixed at 60 Hz, and capacity is bought elsewhere

Status: accepted
Date: 2026-08-30

## Context

`SimTuning.h` has said since the sector work that 60 Hz "is inherited from a build that ran one
world on one machine for one player, and an MMO server is unlikely to keep it — tick rate multiplies
against entity count and region count and server count."
`MmoScalabilityReview.md` finding E7 called that a well-guarded open decision and said it gates
shard-capacity sizing: until it is settled, every per-shard cost number is a number times an unknown.

`MmoScalabilityPlan.md` §4 put it to the owner as decision 2, and the owner took it on 2026-08-30:
**60 Hz stays.** Slice 12 shrank from a substepping change to a record — and then the record was not
written. This is it, five months late, found by reading the plan's own status table against
`Design/Decisions/`.

What the tunnelling gate actually says, measured rather than recalled
(`HullSpecTests::NoHullCanTunnelThroughAnother`, `TUNNEL_HEADROOM` = 0.6):

| Rate | Interceptor travel per tick | Ratio to the smallest capsule radius (1.115 m) | Gate |
|---|---|---|---|
| 20 Hz | 1.700 m | 1.525 | fails |
| 30 Hz | 1.133 m | 1.016 | fails |
| 40 Hz | 0.850 m | 0.762 | fails |
| 50 Hz | 0.680 m | 0.610 | fails |
| **60 Hz** | **0.567 m** | **0.508** | passes |

The lowest rate this hull table admits at all is **50.9 Hz**. There is no round number below 60 that
works, and at 20 Hz two Interceptors pass straight through each other.

## Decision

The tick rate is fixed at 60 Hz. `TICK_HZ` stays a compile-time constant in `SimTuning.h`, the
replay contract goes on assuming it, and no substepping, continuous collision or variable timestep
is built.

Per-shard capacity is bought by **putting fewer entities in a shard**, not fewer ticks in a second.
The tick rate is therefore a *sizing input* from here on, not an open question: a shard's cost is
60 × (per-tick cost at its entity count), and the work that lowers it is interest management,
localized gathering, and regional pathfinding — all of which have landed (slices 6, 11, 14).

`ServerHost::Desc::tickHz` stays a field, because a host that drives a simulation should not have
the rate compiled into it; nothing in this tree sets it to anything but `TICK_HZ`.

## Alternatives considered

- **Lower the rate and add substepping.** The textbook answer: 20 Hz with a swept-capsule or
  substepped narrow phase costs a third of the ticks and keeps the collision honest. Rejected on
  what it costs to get back what it gives away. Substepping puts the collision loop back to
  something near 60 Hz's work in the ticks that need it, so the saving is in the *rest* of the tick —
  steering, avoidance, routing — and those are what slices 11, 13 and 14 had just made local. It
  also lands new machinery inside the one library whose whole property is determinism, at the exact
  moment three other slices were changing it, and every recorded game before it becomes
  unreplayable. Spend the same effort on entity count and the capacity arrives without any of that.
- **Lower the rate without substepping**, and fatten the smallest capsule or slow the Interceptor to
  keep the gate green. Rejected because it is the gate being edited to fit the answer. A 1.7 m
  capsule on a hull that is drawn 2.2 m across is a hull that collides with things it visibly misses,
  and an Interceptor that is not fast is not an Interceptor: both are game design being decided by a
  server-cost argument.
- **Raise the rate.** 120 Hz doubles the margin (ratio 0.254) and doubles the cost. Nothing is
  asking for it — the gate has 15% of room at 60 Hz and the view interpolates between ticks anyway,
  so the rate is not what smoothness depends on.
- **Per-region tick rates**, quiet regions running slower. Genuinely attractive at MMO scale and not
  rejected on merit — deferred, because it needs a region to be a thing the simulation owns rather
  than a thing the pathfinder partitions (ADR 0033), and because a ship crossing between two rates
  is a problem nothing in this tree has had to have an answer for yet. It gets its own record the
  day it is proposed.
- **Leave it open.** What the tree had been doing. Rejected because an open tick rate makes every
  capacity number conditional, and a decision that costs nothing to take and blocks sizing until it
  is taken is the cheapest kind to take.

## Consequences

- **No code changed.** `SimTuning.h` is untouched: the comment that says an MMO server is unlikely
  to keep 60 Hz stays as it is, because it is the honest general observation and this record is the
  specific answer for this tree. A reader who follows it arrives here.
- **`TUNNEL_HEADROOM` = 0.6 is the margin a future change has to re-earn.** The gate is
  parameterized on `TICK_HZ`, so lowering the rate turns `HullSpecTests` red naming the hull rather
  than quietly halving the margin — which is the property that made this decision measurable at all.
  The threshold itself is not in the replay contract and nothing reads it at run time.
- **Shard sizing may now be written down.** Cost per shard is 60 × per-tick cost; the per-tick cost
  at a given entity count is what `SpatialIndexTests` and `InterestTests` already report.
- **Substepping stays un-built and un-needed.** The day a hull is added that fails the gate at
  60 Hz, the suite says so before the branch merges, and the decision is reopened with a specific
  hull attached rather than in the abstract.
- The reversal path is a new record superseding this one, not an edit here.
