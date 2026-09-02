#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
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

// A contiguous run of vertices in a MeshData's soup: what one draw covers.
//
// The same two fields MeshSubMesh already carries, as a type of their own, because the interesting
// operation is over a SET of runs and it has nothing to do with what a submesh is named or where it
// pivots. A caller posing a turret hands the renderer the turret's run; the renderer draws the rest
// of the hull as the complement of the runs it was given.
struct MeshRange
{
  std::uint32_t firstVertex = 0;
  std::uint32_t vertexCount = 0;

  [[nodiscard]] constexpr std::uint32_t End() const noexcept
  {
    return firstVertex + vertexCount;
  }
};

// The most runs the complement of _posedCount runs can need: one before each, and one after the
// last. A caller sizes its scratch with this rather than remembering the arithmetic.
[[nodiscard]] constexpr std::size_t ComplementCapacity(std::size_t _posedCount) noexcept
{
  return _posedCount + 1;
}

// The runs of [0, _vertexCount) that _posed does NOT cover, written into _outGaps, returning how
// many were written. At most ComplementCapacity(_posed.size()), so a caller that sized its buffer
// with that function cannot overflow it, and a smaller buffer is filled and the rest dropped.
//
// **_posed is SORTED IN PLACE**, which is why it is a mutable span. The alternative was a local copy
// with a fixed cap, and a cap that silently ignored the seventh range would be the kind of quiet
// wrongness this function exists to prevent. The caller owns that buffer, has just built it, and has
// no use for its order.
//
// Runs that overlap or touch are merged, so two turrets whose submeshes happen to be adjacent leave
// one gap on each side rather than an empty run between them. A run past the end is clamped and an
// empty one is dropped, so a caller that asked for a part its mesh does not have gets the whole mesh
// back rather than a hole in it.
//
// Pure arithmetic over integers, in the header the data lives in and not in the renderer, because
// this is the half of the submesh draw that can be wrong silently -- a gap that overlaps a posed run
// draws the turret twice, once turning and once not -- and the half a suite can reach without a
// device.
[[nodiscard]] inline std::size_t RangeComplement(std::span<MeshRange> _posed, std::uint32_t _vertexCount,
                                                 std::span<MeshRange> _outGaps) noexcept
{
  std::sort(_posed.begin(), _posed.end(), [](const MeshRange& _a, const MeshRange& _b) { return _a.firstVertex < _b.firstVertex; });

  std::size_t written = 0;
  std::uint32_t at = 0; // the first vertex not yet accounted for
  const auto emit = [&](std::uint32_t _first, std::uint32_t _end)
  {
    if (_end > _first && written < _outGaps.size())
      _outGaps[written++] = MeshRange{_first, _end - _first};
  };

  for (const MeshRange& posed : _posed)
  {
    if (posed.vertexCount == 0 || posed.firstVertex >= _vertexCount)
      continue; // an empty run, or one wholly past the end: it covers nothing
    const std::uint32_t first = posed.firstVertex;
    const std::uint32_t end = std::min(posed.End(), _vertexCount);
    if (first > at)
      emit(at, first);
    at = std::max(at, end); // max, not assignment: a run inside one already walked must not reopen it
  }
  emit(at, _vertexCount);
  return written;
}

// A mesh as it comes off disk: triangle soup, its bounds, the markers its author placed on it, and
// the index of named parts over the soup that lets a consumer address one of them without a second
// copy. Deliberately free of graphics API types, so a loader, a test and a tool can all hold one
// without a device.
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

  // The part named _nameHash, or a run of nothing if this mesh has no such part. A zero-count run
  // rather than an index and a sentinel, because every caller wants the range and none wants the
  // index, and a run of nothing poses nothing and complements to the whole mesh.
  [[nodiscard]] MeshRange RangeOf(std::uint32_t _nameHash) const noexcept
  {
    for (const MeshSubMesh& part : subMeshes)
    {
      if (part.nameHash == _nameHash && _nameHash != 0)
        return MeshRange{part.firstVertex, part.vertexCount};
    }
    return MeshRange{};
  }

  // What that part turns about, in mesh space, or the mesh's own centre when it has no such part.
  [[nodiscard]] DirectX::XMFLOAT3 PivotOf(std::uint32_t _nameHash) const noexcept
  {
    for (const MeshSubMesh& part : subMeshes)
    {
      if (part.nameHash == _nameHash && _nameHash != 0)
        return part.Pivot();
    }
    return BoundsCentre();
  }
};
} // namespace Neuron
