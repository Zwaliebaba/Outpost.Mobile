# 0064 — A mount is bound to its art by a client table of submesh names

Status: accepted
Date: 2026-09-02

## Context

`Combat.md` gave a mount a bearing, an arc, a device and a traverse rate, all `constexpr` in
`GameLogic`. The art gave a hull a submesh called `battleship_turret_0`. Nothing joined the two, and
three pieces of work waited on that join: the turret slew, the `Gun` markers, and the check that the
markers and the mount table agree (`Combat.md` §16, slices 4 and 5 both deferring into 6).

ADR 0002 already decided the direction: content is the renderer's, and what the simulation needs of a
hardpoint arrives as authored numbers in `GameLogic`, never read from a mesh at runtime. So the
binding is not a thing `GameLogic` may have. The question this record answers is where on the client
side it goes and what shape it takes.

## Decision

**The binding is a table of submesh names per hull and mount, in `Outpost`, resolved once per hull
against the mesh it is drawn from.**

- `Outpost/HullParts.h` holds `HULL_MOUNT_ART`: hull id, mount index, and up to three authored
  submesh names — a turret and its barrels, which turn together about the turret's centre.
- `ResolveMounts` turns a row into a `MountView` at the moment a hull first appears: the vertex runs,
  the pivot, the mount's rest bearing and arc from `HullSpec`, and the traverse rate from
  `DeviceSpec`. Nothing is looked up by name in a frame.
- **It is in `Outpost`, not `NeuronClient`**, and the work order was wrong to say otherwise. The
  table names `Game::HullId`, and `NeuronClient` may not list `GameLogic` (AGENTS.md §3). `Outpost`
  is the composition root, is the only thing entitled to see both layers, and already holds the
  hull-to-mesh table this is the sibling of. A headless shard still has none of it, which is the part
  of ADR 0002 that actually mattered.
- **A missing row, a missing part or a fixed device all mean the same thing**: bind nothing, draw one
  instanced hull, exactly as before. That is the design's own rule — content is a diagnostic, never a
  crash (`Combat.md` §3.1) — and it is what lets the table be authored one hull at a time.

## Alternatives considered

- **A field in `HullSpec`.** The mount already lives there, so the part name could too. Rejected
  hard: it puts a *mesh's* vocabulary inside the simulation's contract, so a headless server would
  carry submesh names it can never use, and renaming a submesh in Blender would change a `constexpr`
  table whose every other field changes recorded outcomes. ADR 0002 exists to refuse exactly this.
- **A bone.** The correct answer for a rigged hull, and the format supports one — `MeshMarker` even
  carries `parentBone`. Rejected because **no shipped hull has a rig** (`Combat-slice-3.md` §2.6): a
  turret is a rigid submesh, and its bind-pose centre is a pivot that already exists and is already
  exact. Adding a rig to author a binding that the bounds already give would be content work paying
  for a mechanism nothing needs. The day a rigged hull is authored is the day a slice poses it.
- **The `Gun` marker as the binding.** A marker is a point with an orientation, so it says where a
  muzzle is and not which vertices turn with it. It is the right anchor for the muzzle flash and is
  the wrong one for the slew, and using it for both would have made a marker's position load-bearing
  for geometry it does not describe.
- **A binding read from the mesh by convention** — anything named `*_turret_<n>` is mount *n*.
  Tempting, and it would need no table at all. Rejected because the correspondence is not a naming
  fact: the shipped Frigate carries two `battery` submeshes abeam and two `lance` submeshes forward
  for two mounts whose authored bearings are fore and aft, and the shipped Battleship carries three
  turrets for five mounts. A convention would have silently picked one and been wrong; a table has to
  be written down, which is what makes the disagreement visible.
- **Resolve per frame rather than per hull.** Simpler, and no state to carry. Rejected for the reason
  `ShipView` already copies `restY`, `pickCentre` and the exhaust list out of `MeshData`: the draw
  loop runs per ship per frame, and a name hash lookup there is a cost paid hundreds of times for an
  answer that cannot change.

## Consequences

- **A hull with a turret off its rest leaves the instanced path.** Its parts each need a matrix and
  an instance carries one, so it costs a pipeline switch plus a draw per gap where an instanced hull
  costs a share of one draw. That is a real regression against what ADR-era instancing bought
  (`Design/Archive/MmoScalabilityReview.md` G2), and it is bounded twice: a turret within
  `TURRET_STOWED_RAD` of rest counts as stowed and stays instanced, which is every hull outside a
  fight; and at most `MAX_POSED_HULLS` hulls draw posed in a frame, taken nearest first, so a battle
  too large to pose degrades to turrets pointing where they were authored rather than to a frame
  spike. Neither bound can change what was hit.
- `SceneRenderer` gains two entry points and a rule: `DrawMeshRange` draws one run under its own
  matrix, `DrawMeshComplement` draws everything else. The complement is arithmetic and lives in
  `MeshData.h` as `RangeComplement`, where `NeuronClientTests` can prove it — a gap that overlapped a
  posed run would draw a turret twice, once turning and once not, and that is wrong in silence.
- **The renderer knows nothing about pivots.** A caller that wants a part to turn about its own centre
  hands over the product of the translate, the rotate and the hull matrix. That is what keeps a
  rig-shaped question out of a renderer with no rig, and it is why posing something other than a
  turret later costs no renderer change.
- The table is a set of claims about the shipped art, and a claim nothing checks is a claim that
  rots. The `Tools/` check that holds it — and the `Gun` marker authoring beside it — is the part of
  slice 6 that is **not** in this commit, because what a marker must correspond to is a question the
  shipped art and `HullSpec` currently answer differently (`Combat-slice-6.md` §7).
