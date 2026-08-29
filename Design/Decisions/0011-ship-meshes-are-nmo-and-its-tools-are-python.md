# 0011 — Ship meshes are NMO v2, and its reference tooling is Python

Status: accepted
Date: 2026-08-29

## Context

Hulls are OBJ/MTL: no names, no hierarchy, no animation, and attachment points recovered by a
clustering heuristic that runs in the shipping loader on every boot (`ObjParser.cpp:118-136`),
with the exhaust glow coloured by a placeholder constant for every ship (`WorldView.cpp:728`).
Coming work wants authored data OBJ cannot carry: exhausts with colour, navigation lights, gun
mounts, and articulated parts (turrets, dishes) with bone animation.

The sibling Interstellar Outpost tree had already designed **NMO**, a binary mesh format derived
from CMO with per-submesh skeletons and named markers — proven container discipline (validate
before trust, offsets, natural alignment), but carrying loads that tree's `.pie` corpus and D3D9
target forced: CMO byte-compatibility, matrix keyframes, a texture-atlas team-colour descriptor,
an 8-slot Phong material.

## Decision

Adopt NMO **version 2.0** as the ship mesh format, specified normatively in
[`Design/NmoFormat.md`](../NmoFormat.md): the sibling proposal's container rules, submesh
skeletons with aliasing, and SRT clips, minus the CMO-fidelity baggage, plus typed markers (a
kind string with colour and parameters — `Exhaust`, `NavLight`, `Gun`, open-ended). The magic is
`'NMO2'` so a sibling-dialect file fails in one comparison. The reference tooling is stdlib
Python in `Tools/` — codec, Blender add-on, OBJ converter, tests — because the codec must run
inside Blender, and Blender is the authoring tool the format exists to serve. The engine reader
comes later, in NeuronClient, where content readers live (0002).

## Alternatives considered

- **Adopt the sibling NMO 1.x byte-compatibly, sharing tooling across the two trees.** Rejected:
  it would import D3D9's constraints and CMO's dead weight into a tree with neither, forever, to
  stay compatible with a format that has itself shipped nothing. `Design/NmoFormat.md` §3 records
  every dropped piece and why; §15 Q1 leaves reunification open if the owner wants it.
- **glTF.** Rejected in `Design/NmoFormat.md` §6: a conforming reader means JSON + base64 + an
  extension ecosystem (or a third-party library AGENTS.md §5 forbids by default), and the fields
  this game needs would be custom extensions anyway.
- **Keep OBJ and grow conventions** (more magic material names, sidecar text files). Rejected:
  heuristics in the loader are the disease this format cures, and OBJ has no animation story at
  all.
- **C++ as the reference codec, Python generated or omitted.** Rejected: the Blender add-on must
  read and write the format in Python regardless, so Python is the implementation that cannot be
  avoided; a second, C++ statement of the spec arrives with the engine reader slice and is tested
  against the same generated fixture bytes.

## Consequences

- Two executable statements of the format will exist (Python now, C++ in the reader slice); the
  shared fixture generator and the per-rule malformed-file tests are what keep them agreeing.
- `Tools/` is a new top-level directory outside the solution; its tests run on python3 (plus the
  `bpy` wheel for the Blender one) and are not VS test suites — CI wiring is a later,
  non-blocking-first step, per the linter-promotion practice.
- Content becomes authored: markers, names and rigs are editable in Blender rather than implied
  by material conventions. The converter seeds `Exhaust` markers from the old heuristic once;
  `NavLight`/`Gun` markers only exist where an artist places them.
- The engine changes nothing until the reader slice lands; OBJ stays the shipping path and the
  clustering heuristic stays in `ObjParser` until `Design/NmoFormat.md` §14 slices 2–4 retire it.
