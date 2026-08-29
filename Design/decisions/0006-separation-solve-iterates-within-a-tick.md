# 0006 — The separation solve runs several times per tick

Status: accepted
Date: 2026-08-29

## Context

[`Design/Collision.md`](../Collision.md) §6 chooses a Jacobi solve for pass 5 — gather into scratch,
apply after the loop — over Gauss-Seidel, because Gauss-Seidel is order-dependent and
order-independence is the property the whole tick is built to protect. It notes the cost as
"marginally softer per iteration". That undersells it, and the shape it lands on is not the one the
wording implies.

Measured: a compressed line of Interceptors relaxes in 3.2·N² ticks — 100 for five hulls, 20,250 for
eighty. The trigger is not position but *heading*. A pack with varied headings relaxes in 275 ticks
at N = 40 where the same pack with one heading takes 2,625, because parallel hulls give collinear
contact normals and the pack collapses into lines instead of spreading in two dimensions. A fleet in
formation is parallel by construction, so this is an ordinary arrangement, not a pathology.

The quadratic is a theorem about the model rather than a defect in it. The interior of a compressed
line is translation-invariant: every interior ship sees the same neighbourhood, so any solver that
is local, order-independent and translation-equivariant must give them all the same correction, and
the same correction everywhere is a translation, which lengthens nothing. Expansion is therefore
sourced only at the two ends and reaches the middle by diffusion. Instrumented, the relaxation front
creeps in from both ends and visibly decelerates, which is the square-root-of-time signature the
diffusion account predicts.

## Decision

Run the separation solve several times per tick — eight, each a full Jacobi gather — stopping early
once the largest correction falls below a settle threshold. Each step carries the ends' information
one ship further inward, so k steps divide the quadratic's constant by up to k. Apply the per-tick
clamp to the tick's running total rather than to each step, so extra steps buy convergence and never
extra displacement. Reduce the early-out with `max` rather than `sum`, because max over floats is
exact and order-independent where a sum is neither.

Measured against one step, on compressed parallel packs: 8 hulls 2.9 s → 0.4 s, 16 hulls 10.4 s →
2.5 s, 24 hulls 21.7 s → 6.2 s, 40 hulls 45.4 s → 13.3 s.

## Alternatives considered

- **Gauss-Seidel, which converges faster.** Rejected, as the design rejects it: it makes the answer
  depend on array order, which is the one property this tick exists to protect.
- **Raise the per-tick clamp.** Rejected on measurement: raising it eightfold changed the chain by
  nothing. The clamp is not the rate limit, and it is the prediction error budget.
- **Raise the pair cap, or soften it into a smooth saturation to keep its gradient.** Rejected on
  measurement: four cap settings made no material difference, and the smooth form was *worse* —
  the magnitude it gives up costs more than the gradient it buys.
- **Break the symmetry with a deterministic per-ship jitter.** Rejected: it is noise written into
  the replay contract to work around a property of the model, and it would show as shimmer on ships
  that should be still.
- **Accept the quadratic and document it.** What was done first, and not enough: at forty hulls it
  is forty-five seconds of a formation visibly untangling itself.

## Consequences

- Two contract values, `SEPARATION_ITERATIONS` and `SEPARATION_SETTLE_METRES`; both change
  recorded outcomes.
- Near-free when there is nothing to solve: a pack that is not overlapping produces no corrections,
  so the first step's largest correction is zero and the loop leaves after one. The cost is spent
  only where there is a jam.
- Still quadratic. The theorem above says no local order-independent solver does better, so a future
  attempt has to change one of those three words — a global pressure solve, or accepting order
  dependence — and neither is worth it at the fleet sizes this game has.
- A test pins the case that occurs rather than the one that is easy to construct: twenty-four
  parallel hulls, compressed, unpacked inside 900 ticks and never exceeding the per-tick clamp. It
  fails at one iteration and passes at eight.
