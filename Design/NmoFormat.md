# NMO — the ship mesh format, and the Blender add-on that edits it

**Status: proposed; slice 1 (format spec, codec, Blender add-on, converter, tests) accompanies
this document. No engine code changes yet.** §14 lists the slices; the five decisions that shaped
v2.0's defaults were put to the owner and taken on 2026-08-29 (§15).

This document adapts the **Neuron Mesh Object (NMO)** format proposal from the Interstellar
Outpost tree (`InterstellarOutpost.dx12`, *"Neuron Mesh Object (NMO): Format Design"*, revision
2026-08-27) into this repository, under this repository's rules — the same porting posture as
[SpaceshipExplosion.md](Archive/SpaceshipExplosion.md), which brought that tree's destruction effect
across. The source document designed a binary model format derived from Microsoft's CMO, extended
with per-submesh bone animation and named markers, for a D3D9 engine migrating a 516-model `.pie`
corpus. This tree needs the same *kind* of format for a different reason: its ships are OBJ
triangle soup with no names, no animation, and attachment points recovered by a clustering
heuristic. What transfers is the container discipline and the two extensions; what does not
transfer is everything the `.pie` corpus and D3D9 forced. §3 is the delta, §5 is the normative
result.

The three concrete asks this design answers:

- **R-NMO-A** — Bone animation (named bones, named clips of keyframes), per mesh and per submesh,
  carried by the file and by the Blender add-on.
- **R-NMO-B** — **Typed markers**: engine exhausts (position **and colour**), navigation lights
  (position **and colour**), gun mounts (position) — extensible to marker kinds that do not exist
  yet without changing the format.
- **R-NMO-C** — A Blender add-on that loads and saves the format, covering meshes, materials,
  skeletons, clips and markers, so `.nmo` is an editable format rather than a compiler output.

**Non-goals of v2.0:** scene graphs, cameras, lights, LOD policy, compression, animation curves
(tools bake to keys), model composition, and any runtime blend/priority policy — the file defines
*data*; how clips mix at runtime is an engine decision (unchanged from the source document, and
for its reasons: a mesh format that encodes game behaviour rots the first time the behaviour
changes).

---

## 1. What the tree has today, and why it forces this

Constraints first, per this tree's porting convention. Each shaped the format below.

| Constraint | Where it comes from | What it does to this design |
|---|---|---|
| One vertex format for the whole scene pass: `MeshVertex{px,py,pz,r,g,b}` | [`MeshData.h:13`](../NeuronClient/MeshData.h) | The engine consumes position + colour today. The file still stores normals and UVs (§5.6) — they are authored data Blender holds and dropping them is lossy — but nothing in the engine has to change to adopt the format. |
| Flat shading from screen-space derivatives, winding-immune | [`ScenePS.hlsl`](../NeuronClient/Shaders/ScenePS.hlsl), `GpuHelpers.cpp:83` (`CULL_MODE_NONE`) | Vertex normals are carried, not required. Winding is specified (§5.2) so a culling renderer can arrive later without touching content. |
| Non-indexed draws; `GpuMesh` is one VB and a count | [`RenderTypes.h:32`](../NeuronClient/RenderTypes.h), `SceneRenderer.cpp:171` | The file is indexed (§3); the *loader* expands to triangle soup until an indexed pipeline is worth its slice (§14). Format ≠ renderer capability. |
| Exhaust positions are recovered by union-find clustering of faces carrying the `thruster` material | [`ObjParser.cpp:118-136`](../NeuronClient/ObjParser.cpp), `ObjParser.h:19` | The heuristic runs in the shipping loader on every boot and yields anonymous points. Markers make it *authored* data: named, typed, coloured, oriented — the heuristic's one legitimate home is a converter that runs once (§13). |
| The exhaust glow colour is a placeholder — every ship flies on `SELECTED_COLOUR` | `WorldView.cpp:728` | The Exhaust marker's colour field is the value that replaces it, per model, per nozzle (§9). |
| The explosion effect shatters `MeshData`'s retained triangle soup | [SpaceshipExplosion.md](Archive/SpaceshipExplosion.md) §2 | The NMO loader must keep producing `MeshData`-shaped soup; the shatter, picking and bounds consumers never learn the format changed (§13). |
| Simulation sizes are authored numbers, never derived from meshes | AGENTS.md §2, [ADR 0002](Decisions/0002-content-readers-live-with-their-consumer.md), [`HullSpec.h`](../GameLogic/HullSpec.h) | Markers are presentation data, read by the client. If combat simulation ever needs gun positions, they arrive in GameLogic as authored numbers — generated *offline* from `.nmo` by a tool at most, never read from the mesh at runtime (§9). |
| Content errors are diagnostics, never crashes | AGENTS.md §5, `ObjParser.h:9` | The loader validates and rejects with a reason; it never repairs, asserts or throws on content (§5.12). |
| Left-handed `(east, up, north)`, `LH` everywhere; hulls' bow lands on +Z | AGENTS.md §5, `ObjParser.h:13` | The file is stored in render space directly (§5.2). The Blender add-on owns the axis conversion, in one self-inverse function (§11). |
| Metres, and `constexpr` tuning | AGENTS.md, `ViewTuning.h` | Spatial units are metres 1:1 (a Corvette is ~17 × 22 m and Blender's default unit is the metre). Time is seconds. |
| No third-party libraries without approval; nothing generated is committed | AGENTS.md §5, §1 | The tooling is stdlib-only Python plus Blender itself as the host application. Test fixtures are built by committed scripts at test time, not committed as binaries (§12). |

Why a binary format at all, restated for this tree: OBJ carries no names, no hierarchy, no
animation, no marker semantics, and its text parse re-derives (clustering, bounds) what a tool
already knew. The source document's container-level analysis — identify, version, validate before
trusting a byte; fixed-width little-endian fields; natural alignment; offsets, not sequential
parsing — survives contact with this tree unchanged, and §5 keeps it.

## 2. What is inherited from the source proposal, unchanged

- **Container rules** (§5.1): little-endian, fixed-width, naturally aligned structs with
  `static_assert`-checked sizes; presence is a count, never a flag byte; variable-size data
  reached through offsets, `0` = absent; bulk data fixed-stride; strings length-prefixed UTF-8
  padded to 4; reserved fields are zero on write, ignored on read — that one rule is the whole
  minor-version mechanism; validation before trust, rejection over repair.
- **The two-stream vertex layout**: skinning data in a separate parallel buffer, so rigid meshes
  never pay for it.
- **Per-submesh skeletons with bone aliasing** (§7): the four configurations, mesh-space
  evaluation, palettes as pure-alias tables, and the override rules — the semantics that let a
  turret traverse while the hull idles, without welding the two rigs together.
- **SRT keyframes** (§8): translation/rotation/scale tracks, sorted, clamped, nlerp'd — chosen
  there because matrix keys shear under interpolation and cost 4×; both reasons hold anywhere.
- **Markers ride bones** via the same transform skinning uses — one rule, no second convention.
- **Name lookup practice**: never string-compare per frame; hash at load; no hashes in the file
  (derivable data in a file is a consistency liability).
- **Physical order** (§5.11): metadata first, bulk buffers behind it, offsets authoritative.
- **The versioning policy** (§5.13) and the validation-list discipline, each clause backed by a
  deliberately malformed test file.

## 3. What is changed, and why — the delta table

The source format carries three loads this tree does not: byte-compatibility with CMO (its
requirement said "as defined by CMO"), a 516-model `.pie` corpus with texture-atlas team colour
and terrain-conforming base plates, and D3D9's shader-constant and index-width ceilings. None
binds here, and each removal is listed rather than silent:

| Source NMO v1.0 | This tree, NMO v2.0 | Why |
|---|---|---|
| Magic `'NMO1'`, version 1.x | Magic `'NMO2'`, version 2.0 | The two dialects share a lineage and an extension but not a layout. A 1.x file must fail in one comparison, not half-parse into a confusing material error. Version 1.x is reserved for the Interstellar Outpost dialect; if the two trees ever decide to share one format, the version field is the arbiter (§15 D1). |
| `Vertex` = CMO's 52 B: position, normal, **tangent**, colour, UV | `NmoVertex` = 36 B: position, normal, colour, UV (§5.6) | The tangent was CMO fidelity. It is *derived* data (from normal + UV) that no artist authors and this renderer will not consume for years; carrying it would be 16 bytes of zeros per vertex. Re-adding it is a new `VertexFormat` value behind a major bump — accepted. |
| `Material` = CMO Phong (132 B) + pixel-shader name + 8 texture names + `MaterialExt` atlas descriptor | `NmoMaterial` = 48 B: base colour, emissive colour, render flags, reserved; 4 texture name slots (§5.5) | The Phong block and shader names were CMO fidelity; the atlas descriptor existed for `.pie` team colour and 8-frame texture animation, which have no counterpart here. Base + emissive matches both this game's flat look (`Kd` is all the MTLs carry that the engine reads) and Blender's Principled mapping. Reserved fields take future scalars behind a minor bump. |
| `MatrixKeys` clip encoding kept beside SRT | SRT tracks only (§5.9) | `MatrixKeys` existed because that requirement named CMO literally, and its own open question 3 doubted it would survive. No CMO content will ever reach this tree; one encoding halves the codec, the loader and the test matrix. |
| `Bone` = CMO's 196 B: `invBindPose`, `bindPose`, `localTransform` | `NmoBone` = 136 B: parent, alias, `localTransform`, `invBindPose` (§5.8) | `bindPose` is always the walk of `localTransform` up the hierarchy — the source document says so itself (§5.4) and kept it only for CMO symmetry. `invBindPose` stays stored: it anchors skinning and must not drift by a load-time inverse; `bindPose` is derived at load. |
| `SubMeshFlags::DeformedAtRuntime` | No bits defined; `flags` reserved | Terrain conforming (`flattenImd`) is the other game. The field stays so a future bit is a minor version. |
| Marker = name + position, orientation, scale, bone, flags | Marker record = name + **kind string** + position, orientation, scale, bone, **colour**, **two parameters**, flags (§5.10) | **R-NMO-B.** The source gave markers names but overloaded them with meaning (`Muzzle0`, `Exhaust0` — semantics by prefix). A kind string separates *what it is* from *what it is called*, is open-ended for future kinds without a format change, and the colour/parameter block is what exhausts and navigation lights actually need. |
| Facet ids optional per submesh | Kept, unchanged | Different justification, same field: there the ids preserved a shipped quad corpus; here they make Blender round trips honest — an artist's quads survive export → import (§11). |
| `U16`-preferred / `U32` per-buffer indices | Kept | The ceiling argument was D3D9's; the bandwidth argument is universal. Largest hull today: 1,784 faces — `U16` everywhere in practice. |
| Vertex colour swizzled for D3D9 `D3DCOLOR` at upload | Bytes R,G,B,A ascending, used as-is | The back buffer is `DXGI_FORMAT_R8G8B8A8_UNORM` (`RenderTypes.h:16`); byte order matches, no swizzle exists to document. |
| 80-bone palette split for vs_2_0 constant registers | Aliasing kept; the ceiling rationale retired | D3D12 has no 256-register palette limit. Aliasing earns its place here on the other two grounds: independent articulated parts (a turret is the spaceship case, not the edge case) and small contiguous palettes for the CPU skinner this engine would start with (§7). |
| Sanity caps, CRC-32, `MaxStringBytes`, 4 GB `uint32` offsets | Kept as-is | Nothing about this tree changes the arithmetic. |

Everything else — file header, mesh directory, mesh header shape, buffer records, submesh record
shape, extents, string framing, ordering rules — carries over with only the field-level changes
above.

## 4. Where each piece will live

```
Design/NmoFormat.md                  this document — the format's single normative statement
Tools/BlenderNmo/                    the Blender add-on (also a plain Python package)
├── __init__.py                      registration, operators, menu entries (bl_info + manifest)
├── blender_manifest.toml            Blender 4.2+ extension manifest
├── NmoFormat.py                     the reference codec: reader, writer, validation (§5 executable)
├── NmoScene.py                      the Blender <-> NMO mapping, stated once (§11)
├── NmoImport.py                     .nmo -> scene
└── NmoExport.py                     scene -> .nmo
Tools/ObjToNmo.py                    converts the shipped OBJ/MTL hulls, ObjParser's conventions ported (§13)
Tools/NmoFixture.py                  builds the golden model every test reads (not committed as bytes)
Tools/NmoRoundtripTest.py            codec: byte-exact round trip + one malformed file per §5.12 clause
Tools/NmoBlenderTest.py              headless Blender: fixture and a converted hull, import -> export -> compare

NeuronClient/NmoFile.h               slice 2: the §5 structs in C++, sizes static_assert-ed
NeuronClient/NmoReader.h/.cpp        slice 2: validate per §5.12, expand into MeshData
Outpost/Assets/Meshes/*.nmo          slice 4: the hulls, re-exported with authored markers
```

`Tools/` is a new top-level directory: these are content tools, not build checks, so they do not
belong in `Build/`, and they are not engine code, so not in a project. Python files take
PascalCase names per R7's spirit, as `Build/*.py` already does; `__init__.py` and
`blender_manifest.toml` keep the names their tools require, like `pch.h` does. The codec is
stdlib-only and importable without Blender — it is the executable statement of §5, and the tests
run it with a bare `python3`.

The engine reader lands in NeuronClient, next to `ObjParser` and `DdsImage`, because a mesh's only
consumer is the renderer ([ADR 0002](Decisions/0002-content-readers-live-with-their-consumer.md));
nothing in NeuronCore, GameLogic or NeuronServer learns the format exists.

---

## 5. Normative specification — NMO version 2.0

The C++ below is the on-disk mirror in this repository's target style (flat `namespace Neuron`,
`Nmo`-prefixed type names beside `DdsImage`/`ObjParser`, plain camelCase aggregate fields per R8,
UPPER_CASE constants per R3). Types come from `<cstdint>` and `<DirectXMath.h>`. Every struct is
trivially copyable, needs no packing pragma, and its `static_assert` is part of the specification.
`Tools/BlenderNmo/NmoFormat.py` implements exactly this section and nothing more; where prose and
codec disagree, the codec has a bug.

### 5.1 Container rules

- **Little-endian, fixed-width fields only** (`std::uint32_t`, `std::int32_t`, `float`,
  `std::uint16_t`). No pointers, no size-dependent types on disk.
- **Natural alignment, no packing pragmas.** Sections holding buffer payloads start 16-byte
  aligned; record streams and strings align to 4. The payload can be read with one read and used
  in place.
- **Presence is a count, never a flag byte.**
- **Variable-size data is reached through offsets** — file-relative in the mesh directory,
  mesh-blob-relative everywhere else. Offsets are authoritative; §5.11 is the recommended
  physical order. `0` means absent. Records may be shared between submeshes.
- **Bulk data is fixed-stride; only named records vary.** Vertices, indices, skin vertices, keys,
  facet ids, `NmoSubMesh` and `NmoMeshRef` tables are stride-exact arrays. Only the parsed-once
  record streams (materials, bones, clips, markers) contain strings.
- **Specified orderings** (§5.9): SRT tracks sort by strictly increasing `boneIndex`; each key
  series is strictly increasing in time; bones are topologically ordered
  (`parentIndex < ownIndex`); skin buffer *i* pairs vertex buffer *i*. Loaders never sort.
- **`String`**: `std::uint32_t lengthBytes`, then that many UTF-8 bytes, then zero padding to the
  next 4-byte boundary. Not terminated. `lengthBytes <= MAX_STRING_BYTES`. Invalid UTF-8 is a
  load failure.
- **Reserved means zero on write, ignored on read.**
- **Writers must emit finite floats.** Loaders are not required to check every float; the codec's
  validator does.
- **Validation before trust** (§5.12). A loader rejects; it never repairs.

```cpp
namespace Neuron
{
inline constexpr std::uint32_t NMO_FILE_MAGIC = 0x324F4D4E;   // "NMO2" on disk
inline constexpr std::uint16_t NMO_VERSION_MAJOR = 2;         // 1.x is the Interstellar Outpost dialect
inline constexpr std::uint16_t NMO_VERSION_MINOR = 0;
inline constexpr std::uint32_t NMO_MAX_STRING_BYTES = 1024;
inline constexpr std::uint32_t NMO_TEXTURE_SLOTS = 4;         // 0 base, 1 emissive, 2 normal, 3 reserved
inline constexpr std::uint32_t NMO_BONE_INFLUENCES = 4;
inline constexpr std::int32_t NMO_NO_PARENT = -1;
inline constexpr std::int32_t NMO_NO_BONE = -1;
```

### 5.2 Conventions

| Aspect | Rule |
|---|---|
| Byte order | Little-endian throughout. The magic reads `"NMO2"` on disk only when the byte order is right. |
| Coordinates | Left-handed render space: +X east, +Y up, +Z north. A ship's bow points +Z, matching what `ObjParser` establishes today. **Clockwise front faces.** The current renderer culls nothing and shades winding-immune (`ScenePS.hlsl`), so this binds writers now and renderers later. |
| Units | Metres. Time is **seconds** (`float`). UV origin top-left, V down, normalized to [0,1]. |
| Primitive | Indexed triangle lists only. |
| Vertex colour | Bytes R,G,B,A at ascending addresses — `DXGI_FORMAT_R8G8B8A8_UNORM` order, uploadable as-is. |
| Matrices | `XMFLOAT4X4`, row-major storage, row-vector convention (`v' = v * M`), translation in row 3 — DirectXMath's native reading. |
| Quaternions | `XMFLOAT4` as (x, y, z, w); identity is (0, 0, 0, 1). |

### 5.3 File header and mesh directory

```cpp
struct NmoFileHeader
{
  std::uint32_t magic;         // NMO_FILE_MAGIC
  std::uint16_t versionMajor;  // breaking changes only
  std::uint16_t versionMinor;  // additive changes only (§5.13)
  std::uint32_t headerBytes;   // sizeof(NmoFileHeader) == 32
  std::uint32_t fileBytes;     // total file size, for validation
  std::uint32_t meshCount;
  std::uint32_t flags;         // 0 in v2.0
  std::uint32_t payloadCrc32;  // CRC-32 (poly 0xEDB88320) of bytes [headerBytes, fileBytes); 0 = not computed
  std::uint32_t reserved;      // 0
};
static_assert(sizeof(NmoFileHeader) == 32, "NMO file header size");

struct NmoMeshRef
{
  std::uint32_t offsetBytes;   // from file start; 16-byte aligned
  std::uint32_t lengthBytes;   // whole mesh blob
};
static_assert(sizeof(NmoMeshRef) == 8, "NMO mesh ref size");
```

`NmoMeshRef[meshCount]` immediately follows the header. `uint32` offsets cap a file at 4 GB —
three orders of magnitude above any ship this game will draw. `payloadCrc32` is written by tools
and checked by tools and debug builds; a release loader may skip it.

### 5.4 Mesh header

All offsets in `NmoMeshHeader` and `NmoSubMesh` are **relative to the mesh blob's first byte**:
16-byte aligned for buffer sections, 4-byte aligned for record streams and strings. `0` = absent.

```cpp
struct NmoMeshHeader
{
  std::uint32_t flags;               // 0 in v2.0
  std::uint32_t nameOffset;          // -> String; 0 = unnamed
  std::uint32_t materialCount;
  std::uint32_t materialsOffset;     // -> material records, sequential
  std::uint32_t subMeshCount;
  std::uint32_t subMeshesOffset;     // -> NmoSubMesh[subMeshCount]
  std::uint32_t indexBufferCount;
  std::uint32_t indexBuffersOffset;  // -> buffer records, sequential
  std::uint32_t vertexBufferCount;
  std::uint32_t vertexBuffersOffset;
  std::uint32_t skinBufferCount;     // 0, or == vertexBufferCount
  std::uint32_t skinBuffersOffset;
  std::uint32_t extentsOffset;       // -> NmoMeshExtents
  std::uint32_t boneCount;           // mesh skeleton; 0 = none
  std::uint32_t bonesOffset;         // -> bone records, sequential
  std::uint32_t clipCount;           // mesh clips
  std::uint32_t clipsOffset;         // -> clip records, sequential
  std::uint32_t reserved[7];         // 0; future sections claim these (§5.13)
};
static_assert(sizeof(NmoMeshHeader) == 96, "NMO mesh header size");
```

### 5.5 Materials

A material record is, in order: `String` name, `NmoMaterial`, then exactly `NMO_TEXTURE_SLOTS`
(4) `String` texture names, empty slots included (an empty string is 4 bytes of zero length).
Slot roles: 0 base colour map, 1 emissive map, 2 normal map, 3 reserved. No texture pipeline
consumes them today; they exist so that adding one is not a format change.

```cpp
enum class NmoRenderFlags : std::uint32_t
{
  None = 0,
  DoubleSided = 0x1,                 // do not backface-cull (moot while the renderer culls nothing)
  AlphaBlend = 0x2,                  // draw in the blended overlay pass
  Additive = 0x4,                    // draw in the additive overlay pass
  RaceTinted = 0x8,                  // baseColour is a shade, not a colour: the faction supplies the hue
};

struct NmoMaterial
{
  DirectX::XMFLOAT4 baseColour;      // linear RGBA; what ObjParser's Kd becomes
  DirectX::XMFLOAT4 emissiveColour;  // linear RGB + strength in w; (0,0,0,0) = none
  std::uint32_t renderFlags;         // NmoRenderFlags bitmask
  std::uint32_t reserved[3];         // 0; scalars (roughness, ...) claim these behind a minor bump
};
static_assert(sizeof(NmoMaterial) == 48, "NMO material size");
```

The current engine bakes `baseColour` into vertex colour at load, exactly as `ObjParser` bakes
`Kd` today; `emissiveColour` and the first three flags wait for the renderer that wants them. The
Blender mapping is §11.

**`RaceTinted` — whose colour a material is.** A hull is painted by two authorities. Its structure
— plating, canopy glass — is the *model's*, identical on every ship of that class whoever flies it.
Its livery — the panels and trim that say whose ship this is — is the *faction's*, and the same
model has to wear azure for Core Vanguard Command, red for the Vandal Collective, or whatever a
player picked. A material declares which authority owns it, and it is a material property rather
than a name the loader recognises because "the material called `plate` is the liveried one" is a
convention a renamed material breaks silently, where a flag is authored, round-tripped and visible
in the file.

On a `RaceTinted` material `baseColour` is **a shade, not a colour**: the value says *how bright*
this surface is relative to the livery, and the hue is discarded. The engine's rule is one
multiply — `albedo = liveryColour.rgb × baseColour.rgb` — so a material authored greyscale comes
out as the livery at that brightness, which is why the shipping corpus authors these greyscale
(§13) and why a colour left in one is a trap for the next reader rather than a preview. Materials
without the flag are drawn exactly as authored and never see a livery at all.

Which faction supplies the hue is not the file's business and is deliberately absent from it: a
mesh knows it has a liveried panel, not who is flying it. The mapping from faction to colour is
the client's, and lives where the rest of that mapping already does
([Stations.md](Stations.md) §9.3).

### 5.6 Buffers

Every buffer — index, vertex, skin — is one uniform record: an `NmoBufferHeader`, then
`strideBytes × elementCount` payload bytes, then zero padding to the next 16-byte boundary.
Records of one kind are sequential from their section offset, so every payload stays 16-aligned
and can be `memcpy`'d or walked in place.

```cpp
struct NmoBufferHeader
{
  std::uint32_t format;              // per kind, below
  std::uint32_t strideBytes;         // must match the format
  std::uint32_t elementCount;
  std::uint32_t reserved;            // 0
};
static_assert(sizeof(NmoBufferHeader) == 16, "NMO buffer header size");

enum class NmoIndexFormat : std::uint32_t { U16 = 0, U32 = 1 };  // stride 2 / 4
enum class NmoVertexFormat : std::uint32_t { Standard = 0 };     // stride 36
enum class NmoSkinFormat : std::uint32_t { Standard = 0 };       // stride 32

struct NmoVertex
{
  DirectX::XMFLOAT3 position;
  DirectX::XMFLOAT3 normal;          // authored shading normal; the current renderer ignores it
  std::uint32_t colour;              // RGBA bytes (§5.2)
  DirectX::XMFLOAT2 uv;              // (0,0) when unauthored
};
static_assert(sizeof(NmoVertex) == 36, "NMO vertex size");

struct NmoSkinVertex
{
  std::uint32_t boneIndex[NMO_BONE_INFLUENCES];
  float boneWeight[NMO_BONE_INFLUENCES];
};
static_assert(sizeof(NmoSkinVertex) == 32, "NMO skin vertex size");
```

Writers must use `U16` indices whenever the referenced vertex range fits (half the bandwidth;
every shipped hull fits ten times over); `U32` exists so one huge mesh never forces buffer
splitting. **Pairing rule:** `skinBufferCount` is `0` or equals `vertexBufferCount`; skin buffer
*i* is the companion of vertex buffer *i*, and its `elementCount` is `0` (that VB is rigid) or
equal to the VB's. **Influence rule:** weights are non-negative, sum to 1, sorted descending;
unused influences are index 0, weight 0.

### 5.7 Submeshes

```cpp
struct NmoMeshExtents
{
  DirectX::XMFLOAT3 centre;
  float radius;
  DirectX::XMFLOAT3 boxMin;
  DirectX::XMFLOAT3 boxMax;
};
static_assert(sizeof(NmoMeshExtents) == 40, "NMO mesh extents size");

struct NmoSubMesh
{
  std::uint32_t materialIndex;
  std::uint32_t indexBufferIndex;
  std::uint32_t vertexBufferIndex;   // the companion skin buffer has the same index
  std::uint32_t startIndex;          // first index in the IB
  std::uint32_t primitiveCount;      // triangles
  std::uint32_t baseVertex;          // added to every index at draw time
  std::uint32_t minVertex;           // lowest baseVertex-biased vertex used
  std::uint32_t vertexCount;         // biased vertices lie in [minVertex, minVertex + vertexCount)
  std::uint32_t flags;               // 0 in v2.0; reserved bits
  std::uint32_t nameOffset;          // -> String; the part's role ("Hull", "TurretA", ...)
  std::uint32_t boneCount;           // submesh bone table; 0 = bind to the mesh skeleton
  std::uint32_t bonesOffset;         // -> bone records                   (R-NMO-A)
  std::uint32_t clipCount;
  std::uint32_t clipsOffset;         // -> clip records                   (R-NMO-A)
  std::uint32_t markerCount;
  std::uint32_t markersOffset;       // -> marker records                 (R-NMO-B)
  std::uint32_t facetsOffset;        // -> uint32[primitiveCount]; 0 = absent
  NmoMeshExtents extents;            // bind-pose bounds of this submesh
  std::uint32_t reserved[5];         // 0
};
static_assert(sizeof(NmoSubMesh) == 128, "NMO submesh size");
```

The three range fields beyond CMO's five make the draw call complete and checkable —
`DrawIndexedInstanced(3 * primitiveCount, 1, startIndex, baseVertex, 0)` once an indexed pipeline
exists, and until then they bound exactly which vertices the loader's soup expansion may touch.
Submesh extents are bind-pose bounds; a tool animating a part far from bind pose should inflate
them by the clip's maximum displacement.

**Names are a contract, not decoration** — the role (`Hull`, `TurretA`, `RadarDish`) the engine
maps at load, deliberately a *name* rather than an enum field, so game behaviour never gets baked
into the format. **Facet ids** are one `uint32` per triangle naming the source polygon it was cut
from; triangles sharing an id were one polygon before triangulation, so the Blender importer
rebuilds the artist's quads exactly (§11). The section is optional.

### 5.8 Skeletons and bone records

A **bone record** is: `String` name, then `NmoBone`. The same record shape serves both scopes.

```cpp
struct NmoBone
{
  std::int32_t parentIndex;               // NMO_NO_PARENT for a root; always < own index
  std::int32_t meshBoneIndex;             // mesh scope: NMO_NO_BONE. submesh scope: NMO_NO_BONE = a
                                          // local bone; >= 0 = an alias of that mesh bone
  DirectX::XMFLOAT4X4 localTransform;     // parent-relative rest/default pose; clips override it
  DirectX::XMFLOAT4X4 invBindPose;        // mesh space -> bone space at skin binding
};
static_assert(sizeof(NmoBone) == 136, "NMO bone size");
```

The CMO `bindPose` matrix is dropped: it is always the walk of `localTransform` up the hierarchy
and is rebuilt at load in one forward pass. `invBindPose` stays on disk because it anchors
skinning and must not be re-derived through a runtime inverse. For an **alias** entry
(`meshBoneIndex >= 0`), the matrices are copies of the mesh bone's; loaders read the mesh bone's
and may ignore the copies, so a drifted copy is inert rather than a defect class.

- **Mesh scope** (`NmoMeshHeader.boneCount/bonesOffset`): the mesh skeleton. `meshBoneIndex` must
  be `NMO_NO_BONE`.
- **Submesh scope** (`NmoSubMesh.boneCount/bonesOffset`): a submesh-local table. A table whose
  entries are all aliases is a **bone palette** — it names which mesh bones this submesh uses and
  adds no animation of its own. A local bone may parent onto an aliased entry — that is how a
  turret's root hangs off a hull bone.

`parentIndex` always indexes the table the record sits in. All hierarchies, both scopes, evaluate
in **mesh space**. Skinning indices resolve against the submesh's **bone scope**: its own table
when `boneCount > 0`, else the mesh table. §7 gives the semantics.

### 5.9 Animation clips (R-NMO-A)

A **clip record** is: `String` name, `NmoClip`, `NmoSrtTrack[trackCount]`, then — for each track
in order — its translation keys, its rotation keys, its scale keys.

```cpp
struct NmoClip
{
  float startSeconds;
  float endSeconds;                  // >= startSeconds
  std::uint32_t trackCount;          // one per animated bone; 0 allowed: a held pose
  std::uint32_t flags;               // 0 in v2.0
};
static_assert(sizeof(NmoClip) == 16, "NMO clip size");

struct NmoSrtTrack
{
  std::uint32_t boneIndex;           // into the clip's bone scope; strictly increasing across tracks
  std::uint32_t translationKeyCount;
  std::uint32_t rotationKeyCount;
  std::uint32_t scaleKeyCount;
};
static_assert(sizeof(NmoSrtTrack) == 16, "NMO SRT track size");

struct NmoTranslationKey { float timeSeconds; DirectX::XMFLOAT3 value; };
struct NmoRotationKey    { float timeSeconds; DirectX::XMFLOAT4 value; };  // quaternion
struct NmoScaleKey       { float timeSeconds; float value; };              // uniform
static_assert(sizeof(NmoTranslationKey) == 16, "NMO translation key size");
static_assert(sizeof(NmoRotationKey) == 20, "NMO rotation key size");
static_assert(sizeof(NmoScaleKey) == 8, "NMO scale key size");
```

- **Scope.** Mesh clips index the mesh bone table. Submesh clips index the submesh's bone scope —
  its own table when `boneCount > 0`, else the **mesh** table (same rig, private pose — §7 row 2).
  Clip names are unique within their scope; runtime addressing is `(scope, name)`.
- **Ordering.** Tracks sort by strictly increasing `boneIndex` (no duplicate tracks, binary
  searchable). Each key series is strictly increasing in time.
- **Sampling.** Linear interpolation for translation and scale, shortest-arc nlerp (or slerp) for
  rotation; clamp at series ends; a bone with no track holds its `localTransform`; a track's
  missing channel (count 0) holds that channel of `localTransform`. A constant channel needs one
  key, not ninety — writers should collapse constant tracks (the source document's §8.4, adopted
  as writer guidance from day one).

### 5.10 Markers (R-NMO-B)

A **marker record** is: `String` name, `String` kind, then `NmoMarker`:

```cpp
struct NmoMarker
{
  DirectX::XMFLOAT3 position;        // mesh space, bind pose
  DirectX::XMFLOAT4 orientation;     // quaternion; the marker's direction is its local +Z
  float scale;                       // uniform; nozzle radius, light size, ... 1.0 default
  std::int32_t parentBone;           // into the submesh's bone scope; NMO_NO_BONE = rigid
  DirectX::XMFLOAT4 colour;          // linear RGBA; (1,1,1,1) when the kind has no colour
  float param0;                      // kind-specific, 0 default
  float param1;                      // kind-specific, 0 default
  std::uint32_t flags;               // NmoMarkerFlags bitmask
  std::uint32_t reserved[2];         // 0
};
static_assert(sizeof(NmoMarker) == 72, "NMO marker size");
```

**Kind** is what the marker *is*; **name** is what this one is *called* (`ExhaustPortA`,
`NavPortAft` — artist-facing labels, unique within their submesh across all kinds). An empty kind
is legal and means a plain point. Unknown kinds are legal by construction: a loader keeps every
marker and consumers select the kinds they understand — that is the whole future-proofing
mechanism, and it costs nothing (R-NMO-B "flexible for markers that do not exist yet").

Kinds defined by v2.0, and the fields each consumes:

| Kind | Consumes | Meaning |
|---|---|---|
| `Exhaust` | position, orientation (+Z = plume direction), scale (nozzle radius, m), **colour** (plume/glow, RGB × intensity in a) | Replaces the clustered `attachPoints` and the hard-coded `SELECTED_COLOUR` glow (§9) |
| `NavLight` | position, scale (light size, m), **colour**, param0 (blink period s; 0 = steady), param1 (blink phase, fraction of the period) | New; drawn by the view as a small glow, blinking on real time |
| `Gun` | position, orientation (+Z = muzzle direction), scale (calibre hint, m) | Effects anchor (muzzle flash, projectile spawn visual). Simulation truth stays authored in GameLogic (§9) |

**Marker flags**, and why a marker needs the same distinction a material does:

```cpp
enum class NmoMarkerFlags : std::uint32_t
{
  None = 0,
  RaceTinted = 0x1,                  // colour is a shade; the faction supplies the hue (§5.5)
};
```

An exhaust plume is livery — every ship of a faction burns the faction's colour, and a green plume
on a red hull is the bug this bit exists to prevent. A navigation light is emphatically *not*: port
red and starboard green are a convention older than any faction in this game, and liveried they
would turn red-on-red for the Vandal Collective and blue-on-blue for the Vanguard, which is the
convention destroyed rather than themed. So the bit is per marker and authored, with the defaults
that make the common case free: **`Exhaust` is authored `RaceTinted`, `NavLight` and `Gun` are
not.** They are defaults for a tool to write, not rules a loader enforces — a beacon an artist
wants in faction colour sets the bit, and a faction whose ships burn white clears it.

The rule on a flagged marker is §5.5's, unchanged: `colour.rgb` is a shade, multiplied by the
livery, and its `a` goes on meaning intensity either way.

**Spaces.** `position` is mesh space at bind pose — the same space as vertices, so a tool places
markers on the model and coordinates copy through. A bound marker (`parentBone >= 0`) follows its
bone with the same transform skinning uses: `world = markerLocal × boneWorld × invBindPose` in
effect — one rule shared with §7, no second convention. A rigid marker is
`world = markerLocal × meshWorld`.

**Lookup practice** (unchanged from the source): never string-compare per frame. At load, resolve
kinds to enum values and hash names (FNV-1a 32-bit) into per-submesh sorted arrays; collisions are
a load-time failure for the tool to rename around, and hashes never enter the file.

### 5.11 Recommended physical order

Offsets are authoritative; the reference writer emits each mesh blob as: `NmoMeshHeader`, mesh
name, material records, `NmoSubMesh` table, then per submesh its name / bone records / clip
records / marker records / facet ids, then index buffers, vertex buffers, skin buffers,
`NmoMeshExtents`, mesh bone records, mesh clip records. Metadata parses from the first few KB;
the bulk payload sits contiguously behind it. Appendix A is the stream grammar.

### 5.12 Validation requirements

A conforming loader **must** verify, in order, before using any data — and reject the file on any
failure (never repair, never assert; return false with a reason through `DebugTrace`, the
`IMDLoad`-shaped diagnostic path this tree already uses for content):

1. `magic == NMO_FILE_MAGIC`; `versionMajor == 2`; `headerBytes == 32`; `fileBytes` equals the
   actual size read.
2. Every `NmoMeshRef` window lies within `[headerBytes + meshCount * 8, fileBytes)` and is
   16-byte aligned; all arithmetic in 64 bits before any allocation or pointer math — counts and
   offsets are attacker-controlled until proven otherwise.
3. Every offset in `NmoMeshHeader`/`NmoSubMesh`, plus its section's computed size, lies within
   the mesh blob; alignment as specified; every `String` obeys `NMO_MAX_STRING_BYTES` and valid
   UTF-8.
4. `NmoBufferHeader.format` is a known value and `strideBytes` matches it; the skin/vertex-buffer
   pairing and element-count rules of §5.6 hold.
5. Per submesh: `materialIndex`, `indexBufferIndex`, `vertexBufferIndex` in range; the index range
   `[startIndex, startIndex + 3 * primitiveCount)` fits its IB; `minVertex >= baseVertex`; every
   baseVertex-biased index falls in `[minVertex, minVertex + vertexCount)` and within the VB;
   when `facetsOffset` is set, the section holds exactly `primitiveCount` ids.
6. Per bone table: `parentIndex < ownIndex` (cycles impossible; pose evaluation is one forward
   loop); submesh aliases satisfy `meshBoneIndex < mesh boneCount`; mesh-scope records carry
   `NMO_NO_BONE`; names unique within their table.
7. Per clip: `endSeconds >= startSeconds`; track `boneIndex` values strictly increasing and
   within the clip's bone scope; each key series strictly increasing in time; clip names unique
   within their scope.
8. Per skinned submesh (its VB has a non-empty companion): its bone scope is non-empty; every
   `NmoSkinVertex.boneIndex` of vertices in `[minVertex, minVertex + vertexCount)` is within the
   scope; weights obey §5.6; two skinned submeshes with different bone scopes must not overlap
   vertex ranges (same scope may share freely).
9. Per marker: `parentBone` is `NMO_NO_BONE` or within the submesh's bone scope; names unique
   within the submesh.
10. Unknown flag bits and nonzero reserved fields: ignore (§5.13 guarantees they are ignorable).
    `versionMinor` greater than the loader knows: load, optionally trace.

Sanity caps, recommended, checked before deeper work so a hostile length cannot direct a huge
allocation: `meshCount <= 4096`, `boneCount <= 1024` per scope, `materialCount <= 256`,
`markerCount <= 1024` per submesh, `clipCount <= 256` per scope.

Both implementations are tested against this list on the same bytes: the codec by
`Tools/NmoRoundtripTest.py` (one deliberately malformed file per clause), the engine reader in
slice 2 by an equivalent C++ suite that corrupts fields it locates through the header, so the
tests keep testing the right rule when the fixture changes.

### 5.13 Versioning policy

- **Minor bump (additive only).** New data arrives only where v2.0 loaders already ignore it: a
  reserved field becomes a new count/offset pair to a new section, or a flag bit is defined whose
  meaning is safe to ignore, or a reserved scalar gains a meaning with a zero-equals-absent
  default. Existing sections, strides and record framings never change.
- **Major bump (breaking).** Anything else — including any new `NmoIndexFormat`/
  `NmoVertexFormat`/`NmoSkinFormat` value or clip-payload shape, because a v2.0 loader cannot use
  a payload it cannot interpret.

`NmoRenderFlags::RaceTinted` and `NmoMarkerFlags::RaceTinted` are the first bits this policy would
govern, and they are in **v2.0 rather than a 2.1** on the one ground that makes that honest: the
format has no consumer yet. Slice 1 shipped a codec and a corpus, not a reader, so there is no
build in existence that would ignore the bits and no file in existence that depends on their
absence — the struct sizes are unchanged, the corpus is regenerated from `Art/` anyway, and the
golden fixture is extended to carry one flagged material and one flagged marker in slice 2 (its
generator is committed, so the extension is a reviewed diff rather than a silent byte change). Had an engine reader shipped first, this would have been a minor
bump by the rule above and nothing else about it would differ. The day one has shipped, that
latitude is gone.

---

## 6. What was considered and rejected (beyond the source's own list)

The source document's §8.5 rejections — string tables, whole-file compression, RIFF chunk trees,
name hashes in the file, curve tracks, per-polygon render state — are re-affirmed unread; none of
their premises changed. New to this adaptation:

- **A property-bag marker** (each marker a list of key → typed-value pairs). Maximally flexible,
  and Blender custom properties would map onto it naturally — but it puts a variant codec in
  every reader, gives the validator an open-ended grammar, and every consumer still ends up
  defining a schema per kind anyway. The kind string plus a fixed TRS/colour/parameter block
  covers every marker anyone has proposed (docking ports, hardpoints, shield emitters, damage
  smoke points all fit); if a kind one day genuinely needs structured payload, a reserved
  `NmoMarker` field becomes an offset to a kind-defined blob behind a minor bump.
- **Marker kinds as an enum.** Two bytes cheaper and closed — closed is the defect. R-NMO-B says
  future kinds must not be a format change.
- **Keeping CMO's `MatrixKeys`, `Material`, `Bone.bindPose` and 52-byte `Vertex` for
  cross-dialect compatibility with Interstellar Outpost.** Compatibility with a format that has
  itself shipped nothing buys nothing today and would carry that tree's D3D9 constraints here
  forever. The lineage is preserved where it is cheap (container rules, section shapes, the
  version-field arbitration) and dropped where it is not (§3). Revisit only if the two projects
  decide to share tooling — an alternative the owner has declined once already (§15 D1).
- **Storing markers at mesh scope as well as submesh scope.** A second scope with identical
  records is more spec for zero expressiveness — a one-submesh ship already has a natural home
  for its markers, and a marker that conceptually belongs to the whole mesh sits on the hull
  submesh with equal effect.
- **A `Colour` on materials only, with exhaust colour derived from the nozzle's material** (the
  current `thruster`-material trick, formalized). Rejected: it revives the exact indirection this
  format exists to kill — geometry as a side channel for effect data, one colour per material
  forcing one material per differently-coloured nozzle, and nothing for markers that have no
  faces at all (nav lights, guns).
- **glTF instead of a bespoke format.** The obvious modern answer, and someone will propose it
  again, so: a conforming reader needs JSON, base64 and an extension ecosystem — a hand-written
  C++ parser far larger than this whole format, or a third-party library AGENTS.md §5 forbids by
  default; markers-with-colour, bone palettes and facet ids would be custom extensions anyway, so
  the editability argument collapses to "Blender can open it", which the add-on provides here at
  a fraction of the surface; and its scene-graph generality is exactly the machinery §1's engine
  does not want to validate at load. The container disciplines that make glTF good (typed
  buffers, views, validation) are the ones NMO already inherited.

## 7. Submesh bone animation — semantics (R-NMO-A)

The four configurations, inherited:

| `boneCount` | `clipCount` | Meaning | Typical use here |
|---|---|---|---|
| 0 | 0 | Rigid, or skinned against the mesh skeleton, driven by mesh clips | hull plating |
| 0 | > 0 | Clips scoped to this submesh but indexing the **mesh** skeleton — same rig, private pose | a variant idle on one part |
| > 0, all aliased | 0 | A **bone palette**: names which mesh bones this submesh uses; mesh clips drive them | splitting a big rig for a CPU skinner's locality |
| > 0, local or mixed | >= 0 | An independent articulated part with its own skeleton and clips | **a turret on a hull, a radar dish, bay doors** — the spaceship case |

Pose evaluation, per submesh, per frame: (1) evaluate the mesh pose — mesh clips over mesh bones,
one forward loop in table order, unkeyed bones holding `localTransform`; (2) if the submesh has a
bone table, aliased entries take the mesh bone's evaluated mesh-space transform, local entries
evaluate the same way, resolving a `parentIndex` that lands on an alias through the mesh pose; a
submesh clip that keys an aliased entry overrides the mesh pose *for this submesh only*; (3) skin
matrices are `palette[j] = invBindPose[j] × world[j]` over the submesh's bone scope; rigid
submeshes skip it all; a rigid submesh whose *whole* geometry rides one bone (the common turret)
needs no skin buffer — the loader may drive it as a per-submesh world transform.

Aliasing stays in the format even though D3D12 retired the register-ceiling argument that
motivated it there: the partition between a hull rig and a turret rig is *data* (which bones
belong to which part), the palette keeps a CPU skinner's matrix array small and contiguous, and
without it every loader would re-derive per-submesh bone usage by scanning skin vertices.

What the format deliberately does **not** define, unchanged: cross-fade, layering, priorities.
The animation system that one day replaces "no animation" gets its own design; this file hands it
cleanly separated scopes.

## 8. Facet ids and round-tripping

One `uint32` per triangle, naming the source polygon. The Blender exporter fan-triangulates each
polygon in loop order and stamps every resulting triangle with the polygon's index; the importer
groups consecutive triangles by id, reassembles each group's boundary loop, and hands Blender
back the artist's quads and n-gons. A group whose triangles do not form one simple polygon
imports as triangles — malformed grouping degrades, never fails. Exported vertices are welded on
the whole attribute tuple (position, normal, colour, UV), so authored seams stay split and flat
interiors collapse.

## 9. Consumers: how the markers reach the screen (and who never sees them)

- **Exhaust** — `MeshLibrary` today copies `MeshData::attachPoints` into
  `ShipView::thrusterLocals` (`WorldView.cpp:100`) and every glow draws `SELECTED_COLOUR`
  (`WorldView.cpp:728`). Slice 3 replaces the points with `Exhaust` marker positions and the
  constant with the marker's colour; intensity handling (`THRUSTER_*` in `ViewTuning.h`) is
  unchanged — the marker colours the effect, the view still animates it. Where the marker is
  `RaceTinted` (§5.10, and every shipped exhaust is) that colour is a shade and the view multiplies
  it by the flying faction's livery, so one authored plume burns azure, red or the player's own
  without a second marker.
- **NavLight** — new in slice 3: one glow pip per marker, colour from the marker, blinking
  on `param0`/`param1` against real time (presentation state, so it lives in `WorldView`, not the
  simulation — AGENTS.md §5).
- **Gun** — carried by the format and the add-on now; consumed when combat arrives. The boundary
  stands as ADR 0002 drew it: whatever the *simulation* needs of a hardpoint arrives as authored
  numbers in GameLogic. If hand-maintaining those numbers ever hurts, the sanctioned shape is an
  offline generator — a `Tools/` script reading `.nmo` and emitting a reviewed header — never a
  runtime read.
- **Everything else** (`MeshData` soup for the explosion shatter, bounds for picking, `RestY`) is
  produced by the loader exactly as `ObjParser` produces it today; those consumers do not change.

## 10. Loading and runtime practice (slice 2 guidance)

- One read (`BinaryFile::ReadFile`), validate per §5.12, then parse the named records once into
  compact tables and expand geometry into `MeshData` — indexed → soup, material `baseColour`
  baked into vertex colour, exactly the shape every current consumer holds
  ([SpaceshipExplosion.md](Archive/SpaceshipExplosion.md) §2 depends on it). The in-place-view design the
  source document specifies stays available for the day the renderer goes indexed; nothing in the
  layout prevents it, which is the point of the alignment rules.
- A `.nmo` that fails validation traces the failing rule and returns false; `MeshLibrary` skips
  the hull, which is the same diagnostic-not-a-crash treatment a missing OBJ gets at boot.
- Markers: resolve kind strings once, hash names once (§5.10); `MeshData` grows a marker list the
  view reads by kind. `attachPoints` keeps working until slice 4 removes the OBJ path.

## 11. The Blender add-on (R-NMO-C)

`Tools/BlenderNmo/` targets Blender **4.2+** (extension platform; both `blender_manifest.toml`
and legacy `bl_info` ship, so either installer works) and is exercised headlessly against
**5.0.1** via the `bpy` wheel. The scene mapping lives in one module, `NmoScene.py`, so import
and export cannot drift:

| NMO | Blender |
|---|---|
| Mesh | Collection |
| Submesh | Mesh object in it, named for its role |
| `NmoMaterial` | Material: base colour → Principled Base Color (+ viewport colour), emissive → Emission, flags/texture slots → custom properties |
| Mesh / submesh bone table | Armature object tagged with its scope; submesh objects parented under the mesh armature when one exists |
| Pure-alias table (a palette) | `nmo_palette` property listing mesh-bone names — deliberately not a second armature an artist could desynchronise |
| Clip | Action; SRT tracks ↔ pose-bone location/rotation/scale F-curves, baked at export |
| Marker | Empty: kind → display type (`Exhaust` cone, `NavLight` sphere, `Gun` single arrow, else plain axes), colour → `object.color`, kind/params → `nmo_kind`/`nmo_param0`/`nmo_param1` custom properties, bone binding → bone parenting |
| Facet ids | Rebuilt into real polygons on import; regenerated by triangulation on export (§8) |
| Skin weights | Vertex groups named for bones + an Armature modifier |

**Axes.** NMO is Y-up left-handed; Blender is Z-up right-handed. One self-inverse component swap
`(x, y, z) ↔ (x, z, y)` converts both directions, so a model imported and exported unchanged
comes back byte-identical rather than accumulating error. Consequences an author sees: **the bow
points +Y in Blender, up is +Z**, and the exporter fixes triangle winding for §5.2's clockwise
rule (the swap is a reflection; the exact index treatment is settled by the headless test, not by
prose). A marker's direction is its Blender empty's visible axis — the add-on applies the fixed
conjugation that keeps "point the arrow at it" true on both sides of the swap.

**Animation compatibility.** Blender 4.4 introduced slotted actions and 5.0 removed legacy
`Action.fcurves`; one helper in `NmoScene.py` is the only place either API shape is named, which
is what keeps a single add-on working across 4.2 → 5.x.

**What the headless test verifies** (`Tools/NmoBlenderTest.py`, run on the `bpy` wheel): import
the golden fixture — two submeshes over one shared vertex buffer, a mesh skeleton, a local
turret skeleton with an SRT clip, an alias palette, all three marker kinds with colours, facet
ids, skin weights — export it, re-read with the codec, and compare structurally; the axis and
winding invariants numerically; and the same import → export → compare over a `Tools/ObjToNmo.py`
conversion of the shipped Corvette. Each phase runs in its own interpreter: resetting the scene
in-process (`wm.read_factory_settings`) corrupts the heap intermittently under the `bpy` wheel,
and a fresh process isolates better than any reset could.

## 12. Testing policy

- `Tools/NmoRoundtripTest.py` — pure Python, no Blender: fixture → write → read → write is
  byte-identical; the validator accepts the fixture and rejects one purpose-built malformed file
  per §5.12 clause; CRC round-trips.
- `Tools/NmoBlenderTest.py` — §11 above; skips with a clear message where `bpy` is unavailable.
- Slice 2 ports the malformed-file suite to C++ against the same bytes: the golden `.nmo` is
  checked in beside the `.dds` font assets, with `Tools/NmoFixture.py` as its committed
  generator — decided with the owner as a deliberate, narrow exception to "nothing generated is
  committed" (§15 D3), so the VS suite reads a file rather than growing a python dependency.
  Regenerating and re-comparing the committed fixture is part of that slice's acceptance.
- CI: the two Python tests are candidates for the Linux job beside `CheckFormat.py` — they need
  only python3 for the first and a pip-installed `bpy` for the second (~300 MB; worth its own
  non-blocking job first, per this tree's linter-promotion practice).

## 13. Content migration

The ten OBJ hulls convert mechanically: `Tools/ObjToNmo.py` ports ObjParser's conventions — the
same OBJ subset, the same Z negation (plus the winding reversal §5.2 makes necessary), the same
`Kd` materials, and the same union-find exhaust clustering, run *for the last time* to seed
`Exhaust` markers with positions from the clusters, radii from their spread, and colour from the
hull's `thruster` material `Kd`, which is what the authoring intent always was. One submesh per
material, named for it; the `Kd` is also baked into vertex colour, so the engine's flat-colour
path draws the converted hull identically. Every output is read back through the codec's full
validation before it is written.

All ten hulls convert clean today (checked in this slice; the Corvette also round-trips through
Blender in `Tools/NmoBlenderTest.py`). What a converter cannot invent — `NavLight` and `Gun`
markers, per-nozzle colour overrides, role-named submeshes — is authored in Blender afterwards,
which is the add-on's job. The converted `.nmo` files land in `Outpost/Assets/Meshes/` in slice
4, when the loader exists to read them; until then conversion is one command, not a checked-in
tree. The heuristic thereby moves from the shipping loader into a tool that runs once — the
source document's converter philosophy, at one-fiftieth the corpus size. `.pie` migration
machinery (roles tables, canonical-name passes, corpus surveys) has no counterpart here and is
dropped entirely.

### 13.1 The material vocabulary, and which half of it is livery

The corpus authors five material names (`Stargate` adds a sixth, `aperture`, which follows
`accent`), and they divide exactly as §5.5 divides authority. The GLBs as authored carry green
hues and no flags; this table is what `Art/Meshes/GlbToNmo.py` applies to them, **by material
name, once, at conversion** — a `RaceTinted` or `baseColour` value present in a GLB material's
`extras` wins over the row, so an author can override it at source without the converter
learning a new name. `Exhaust` markers are flagged by kind the same way, `NavLight` and `Gun` are
not, and a marker's `nmo_flags` in `extras` wins likewise. The name convention lives in the tool
that runs once and never in the loader, which is the distinction §5.5 draws (slice 5 lands it):

| Material | `RaceTinted` | `baseColour` | Why |
|---|---|---|---|
| `hull` | no | `0.27 0.27 0.27` | Structural plating. Neutral grey, and it is the ship's own |
| `glass` | no | `0.12 0.12 0.12` | Canopy. Dark enough to read as a window against the hull |
| `plate` | **yes** | `0.45 0.45 0.45` | The painted panels — the biggest liveried area |
| `accent` | **yes** | `0.80 0.80 0.80` | Trim: the stripe that names the faction at distance |
| `thruster` | **yes** | `1.00 1.00 1.00` | Nozzles, the brightest thing on an unlit hull |

The three liveried rows are greyscale on purpose, per §5.5 — they are a **brightness ladder**, and
that ladder is the whole content of them: plate reads as body, accent as trim, thruster as hot, at
every livery and in the same order. A hue painted into one would be discarded by the multiply and
would mislead whoever opened the file next.

The two generic rows are much lighter than the corpus carried before the flag existed (`hull` was
`0.024 0.027 0.025`, near black). They had to be. Those values were authored against a renderer
that lerped 55 % of a light tint over the whole hull, which lifted them to a mid grey on screen;
nothing lifts them now, so the authored value is the final one, and it is the *rendered* brightness
that has to be preserved rather than the number. `glass` is pulled well under `hull` deliberately:
the two rendered three units apart before, so a canopy was invisible and the tint was hiding it.

The liveried shades are absolute, not relative to a faction: `livery × 1.0` is the livery at full
strength, which is what a nozzle should be. A faction colour is therefore authored at the
brightness its thrusters should burn, and every other liveried surface falls out of the ladder.

## 14. Slices

1. **Tools and specification** *(accompanies this document; no engine change)* — §4's `Tools/`
   tree: codec, Blender add-on, OBJ converter, fixture, both test scripts, all passing on
   python3 + bpy 5.0.1; AGENTS.md map row for `Tools/`; the ADR for adopting the format. Out of
   scope: anything under a `.vcxproj`.
2. **Engine reader** *(NeuronClient; Windows)* — `NmoFile.h` structs with `static_assert`s,
   `NmoReader` validating per §5.12 and expanding into `MeshData` (+ markers), C++ malformed-file
   tests over the shared fixture bytes; registered in the project files; no caller yet.
3. **Marker consumers** *(Outpost)* — `MeshLibrary` loads `.nmo` when present (OBJ fallback
   stays); `ShipView` takes exhaust positions/colours from `Exhaust` markers; nav-light pips with
   blink; screenshots at two window sizes.
4. **Content swap** *(Assets)* — hulls re-exported per §13 with authored markers; OBJ files and
   `ObjParser`'s clustering retire (parser itself may stay for dev import); the
   `SpaceshipExplosion` and picking paths re-verified over the new loader's soup.
5. **Liveries** *(NeuronClient shaders, Outpost)* — the visible half of `RaceTinted` (§5.5,
   §5.10): the scene shader stops tinting a whole hull and multiplies the flagged surfaces by the
   flying faction's colour, the faction-to-colour mapping becomes the table
   [Stations.md](Stations.md) §9.3 describes, and exhaust plumes follow it. The converter gains
   §13.1's table and the corpus is regenerated in the same commit as the shader, so the shades and
   the multiply that gives them meaning never ship apart. Its work order is
   [NmoFormat-slice-5.md](NmoFormat-slice-5.md).
6. **Articulated parts** *(later, own design note)* — pose evaluation, per-submesh transforms in
   the renderer, the first animated turret/dish; where indexed drawing and GPU skinning earn
   their slices, they hang off this one.

Slices 2→3→4→5 are ordered; 6 is independent after 2. One slice per layer at a time, per the loop
in [Design/README.md](README.md).

**On the numbering.** The work orders in `Design/` renumbered 3 and 4 against this list — the
content swap was brought in front of the marker consumers, and
[NmoFormat-slice-3.md](NmoFormat-slice-3.md) §2.7 says why. Each work order names the slice of
this list it implements, so the two numberings can be reconciled by reading either file's opening
paragraph. The livery slice is 5 in both.

## 15. Decisions taken with the owner (2026-08-29)

The first draft of this document listed five open questions with chosen defaults; each was put
to the owner and decided. They are recorded here, not deleted, because every one of them is an
alternative someone will propose again:

1. **Dialect unification — decided: separate dialects.** v2.0 deliberately diverges from the
   Interstellar Outpost NMO (§3): shared lineage and ideas, distinct magic, no byte
   compatibility. Folding back behind that tree's `MatrixKeys`/CMO-fidelity constraints was
   declined; the version field remains the arbiter if the trees ever reconverge.
2. **Marker kind vocabulary — decided: the three required kinds.** `Exhaust`, `NavLight`, `Gun`
   are defined; `DockingPort`, `Hardpoint`, `ShieldEmitter` and the rest arrive when a consumer
   exists, which the kind-string mechanism makes a table row, not a format change.
3. **Slice 2 fixture transport — decided: check the `.nmo` in.** The golden fixture is committed
   beside the `.dds` font assets with `Tools/NmoFixture.py` as its committed generator, so the
   VS suite reads a file rather than growing a python dependency. This is a deliberate, narrow
   exception to "nothing generated is committed": the binary is regenerable, and its generator's
   diff is its review. Slice 2 states it in its work order.
4. **Texture slots — decided: keep four.** 16 empty bytes per material keeps a future texture
   pipeline (base/emissive/normal maps) out of major-version territory (§5.5).
5. **Blink policy — decided: per marker.** `NavLight` blink lives in the marker's params
   (period/phase, zero meaning steady), authored per light in Blender; `ViewTuning.h` stays out
   of it.

---

## Appendix A — stream grammar (recommended writer layout)

`String` = `uint32` byte length + UTF-8 + zero-pad to 4. Offsets are authoritative (§5.4); this
is the order the reference writer emits.

```
// .NMO version 2.0
//
// NmoFileHeader (32 bytes: magic 'NMO2', version 2.0, headerBytes, fileBytes,
//                meshCount, flags, payloadCrc32, reserved)
// NmoMeshRef[meshCount] (8 bytes each: offsetBytes, lengthBytes)
// { [meshCount]  - each blob at NmoMeshRef.offsetBytes, 16-byte aligned
//      NmoMeshHeader (96 bytes - counts and blob-relative offsets of every section)
//      String - name of mesh
//      { [materialCount]
//          String - name of material
//          NmoMaterial structure (48 bytes: baseColour, emissiveColour, renderFlags, reserved)
//          { [4] String - texture name (slot 0 base, 1 emissive, 2 normal, 3 reserved) }
//      }
//      NmoSubMesh[subMeshCount] (128 bytes each - draw range, flags, extents, and the
//                                offsets/counts of everything below)
//      { [per submesh, at the offsets its NmoSubMesh record names]
//          String - name of submesh                          - the part's role
//          { [subMesh.boneCount]                             - R-NMO-A
//              String - name of bone
//              NmoBone structure (136 bytes: parentIndex, meshBoneIndex,
//                                 localTransform, invBindPose)
//          }
//          { [subMesh.clipCount]                             - R-NMO-A
//              String - name of clip
//              NmoClip structure (16 bytes: startSeconds, endSeconds, trackCount, flags)
//              NmoSrtTrack[trackCount] (16 bytes each; boneIndex strictly increasing)
//              { [per track, in order]
//                  NmoTranslationKey[] NmoRotationKey[] NmoScaleKey[]   - times strictly increasing
//              }
//          }
//          { [subMesh.markerCount]                           - R-NMO-B
//              String - name of marker
//              String - kind of marker ("Exhaust", "NavLight", "Gun", ...)
//              NmoMarker structure (72 bytes: position, orientation, scale, parentBone,
//                                   colour, param0, param1, flags, reserved)
//          }
//          UINT[subMesh.primitiveCount] - facet ids, when facetsOffset != 0
//      }
//      { [indexBufferCount]
//          NmoBufferHeader (16 bytes) - uint16[] or uint32[] indices - pad to 16
//      }
//      { [vertexBufferCount]
//          NmoBufferHeader (16 bytes) - NmoVertex[36] - pad to 16
//      }
//      { [skinBufferCount]  - 0 or vertexBufferCount; pairs by index
//          NmoBufferHeader (16 bytes) - NmoSkinVertex[32] - pad to 16
//      }
//      NmoMeshExtents structure (40 bytes)
//      { [boneCount]   - mesh skeleton; meshBoneIndex always -1
//          String - name of bone
//          NmoBone structure
//      }
//      { [clipCount]   - mesh clips, framed exactly as the submesh clips above
//          String - name of clip
//          NmoClip structure
//          NmoSrtTrack[] ...keys...
//      }
// }
```
