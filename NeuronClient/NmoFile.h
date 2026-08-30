#pragma once

#include <DirectXMath.h>

#include <cstdint>

// The NMO version 2.0 on-disk layout, transcribed from Design/NmoFormat.md section 5 and nothing
// else: no functions, no helpers, no includes from this project. It is meant to be read beside that
// document and diffed by eye, so keep the order the document has and change nothing here that the
// document does not say first.
//
// Every static_assert below is part of the specification rather than a nicety. "Read the header,
// then index the array" is only legal because these sizes are exact, and a padding change from a
// future compiler switch is precisely the defect they exist to catch.
namespace Neuron
{
inline constexpr std::uint32_t NMO_FILE_MAGIC = 0x324F4D4E; // "NMO2" on disk
inline constexpr std::uint16_t NMO_VERSION_MAJOR = 2;       // 1.x is the Interstellar Outpost dialect
inline constexpr std::uint16_t NMO_VERSION_MINOR = 0;
inline constexpr std::uint32_t NMO_MAX_STRING_BYTES = 1024;
inline constexpr std::uint32_t NMO_TEXTURE_SLOTS = 4; // 0 base, 1 emissive, 2 normal, 3 reserved
inline constexpr std::uint32_t NMO_BONE_INFLUENCES = 4;
inline constexpr std::int32_t NMO_NO_PARENT = -1;
inline constexpr std::int32_t NMO_NO_BONE = -1;

// The sanity caps of section 5.12: bounds on what a hostile count can make a reader allocate
// before the deeper checks have run.
inline constexpr std::uint32_t NMO_MAX_MESHES = 4096;
inline constexpr std::uint32_t NMO_MAX_BONES = 1024;
inline constexpr std::uint32_t NMO_MAX_MATERIALS = 256;
inline constexpr std::uint32_t NMO_MAX_MARKERS = 1024;
inline constexpr std::uint32_t NMO_MAX_CLIPS = 256;

struct NmoFileHeader
{
  std::uint32_t magic;        // NMO_FILE_MAGIC
  std::uint16_t versionMajor; // breaking changes only
  std::uint16_t versionMinor; // additive changes only (5.13)
  std::uint32_t headerBytes;  // sizeof(NmoFileHeader) == 32
  std::uint32_t fileBytes;    // total file size, for validation
  std::uint32_t meshCount;
  std::uint32_t flags;        // 0 in v2.0
  std::uint32_t payloadCrc32; // CRC-32 (poly 0xEDB88320) of [headerBytes, fileBytes); 0 = not computed
  std::uint32_t reserved;     // 0
};
static_assert(sizeof(NmoFileHeader) == 32, "NMO file header size");

struct NmoMeshRef
{
  std::uint32_t offsetBytes; // from file start; 16-byte aligned
  std::uint32_t lengthBytes; // whole mesh blob
};
static_assert(sizeof(NmoMeshRef) == 8, "NMO mesh ref size");

struct NmoMeshHeader
{
  std::uint32_t flags;      // 0 in v2.0
  std::uint32_t nameOffset; // -> String; 0 = unnamed
  std::uint32_t materialCount;
  std::uint32_t materialsOffset; // -> material records, sequential
  std::uint32_t subMeshCount;
  std::uint32_t subMeshesOffset; // -> NmoSubMesh[subMeshCount]
  std::uint32_t indexBufferCount;
  std::uint32_t indexBuffersOffset; // -> buffer records, sequential
  std::uint32_t vertexBufferCount;
  std::uint32_t vertexBuffersOffset;
  std::uint32_t skinBufferCount; // 0, or == vertexBufferCount
  std::uint32_t skinBuffersOffset;
  std::uint32_t extentsOffset; // -> NmoMeshExtents
  std::uint32_t boneCount;     // mesh skeleton; 0 = none
  std::uint32_t bonesOffset;   // -> bone records, sequential
  std::uint32_t clipCount;     // mesh clips
  std::uint32_t clipsOffset;   // -> clip records, sequential
  std::uint32_t reserved[7];   // 0; future sections claim these (5.13)
};
static_assert(sizeof(NmoMeshHeader) == 96, "NMO mesh header size");

enum class NmoRenderFlags : std::uint32_t
{
  None = 0,
  DoubleSided = 0x1, // do not backface-cull (moot while the renderer culls nothing)
  AlphaBlend = 0x2,  // draw in the blended overlay pass
  Additive = 0x4,    // draw in the additive overlay pass
  RaceTinted = 0x8,  // baseColour is a shade, not a colour: the faction supplies the hue
};

struct NmoMaterial
{
  DirectX::XMFLOAT4 baseColour;     // linear RGBA
  DirectX::XMFLOAT4 emissiveColour; // linear RGB + strength in w; (0,0,0,0) = none
  std::uint32_t renderFlags;        // NmoRenderFlags bitmask
  std::uint32_t reserved[3];        // 0; scalars (roughness, ...) claim these behind a minor bump
};
static_assert(sizeof(NmoMaterial) == 48, "NMO material size");

struct NmoBufferHeader
{
  std::uint32_t format;      // per kind, below
  std::uint32_t strideBytes; // must match the format
  std::uint32_t elementCount;
  std::uint32_t reserved; // 0
};
static_assert(sizeof(NmoBufferHeader) == 16, "NMO buffer header size");

enum class NmoIndexFormat : std::uint32_t
{
  U16 = 0,
  U32 = 1,
}; // stride 2 / 4

enum class NmoVertexFormat : std::uint32_t
{
  Standard = 0,
}; // stride 36

enum class NmoSkinFormat : std::uint32_t
{
  Standard = 0,
}; // stride 32

struct NmoVertex
{
  DirectX::XMFLOAT3 position;
  DirectX::XMFLOAT3 normal; // authored shading normal; the current renderer ignores it
  std::uint32_t colour;     // RGBA bytes (5.2)
  DirectX::XMFLOAT2 uv;     // (0,0) when unauthored
};
static_assert(sizeof(NmoVertex) == 36, "NMO vertex size");

struct NmoSkinVertex
{
  std::uint32_t boneIndex[NMO_BONE_INFLUENCES];
  float boneWeight[NMO_BONE_INFLUENCES];
};
static_assert(sizeof(NmoSkinVertex) == 32, "NMO skin vertex size");

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
  std::uint32_t vertexBufferIndex; // the companion skin buffer has the same index
  std::uint32_t startIndex;        // first index in the IB
  std::uint32_t primitiveCount;    // triangles
  std::uint32_t baseVertex;        // added to every index at draw time
  std::uint32_t minVertex;         // lowest baseVertex-biased vertex used
  std::uint32_t vertexCount;       // biased vertices lie in [minVertex, minVertex + vertexCount)
  std::uint32_t flags;             // 0 in v2.0; reserved bits
  std::uint32_t nameOffset;        // -> String; the part's role ("Hull", "TurretA", ...)
  std::uint32_t boneCount;         // submesh bone table; 0 = bind to the mesh skeleton
  std::uint32_t bonesOffset;       // -> bone records                   (R-NMO-A)
  std::uint32_t clipCount;
  std::uint32_t clipsOffset; // -> clip records                   (R-NMO-A)
  std::uint32_t markerCount;
  std::uint32_t markersOffset; // -> marker records                 (R-NMO-B)
  std::uint32_t facetsOffset;  // -> uint32[primitiveCount]; 0 = absent
  NmoMeshExtents extents;      // bind-pose bounds of this submesh
  std::uint32_t reserved[5];   // 0
};
static_assert(sizeof(NmoSubMesh) == 128, "NMO submesh size");

struct NmoBone
{
  std::int32_t parentIndex;           // NMO_NO_PARENT for a root; always < own index
  std::int32_t meshBoneIndex;         // mesh scope: NMO_NO_BONE. submesh scope: NMO_NO_BONE = a
                                      // local bone; >= 0 = an alias of that mesh bone
  DirectX::XMFLOAT4X4 localTransform; // parent-relative rest/default pose; clips override it
  DirectX::XMFLOAT4X4 invBindPose;    // mesh space -> bone space at skin binding
};
static_assert(sizeof(NmoBone) == 136, "NMO bone size");

struct NmoClip
{
  float startSeconds;
  float endSeconds;         // >= startSeconds
  std::uint32_t trackCount; // one per animated bone; 0 allowed: a held pose
  std::uint32_t flags;      // 0 in v2.0
};
static_assert(sizeof(NmoClip) == 16, "NMO clip size");

struct NmoSrtTrack
{
  std::uint32_t boneIndex; // into the clip's bone scope; strictly increasing across tracks
  std::uint32_t translationKeyCount;
  std::uint32_t rotationKeyCount;
  std::uint32_t scaleKeyCount;
};
static_assert(sizeof(NmoSrtTrack) == 16, "NMO SRT track size");

struct NmoTranslationKey
{
  float timeSeconds;
  DirectX::XMFLOAT3 value;
};

struct NmoRotationKey
{
  float timeSeconds;
  DirectX::XMFLOAT4 value; // quaternion
};

struct NmoScaleKey
{
  float timeSeconds;
  float value; // uniform
};
static_assert(sizeof(NmoTranslationKey) == 16, "NMO translation key size");
static_assert(sizeof(NmoRotationKey) == 20, "NMO rotation key size");
static_assert(sizeof(NmoScaleKey) == 8, "NMO scale key size");

enum class NmoMarkerFlags : std::uint32_t
{
  None = 0,
  RaceTinted = 0x1, // colour is a shade; the faction supplies the hue (5.5)
};

struct NmoMarker
{
  DirectX::XMFLOAT3 position;    // mesh space, bind pose
  DirectX::XMFLOAT4 orientation; // quaternion; the marker's direction is its local +Z
  float scale;                   // uniform; nozzle radius, light size, ... 1.0 default
  std::int32_t parentBone;       // into the submesh's bone scope; NMO_NO_BONE = rigid
  DirectX::XMFLOAT4 colour;      // linear RGBA; (1,1,1,1) when the kind has no colour
  float param0;                  // kind-specific, 0 default
  float param1;                  // kind-specific, 0 default
  std::uint32_t flags;           // NmoMarkerFlags bitmask
  std::uint32_t reserved[2];     // 0
};
static_assert(sizeof(NmoMarker) == 72, "NMO marker size");
} // namespace Neuron
