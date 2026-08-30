# Work order — NMO slice 2: the engine reader

Implements slice 2 of [`NmoFormat.md`](NmoFormat.md) §14: the §5 structs in C++, a reader that
validates per §5.12 and expands a mesh into `MeshData`, markers resolved and hashed at load, and a
malformed-file suite over the same bytes the Python codec is tested against. **No caller changes**
— `MeshLibrary` still loads OBJ when this lands, and nothing on screen moves.

**Layer:** `NeuronClient` and `Tests/NeuronClientTests`, plus the one `Tools/` change §2.0 states.
**Depends on:** slice 1 (the codec, the add-on, `Tools/NmoFixture.py`), landed.
**Blocks:** slice 3 (the loader switch) and through it slice 4.

---

## 1. Why this is a slice

The reader is the whole risk in this format and none of the visible reward. It parses
attacker-shaped data — every count and offset in the file is arithmetic that ends in a pointer —
and it is the one piece where a defect is silent: a mesh that loads and draws can still have read
four bytes past a buffer. So it lands on its own, with a test suite that corrupts one field per
validation clause and demands a rejection, before any caller depends on it and while the OBJ path
still works. When slice 3 flips `MeshLibrary` over, the only question left is content.

---

## 2. Scope

### 2.0 `Tools/` — the codec learns the two bits, and the fixture carries them

The design added `NmoRenderFlags::RaceTinted` (0x8) and `NmoMarkerFlags::RaceTinted` (0x1) after
slice 1 shipped, and the codec — "where prose and codec disagree, the codec has a bug" — does not
name either. Before the C++ reader is written against them:

- `Tools/BlenderNmo/NmoFormat.py` gains `RENDER_FLAG_RACE_TINTED = 0x8` beside the three render
  flags it has, and `MARKER_FLAG_RACE_TINTED = 0x1`.
- `Tools/NmoFixture.py` sets `RENDER_FLAG_RACE_TINTED` on one of the Gunship's two materials and
  `MARKER_FLAG_RACE_TINTED` on one `Exhaust` marker, leaving one of each unflagged, so both values
  of both bits are in the golden bytes.
- `Tools/NmoRoundtripTest.py` asserts both bits survive the round trip; its check count grows and
  the new number replaces 64 in §4.

The committed fixture (§2.5) is generated *after* this, so the C++ positive tests can assert the
bits arrive. No other `Tools/` file changes; the add-on already round-trips the flag words as the
`nmo_render_flags` / `nmo_flags` custom properties.

### 2.1 `NeuronClient/NmoFile.h` — the format, as structs

The C++ blocks of [`NmoFormat.md`](NmoFormat.md) §5.1–§5.10, transcribed verbatim and in order:
`NMO_FILE_MAGIC` through `NMO_NO_BONE`, `NmoFileHeader`, `NmoMeshRef`, `NmoMeshHeader`,
`NmoRenderFlags`, `NmoMaterial`, `NmoBufferHeader`, `NmoIndexFormat`/`NmoVertexFormat`/
`NmoSkinFormat`, `NmoVertex`, `NmoSkinVertex`, `NmoMeshExtents`, `NmoSubMesh`, `NmoBone`,
`NmoClip`, `NmoSrtTrack`, the three key structs, `NmoMarkerFlags`, `NmoMarker`.

**Every `static_assert` in §5 is copied with it.** They are the specification, not a nicety: the
struct sizes are what make "read the header, then index the array" legal at all, and a silent
padding change from a future compiler switch is exactly the defect they exist to catch.

Header only, no `.cpp`, no functions — a mirror of the disk layout and nothing else. `#include
<DirectXMath.h>` and `<cstdint>`; nothing from this project. It must remain the file you can read
beside §5 and diff by eye.

### 2.2 `NeuronClient/MeshData.h` — markers reach the client

`MeshData` grows the marker list §10 calls for, and keeps everything it already has:

```cpp
enum class MarkerKind : std::uint32_t
{
  Point,      // an empty kind string: a plain point
  Exhaust,
  NavLight,
  Gun,
  Unknown,    // a kind this build does not know; kept, never dropped (NmoFormat.md 5.10)
};

struct MeshMarker
{
  MarkerKind kind = MarkerKind::Point;
  std::uint32_t nameHash = 0;                             // FNV-1a 32 of the marker's name
  DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};           // mesh space, bind pose
  DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};  // direction is local +Z
  float scale = 1.0f;                                     // nozzle radius, light size, metres
  DirectX::XMFLOAT4 colour{1.0f, 1.0f, 1.0f, 1.0f};       // linear RGBA
  float param0 = 0.0f;
  float param1 = 0.0f;
  // colour is a shade and the faction supplies the hue (NmoFormat.md 5.10). Carried as a bool
  // rather than the file's flag word because one bit is defined and a consumer asking "is this
  // liveried" should not be asking it to bitmask arithmetic.
  bool raceTinted = false;
};
```

and on `MeshData`: `std::vector<MeshMarker> markers;`.

Names are hashed, never stored — §5.10's lookup practice, and the reason nothing here holds a
`std::string`. **A hash collision within one file is a load failure** naming both markers, so the
tool renames rather than the engine guessing; that is a validation clause like any other (§2.3).

`attachPoints` **stays**, and the reader fills it with the position of every `Exhaust` marker in
file order. It is the bridge that lets slice 3 switch the loader without touching `WorldView` at
all: the thruster glow keeps working, unchanged, from data that is now authored instead of
clustered. Slice 4 replaces its one consumer with the marker list and deletes the field. Say so at
the declaration, so nobody mistakes it for a permanent second spelling of the same thing.

`BoundsCentre`, `HalfExtents`, `RestY` and `Empty` are untouched. **`MeshVertex` gains one field**,
and it is the only vertex-format change this design makes:

```cpp
struct MeshVertex
{
  float px, py, pz;
  float r, g, b;
  // 0 for a surface the model paints, 1 for one the faction paints (NmoFormat.md 5.5). It is a
  // float and not a bit because it is a vertex attribute the input assembler has to hand the
  // shader, and it is per vertex and not per draw because both kinds of surface are on one hull
  // and this renderer draws a hull in one call.
  //
  // The initialiser is load-bearing: it is what lets every existing MeshVertex{x,y,z,r,g,b} in the
  // tree go on compiling and mean "the model's own paint", which is the right answer for all of
  // them -- the ground quad, the decals, and every vertex ObjParser will emit until slice 3
  // deletes it.
  float race = 0.0f;
};
```

The value is constant across a triangle — a triangle belongs to one submesh, a submesh to one
material — so nothing interpolates across a material seam and the pixel stage sees exactly 0 or 1.

Growing the format reaches three places, all in this layer, and none of them changes what is on
screen this slice: **both** per-vertex layouts in `SceneRenderer.cpp` — `SCENE_ELEMENTS` and
`SCENE_INSTANCED_ELEMENTS` — gain the element (`"RACE", 0, DXGI_FORMAT_R32_FLOAT, 0, 24`), the
vertex stride the buffer view carries becomes `sizeof(MeshVertex)` if it is not already, and
`Scene.hlsli`'s `VsIn`/`VsOut` gain `float race` with both vertex shaders passing it through. The
decal pipelines share `SCENE_ELEMENTS` with a `Decal.hlsli` `VsIn` that never declares `RACE`; that
is legal — an input element no shader consumes is ignored — and a one-line comment at the layout
says so, so nobody adds it to `Decal.hlsli` to "fix" a warning that will not come.

`ScenePS` **does not read it yet** — that is slice 5, and until then this is a channel that is
carried and ignored, which is exactly what makes it safe to land here. The unit quad and
`ObjParser` need no edit at all, which is the initialiser earning its place.

`MeshShatter` reads `MeshVertex::r/g/b` and is unaffected: it copies colours it does not
interpret. Slice 5 is where a shard learns whose paint it was wearing.

### 2.3 `NeuronClient/NmoReader.h/.cpp` — validate, then expand

```cpp
class NmoReader
{
public:
  // _dir is relative to the asset root unless it carries a drive or a root; FileSys resolves it.
  // _name carries no extension: ".nmo" is appended.
  [[nodiscard]] static bool Load(const std::wstring& _dir, const std::wstring& _name, MeshData& _outMesh);
};
```

The signature is `ObjParser::Load`'s, field for field, so slice 3's change at the call site is one
identifier. One `BinaryFile::ReadFile`, then:

**Validation, §5.12 clauses 1–10 in that order, before any data is used.** Reject on the first
failure: `DebugTrace` the clause number and what was wrong, return `false`, leave `_outMesh`
untouched. Never repair, never `ASSERT`, never throw — this is content, and content errors are
diagnostics (AGENTS.md §5). The recommended sanity caps are checked too, before any allocation
sized from the file.

**All offset arithmetic in 64 bits**, on `std::uint64_t`, before it is narrowed to index anything.
Clause 2 says so for the mesh directory and it holds everywhere: `offset + count * stride` in
32-bit wraps, and a wrapped range passes a naive bounds check. One small helper that takes
(offset, count, stride) and returns whether the window lies inside the blob, used by every section,
is the shape to write — it is the rule stated once instead of thirty times.

**Expansion into `MeshData`** (§10): mesh 0 is the hull. A file carrying more than one mesh traces
and uses the first; that is a diagnostic, not a rejection, because the format allows it and this
engine has no consumer for a second one.

Per submesh, per triangle, per corner: read the index at `startIndex + i` in its index buffer,
widen `U16` or copy `U32`, add `baseVertex`, and emit a `MeshVertex` from that `NmoVertex`'s
position and its colour, **modulated by the submesh material's `baseColour`**:

> `MeshVertex.rgb = NmoVertex.colour.rgb / 255 × material.baseColour.rgb`
>
> `MeshVertex.race = (material.renderFlags & NmoRenderFlags::RaceTinted) ? 1.0f : 0.0f`

The second line is the whole of this slice's livery work, and it is one line because the format
put the answer where the answer belongs. A submesh has exactly one material, so the flag is read
once per submesh and written to every vertex the submesh emits — no name matching, no lookup, no
heuristic. On a flagged material `baseColour` is a shade rather than a colour
([NmoFormat.md](NmoFormat.md) §5.5) and the multiply above still applies unchanged: the shade
lands in `rgb`, the livery multiplies it in the pixel shader in slice 5, and a build that never
gets slice 5 draws the shade as a grey ship, which is a legible wrong rather than a black one.

That rule is not cosmetic and it is worth the comment it needs. The two conversion paths in
`Tools/` fill these two fields differently: the Blender export writes white vertices and puts the
authored colour in the material (that is what a mesh with no colour attribute produces, which is
every hull authored in a modelling package), while `ObjToNmo.py` bakes `Kd` into both. Under the
rule above the first is exactly right and the second comes out squared. That is the correct
trade: the shipping corpus is the first kind (slice 3), and `ObjToNmo.py` retires with the OBJ
hulls in slice 4 rather than being taught a third convention. The comment at the multiply says
this, so the next person to convert an OBJ does not spend an afternoon on a dark hull.

Normals and UVs are read and validated, then dropped: `MeshVertex` has neither, and the day it
grows them the file already carries the data (design §1).

Extents come from the file (`NmoMeshExtents`) rather than being recomputed — the writer computed
them over the same vertices and §5.12 clause 3 has already proved the section is there. `boundsMin`
/`boundsMax` take `boxMin`/`boxMax`.

Markers: for every submesh, in submesh then file order, resolve the kind string to `MarkerKind`
once, hash the name once, resolve `flags & NmoMarkerFlags::RaceTinted` into `MeshMarker::raceTinted`
once, and append. Bone-bound markers (`parentBone >= 0`) are read and kept with their position as
the file states it — this engine has no pose evaluation until slice 6, and a marker on a bone at
bind pose is where the bind pose puts it.

Skin buffers, bone tables and clips are **validated and skipped**. Nothing in this engine consumes
them yet, and a file that carries a rig this reader silently ignored is better than one it refused;
what it must never do is fail to check them, because clause 8's overlapping-scope rule is the one
that keeps a later slice's skinning honest.

The load trace matches `ObjParser`'s in shape, so the boot log reads the same: name, triangles,
dimensions, marker count by kind.

### 2.4 `NeuronClient/NeuronClient.h` — the umbrella

`NmoFile.h` and `NmoReader.h` join the content-reader block beside `DdsImage.h` and `ObjParser.h`,
in the file's existing order.

### 2.5 `Tests/NeuronClientTests/NmoReaderTests.cpp` — the malformed-file suite

The C++ half of §12: the same fixture bytes the Python round-trip test uses, one deliberately
corrupted copy per §5.12 clause, each asserting `Load` returns `false` and leaves the `MeshData`
empty.

**The fixture.** `Tools/NmoFixture.py` writes it; the bytes are committed as
`Tests/NeuronClientTests/Assets/NmoFixture.nmo` and registered in the test `.vcxproj` and
`.filters` with `<CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>`, so it lands in
`x64\Debug\Assets\`. The suite calls `FileSys::SetHomeDirectory(L".")` once — which appends
`\Assets\`, the same shape the app uses — and loads `L""`, `L"NmoFixture"`. This is §15 D3's
sanctioned exception to "nothing generated is committed", and the test file says so at the top,
naming its generator. **Regenerating the fixture and re-comparing is part of this slice's
acceptance**, so the committed bytes are proven to be what the generator makes.

No test in this suite has ever read a file from disk, so **prove the working directory before
building the rest of the suite**: one test that loads the untouched fixture and asserts it
succeeds. If `vstest` runs the suite from somewhere else, that test says so in one line instead of
twenty tests failing for a reason none of them names.

Corrupting is done by locating each field through the header rather than by a hard-coded offset —
`fileBytes` at 12, `meshCount` at 20, and so on, read from the loaded buffer — so the tests keep
testing the rule they name when the fixture changes.

One clause, one test, named for it. At minimum: bad magic; major version 1; `headerBytes` not 32;
`fileBytes` disagreeing with the file; a mesh ref reaching past the end; a mesh ref that is not
16-byte aligned; an offset + size that wraps in 32 bits; a string longer than
`NMO_MAX_STRING_BYTES`; invalid UTF-8 in a name; an unknown buffer format; a stride that
contradicts its format; `skinBufferCount` neither 0 nor `vertexBufferCount`; a submesh
`materialIndex` out of range; an index range running past its IB; a biased index outside
`[minVertex, minVertex + vertexCount)`; a facet section whose length is not `primitiveCount`; a
bone whose `parentIndex >= ` its own index; duplicate bone names in one table; a clip with
`endSeconds < startSeconds`; track `boneIndex` values not strictly increasing; keys not strictly
increasing in time; a skin `boneIndex` outside its scope; weights that do not sum to 1; a marker
`parentBone` outside its scope; duplicate marker names in one submesh; and two marker names that
collide under FNV-1a. **That last one is not a patch of the fixture**: a colliding pair has to be
found by search and will not be the length of any name the fixture holds, so the test writes a
minimal one-submesh file of its own with the two names and asserts the reader refuses it. It is
the one test in the suite allowed to build bytes rather than corrupt them.

Plus the positive tests: the fixture loads; the triangle count matches the fixture's; the bounds
match its extents; every marker arrives with its kind, colour, scale and params; the flagged
marker arrives `raceTinted` and the unflagged one does not; every vertex of the flagged material's
submesh carries `race == 1.0f` and every vertex of the other's `race == 0.0f`; `attachPoints`
holds one entry per `Exhaust` marker, in file order; and a file with a truncated tail — the
cheapest real-world corruption there is — is rejected rather than read.

### 2.6 Project files

`NmoFile.h`, `NmoReader.h`, `NmoReader.cpp` in `NeuronClient.vcxproj` and `.filters`;
`NmoReaderTests.cpp` and `Assets\NmoFixture.nmo` in the test project and its filters.
`python Build/CheckProjectFiles.py` passes, which is what proves it.

### 2.7 What this slice deliberately does **not** do

- **No caller.** `MeshLibrary` is not touched. `NmoReader` compiles, is tested, and has no user;
  that is the point of landing it alone.
- **No asset moves.** Nothing appears in `Outpost/Assets/Meshes/`. The `.nmo` files in `Art/Meshes/`
  stay where they are.
- **No `ObjParser` change**, and no deletion. That is slice 3.
- **No pose evaluation, no skinning, no indexed drawing.** Bones and clips are validated and
  skipped; `SceneRenderer` keeps taking triangle soup.
- **No emissive.** `NmoMaterial::emissiveColour` is read and validated, and no consumer sees it.
  The hulls carry real emissive values (the thruster material's is `(0.27, 0.807, 0.01)` at
  strength 1.3 on the Corvette); a pass that uses them is its own slice against its own design.
  Note that those values are *green*, which the livery makes wrong the same way the base colours
  were wrong — whichever slice lights them re-authors them as shades first.
- **No livery, though the flag is read.** `RaceTinted` reaches `MeshVertex::race` and
  `MeshMarker::raceTinted` and stops there: no shader multiplies by anything, no faction supplies a
  colour, and the hulls draw in the greyscale shades the corpus authors. `DoubleSided`,
  `AlphaBlend` and `Additive` reach nothing at all. Slice 5 is the visible half.
- **No `MeshShatter`, `WorldView` or `ShipExplosion` edit.** They hold `MeshData`, and the field
  this slice adds to `MeshVertex` is one `MeshShatter` copies without interpreting.

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| [`Design/NmoFormat.md`](NmoFormat.md) §5 | The normative spec. §5.12 is the validation list, in order, and its clause numbers are the test names |
| `Tools/BlenderNmo/NmoFormat.py` | The executable statement of §5 — where prose and codec disagree, the codec has the bug (§5). Its `read`/`_validate_model` are the reader you are porting |
| `Tools/NmoFixture.py` | The golden model, and the generator the committed fixture must still match |
| `Tools/NmoRoundtripTest.py` | The Python suite's malformed-file list — the C++ suite is its mirror over the same bytes |
| `NeuronClient/ObjParser.h/.cpp` | The `Load` signature to match, the `DebugTrace` diagnostic shape, and the "content errors fail closed" posture |
| `NeuronClient/DdsImage.h/.cpp` | This tree's other binary content reader: how it validates a header it does not trust, and reports rather than throws |
| `NeuronCore/FileSys.h` | `BinaryFile::ReadFile`, `FileSys::ResolvePath`, `SetHomeDirectory` |
| `NeuronClient/MeshData.h` | The soup every downstream consumer holds, and must keep holding |
| `Art/Meshes/*.nmo` | Eleven real files, with markers, to point the reader at while developing |

---

## 4. Acceptance

Decided by tests, because everything here is (Design/README.md):

- `NeuronClientTests` green, with the new suite: one test per §5.12 clause listed in §2.5, each
  asserting rejection; the positive tests; and the working-directory test.
- **The committed fixture is regenerated and compared** in the pull request: re-run
  `Tools/NmoFixture.py`, `fc /b` against the committed bytes, and state that they are identical.
- `python Tools/NmoRoundtripTest.py` passes with its new check count, and the pull request states
  the number — the fixture serves both suites and §2.0 is the only change it takes.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds; all four suites run and are green; say which you ran.
- `git diff --stat` shows no path under `Outpost/`, `GameLogic/`, `NeuronCore/` or `NeuronServer/`;
  the pull request says so.
- A code read, stated in the report: every file-derived length is computed in 64 bits before it is
  used to index or size anything, and no path allocates from a count that has not been capped.
- **The game still looks exactly as it did.** `MeshVertex` grew and the input layout grew with it,
  so the check is a screenshot beside one from before the slice: same hulls, same colours, same
  ground. A wrong stride shows up as geometry exploding, and a shader that reads the new channel
  by accident shows up as a black ship; neither is subtle, and both are this slice's bug.
- No decision record is due — the format, its home and its tooling were all decided by
  [ADR 0011](../Decisions/0011-ship-meshes-are-nmo-and-its-tools-are-python.md), and the record that
  owes for liveries is slice 5's, where the rule becomes visible. Say so.
- `Design/NmoFormat.md` §14 marks slice 2 landed; this file moves to `Design/Archive/`.

---

## 5. Assumptions the implementer may make

- **One mesh per file.** Every shipped hull holds exactly one. The reader handles `meshCount > 1`
  by tracing and using mesh 0; no test needs to cover a two-mesh file beyond the fixture, which
  already is one.
- **Markers are rigid.** No hull carries a bone, so no marker is bone-bound in practice. The
  reader still validates `parentBone` and keeps it, and the fixture covers the bound case.
- **`payloadCrc32` is checked in Debug and skipped in Release**, per §5.3. A file with `0` there is
  legal and is not checked at all.
- **The trace format is yours**, as long as it names the file, the triangle count and the failing
  clause on rejection. It is read by a person at boot, not parsed.
- **`attachPoints` is temporary and is allowed to look it.** It is written by this slice and
  deleted by slice 4; a comment saying so is the whole of its documentation.
- **The suite may be slow to write and dull to read.** Twenty-five near-identical corruption tests
  is the correct shape for this file — the alternative is one clever data-driven test that fails
  without naming the clause, which is worse in the only moment it matters.
