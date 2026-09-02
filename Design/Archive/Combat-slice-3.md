# Work order — Combat slice 3: the rig the reader already proves

Implements slice 3 of [`Combat.md`](Combat.md) §16: `MeshData` stops discarding the structure
`NmoReader` has always validated, so that a later slice can move one part of a hull without moving
the rest (design §10.1).

**Status: landed 2026-09-01 and in review, and narrower than design §10.1 assumed.** §2.6 states
the correction the shipped art forced — submeshes and marker `parentBone` landed, bones and clips did
not — and why it makes the slice smaller rather than weaker. No decision record: nothing moved
between libraries and no rule changed. What it hands slice 6 is a hull whose parts can be addressed
by name, and what it hands slice 5 is the reason the `Gun` markers wait. Design §10.1 and §14 were
amended to match in the pass that followed (ADR 0054); §2.6 below is what they said before.

**Layer:** `NeuronClient` and `NeuronClientTests`.
**Depends on:** nothing. It reads a format that has not changed and content that is already
committed; slices 1 and 2 are beside it rather than under it.
**Blocks:** slice 6, which cannot turn a turret it cannot address, and whose `Gun` markers need a
submesh to belong to. It blocked slice 4 when the slew was still in that order; §2.7 there moved it.

---

## 1. Why this is a slice

`NmoReader` parses, bounds-checks and validates every submesh, bone table, clip, key series and
marker in a file — and then `Expand` flattens the lot into one triangle soup and throws the
structure away. Its own header says so: *"what it validates and then deliberately skips: normals,
UVs, emissive, skin buffers, bone tables and clips — nothing in this engine poses a bone yet."*

That was the right trade for as long as a hull was one rigid object. A turret that tracks its
target is the first thing in this game that needs one part of a hull to move while the rest holds
still, and the cheapest honest way to get there is to stop throwing away what the reader already
proved.

It is separated from slice 4 by layer, which is what lets the two run in parallel: this one changes
`NeuronClient` and touches nothing that draws.

---

## 2. Scope

### 2.1 `NeuronClient/MeshData.h` — submeshes

```cpp
struct MeshSubMesh
{
  std::uint32_t nameHash = 0;    // FNV-1a 32 of the submesh's name, hashed as markers are
  std::uint32_t firstVertex = 0; // into MeshData::verts
  std::uint32_t vertexCount = 0;
  std::uint32_t firstMarker = 0; // into MeshData::markers
  std::uint32_t markerCount = 0;
  DirectX::XMFLOAT3 boundsMin{0.0f, 0.0f, 0.0f}; // this part's own bind-pose bounds
  DirectX::XMFLOAT3 boundsMax{0.0f, 0.0f, 0.0f};
  [[nodiscard]] DirectX::XMFLOAT3 Pivot() const noexcept; // the centre of those bounds
};
```

`MeshData` gains `std::vector<MeshSubMesh> subMeshes`, in file order, and **nothing else changes for
an existing consumer**: `verts` is the same soup in the same order, so `SceneRenderer` and
`MeshShatter` go on drawing a whole hull without knowing this exists. That property is the point —
a slice that made every drawing path opt in would be a much larger slice than this one.

**Names are hashed, never stored**, which is `MeshMarker::nameHash`'s rule and inherits its
consequence: two submesh names that collide under FNV-1a are one part as far as any consumer is
concerned, so the load fails naming both, exactly as it already does for markers. Extend the
existing collision sweep rather than writing a second one.

**`Pivot()` is the centre of the part's own bind-pose bounds**, and it is what a turret turns about.
It is derived rather than authored because the format already carries `NmoSubMesh::extents` per
submesh and the shipped hulls populate it — `battleship_turret_0` is
`min=(-4.83, 11.33, 14.67) max=(4.83, 15.04, 24.33)`, whose centre is a pivot a gun would actually
be mounted on. A submesh whose file states no extents accumulates them from its own vertices, which
is what `Expand` already does for a mesh that states none.

### 2.2 `NeuronClient/MeshData.h` — one field on a marker

`MeshMarker` gains `std::int32_t parentBone = -1`, which the reader reads, validates and currently
drops on the floor in `Expand`. It costs four bytes and it is the difference between a muzzle that
rides a barrel and one that floats where the barrel used to be, the day a hull is authored with a
rig. Nothing in this slice reads it.

### 2.3 `NeuronClient/NmoReader.cpp` — `Expand` keeps what it counts

`Expand` already walks the submeshes in order and emits their vertices contiguously; it records
where each run started and how long it is instead of forgetting. The marker loop is already inside
the submesh loop, so a marker range is the same bookkeeping.

Nothing about validation changes. No clause moves, no rejection is added or removed, and a file that
loaded before loads identically.

### 2.4 `NeuronClient/NmoReader.h` — the header stops claiming otherwise

The class comment lists what the reader "validates and then deliberately skips". Submeshes and
marker bone bindings come off that list; bone tables, clips and skin buffers stay on it, with the
reason narrowed to the one that is still true (§2.6).

### 2.5 What this slice does not touch

- **Anything that draws.** No renderer, no `WorldView`, no pipeline. Slice 4.
- **Bone tables, clips and skin buffers.** They stay validated and skipped (§2.6).
- **The format, the fixture, and the shipped meshes.** No byte of content changes; `Tools/` is not
  touched. Authoring `Gun` markers is slice 5.
- **`GameLogic`.** The simulation still learns nothing about art (ADR 0002).

### 2.6 The correction: the shipped hulls have no rig at all

Design §10.1 **as it stood when this order was written** said this slice makes `MeshData` grow
"named submeshes, bones, clips, and `parentBone` on markers", and described the shipped Battleship
as carrying "three turret submeshes with barrel bones". **The bones are not there.** Read back from
the committed content:

| Hull | Submeshes | Mesh bones | Clips | Skin buffers |
|---|---|---|---|---|
| Battleship | 26 (incl. `battleship_turret_0..2`, `battleship_barrel_0..2` and their mirrors) | 0 | 0 | 0 |
| Corvette | 12 (incl. `corvette_turret`, `corvette_turret.001`) | 0 | 0 | 0 |
| Frigate | 17 | 0 | 0 | 0 |
| Interceptor | 9 (incl. `interceptor_railgun`) | 0 | 0 | 0 |
| Carrier | 24 | 0 | 0 | 0 |
| Miner | 16 (incl. `miner_drill`, `miner_drilltip`) | 0 | 0 | 0 |

Every hull in the game is a set of **named rigid submeshes and nothing else**. The only file in the
tree with a bone, a clip or a skin buffer is the golden fixture, which exists to prove the reader
against the format rather than to be flown.

So the design's plan and this order part company, and the order follows the content:

- **Submeshes and `parentBone` land**, because they are what the shipped art actually offers and
  what slice 4 can actually consume. A turret turns about its own bind-pose centre, which every
  shipped turret states.
- **Bones and clips do not land.** Nothing in the tree could pose one, no shipped hull has one, and
  a table carried in a struct that nothing reads is the field-before-its-consumer this tree keeps
  out — the same objection `Fleets.md` §4.1 drew, cited in slice 1's own order. The reader goes on
  validating them, so the day a rigged hull is authored the bytes are already proved and the slice
  that poses them picks them up with the argument for doing so in hand.

This is a scope correction rather than a disagreement with the design's shape: the design wanted a
turret that turns, and after this slice one can be addressed and slice 6 turns it. Design §10.1 and
§14 now say what landed, with this section the record of what they said before and why they changed
(ADR 0054).

What it costs is that a turret and its barrels are separate submeshes which slice 6 must turn
together, and the binding between a mount and its geometry is a client-side table rather than
something read out of a rig — which is where ADR 0002 would have put it anyway.

---

## 3. What to build on

- **`Expand`** — it already walks submeshes in order, emits their vertices contiguously and collects
  their markers. Every range this slice keeps is a number that function already has in hand.
- **The marker name collision sweep** in the same function — the precedent for hashed names, and the
  loop the submesh sweep joins rather than duplicates.
- **`MeshData::HalfExtents` / `BoundsCentre`** — the shape `MeshSubMesh::Pivot` takes, and the
  reason bounds are carried as a min/max pair rather than a centre and a size.
- **`NmoSubMesh::extents`** (`NmoFile.h`) — already read into `SubMeshView::record`, already
  populated by the shipped art, and never yet used.
- **`Tests/NeuronClientTests/Assets/NmoFixture.nmo`** — the golden file, whose `Gunship` mesh has a
  named `Hull` and `Turret` and whose second mesh is deliberately nameless.

---

## 4. Acceptance

**`Tests/NeuronClientTests/NmoReaderTests.cpp`** — extended.

| Test | Decides |
|---|---|
| `TheFixtureKeepsItsSubMeshes` | two submeshes, named `Hull` and `Turret`, in file order |
| `ASubMeshOwnsAContiguousRunOfVertices` | the ranges tile `verts` exactly, in order, with no gap and no overlap |
| `ASubMeshOwnsItsOwnMarkers` | the fixture's five hull markers and one turret marker land under the right parts |
| `ASubMeshCarriesItsOwnBounds` | the turret's bounds are the turret's, not the hull's, and its pivot sits inside them |
| `AMarkerRemembersItsBone` | the fixture's bone-parented `Muzzle` keeps its index; an unparented marker reads -1 |
| `CollidingSubMeshNamesAreRejected` | two names that hash alike fail the load naming both, as two marker names already do |

Two rows in that table did not land as written. `AnUnnamedSubMeshHashesToZero` is covered by the
shipped-art check asserting the opposite property over real content — every shipped part *is* named
— and the fixture has no unnamed submesh to point at; `CollidingSubMeshNamesAreRejected` is enforced
in the reader and asserted by the Python check over the art, rather than by corrupting the fixture,
which is the one thing this suite does that a name collision cannot be built into cheaply.

**A content check, because the shipped art is what slice 4 will actually run against** — and it is
**`Tools/NmoShippedArtTest.py`**, not a row in this suite. `NeuronClientTests` deploys the golden
fixture and nothing else, so a C++ row over the game's meshes would skip silently and pass for ever,
which is worse than no row at all; and giving the engine's suite the game's art would point a
dependency the way AGENTS.md §2 spends most of its rules keeping shut. The check lives beside the
other stdlib-only codec tests, is run as `NmoRoundtripTest.py` is, and asserts over every shipped
hull: every part named, every part stating bind-pose extents, no two names colliding under FNV-1a,
and the triangles summing to the mesh. It is the check that would have caught §2.6 before the design
was written.

**The existing suites** — every row passes unedited. `MeshShatterTests` in particular, which reads
`verts` and must not notice that anything happened.

**The tree**

- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`, clang-tidy clean.
- Debug|x64 builds and all four suites run, with the debug STL's bounds checking on.
- No screenshot: nothing visual until slice 4.
- No decision record is due. Nothing moves between libraries, no dependency rule changes, and
  §2.6's narrowing is a scope call recorded here rather than an architecture one — but if slice 4
  finds that a client-side mount-to-submesh table wants arguing, that is where the record belongs.
- `Combat.md` §16 marks slice 3 *in review*, and §10.1's claim about barrel bones is corrected in
  the same commit.

---

## 5. Assumptions the implementer may make

- **Nothing reads a submesh yet.** `NeuronClientTests` is the only consumer until slice 4.
- **A hull is still one draw call.** This slice adds an index over `verts`, not a second way to
  draw it, and no existing consumer changes a line.
- **Bounds may be absent per submesh** exactly as they may be absent per mesh; the accumulate path
  already exists and is reused rather than re-argued.
- **`parentBone` is carried and unread**, which is the one field here whose consumer does not exist
  yet. It is four bytes on a struct that is already loaded once per hull, and the alternative is
  reading the format again the day a rig arrives.
