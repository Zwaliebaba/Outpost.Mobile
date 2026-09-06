# 0070 — The camera's zoom decides the minimap's reach

Status: accepted
Date: 2026-09-06

## Context

[ADR 0068](0068-the-cameras-zoom-decides-the-interest-radius.md) raised the zoom limit to 9 889 m,
which frames a whole sector, and made the interest radius follow it so the wide view would have
ships in it. `HUD_MINIMAP_HALF_RANGE` was not part of that change and stayed at 4 000 m. Nobody
noticed, because a minimap that is too small looks exactly like a minimap.

It is measurably too small. The map rectangle is 212 × 140 px at one metres-per-pixel, so 4 000 m
east and west is 2 641 m north and south. At the widest zoom, the camera's ground footprint at the
default 52-degree pitch runs from 3 927 m *south* of the target to 7 685 m *north* of it. Two
things follow, and the owner reported the second as a bug:

- **A tap cannot name most of what is on screen.** The map is the only way to order a fleet
  somewhere off the visible plane, and at the widest zoom the northern five kilometres of the frame
  are not on it at any pixel.
- **The frustum quad silently stops being drawn.** `ClipToRect` draws an edge only where it crosses
  the map rectangle. Past about 3 400 m of zoom the far edge is outside it, past about 6 700 m all
  four are, and the one cue that would have shown the player the scale mismatch disappears exactly
  when the mismatch appears.

The order itself was never wrong — `Hud::HandlePointer` inverts `DrawMinimap`'s mapping exactly, and
`UniverseView::ViewX` and `UniversePosAt` are inverses because the view origin never moves. What was
wrong is how far it could reach, and the absence of anything on screen saying so.

## Decision

The map's east–west reach is `MinimapHalfRangeMetres(distance)`: `HUD_MINIMAP_RANGE_FACTOR` of the
camera's orbit distance, floored at `HUD_MINIMAP_HALF_RANGE`. North and south follow from the
rectangle's shape, as they always did.

The factor is 0.75, and it is the camera's own horizontal reach rather than a taste: the frame's
half-width at the target plane is `distance · tan(fov/2) · aspect`, which is 0.736 of the distance
at `CAMERA_FOV_DEG` on 16:9. The rounding up is the headroom. It equals
`CAMERA_INTEREST_RADIUS_FACTOR` and is deliberately not the same constant — two decisions that agree
on a number are still two decisions, and sharing the symbol would make a retune of either silently a
retune of the other.

There is no ceiling of its own. `CAMERA_MAX_ZOOM_SECTORS` is already one: the widest the camera goes
puts the reach at 7 417 m and the map at 14.8 km across. Below 5 333 m of orbit distance the floor
wins and the map is exactly what it has always been, which is every zoom the player starts at.

The reach is derived from the orbit distance and not from the frustum's ground corners, for the
reason ADR 0068 gave when it made the same choice for the interest radius: at shallow pitch two
corners look at the sky and have no ground point, and the far pair run to the horizon, so a
corner-derived reach would jump as the player tilted. The orbit distance is monotone in the zoom.

`Hud::ProjectMinimap` is the seam. The draw and the tap took the mapping separately before, and the
tap's comment said so — "the inverse of `DrawMinimap`'s mapping, against the same camera target and
half-range". That held while the reach was a constant. One object both sites build is what makes it
hold now that it is not.

## Alternatives considered

- **Leave the minimap a fixed-scale tactical inset and loosen the camera's follow instead.** The
  competing reading of the same report, and a coherent one: a fixed scale means a glance at the map
  always means the same distance, which is most of what a tactical map is for, and the fleet
  appearing not to move is `UniverseView::UpdateFeedback` pinning the camera to the centroid of the
  selected moving ships every frame rather than anything the map does. It loses the thing the zoom
  was raised for a second time — a player who can see a sector still could not order into one — and
  it trades a map that cannot reach for a camera that does not follow, which is a worse trade at the
  grain this game is played at.
- **Widen `HUD_MINIMAP_HALF_RANGE` to a fixed number that covers the widest zoom.** One line, and it
  keeps the fixed scale. At 7 417 m the boot scene's three hulls, the Vandal base and its patrol are
  a single cluster nine pixels across, and every zoom pays for the one nobody is at.
- **Match the interest radius.** Tempting, because that is honestly what the wire brings and a map
  showing more is drawing empty space where records exist. It runs 2 000 to 4 096 m under
  `CameraInterestRadiusMetres`, so it would *shrink* the map at boot and barely grow it at the top —
  the opposite of the problem. The two numbers answer different questions: what the server sends,
  and how far the player may point.
- **Scale the map to contain the whole frustum.** What the report literally asks for, and not
  achievable: at 5 degrees of pitch the far ground intersection is at the horizon. Containing the
  footprint symmetrically about the target also spends half the map on the 3 927 m behind the camera
  to reach the 7 685 m in front of it.

## Consequences

- The map's scale now varies, so a distance read off it is only as good as the zoom the player
  remembers. The sector boundaries are drawn in metres and are the honest cue; the faint grid is
  fixed pixels and is decoration, which it was already (`HUD_MINIMAP_GRID_PX`).
- A gate at `GalaxyDesc::gateRingMetres`, 7 000 m, can now be *inside* the map east or west at the
  widest zoom, where before every gate was always clamped to the edge. North and south it is still
  clamped at every zoom, since the rectangle is shorter than it is wide.
- Nothing about the mark snap changes: a tap within `HUD_MINIMAP_MARK_PX` of a station or gate
  diamond still means that mark's true position, which is what lets one tap reach a gate the map's
  bare reach stops short of.
- **The camera's follow is untouched, and it is still why a moving fleet looks stationary.**
  `UniverseView::UpdateFeedback` sets the camera goal to the centroid of the selected *moving* ships
  every frame, so an ordered fleet is pinned to the middle of the screen and to the middle of the
  map, and at 28 m/s under a sector-wide frame the world slides past it at about three pixels a
  second. This record makes the order reach where the player pointed; it does not make the reaching
  visible. That is a separate question and it has not been decided here.
- `Outpost` has no test suite — its logic is D3D12- and WinRT-bound (AGENTS.md 2) — so the round
  trip, the floor, the monotonicity and the frame fit were checked by lifting `MinimapProjection`
  and `MinimapHalfRangeMetres` into a standalone program, not by a row in a suite. That is the same
  standard of evidence the rest of this layer runs on, and it is stated rather than implied.
