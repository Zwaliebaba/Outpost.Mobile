#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace Neuron
{
// One vertex format for everything the scene pass draws. No normal: the pixel shader takes the face
// normal from screen-space derivatives, which is exactly the flat shading this game wants, and one
// format means one input layout and one pipeline for hulls, ground and decals alike.
struct MeshVertex
{
  float px, py, pz;
  float r, g, b;
  // 0 for a surface the model paints, 1 for one the faction paints (Design/Archive/NmoFormat.md 5.5). It is
  // a float and not a bit because it is a vertex attribute the input assembler has to hand the
  // shader, and it is per vertex and not per draw because both kinds of surface are on one hull and
  // this renderer draws a hull in one call.
  //
  // The initialiser is load-bearing: it is what lets every existing MeshVertex{x,y,z,r,g,b} in the
  // tree go on compiling and mean "the model's own paint", which is the right answer for all of
  // them -- the ground quad and the decals included.
  float race = 0.0f;
};

// What a marker is, resolved from the file's kind string once at load. The format carries any kind
// string and a reader keeps every marker it finds (Design/Archive/NmoFormat.md 5.10), which is why Unknown
// exists: a kind this build has never heard of is carried, not dropped, and a consumer that does
// not understand it simply does not select it.
enum class MarkerKind : std::uint32_t
{
  Point, // an empty kind string: a plain point
  Exhaust,
  NavLight,
  Gun,
  Unknown,
};

// One authored attachment: an exhaust nozzle, a navigation light, a gun mount. Names are hashed
// rather than stored -- section 5.10's lookup practice, and the reason nothing here holds a
// std::string.
struct MeshMarker
{
  MarkerKind kind = MarkerKind::Point;
  std::uint32_t nameHash = 0;                            // FNV-1a 32 of the marker's name
  DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};          // mesh space, bind pose
  DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f}; // direction is local +Z
  float scale = 1.0f;                                    // nozzle radius, light size, metres
  DirectX::XMFLOAT4 colour{1.0f, 1.0f, 1.0f, 1.0f};      // linear RGBA
  float param0 = 0.0f;
  float param1 = 0.0f;
  // colour is a shade and the faction supplies the hue (Design/Archive/NmoFormat.md 5.10). Carried as a
  // bool rather than the file's flag word because one bit is defined and a consumer asking "is this
  // liveried" should not be asking it to bitmask arithmetic.
  bool raceTinted = false;

  // Which bone of its submesh's own table this marker rides, or -1 for one bolted to the mesh. The
  // reader has always read and validated it (Design/Archive/NmoFormat.md 5.10) and Expand used to
  // drop it; it is carried now because it is the difference between a muzzle that follows a barrel
  // and one that floats where the barrel used to be.
  //
  // Nothing reads it yet, and no shipped hull sets it: every hull in the game is rigid submeshes
  // with no rig at all (Design/Combat-slice-3.md 2.6). It is four bytes on a struct loaded once per
  // hull, against reading the format a second time the day a rigged hull is authored.
  std::int32_t parentBone = -1;
};

// One named part of a mesh: a hull, a turret, a nacelle.
//
// It is an INDEX over the triangle soup rather than a second copy of it. Expand emits every
// submesh's vertices contiguously and in file order, so a part is a run, and a consumer that does
// not care -- the scene pass, the shatter -- goes on drawing MeshData::verts whole without knowing
// this exists. That is what makes turning one part of a hull cost nothing for every hull that does
// not.
struct MeshSubMesh
{
  // FNV-1a 32 of the part's name, hashed and not stored for MeshMarker::nameHash's reason. An
  // unnamed submesh hashes to 0 and is addressable by its index, which the format allows and the
  // golden fixture's second mesh deliberately exercises.
  std::uint32_t nameHash = 0;

  std::uint32_t firstVertex = 0; // into MeshData::verts
  std::uint32_t vertexCount = 0;
  std::uint32_t firstMarker = 0; // into MeshData::markers
  std::uint32_t markerCount = 0;

  // This part's own bind-pose bounds, from the file where it states them and accumulated from its
  // own vertices where it does not -- the mesh-level rule (Design/Archive/NmoFormat.md 5.9), one
  // scope down.
  DirectX::XMFLOAT3 boundsMin{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 boundsMax{0.0f, 0.0f, 0.0f};

  // What a part turns about. A turret bolted to a hull pivots on its own centre, and every shipped
  // turret states bounds that put that centre where a gun would actually be mounted -- the
  // Battleship's forward turret spans (-4.83, 11.33, 14.67) to (4.83, 15.04, 24.33), and turns on
  // the middle of it.
  [[nodiscard]] DirectX::XMFLOAT3 Pivot() const noexcept
  {
    return DirectX::XMFLOAT3((boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f, (boundsMin.z + boundsMax.z) * 0.5f);
  }
};

// A mesh as it comes off disk: triangle soup, its bounds, and the markers its author placed on it.
// Deliberately free of graphics API types, so a loader, a test and a tool can all hold one without
// a device.
struct MeshData
{
  std::vector<MeshVertex> verts;
  DirectX::XMFLOAT3 boundsMin{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 boundsMax{0.0f, 0.0f, 0.0f};

  // Every marker the file carried, in submesh then file order, whatever its kind.
  std::vector<MeshMarker> markers;

  // The mesh's parts, in file order, indexing the two vectors above. Empty for nothing: every mesh
  // has at least one submesh, so a reader that produced none produced no geometry either.
  std::vector<MeshSubMesh> subMeshes;

  [[nodiscard]] DirectX::XMFLOAT3 BoundsCentre() const noexcept
  {
    return DirectX::XMFLOAT3((boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f, (boundsMin.z + boundsMax.z) * 0.5f);
  }

  // Never zero on any axis: a flat mesh would otherwise pick as an infinitely thin slab.
  [[nodiscard]] DirectX::XMFLOAT3 HalfExtents() const noexcept
  {
    const float x = (boundsMax.x - boundsMin.x) * 0.5f;
    const float y = (boundsMax.y - boundsMin.y) * 0.5f;
    const float z = (boundsMax.z - boundsMin.z) * 0.5f;
    return DirectX::XMFLOAT3(x > 0.5f ? x : 0.5f, y > 0.5f ? y : 0.5f, z > 0.5f ? z : 0.5f);
  }

  // How far the mesh has to be lifted for its lowest vertex to rest on y = 0.
  [[nodiscard]] float RestY() const noexcept
  {
    return -boundsMin.y;
  }

  [[nodiscard]] bool Empty() const noexcept
  {
    return verts.empty();
  }
};
} // namespace Neuron
