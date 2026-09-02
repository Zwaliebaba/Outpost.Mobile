# Work order — Combat slice 6: the turret turns, and the content it needs

Implements slice 6 of [`Combat.md`](Combat.md) §16, the last one, and the slice that closes the
combat design.

**Layer:** `NeuronClient` + `Outpost` + `Tools/`.
**Depends on:** slices 3 and 4 — a `MeshData` that keeps named submeshes, and a client that already
draws muzzles, tracers and pips off the fire message.
**Blocks:** nothing. When it lands, `Combat.md` moves to `Archive/` with its six work orders.

---

## 1. Why this is the last slice, and why it was cut out of two others

Three things were deferred here, and all three wait on the same missing piece: **a binding from a
mount to the parts of the hull that carry it.**

Slice 4 shipped muzzle flashes, tracers, impacts and condition pips, and cut out **the turret slew**
and **the target bar**: the slew needs a renderer entry point that does not exist — a draw of one
submesh range, and the hull drawn as its own complement — and only a screenshot can accept either
(`Combat-slice-4.md`). Slice 5 measured the pacing targets and cut out **the `Gun` markers** and
**the mount-versus-marker consistency check**, because slice 3 found the muzzle position is already
exact in the art as a submesh's bind-pose centre, so authoring markers before a table reads them
would put a third copy of a number the art and `HullSpec` already carry (`Combat-slice-5.md` §3).

The binding is the piece. It is a **client-side table read off submesh names**, which is where ADR
0002 puts it: content is the renderer's, and a headless server has none of it.

## 2. Scope

1. **`NeuronClient/SceneRenderer` — a submesh-range draw, and the complement.**

   `DrawMesh` draws a whole mesh. This slice adds a way to draw **one submesh range under its own
   world matrix**, and to draw **the rest of the hull** — the complement of the ranges a caller is
   posing itself. A hull with a turning turret is then two or three draws instead of one: the
   complement at the hull's matrix, each posed part at its own.

   The complement is the awkward half and it is the reason this is a renderer change rather than a
   caller's. A submesh is a contiguous vertex run (`MeshData.h`), so the complement of *k* posed
   ranges is at most *k+1* runs, and the entry point takes them as a span rather than making every
   caller compute them. A hull with no posed part is one draw, exactly as now, and pays nothing.

2. **`NeuronClient` — the mount-to-part table.**

   A table binding mount index to submesh name hash, per hull, read at load time off the names the
   art already carries (`battleship_turret_0` and its siblings). It lives in the client for ADR
   0002's reason and is authored beside the mesh library rather than in `GameLogic`, which must not
   learn what a hull looks like.

   A hull whose art has no turret submesh binds nothing and draws exactly as it does today. That is
   most of the roster, and it is why this slice cannot regress a hull it does not touch.

3. **`Outpost` — the slew.**

   Each mount's part turns toward the last target that mount was seen firing at, at the device's
   `traverseRadPerSec`, and drifts back to rest when there is nothing. **Presentation only**: it is
   driven from the fire message the client already receives, it feeds nothing back, and a mount
   whose aim the simulation settled is not consulted — the server decided the shot landed and this
   is a drawing of it (§10.2). A fixed mount does not slew, which falls out of `Fixed()` and needs
   no branch of its own.

4. **`Outpost` — the target bar** (§10.3, cut out of slice 4). A thin condition bar on the ordered
   target's selection bracket, and the HUD stat panel's `HULL` bar finally reading the record's
   `hullFraction` instead of the hard-coded whole it shows today.

5. **`Tools/` and the art — the `Gun` markers, and the check that they agree.**

   Author a `Gun` marker per mount on the hulls that carry turrets, at the position the binding now
   reads, and extend `Tools/NmoShippedArtTest.py` to assert **that the markers and `HullSpec`'s
   mount table agree** — the consistency check slice 5 deferred. It needs to see the simulation's
   table and the game's art at once, and §3 below decides how.

6. **Prose in the same commit**: `Combat.md` §16's slice 6 entry becomes what landed, §10.1's "start
   turning in slice 6" becomes the present tense, and §10.2's note that the slew "is slice 6 for
   exactly that reason" says what the entry point turned out to be.

7. **A decision record** for the mount-to-part table: a client table read off submesh names, and why
   not a marker, a bone or a field in `HullSpec`.

## 3. The one choice this order leaves open, and how to close it

The consistency check needs `HullSpec`'s mount table, which is a C++ `constexpr` table, and the art,
which is NMO under `Outpost/Assets/Meshes/`. `Tools/` is stdlib Python and sees the art but not the
table. Slice 5 named the two candidates and did not choose:

- **Parse `HullSpec.h`** from the Python check. No build step, no generated file, and a parser for a
  C++ aggregate that will break the day the table's shape changes.
- **Generate a small table** the check reads. Robust to the header's formatting, and it adds a
  generated file to a tree whose rule is that nothing generated is committed (AGENTS.md §3).

**Take the parse, and bound it**: the check reads only `HULL_SPECS`' mount bearings and counts, it
fails loudly rather than silently when it cannot find them, and the rule it enforces is stated in
`HullSpec.h` beside the table so the day the shape changes the reason to look is next to it. A
generated file would be a second source of truth for a table that is already the only one, which is
the thing ADR 0058 exists to refuse — and the parser's fragility is a red check rather than a wrong
answer.

## 4. How it must behave

1. A hull with no bound part draws in exactly one call, and no frame it appears in changes.
2. A bound part turns at its device's traverse rate, never faster, and rests when the mount has no
   remembered target.
3. Nothing here reaches the simulation: no order, no message, no field on any snapshot. Turn the
   slew off and the same shots land on the same ticks.
4. The `HULL` bar and the target bar read `hullFraction` off the record, and a target this client
   holds no record for draws an outline rather than a full bar — `Combat.md` §10.3's rule for pips,
   applied to the one bar that did not have it.
5. The art check fails when a marker and its mount disagree, and says which hull and which mount.

## 5. Acceptance

- **Screenshots at two window sizes** — a fight and a quiet frame — which are what accepts this
  slice *and* what pays the debt slice 4 left. `Combat.md` §16 records them as owed against slice 4;
  this order is where they come due.
- `NeuronClientTests`: the complement of a set of submesh ranges is the ranges the mesh does not
  cover, over an empty set, one range, adjacent ranges and the whole mesh.
- `GameLogicTests` unchanged and green, which is the claim that nothing simulated moved.
- `python Tools/NmoShippedArtTest.py` passes and fails on a planted disagreement.
- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`, clang-tidy over the changed
  sources.
- The decision record is in `Design/Decisions/` and indexed.
- **`Combat.md` moves to `Design/Archive/` with its six work orders in this commit**, every slice
  landed, and every citation to them is retargeted in the same commit.

## 6. Assumptions the implementer may make

- **Bones and clips stay skipped.** No shipped hull has a rig (§10.1); a turret turns about its
  bind-pose centre, and the day a rigged hull is authored is the day a slice poses it.
- **A turret and its barrels are separate submeshes** and are turned together by the binding, which
  is why the table binds a mount to a *set* of names rather than one.
- **The slew is not in the replay contract** and never enters it. If a later design wants a turret's
  angle to decide a shot, that is a simulation field and a format bump, and it is not this.
