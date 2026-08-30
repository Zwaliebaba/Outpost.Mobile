# 0004 — The separation clamp caps what a pair closes, then splits it

Status: accepted
Date: 2026-08-29

## Context

[`Design/Archive/Collision.md`](../Archive/Collision.md) §9 gives separation two mechanisms. Authority decides how a
contacting pair splits the correction, so that a Carrier does not shoulder aside for an Interceptor:
each side takes the *other's* authority over the sum. A per-tick clamp then bounds "the total
correction applied to a ship in one tick", expressed as a fraction of that ship's own capsule
radius, and the document is explicit that the clamp is not optional — it is what makes deep overlap
unwind instead of exploding, and it *is* the prediction error budget that §10 relies on when it
makes avoidance server-only.

The two mechanisms fight. Clamping each ship's own displacement scales the two sides of a pair by
different factors, because their radii differ — which is exactly the case authority exists to
handle. An Interceptor 10.8 m inside a Carrier wants a 5.06 m correction and is cut to 0.28 m by its
own small radius, while the Carrier's 0.34 m share passes unclamped. Measured over the encounter:
the capital moved 3.8 m to the fighter's 6.9 m, a ratio of 1.8:1 where authority says 15:1. The
clamp inverts the split precisely when it binds, and it binds exactly when the split matters most.

## Decision

Cap what the *pair* closes in a tick, then split that by authority — never the other way round. The
cap is a fraction of the smaller hull's capsule radius, so both sides derive the same number from
the same two radii with no arbitration, and the gather stays order-independent. Keep a per-ship
clamp as the backstop for a ship in contact with many others at once; it is what bounds the
prediction error budget, and it is now rarely the binding constraint on a single pair.

## Alternatives considered

- **The document's form, clamping each ship.** Rejected on the measurement above: 1.8:1 against
  15:1, and visibly wrong on screen, which is the thing §9 opens by saying it must not be.
- **Drop the per-ship clamp and keep only the pair cap.** Rejected: a ship with eight contacts could
  then move eight cap-widths in a tick, and the prediction error budget stops being a number.
- **Clamp by a shared scale, such as the pair's combined radii.** Rejected: a ship's total is a sum
  over pairs of different sizes, so no single pair's scale bounds it.

## Consequences

- The authority ratio survives the clamp: the same encounter now measures 15:1.
- Two contract values instead of one, `SEPARATION_PAIR_CLOSE_FRACTION` and
  `SEPARATION_CLAMP_FRACTION`, and both change recorded outcomes.
- The worst single-tick displacement is still exactly the per-ship clamp, which the dense-spawn test
  measures, so §10's argument for server-only avoidance is unaffected.
- §9 of the design document now disagrees with the code. The Slices section of that document points
  here.
