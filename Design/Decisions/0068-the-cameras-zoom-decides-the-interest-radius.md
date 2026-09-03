# 0068 — The camera's zoom decides the interest radius, under a ceiling the server owns

Status: accepted
Date: 2026-09-03

## Context

The camera could pull back 900 m, which frames 745 m of the plane at a 45-degree field of view. A
sector is 8 192 m — the unit the HUD names under `SECTOR` and the minimap draws boundaries for — so
the player could never see one, and the widest view of a system a fleet was crossing was a tenth of
the cell it was crossing.

Raising the limit is arithmetic: `CameraMaxZoomMetres` puts the distance at 9 889 m and the far
plane follows it to 26 273 m. What raising it *exposes* is not. `INTEREST_RADIUS_METRES` is 2 000 m
and `Publisher` sends one subscriber only what is inside that circle of its centre, which
`UniverseSimulation::SetViewCentre` puts under the camera target every frame. A sector-wide frame
over a 2 km bubble is a picture of the sector with the ships left out of everything past its middle
sixth — not because they are not there, but because this client was never told. The zoom would have
been a scenery feature.

Interest is also the one number a subscriber converts directly into per-tick server work and
egress (`Design/Archive/MmoScalabilityReview.md` E4). A radius the client picks is a client
choosing what the server spends.

## Decision

The composition root pushes a radius alongside the centre each frame, derived from the camera's
orbit distance (`CameraInterestRadiusMetres`): the visible ground, floored at whatever `Server.cfg`
configured and capped at half a sector. `InterestSet::SetRadiusMetres` and
`Publisher::SetRadiusMetres` are the seam, on `SetCentre`'s terms exactly — outside the replay
contract, absent from the wire, changing what is sent and never what is simulated.

The cap is the server's and not the camera's. Half a sector is the radius whose diameter is the
sector the widest zoom frames, so the two limits state the same thing, and a client cannot ask for
more however it is modified. Below about 2 667 m of zoom the floor wins and a zoomed-in client
costs exactly what it always did.

## Alternatives considered

- **Leave the radius alone.** Cheapest and honest: the wide view would still show the planets,
  stations and gates, which are static content the client already holds. It loses the only thing
  the zoom was asked for — seeing where the fleets are — and would have shipped a view whose empty
  outer ring reads as a bug rather than as a budget.
- **Widen `interestRadiusMetres` in `Server.cfg` outright.** One line, no new seam. It pays the
  bandwidth and the per-tick query on every client at every zoom, including one sitting at 40 m
  watching a single Corvette, and it makes a deployment number carry a presentation decision.
- **Let the client request any radius it likes.** Simplest seam, and the wrong one the day the
  server is not in this process: a subscriber that names its own cost is a subscriber that can name
  any cost.
- **Derive the radius from the frustum's four ground corners instead of the orbit distance.** More
  exact, and unstable: at shallow pitch two corners look at the sky and have no ground point at all,
  so the radius would jump as the player tilted. The orbit distance is monotone in the zoom, which
  is the property that matters.

## Consequences

- A zoomed-out client costs more: the interest circle is up to four times the radius and sixteen
  times the area of the configured one, and the snapshot fragments across more datagrams
  accordingly (`ShipsPerSnapshotFragment`). That is the bandwidth the feature buys, and it is
  bounded — by the cap, not by the player.
- The frame is wider than the circle at its corners: interest is a circle and the sector it frames
  is a square, so hulls in the corners of the widest view are still not sent. The marks stop at the
  circle's edge and nothing pretends otherwise.
- `ShardApp` never calls the setter, so a shard server keeps the configured radius and nothing
  about it changes.
- A radius change is an ordinary enter and an ordinary leave to the subscriber
  (`InterestTests::WideningTheRadiusEntersWhatItReachedAndNarrowingLeavesIt`), which is what lets
  the client hold no special case for one.
