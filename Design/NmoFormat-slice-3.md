# Work order — NMO slice 3: the content swap and the loader switch

Implements the **content swap** of [`NmoFormat.md`](NmoFormat.md) §14 — that document's slice 4 —
brought in front of the marker consumers, and closes it out: the hulls ship as `.nmo`,
`MeshLibrary` loads them through `NmoReader`, and `ObjParser`, the OBJ/MTL assets and the runtime
clustering heuristic are deleted. Nothing on screen is meant to change.

**Layer:** `NeuronClient`, `Outpost/Assets`, `Tests/NeuronClientTests`.
**Depends on:** slice 2 (`NmoReader`, `MeshData::markers`, `attachPoints` filled from `Exhaust`).
**Blocks:** slice 4 (marker consumers).

**Why the design's order is swapped, and the record that owes for it.** `NmoFormat.md` §14 has the
marker consumers third and the content swap fourth, with slice 3's `MeshLibrary` "loads `.nmo`
when present (OBJ fallback stays)". That fallback only makes sense if the OBJ hulls outlive the
swap, and the owner has decided they do not: `ObjParser`, the twenty `.obj`/`.mtl` files and the
clustering all go. A fallback written in one slice and deleted in the next is a branch nobody ever
exercises, so the swap comes first and the fallback is never written. This slice carries a
decision record for the reorder and for the corpus change beneath it (§2.7).

---

## 1. Why this is a slice

This is the one that can break the game, and it is the one that must not look like it did
anything. Every hull the player sees changes format, changes source corpus and changes loader in a
single commit, and the honest test of that is a screenshot that is boring. Keeping the marker
consumers out of it is what makes the boring screenshot meaningful: if the fleet looks the same
after this slice, the swap is right, and when slice 4 changes how an exhaust is coloured, that
change is the only variable in its own screenshot.

---

## 2. Scope

### 2.1 `Outpost/Assets/Meshes/` — the corpus

Convert every hull from the authored GLBs and land the results:

```
python Art/Meshes/GlbToNmo.py --out Outpost/Assets/Meshes
```

Eleven files: `Battleship`, `Bomber`, `Carrier`, `Corvette`, `Fighter`, `Frigate`, `Hauler`,
`Interceptor`, `Miner`, `Stargate`, `Structure`. The converter reads each result back through the
codec and prints what it holds; **paste that output into the pull request** — it is the content
evidence, and it is the only place the marker counts are visible before slice 4 draws them.

Delete the twenty `.obj` and `.mtl` files, and their `<None>` entries in `Outpost.vcxproj` and
`Outpost.vcxproj.filters`. Register the eleven `.nmo` in their place, in the same `Assets\Meshes`
filter, with the same `<CopyToOutputDirectory>` treatment the `.obj` entries had — copy the
existing element shape rather than inventing one, and check the deployed output actually contains
them before concluding it worked.

`Fighter.nmo` and `Miner.nmo` have no OBJ counterpart and no `HULL_MESHES` row. They ship anyway:
they are hulls this corpus has and the game will want. Do **not** add rows for them here — a new
hull in the scene is a design question, not a format one.

### 2.2 `NeuronClient/MeshLibrary.cpp` — the switch

`ObjParser::Load` becomes `NmoReader::Load`. That is the whole change: the signature matched on
purpose (slice 2 §2.3), the failure path is unchanged, and a hull that cannot be read is still a
logged diagnostic that skips the mesh and lets the ship simulate.

The comment on `MeshLibrary::Load` in the header says "Loads `_dir/_name.obj` (and its `.mtl`)".
It becomes `.nmo`, and the sentence about the `.mtl` goes.

### 2.3 `NeuronClient/ObjParser.h`, `.cpp` — deleted

Both files, their `NeuronClient.vcxproj` and `.filters` entries, and the `#include "ObjParser.h"`
in `NeuronClient.h`. With them goes `ClusterAttachPoints` — the union-find over `thruster`-material
face centroids that has run in the shipping loader on every boot since the hulls arrived. Its
conventions live on in `Tools/ObjToNmo.py`, which is where a heuristic that runs once belongs
([ADR 0011](Decisions/0011-ship-meshes-are-nmo-and-its-tools-are-python.md), design §13); the
`Tools/` copy stays and is not touched by this slice.

`MeshData::attachPoints` **stays** — `NmoReader` fills it from the `Exhaust` markers and
`WorldView` still reads it. It is slice 4 that removes both ends.

### 2.4 `Tests/NeuronClientTests/ObjParserTests.cpp` — deleted, minus what it was really testing

Three of its four tests are `MeshData` tests wearing the wrong file's name:
`EmptyBoundsAreUsable` and `BoundsGiveTheCentreAndTheLift` never mention `ObjParser`, and they
guard the degenerate-extent rule every consumer divides by. **Move them, unchanged, into
`NmoReaderTests.cpp`** with a note that they came from `ObjParserTests` and what they are for.
`AMissingMeshFailsClosed` has its equivalent in the NMO suite already (slice 2 §2.5); if it does
not, add it there before deleting this file.

Then delete `ObjParserTests.cpp` and its project entries.

### 2.5 `Outpost/OutpostApp.cpp` — nothing, and check that it is nothing

`MESH_DIR` is `L"Meshes\\"` and `HULL_MESHES` names hulls without extensions. Neither changes.
Confirm it in the report rather than assuming it: if anything in the boot path spells `.obj`, this
is where it would be.

### 2.6 The sentences this makes false

- **`AGENTS.md`**, opening section: "OBJ/MTL hulls" in the list of what the renderer draws, and
  "no content pipeline beyond OBJ and DDS" in the deliberately-not-here-yet paragraph. Both become
  NMO. The `Tools/` map row (§2) gains `Art/Meshes/GlbToNmo.py` — see §2.7.
- **`Design/NmoFormat.md`** §1's constraint table cites `ObjParser.cpp:118-136` and
  `ObjParser.h:19` for the clustering, and §9 cites `WorldView.cpp:100`/`728`. A design is never
  rewritten to match what was built (Design/README.md) — leave the argument alone. §14 gains the
  landed marks and the note that 3 and 4 traded places, which is what the decision record explains.
- Any comment in `NeuronClient` or `Outpost` that says a hull is an OBJ. Grep for `obj`, `mtl`
  and `Wavefront` and fix what is now a lie; change nothing else while you are in those files.

### 2.7 The decision record

`Design/Decisions/0035-ship-hulls-are-authored-in-glb-and-converted-to-nmo.md`, in this pull
request, covering three things that were decided outside the design and that someone will propose
again:

- **The corpus is authored elsewhere and arrives as GLB.** `Art/Meshes/*.glb` is the source of
  truth for hull geometry, materials and markers; `Art/Meshes/GlbToNmo.py` converts it through the
  Blender add-on. `NmoFormat.md` §13 assumed the corpus would be the ten OBJ hulls converted by
  `ObjToNmo.py`, with markers seeded from the clustering heuristic; that is not what happened —
  the hulls were re-authored with markers placed by hand, and the GLBs carry them as node
  `extras`. `ObjToNmo.py` stays as the OBJ path's record and the Blender test's fixture source,
  not as the content pipeline.
- **Slices 3 and 4 traded places**, and why: no OBJ fallback branch is ever written.
- **`ObjParser` is deleted rather than kept for development import**, which §14 slice 4 permitted.
  Two mesh readers for one format each, one of which reads a format no asset in the tree uses, is
  the kind of thing that is easy to keep and hard to justify; the greenfield rule says the tree
  carries what it uses.

Note the converter's location honestly: it sits in `Art/Meshes/` beside the GLBs it reads, not in
`Tools/` where §4 puts content tools. That is where the owner asked for it and it is where an
artist will look for it; the `Tools/` map row in `AGENTS.md` §2 gains a sentence pointing at it so
the tree is not silently split.

### 2.8 What this slice deliberately does **not** do

- **No marker consumption.** `ShipView::thrusterLocals` still comes from `attachPoints`; the glow
  is still `SELECTED_COLOUR`/`HOSTILE_ACCENT_COLOUR`; there are no nav lights. All of that is
  slice 4, and this slice's screenshots must show the old look.
- **No `WorldView`, `Hud`, `ShipExplosion`, `MeshShatter` or `SceneRenderer` edit.**
- **No `ViewTuning.h` constant.** Nothing here is tunable.
- **No new hull in the scene.** `Fighter` and `Miner` ship and are not spawned.
- **No re-authoring.** If a converted hull looks wrong, the fix is in the GLB or in the converter,
  reported as a content defect — not a hand-edited `.nmo`.
- **No `Tools/` change**, including `ObjToNmo.py`, which stays exactly as it is.

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| `NeuronClient/NmoReader.h` (slice 2) | `Load`, signature-identical to `ObjParser::Load` |
| `NeuronClient/MeshLibrary.cpp` | The one call site, its `DebugTrace` and its `INVALID_MESH` return |
| `Art/Meshes/GlbToNmo.py` | The converter; `--out` writes elsewhere, and it prints a read-back summary per file |
| `Art/Meshes/*.glb` | The authored corpus: eleven hulls, markers as node `extras` |
| `Outpost/Outpost.vcxproj` + `.filters` | The `<None>` element shape the `.obj` entries use, and the `Assets\Meshes` filter |
| `Tests/NeuronClientTests/ObjParserTests.cpp` | Two `MeshData` tests to rescue before it is deleted |
| `Design/Archive/Hostiles-slice-3.md` §5 | The precedent for stating a content assumption instead of fixing a mesh mid-slice |

---

## 4. Acceptance

Visual, because only a screen can decide "nothing changed", at two window sizes — 1600×900 and one
other — in the pull request:

- **Boot, before and after.** The same seed (F5 is not pressed between them), the same camera: the
  fleet, the station and the three Interceptors are the same shapes in the same colours. Hull
  colour is the check that matters most — it now arrives through the material rather than through
  vertex colour (slice 2 §2.3), so a hull that comes out black or washed out is this slice's bug
  and the screenshot is where it shows.
- **Thrusters still glow and still trail.** Unchanged, from `Exhaust` markers via `attachPoints`
  instead of from clustering. A ship under way, at two window sizes.
- **F4 still shatters.** The explosion reads `MeshData`'s soup; the soup now comes from a different
  reader. One screenshot mid-explosion.
- **Selection and picking.** A tap selects a hull and the ring sits on it — bounds now come from
  the file's extents rather than being accumulated by the parser, and picking divides by them.

Not visual:

- The boot event log shows eleven hulls loaded with no content diagnostic; paste it.
- `GlbToNmo.py`'s conversion output, pasted, showing 11 of 11 and the marker counts.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass — the project-file
  check is the one that catches a deleted asset still listed, which is the likeliest mistake here.
- Debug|x64 builds; all four suites green. `NeuronClientTests` has lost `ObjParserTests` and kept
  its two rescued tests; say so by name.
- `git diff --stat` shows `.obj`, `.mtl`, `ObjParser.*` and `ObjParserTests.cpp` deleted, and no
  path under `GameLogic/`, `NeuronCore/` or `NeuronServer/`.
- The decision record of §2.7 is in this pull request and listed in `Design/Decisions/README.md`.
- `Design/NmoFormat.md` §14 marks this slice landed; this file moves to `Design/Archive/`; the
  `AGENTS.md` sentences of §2.6 have changed.

---

## 5. Assumptions the implementer may make

- **The GLB corpus is correct as authored.** It converts clean today — eleven files, 11 of 11,
  every one read back through the codec. If a hull looks wrong on screen, report it as a content
  defect with the screenshot; do not edit geometry in this slice.
- **Submesh counts are much higher than the OBJ hulls', and that is fine.** The Corvette is 12
  submeshes where the OBJ was 5, the Structure is 117, because the GLBs are authored per part
  rather than per material. Nothing in this slice draws per submesh — the reader expands
  everything into one soup and `SceneRenderer` issues one draw, exactly as before.
- **Exhaust markers point up, not aft.** The GLB marker nodes carry identity rotation, which the
  axis conversion lands on NMO `+Y`. No consumer reads a marker's direction in this slice or the
  next, so it is inert; it is stated here so it is found on purpose in slice 5 rather than by
  surprise. Fixing it is a change to the GLB source or one defaulting line in the converter, and
  it is not this slice's.
- **`Structure` and `Stargate` carry only `NavLight` markers**, so their `attachPoints` are empty
  and they keep the still, unlit look they have today until slice 4 gives them nav lights.
- **Emissive material values are carried and ignored.** Every hull has them; no pass reads them.
