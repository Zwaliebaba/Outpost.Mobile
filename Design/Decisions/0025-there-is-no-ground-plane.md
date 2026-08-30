# 0025 — There is no ground plane, and the scene pass has no grid

Status: accepted
Date: 2026-08-29

## Context

The scene started as a fleet on a lit plane: a 4 km quad that followed the camera, with a procedural
grid drawn on it by `ScenePS` — spacing, line width and a distance fade, switched on per draw by a
`bool _isGround` on `SceneRenderer::DrawMesh` and carried to the shader as `gridParams.w`.

The plane did two jobs. It gave the eye something to read motion and scale against, and it hid
everything below the horizon.

The sky (`0024`, `Design/Archive/Skybox.md`) took over the first job and made the second one a cost. A star
field wraps the whole sphere, and the plane was occluding the lower half of it. With the plane gone
the fleet sits in open space with the sky behind it in every direction, which is what the game is
about; with it there, the game looks like it is happening in a hangar.

The plane was never simulation. `Game::World` has no ground in it: a ship's position is a 2-D world
position, movement solves in that plane, and `Camera::RayToGround` intersects **y = 0** as
arithmetic. The quad was a drawing of that plane, not the plane itself.

## Decision

The ground quad is not drawn, and the scene pass has no ground path.

- `SceneRenderer::DrawMesh` loses its `bool _isGround`, `ScenePS` loses the grid branch, and
  `Scene.hlsli`'s `PsConstants` loses `gridColour` and `gridParams`.
- `SceneFrame` loses `gridColour`, `gridSpacing`, `gridLineWidthPx` and `gridFadeDistance`.
- `ViewTuning.h` loses `GROUND_COLOUR`, `GROUND_SIZE` and the four `GRID_*` values.
- The shared pixel root-constant block shrinks from 20 DWORDs to 12, and `Decal.hlsli` — which had
  declared `unusedA`/`unusedB` purely to keep `cameraPos` at the same offset as `Scene.hlsli`'s —
  loses them and moves `cameraPos` to offset 8 with it.

What stays is the **plane at y = 0**: a move order lands on it, ships fly at `SHIP_HOVER_HEIGHT`
above it, and selection rings, order markers and the shock ring are all decals drawn on it. That
plane is arithmetic in `Camera` and always was; nothing about ordering, picking or formation
changed.

`HUD_PANEL_OUTLINE` had been defined as `GRID_COLOUR` with a different alpha — "the grid, as a
rule". It now carries the same three numbers literally, with a comment saying where they came from:
the color outlived the grid because the HUD had adopted it.

## Alternatives considered

- **Keep the plane, drop only the grid lines.** The owner was asked and chose the whole plane. An
  unlit plane at `GROUND_COLOUR` 0.075 against a near-black sky reads as a distinctly lighter floor,
  so it would have kept the hangar look and the occlusion while losing the one part that was
  actually doing work.
- **Keep the grid but make it transparent below the horizon.** More machinery than the feature is
  worth, and it would keep four tuning constants and a shader branch alive to serve a look nobody
  asked for.
- **Leave the `_isGround` path in `SceneRenderer` for a future game.** Rejected: `Neuron*` is a
  shared engine and a ground grid is a plausible thing for a second game to want, but dead code kept
  on that argument is how an engine accumulates paths nothing exercises. It is four constants, a
  shader branch and a bool; the day a game needs it, it is a smaller change to write than to have
  maintained. This record is where to find what it was.

## Consequences

- The scene pass is simpler and its shared root-constant block is 40 % smaller. Nothing measured
  faster; that was not the point.
- With the plane gone, a body's unlit side is now drawn against black instead of against the plane,
  so the barren moon reads as scattered lit facets rather than as a sphere from some angles. That is
  the existing flat-shaded body pipeline being seen honestly, not a regression, and it is worth
  knowing before somebody files it as one.
- `SKY_COLOUR` is now what fills the lower half of the frame. It was lowered to near-black in the
  same slice for the sky's sake, which is also the right value for this.
- Any future ground — a landing pad, a station deck — is new geometry and a new decision, not a flag
  on `DrawMesh`.
