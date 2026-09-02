#include "pch.h"
#include "NmoReader.h"

#include "NmoFile.h"

#include "FileSys.h"

using namespace DirectX;

namespace Neuron
{
namespace
{
constexpr std::uint32_t FNV_OFFSET_BASIS = 0x811C9DC5u;
constexpr std::uint32_t FNV_PRIME = 0x01000193u;
constexpr std::uint32_t CRC32_POLYNOMIAL = 0xEDB88320u; // Design/Archive/NmoFormat.md 5.3
constexpr float WEIGHT_SUM_TOLERANCE = 1e-3f;
constexpr std::uint32_t BUFFER_ALIGNMENT = 16;
constexpr std::uint32_t RECORD_ALIGNMENT = 4;
constexpr std::uint32_t COLOUR_MAX = 255;

std::string Narrow(const std::wstring& _text)
{
  if (_text.empty())
    return {};

  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, _text.data(), static_cast<int>(_text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(bytes), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, _text.data(), static_cast<int>(_text.size()), result.data(), bytes, nullptr, nullptr);
  return result;
}

// FNV-1a 32, over the UTF-8 bytes exactly as the file holds them. Marker names are hashed once at
// load and never string-compared again (Design/Archive/NmoFormat.md 5.10); the hash is never written to a
// file, because derivable data on disk is a consistency liability.
std::uint32_t Fnv1a(std::string_view _text) noexcept
{
  std::uint32_t hash = FNV_OFFSET_BASIS;
  for (const char character : _text)
  {
    hash ^= static_cast<std::uint8_t>(character);
    hash *= FNV_PRIME;
  }
  return hash;
}

#if defined(_DEBUG)
// Bit-wise rather than table-driven: this runs once per file in a Debug build and a 1 KB table
// would cost more to justify than to compute. A release loader may skip the check entirely (5.3),
// so the function is not compiled into one at all.
std::uint32_t Crc32(std::span<const std::uint8_t> _bytes) noexcept
{
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::uint8_t byte : _bytes)
  {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
    {
      crc = (crc >> 1) ^ (CRC32_POLYNOMIAL & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}
#endif

// Overlong forms, surrogates and anything past U+10FFFF are refused as well as the obvious
// malformed sequences. The reference codec decodes strictly, so a form this accepted and Python
// refused would be a file one implementation loads and the other does not.
bool IsValidUtf8(std::span<const std::uint8_t> _bytes) noexcept
{
  constexpr std::uint32_t SMALLEST_FOR_LENGTH[4] = {0, 0x80, 0x800, 0x10000};
  std::size_t at = 0;
  while (at < _bytes.size())
  {
    const std::uint8_t lead = _bytes[at];
    std::size_t continuations = 0;
    std::uint32_t code = 0;
    if (lead < 0x80)
    {
      ++at;
      continue;
    }
    else if ((lead & 0xE0u) == 0xC0u)
    {
      continuations = 1;
      code = lead & 0x1Fu;
    }
    else if ((lead & 0xF0u) == 0xE0u)
    {
      continuations = 2;
      code = lead & 0x0Fu;
    }
    else if ((lead & 0xF8u) == 0xF0u)
    {
      continuations = 3;
      code = lead & 0x07u;
    }
    else
    {
      return false;
    }

    if (continuations >= _bytes.size() - at)
      return false;
    for (std::size_t step = 1; step <= continuations; ++step)
    {
      const std::uint8_t next = _bytes[at + step];
      if ((next & 0xC0u) != 0x80u)
        return false;
      code = (code << 6) | (next & 0x3Fu);
    }
    if (code < SMALLEST_FOR_LENGTH[continuations] || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu))
      return false;
    at += continuations + 1;
  }
  return true;
}

MarkerKind KindFromString(std::string_view _kind) noexcept
{
  if (_kind.empty())
    return MarkerKind::Point;
  if (_kind == "Exhaust")
    return MarkerKind::Exhaust;
  if (_kind == "NavLight")
    return MarkerKind::NavLight;
  if (_kind == "Gun")
    return MarkerKind::Gun;
  return MarkerKind::Unknown; // kept, never dropped: unknown kinds are legal by construction (5.10)
}

// --- the parsed views -----------------------------------------------------------------------
// Nothing below owns bytes. Every span was proved to lie inside the window it came from before it
// was made, which is what makes it safe to hold one at all.

struct BufferView
{
  std::uint32_t format = 0;
  std::uint32_t strideBytes = 0;
  std::uint32_t elementCount = 0;
  std::span<const std::uint8_t> payload;
};

struct BoneView
{
  std::string_view name;
  std::int32_t parentIndex = NMO_NO_PARENT;
  std::int32_t meshBoneIndex = NMO_NO_BONE;
};

struct KeySeries
{
  std::span<const std::uint8_t> bytes;
  std::uint32_t count = 0;
  std::uint32_t strideBytes = 0;
};

struct TrackView
{
  std::uint32_t boneIndex = 0;
  KeySeries translation;
  KeySeries rotation;
  KeySeries scale;
};

struct ClipView
{
  std::string_view name;
  float startSeconds = 0.0f;
  float endSeconds = 0.0f;
  std::vector<TrackView> tracks;
};

struct MarkerView
{
  std::string_view name;
  std::string_view kind;
  NmoMarker record = {};
};

struct SubMeshView
{
  NmoSubMesh record = {};
  std::string_view name;
  bool named = false;
  std::vector<BoneView> bones;
  std::vector<ClipView> clips;
  std::vector<MarkerView> markers;
  std::span<const std::uint8_t> facets;
  bool hasFacets = false;
};

struct MeshView
{
  NmoMeshHeader header = {};
  std::string_view name;
  std::vector<NmoMaterial> materials;
  std::vector<BufferView> indexBuffers;
  std::vector<BufferView> vertexBuffers;
  std::vector<BufferView> skinBuffers;
  std::vector<BoneView> bones;
  std::vector<ClipView> clips;
  std::vector<SubMeshView> subMeshes;
  NmoMeshExtents extents = {};
  bool hasExtents = false;
};

// Bounds-checked reads over one contiguous window, carrying the label every diagnostic wants. A
// take is proved before it happens, so a hostile offset or count names the clause it broke rather
// than reading past the buffer and being noticed four slices later.
class Cursor
{
public:
  Cursor(std::span<const std::uint8_t> _bytes, std::string_view _label) noexcept
    : m_bytes(_bytes),
      m_label(_label)
  {
  }

  [[nodiscard]] std::uint64_t SizeBytes() const noexcept
  {
    return m_bytes.size();
  }

  template <class... Types> bool Reject(int _clause, std::string_view _fmt, Types&&... _args) const
  {
    DebugTrace("{}: rejected by 5.12.{} -- ", m_label, _clause);
    DebugTrace(_fmt, _args...);
    DebugTrace("\n");
    return false;
  }

  // The whole of the arithmetic rule, stated once instead of at thirty call sites: offset plus
  // count times stride overflows in 32 bits, and a wrapped range passes a naive bounds check, so
  // every window is measured in 64 bits before anything is indexed or sized from it (5.12.2).
  [[nodiscard]] bool Take(std::uint64_t _offset, std::uint64_t _count, std::uint64_t _strideBytes, int _clause, const char* _what,
                          std::span<const std::uint8_t>& _outWindow) const
  {
    const std::uint64_t sizeBytes = SizeBytes();
    const std::uint64_t windowBytes = _count * _strideBytes; // both are file uint32s: no wrap in 64 bits
    if (_offset > sizeBytes || windowBytes > sizeBytes - _offset)
      return Reject(_clause, "{} at {} for {} bytes escapes a {}-byte window", _what, _offset, windowBytes, sizeBytes);
    _outWindow = m_bytes.subspan(static_cast<std::size_t>(_offset), static_cast<std::size_t>(windowBytes));
    return true;
  }

  template <typename T> [[nodiscard]] bool Read(std::uint64_t _offset, int _clause, const char* _what, T& _out) const
  {
    std::span<const std::uint8_t> window;
    if (!Take(_offset, 1, sizeof(T), _clause, _what, window))
      return false;
    std::memcpy(&_out, window.data(), sizeof(T));
    return true;
  }

  // A String is a uint32 byte length, that many UTF-8 bytes, and zero padding to the next 4 (5.1).
  [[nodiscard]] bool ReadString(std::uint64_t _offset, const char* _what, std::string_view& _outText, std::uint64_t& _outNext) const
  {
    if (_offset % RECORD_ALIGNMENT != 0)
      return Reject(3, "{} string at {} is not 4-aligned", _what, _offset);

    std::uint32_t lengthBytes = 0;
    if (!Read(_offset, 3, _what, lengthBytes))
      return false;
    if (lengthBytes > NMO_MAX_STRING_BYTES)
      return Reject(3, "{} string of {} bytes exceeds the {}-byte cap", _what, lengthBytes, NMO_MAX_STRING_BYTES);

    std::span<const std::uint8_t> window;
    if (!Take(_offset + sizeof(std::uint32_t), lengthBytes, 1, 3, _what, window))
      return false;
    if (!IsValidUtf8(window))
      return Reject(3, "{} string is not valid UTF-8", _what);

    _outText = window.empty() ? std::string_view() : std::string_view(reinterpret_cast<const char*>(window.data()), window.size());
    _outNext = _offset + sizeof(std::uint32_t) + lengthBytes + ((RECORD_ALIGNMENT - lengthBytes % RECORD_ALIGNMENT) % RECORD_ALIGNMENT);
    return true;
  }

  // Presence is a count and 0 means absent, so a count with no offset behind it is a malformed
  // header rather than an empty section (5.1, 5.4).
  [[nodiscard]] bool Section(std::uint64_t _offset, std::uint64_t _count, const char* _what) const
  {
    if (_count != 0 && _offset == 0)
      return Reject(3, "{} count is {} but the section offset is 0", _what, _count);
    return true;
  }

private:
  std::span<const std::uint8_t> m_bytes;
  std::string m_label;
};

// --- clause 3 and 4: framing ------------------------------------------------------------------

bool ReadBoneRecords(const Cursor& _cursor, std::uint64_t _offset, std::uint32_t _count, const char* _scope, std::vector<BoneView>& _out)
{
  if (_count > NMO_MAX_BONES)
    return _cursor.Reject(3, "{} bone count {} exceeds the sanity cap {}", _scope, _count, NMO_MAX_BONES);

  std::uint64_t at = _offset;
  _out.reserve(_count);
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    BoneView bone;
    if (!_cursor.ReadString(at, "bone name", bone.name, at))
      return false;
    NmoBone record = {};
    if (!_cursor.Read(at, 3, "bone", record))
      return false;
    at += sizeof(NmoBone);
    bone.parentIndex = record.parentIndex;
    bone.meshBoneIndex = record.meshBoneIndex;
    _out.push_back(bone);
  }
  return true;
}

bool ReadClipRecords(const Cursor& _cursor, std::uint64_t _offset, std::uint32_t _count, const char* _scope, std::vector<ClipView>& _out)
{
  if (_count > NMO_MAX_CLIPS)
    return _cursor.Reject(3, "{} clip count {} exceeds the sanity cap {}", _scope, _count, NMO_MAX_CLIPS);

  std::uint64_t at = _offset;
  _out.reserve(_count);
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    ClipView clip;
    if (!_cursor.ReadString(at, "clip name", clip.name, at))
      return false;
    NmoClip record = {};
    if (!_cursor.Read(at, 3, "clip", record))
      return false;
    at += sizeof(NmoClip);
    clip.startSeconds = record.startSeconds;
    clip.endSeconds = record.endSeconds;

    // A track is at least its own record, so a count that could not fit in what is left is refused
    // before it sizes anything (the clause 2 rule, applied to a count rather than an offset).
    if (static_cast<std::uint64_t>(record.trackCount) * sizeof(NmoSrtTrack) > _cursor.SizeBytes())
      return _cursor.Reject(3, "{} clip track count {} cannot fit the mesh blob", _scope, record.trackCount);

    std::vector<NmoSrtTrack> tracks(record.trackCount);
    for (std::uint32_t track = 0; track < record.trackCount; ++track)
    {
      if (!_cursor.Read(at, 3, "clip track", tracks[track]))
        return false;
      at += sizeof(NmoSrtTrack);
    }

    clip.tracks.reserve(record.trackCount);
    for (const NmoSrtTrack& track : tracks)
    {
      TrackView view;
      view.boneIndex = track.boneIndex;
      view.translation.count = track.translationKeyCount;
      view.translation.strideBytes = sizeof(NmoTranslationKey);
      view.rotation.count = track.rotationKeyCount;
      view.rotation.strideBytes = sizeof(NmoRotationKey);
      view.scale.count = track.scaleKeyCount;
      view.scale.strideBytes = sizeof(NmoScaleKey);
      for (KeySeries* series : {&view.translation, &view.rotation, &view.scale})
      {
        if (!_cursor.Take(at, series->count, series->strideBytes, 3, "clip keys", series->bytes))
          return false;
        at += series->bytes.size();
      }
      clip.tracks.push_back(std::move(view));
    }
    _out.push_back(std::move(clip));
  }
  return true;
}

bool ReadMarkerRecords(const Cursor& _cursor, std::uint64_t _offset, std::uint32_t _count, std::vector<MarkerView>& _out)
{
  if (_count > NMO_MAX_MARKERS)
    return _cursor.Reject(3, "marker count {} exceeds the sanity cap {}", _count, NMO_MAX_MARKERS);

  std::uint64_t at = _offset;
  _out.reserve(_count);
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    MarkerView marker;
    if (!_cursor.ReadString(at, "marker name", marker.name, at))
      return false;
    if (!_cursor.ReadString(at, "marker kind", marker.kind, at))
      return false;
    if (!_cursor.Read(at, 3, "marker", marker.record))
      return false;
    at += sizeof(NmoMarker);
    _out.push_back(marker);
  }
  return true;
}

bool ReadBuffers(const Cursor& _cursor, std::uint64_t _offset, std::uint32_t _count, const char* _kind, std::vector<BufferView>& _out)
{
  if (_count != 0 && _offset % BUFFER_ALIGNMENT != 0)
    return _cursor.Reject(3, "{} buffer section at {} is not 16-aligned", _kind, _offset);
  if (static_cast<std::uint64_t>(_count) * sizeof(NmoBufferHeader) > _cursor.SizeBytes())
    return _cursor.Reject(3, "{} buffer count {} cannot fit the mesh blob", _kind, _count);

  std::uint64_t at = _offset;
  _out.reserve(_count);
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    NmoBufferHeader header = {};
    if (!_cursor.Read(at, 3, "buffer header", header))
      return false;
    at += sizeof(NmoBufferHeader);

    BufferView view;
    view.format = header.format;
    view.strideBytes = header.strideBytes;
    view.elementCount = header.elementCount;
    if (!_cursor.Take(at, view.elementCount, view.strideBytes, 3, "buffer payload", view.payload))
      return false;
    const std::uint64_t payloadBytes = view.payload.size();
    at += payloadBytes + ((BUFFER_ALIGNMENT - payloadBytes % BUFFER_ALIGNMENT) % BUFFER_ALIGNMENT);
    _out.push_back(view);
  }
  return true;
}

// The index at _element of a buffer whose stride clause 4 has already agreed with its format.
std::uint32_t ReadIndex(const BufferView& _buffer, std::uint64_t _element) noexcept
{
  const std::size_t at = static_cast<std::size_t>(_element * _buffer.strideBytes);
  if (_buffer.strideBytes == sizeof(std::uint16_t))
  {
    std::uint16_t narrow = 0;
    std::memcpy(&narrow, _buffer.payload.data() + at, sizeof(narrow));
    return narrow;
  }
  std::uint32_t wide = 0;
  std::memcpy(&wide, _buffer.payload.data() + at, sizeof(wide));
  return wide;
}

std::uint32_t IndexStride(std::uint32_t _format) noexcept
{
  if (_format == static_cast<std::uint32_t>(NmoIndexFormat::U16))
    return 2;
  if (_format == static_cast<std::uint32_t>(NmoIndexFormat::U32))
    return 4;
  return 0; // unknown: the caller rejects
}

bool ReadMeshBlob(const Cursor& _cursor, MeshView& _out)
{
  if (!_cursor.Read(0, 3, "mesh header", _out.header))
    return false;

  const NmoMeshHeader& header = _out.header;
  if (header.nameOffset != 0)
  {
    std::uint64_t unused = 0;
    if (!_cursor.ReadString(header.nameOffset, "mesh name", _out.name, unused))
      return false;
  }

  if (header.materialCount > NMO_MAX_MATERIALS)
    return _cursor.Reject(3, "material count {} exceeds the sanity cap {}", header.materialCount, NMO_MAX_MATERIALS);
  if (static_cast<std::uint64_t>(header.subMeshCount) * sizeof(NmoSubMesh) > _cursor.SizeBytes())
    return _cursor.Reject(3, "submesh count {} cannot fit the mesh blob", header.subMeshCount);

  if (!_cursor.Section(header.materialsOffset, header.materialCount, "material") ||
      !_cursor.Section(header.subMeshesOffset, header.subMeshCount, "submesh") ||
      !_cursor.Section(header.indexBuffersOffset, header.indexBufferCount, "index buffer") ||
      !_cursor.Section(header.vertexBuffersOffset, header.vertexBufferCount, "vertex buffer") ||
      !_cursor.Section(header.skinBuffersOffset, header.skinBufferCount, "skin buffer") ||
      !_cursor.Section(header.bonesOffset, header.boneCount, "mesh bone") ||
      !_cursor.Section(header.clipsOffset, header.clipCount, "mesh clip"))
    return false;

  // Clause 4's pairing rule, at the level it is actually stated: skin buffer i is the companion of
  // vertex buffer i, so the count is 0 or equal. Checked before the sections are walked, because a
  // count that is neither reads a buffer header out of bytes that are not one and would otherwise
  // report whatever those bytes happened to say.
  if (header.skinBufferCount != 0 && header.skinBufferCount != header.vertexBufferCount)
    return _cursor.Reject(4, "{} skin buffers for {} vertex buffers; the count is 0 or equal", header.skinBufferCount,
                          header.vertexBufferCount);

  std::uint64_t at = header.materialsOffset;
  _out.materials.reserve(header.materialCount);
  for (std::uint32_t index = 0; index < header.materialCount; ++index)
  {
    std::string_view name;
    if (!_cursor.ReadString(at, "material name", name, at))
      return false;
    NmoMaterial material = {};
    if (!_cursor.Read(at, 3, "material", material))
      return false;
    at += sizeof(NmoMaterial);
    for (std::uint32_t slot = 0; slot < NMO_TEXTURE_SLOTS; ++slot)
    {
      std::string_view texture;
      if (!_cursor.ReadString(at, "material texture", texture, at))
        return false;
    }
    _out.materials.push_back(material);
  }

  if (!ReadBuffers(_cursor, header.indexBuffersOffset, header.indexBufferCount, "index", _out.indexBuffers) ||
      !ReadBuffers(_cursor, header.vertexBuffersOffset, header.vertexBufferCount, "vertex", _out.vertexBuffers) ||
      !ReadBuffers(_cursor, header.skinBuffersOffset, header.skinBufferCount, "skin", _out.skinBuffers))
    return false;

  // Clause 4: a format has to be one this build knows, and its stride has to agree with it.
  for (std::size_t index = 0; index < _out.indexBuffers.size(); ++index)
  {
    const BufferView& buffer = _out.indexBuffers[index];
    const std::uint32_t stride = IndexStride(buffer.format);
    if (stride == 0)
      return _cursor.Reject(4, "index buffer {} carries unknown format {}", index, buffer.format);
    if (buffer.strideBytes != stride)
      return _cursor.Reject(4, "index buffer {} stride {} does not match format {}", index, buffer.strideBytes, buffer.format);
  }
  for (std::size_t index = 0; index < _out.vertexBuffers.size(); ++index)
  {
    const BufferView& buffer = _out.vertexBuffers[index];
    if (buffer.format != static_cast<std::uint32_t>(NmoVertexFormat::Standard) || buffer.strideBytes != sizeof(NmoVertex))
      return _cursor.Reject(4, "vertex buffer {} format {} stride {} is not Standard/36", index, buffer.format, buffer.strideBytes);
  }
  for (std::size_t index = 0; index < _out.skinBuffers.size(); ++index)
  {
    const BufferView& buffer = _out.skinBuffers[index];
    if (buffer.format != static_cast<std::uint32_t>(NmoSkinFormat::Standard) || buffer.strideBytes != sizeof(NmoSkinVertex))
      return _cursor.Reject(4, "skin buffer {} format {} stride {} is not Standard/32", index, buffer.format, buffer.strideBytes);
  }

  // And the other half of it: a companion holds either no elements at all (that buffer is rigid) or
  // one per vertex.
  for (std::size_t index = 0; index < _out.skinBuffers.size(); ++index)
  {
    const std::uint32_t paired = _out.vertexBuffers[index].elementCount;
    const std::uint32_t skins = _out.skinBuffers[index].elementCount;
    if (skins != 0 && skins != paired)
      return _cursor.Reject(4, "skin buffer {} holds {} elements for a {}-vertex buffer", index, skins, paired);
  }

  if (header.extentsOffset != 0)
  {
    if (!_cursor.Read(header.extentsOffset, 3, "mesh extents", _out.extents))
      return false;
    _out.hasExtents = true;
  }

  if (!ReadBoneRecords(_cursor, header.bonesOffset, header.boneCount, "mesh", _out.bones) ||
      !ReadClipRecords(_cursor, header.clipsOffset, header.clipCount, "mesh", _out.clips))
    return false;

  at = header.subMeshesOffset;
  _out.subMeshes.reserve(header.subMeshCount);
  for (std::uint32_t index = 0; index < header.subMeshCount; ++index)
  {
    SubMeshView sub;
    if (!_cursor.Read(at, 3, "submesh", sub.record))
      return false;
    at += sizeof(NmoSubMesh);

    if (sub.record.nameOffset != 0)
    {
      std::uint64_t unused = 0;
      if (!_cursor.ReadString(sub.record.nameOffset, "submesh name", sub.name, unused))
        return false;
      sub.named = true;
    }
    if (!_cursor.Section(sub.record.bonesOffset, sub.record.boneCount, "submesh bone") ||
        !_cursor.Section(sub.record.clipsOffset, sub.record.clipCount, "submesh clip") ||
        !_cursor.Section(sub.record.markersOffset, sub.record.markerCount, "marker"))
      return false;
    if (!ReadBoneRecords(_cursor, sub.record.bonesOffset, sub.record.boneCount, "submesh", sub.bones) ||
        !ReadClipRecords(_cursor, sub.record.clipsOffset, sub.record.clipCount, "submesh", sub.clips) ||
        !ReadMarkerRecords(_cursor, sub.record.markersOffset, sub.record.markerCount, sub.markers))
      return false;
    if (sub.record.facetsOffset != 0)
    {
      if (!_cursor.Take(sub.record.facetsOffset, sub.record.primitiveCount, sizeof(std::uint32_t), 3, "facet ids", sub.facets))
        return false;
      sub.hasFacets = true;
    }
    _out.subMeshes.push_back(std::move(sub));
  }
  return true;
}

// --- clauses 5 to 9: structure -----------------------------------------------------------------

bool ValidateBoneTable(const Cursor& _cursor, const std::vector<BoneView>& _bones, std::uint32_t _meshBoneCount, const char* _scope,
                       bool _isMeshScope)
{
  for (std::size_t index = 0; index < _bones.size(); ++index)
  {
    const BoneView& bone = _bones[index];
    for (std::size_t other = 0; other < index; ++other)
    {
      if (_bones[other].name == bone.name)
        return _cursor.Reject(6, "{} bone name '{}' is not unique", _scope, bone.name);
    }
    if (bone.parentIndex < NMO_NO_PARENT || static_cast<std::int64_t>(bone.parentIndex) >= static_cast<std::int64_t>(index))
      return _cursor.Reject(6, "{} bone '{}' parent {} is not before it in the table", _scope, bone.name, bone.parentIndex);
    if (_isMeshScope)
    {
      if (bone.meshBoneIndex != NMO_NO_BONE)
        return _cursor.Reject(6, "mesh bone '{}' carries meshBoneIndex {}; mesh-scope records carry -1", bone.name, bone.meshBoneIndex);
    }
    else if (bone.meshBoneIndex != NMO_NO_BONE &&
             (bone.meshBoneIndex < 0 || static_cast<std::uint32_t>(bone.meshBoneIndex) >= _meshBoneCount))
    {
      return _cursor.Reject(6, "{} bone '{}' aliases mesh bone {} of {}", _scope, bone.name, bone.meshBoneIndex, _meshBoneCount);
    }
  }
  return true;
}

float KeyTime(const KeySeries& _series, std::uint32_t _index) noexcept
{
  float time = 0.0f;
  std::memcpy(&time, _series.bytes.data() + static_cast<std::size_t>(_index) * _series.strideBytes, sizeof(float));
  return time;
}

bool ValidateClips(const Cursor& _cursor, const std::vector<ClipView>& _clips, std::size_t _scopeSize, const char* _scope)
{
  for (std::size_t index = 0; index < _clips.size(); ++index)
  {
    const ClipView& clip = _clips[index];
    for (std::size_t other = 0; other < index; ++other)
    {
      if (_clips[other].name == clip.name)
        return _cursor.Reject(7, "{} clip name '{}' is not unique", _scope, clip.name);
    }
    if (!(clip.endSeconds >= clip.startSeconds))
      return _cursor.Reject(7, "clip '{}' ends ({}) before it starts ({})", clip.name, clip.endSeconds, clip.startSeconds);

    std::int64_t previousBone = -1;
    for (const TrackView& track : clip.tracks)
    {
      if (static_cast<std::int64_t>(track.boneIndex) <= previousBone)
        return _cursor.Reject(7, "clip '{}' tracks are not strictly increasing by bone", clip.name);
      previousBone = track.boneIndex;
      if (track.boneIndex >= _scopeSize)
        return _cursor.Reject(7, "clip '{}' keys bone {} of a {}-bone scope", clip.name, track.boneIndex, _scopeSize);
      for (const KeySeries* series : {&track.translation, &track.rotation, &track.scale})
      {
        for (std::uint32_t key = 1; key < series->count; ++key)
        {
          if (!(KeyTime(*series, key) > KeyTime(*series, key - 1)))
            return _cursor.Reject(7, "clip '{}' bone {} keys are not strictly increasing in time", clip.name, track.boneIndex);
        }
      }
    }
  }
  return true;
}

bool ValidateMesh(const Cursor& _cursor, const MeshView& _mesh)
{
  if (!ValidateBoneTable(_cursor, _mesh.bones, 0, "mesh", true) || !ValidateClips(_cursor, _mesh.clips, _mesh.bones.size(), "mesh"))
    return false;

  // Clause 8's overlap rule: which submesh bone scope has claimed each skinned vertex. Two scopes
  // over one vertex would mean two answers to "which palette does this skin against", and the
  // slice that finally skins would have to guess.
  std::vector<std::vector<const void*>> claims;
  if (!_mesh.skinBuffers.empty())
  {
    claims.resize(_mesh.vertexBuffers.size());
    for (std::size_t buffer = 0; buffer < _mesh.vertexBuffers.size(); ++buffer)
    {
      claims[buffer].assign(_mesh.vertexBuffers[buffer].elementCount, nullptr);
    }
  }

  for (std::size_t index = 0; index < _mesh.subMeshes.size(); ++index)
  {
    const SubMeshView& sub = _mesh.subMeshes[index];
    const NmoSubMesh& record = sub.record;

    if (record.materialIndex >= _mesh.materials.size())
      return _cursor.Reject(5, "submesh {} names material {} of {}", index, record.materialIndex, _mesh.materials.size());
    if (record.indexBufferIndex >= _mesh.indexBuffers.size())
      return _cursor.Reject(5, "submesh {} names index buffer {} of {}", index, record.indexBufferIndex, _mesh.indexBuffers.size());
    if (record.vertexBufferIndex >= _mesh.vertexBuffers.size())
      return _cursor.Reject(5, "submesh {} names vertex buffer {} of {}", index, record.vertexBufferIndex, _mesh.vertexBuffers.size());

    const BufferView& indexBuffer = _mesh.indexBuffers[record.indexBufferIndex];
    const BufferView& vertexBuffer = _mesh.vertexBuffers[record.vertexBufferIndex];
    const std::uint64_t indexEnd = static_cast<std::uint64_t>(record.startIndex) + 3ull * record.primitiveCount;
    if (indexEnd > indexBuffer.elementCount)
      return _cursor.Reject(5, "submesh {} index range {}..{} escapes its {}-index buffer", index, record.startIndex, indexEnd,
                            indexBuffer.elementCount);
    if (record.minVertex < record.baseVertex)
      return _cursor.Reject(5, "submesh {} minVertex {} is below baseVertex {}", index, record.minVertex, record.baseVertex);
    const std::uint64_t vertexEnd = static_cast<std::uint64_t>(record.minVertex) + record.vertexCount;
    if (vertexEnd > vertexBuffer.elementCount)
      return _cursor.Reject(5, "submesh {} vertex window {}..{} escapes its {}-vertex buffer", index, record.minVertex, vertexEnd,
                            vertexBuffer.elementCount);

    for (std::uint64_t step = 0; step < 3ull * record.primitiveCount; ++step)
    {
      const std::uint64_t biased = static_cast<std::uint64_t>(ReadIndex(indexBuffer, record.startIndex + step)) + record.baseVertex;
      if (biased < record.minVertex || biased >= vertexEnd)
        return _cursor.Reject(5, "submesh {} uses vertex {} outside [{}, {})", index, biased, record.minVertex, vertexEnd);
    }

    // "The facet section holds exactly primitiveCount ids" needs no comparison here: the section
    // carries no length of its own, so the take that measured primitiveCount * 4 against the blob
    // in ReadMeshBlob *is* the clause, and a section that cannot hold them was already rejected.

    if (sub.named)
    {
      for (std::size_t other = 0; other < index; ++other)
      {
        if (_mesh.subMeshes[other].named && _mesh.subMeshes[other].name == sub.name)
          return _cursor.Reject(5, "submesh name '{}' is not unique", sub.name);
      }
    }

    if (!ValidateBoneTable(_cursor, sub.bones, static_cast<std::uint32_t>(_mesh.bones.size()), "submesh", false))
      return false;
    const std::size_t scopeSize = sub.bones.empty() ? _mesh.bones.size() : sub.bones.size();
    if (!ValidateClips(_cursor, sub.clips, scopeSize, "submesh"))
      return false;

    for (std::size_t marker = 0; marker < sub.markers.size(); ++marker)
    {
      const MarkerView& view = sub.markers[marker];
      for (std::size_t other = 0; other < marker; ++other)
      {
        if (sub.markers[other].name == view.name)
          return _cursor.Reject(9, "submesh {} marker name '{}' is not unique", index, view.name);
      }
      if (view.record.parentBone != NMO_NO_BONE &&
          (view.record.parentBone < 0 || static_cast<std::size_t>(view.record.parentBone) >= scopeSize))
        return _cursor.Reject(9, "marker '{}' rides bone {} of a {}-bone scope", view.name, view.record.parentBone, scopeSize);
    }

    // Clause 8: a submesh whose vertex buffer has a non-empty companion is skinned.
    if (_mesh.skinBuffers.empty() || _mesh.skinBuffers[record.vertexBufferIndex].elementCount == 0)
      continue;
    if (scopeSize == 0)
      return _cursor.Reject(8, "submesh {} is skinned but has no bone scope", index);

    const BufferView& skinBuffer = _mesh.skinBuffers[record.vertexBufferIndex];
    const void* scopeKey = sub.bones.empty() ? static_cast<const void*>(&_mesh.bones) : static_cast<const void*>(&sub.bones);
    for (std::uint32_t vertex = record.minVertex; vertex < vertexEnd; ++vertex)
    {
      NmoSkinVertex skin = {};
      std::memcpy(&skin, skinBuffer.payload.data() + static_cast<std::size_t>(vertex) * sizeof(NmoSkinVertex), sizeof(skin));
      float sum = 0.0f;
      for (std::uint32_t influence = 0; influence < NMO_BONE_INFLUENCES; ++influence)
      {
        if (skin.boneIndex[influence] >= scopeSize)
          return _cursor.Reject(8, "submesh {} vertex {} skins bone {} of a {}-bone scope", index, vertex, skin.boneIndex[influence],
                                scopeSize);
        if (skin.boneWeight[influence] < 0.0f)
          return _cursor.Reject(8, "submesh {} vertex {} has a negative weight", index, vertex);
        if (influence > 0 && skin.boneWeight[influence] > skin.boneWeight[influence - 1])
          return _cursor.Reject(8, "submesh {} vertex {} weights are not descending", index, vertex);
        sum += skin.boneWeight[influence];
      }
      if (std::fabs(sum - 1.0f) > WEIGHT_SUM_TOLERANCE)
        return _cursor.Reject(8, "submesh {} vertex {} weights sum to {}", index, vertex, sum);

      const void*& owner = claims[record.vertexBufferIndex][vertex];
      if (owner == nullptr)
        owner = scopeKey;
      else if (owner != scopeKey)
        return _cursor.Reject(8, "vertex {} of buffer {} is skinned by two different bone scopes", vertex, record.vertexBufferIndex);
    }
  }
  return true;
}

// --- expansion --------------------------------------------------------------------------------

// The one rule that decides what a hull's colour is, and it is worth the comment it needs.
//
// The two conversion paths in Tools/ fill the file's two colour fields differently: a Blender
// export writes white vertices and puts the authored colour in the material, which is what a mesh
// with no colour attribute produces and so what every hull authored in a modelling package looks
// like, while ObjToNmo.py bakes Kd into both. Under this multiply the first is exactly right and
// the second comes out squared -- which is the correct trade, because the shipping corpus is the
// first kind and ObjToNmo.py is now the OBJ path's record rather than the content pipeline
// (Design/Decisions/0035). If a converted OBJ hull ever looks dark, this is why.
MeshVertex ToMeshVertex(const NmoVertex& _vertex, const XMFLOAT4& _baseColour, float _race) noexcept
{
  const float r = static_cast<float>(_vertex.colour & 0xFFu) / COLOUR_MAX;
  const float g = static_cast<float>((_vertex.colour >> 8) & 0xFFu) / COLOUR_MAX;
  const float b = static_cast<float>((_vertex.colour >> 16) & 0xFFu) / COLOUR_MAX;
  return MeshVertex{
    _vertex.position.x, _vertex.position.y, _vertex.position.z, r * _baseColour.x, g * _baseColour.y, b * _baseColour.z, _race};
}

bool Expand(const Cursor& _cursor, const MeshView& _mesh, MeshData& _outMesh)
{
  std::uint64_t triangles = 0;
  std::size_t markers = 0;
  for (const SubMeshView& sub : _mesh.subMeshes)
  {
    triangles += sub.record.primitiveCount;
    markers += sub.markers.size();
  }
  _outMesh.verts.reserve(static_cast<std::size_t>(triangles) * 3);
  _outMesh.markers.reserve(markers);
  _outMesh.subMeshes.reserve(_mesh.subMeshes.size());

  std::vector<std::string_view> names; // kept only for the collision sweep below
  names.reserve(markers + _mesh.subMeshes.size());
  std::vector<std::string_view> partNames;
  partNames.reserve(_mesh.subMeshes.size());

  for (const SubMeshView& sub : _mesh.subMeshes)
  {
    // Where this part's run begins. Expand has always emitted the submeshes in order and
    // contiguously; all that is new is writing down where each one started
    // (Design/Archive/Combat-slice-3.md 2.3).
    MeshSubMesh part;
    part.nameHash = sub.named ? Fnv1a(sub.name) : 0u;
    part.firstVertex = static_cast<std::uint32_t>(_outMesh.verts.size());
    part.firstMarker = static_cast<std::uint32_t>(_outMesh.markers.size());

    const NmoSubMesh& record = sub.record;
    const NmoMaterial& material = _mesh.materials[record.materialIndex];
    // A submesh has exactly one material, so the flag is read once here and written to every vertex
    // the submesh emits: no name matching, no lookup, no heuristic (Design/Archive/NmoFormat.md 5.5).
    const float race = (material.renderFlags & static_cast<std::uint32_t>(NmoRenderFlags::RaceTinted)) != 0 ? 1.0f : 0.0f;
    const BufferView& indexBuffer = _mesh.indexBuffers[record.indexBufferIndex];
    const BufferView& vertexBuffer = _mesh.vertexBuffers[record.vertexBufferIndex];

    for (std::uint64_t step = 0; step < 3ull * record.primitiveCount; ++step)
    {
      const std::size_t vertexIndex = static_cast<std::size_t>(ReadIndex(indexBuffer, record.startIndex + step)) + record.baseVertex;
      NmoVertex vertex = {};
      std::memcpy(&vertex, vertexBuffer.payload.data() + vertexIndex * sizeof(NmoVertex), sizeof(vertex));
      _outMesh.verts.push_back(ToMeshVertex(vertex, material.baseColour, race));
    }

    for (const MarkerView& view : sub.markers)
    {
      MeshMarker marker;
      marker.kind = KindFromString(view.kind);
      marker.nameHash = Fnv1a(view.name);
      marker.position = view.record.position;
      marker.orientation = view.record.orientation;
      marker.scale = view.record.scale;
      marker.colour = view.record.colour;
      marker.param0 = view.record.param0;
      marker.param1 = view.record.param1;
      marker.raceTinted = (view.record.flags & static_cast<std::uint32_t>(NmoMarkerFlags::RaceTinted)) != 0;
      marker.parentBone = view.record.parentBone;
      _outMesh.markers.push_back(marker);
      names.push_back(view.name);
    }

    part.vertexCount = static_cast<std::uint32_t>(_outMesh.verts.size()) - part.firstVertex;
    part.markerCount = static_cast<std::uint32_t>(_outMesh.markers.size()) - part.firstMarker;

    // The part's own bind-pose bounds, stated by the file or accumulated from what it just emitted.
    // The absent case is the mesh-level one exactly: a writer that did not state something it was
    // not obliged to state, rather than anything being repaired.
    if (record.extents.boxMin.x <= record.extents.boxMax.x)
    {
      part.boundsMin = record.extents.boxMin;
      part.boundsMax = record.extents.boxMax;
    }
    else
    {
      XMFLOAT3 partMin(1e30f, 1e30f, 1e30f);
      XMFLOAT3 partMax(-1e30f, -1e30f, -1e30f);
      for (std::uint32_t at = 0; at < part.vertexCount; ++at)
      {
        const MeshVertex& vertex = _outMesh.verts[part.firstVertex + at];
        partMin = XMFLOAT3(std::min(partMin.x, vertex.px), std::min(partMin.y, vertex.py), std::min(partMin.z, vertex.pz));
        partMax = XMFLOAT3(std::max(partMax.x, vertex.px), std::max(partMax.y, vertex.py), std::max(partMax.z, vertex.pz));
      }
      if (partMin.x <= partMax.x)
      {
        part.boundsMin = partMin;
        part.boundsMax = partMax;
      }
    }

    _outMesh.subMeshes.push_back(part);
    if (sub.named)
      partNames.push_back(sub.name);
  }

  // Names are hashed and never stored, so two names that hash alike are one marker as far as every
  // consumer is concerned. That is a load failure naming both, for the tool to rename around,
  // rather than an engine quietly picking one (Design/Archive/NmoFormat.md 5.10).
  for (std::size_t index = 0; index < names.size(); ++index)
  {
    for (std::size_t earlier = 0; earlier < index; ++earlier)
    {
      if (_outMesh.markers[earlier].nameHash == _outMesh.markers[index].nameHash && names[earlier] != names[index])
        return _cursor.Reject(9, "marker names '{}' and '{}' collide under FNV-1a; rename one", names[earlier], names[index]);
    }
  }

  // And the same sweep over the parts, because a submesh name is hashed on the same terms and a
  // consumer addressing a turret by hash has the same right not to get the wrong one. Unnamed parts
  // are left out of it: they all hash to zero on purpose and are addressed by index.
  for (std::size_t index = 0; index < partNames.size(); ++index)
  {
    for (std::size_t earlier = 0; earlier < index; ++earlier)
    {
      if (Fnv1a(partNames[earlier]) == Fnv1a(partNames[index]) && partNames[earlier] != partNames[index])
        return _cursor.Reject(9, "submesh names '{}' and '{}' collide under FNV-1a; rename one", partNames[earlier], partNames[index]);
    }
  }

  if (_mesh.hasExtents)
  {
    _outMesh.boundsMin = _mesh.extents.boxMin;
    _outMesh.boundsMax = _mesh.extents.boxMax;
  }
  else
  {
    // A file without an extents section is legal (0 means absent), and every consumer divides by
    // these, so they are accumulated rather than left at zero. Nothing is being repaired here: the
    // writer simply did not state something it was not obliged to state.
    XMFLOAT3 boundsMin(1e30f, 1e30f, 1e30f);
    XMFLOAT3 boundsMax(-1e30f, -1e30f, -1e30f);
    for (const MeshVertex& vertex : _outMesh.verts)
    {
      boundsMin = XMFLOAT3(std::min(boundsMin.x, vertex.px), std::min(boundsMin.y, vertex.py), std::min(boundsMin.z, vertex.pz));
      boundsMax = XMFLOAT3(std::max(boundsMax.x, vertex.px), std::max(boundsMax.y, vertex.py), std::max(boundsMax.z, vertex.pz));
    }
    if (boundsMin.x > boundsMax.x)
    {
      boundsMin = XMFLOAT3(0.0f, 0.0f, 0.0f);
      boundsMax = XMFLOAT3(0.0f, 0.0f, 0.0f);
    }
    _outMesh.boundsMin = boundsMin;
    _outMesh.boundsMax = boundsMax;
  }
  return true;
}
} // namespace

bool NmoReader::Load(const std::wstring& _dir, const std::wstring& _name, MeshData& _outMesh)
{
  const std::string label = Narrow(_name) + ".nmo";
  const ByteBuffer file = BinaryFile::ReadFile(_dir + _name + L".nmo");
  if (file.empty())
  {
    DebugTrace("{}: cannot be read\n", label);
    return false;
  }

  const std::span<const std::uint8_t> bytes(file);
  const Cursor whole(bytes, label);

  // Clause 1: the file header.
  if (bytes.size() < sizeof(NmoFileHeader))
    return whole.Reject(1, "{} bytes cannot hold a file header", bytes.size());
  NmoFileHeader header = {};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (header.magic != NMO_FILE_MAGIC)
    return whole.Reject(1, "magic {:#010x} is not \"NMO2\"", header.magic);
  if (header.versionMajor != NMO_VERSION_MAJOR)
    return whole.Reject(1, "major version {} is not {}", header.versionMajor, NMO_VERSION_MAJOR);
  if (header.headerBytes != sizeof(NmoFileHeader))
    return whole.Reject(1, "headerBytes {} is not {}", header.headerBytes, sizeof(NmoFileHeader));
  if (header.fileBytes != bytes.size())
    return whole.Reject(1, "fileBytes {} but {} bytes were read", header.fileBytes, bytes.size());
  if (header.meshCount > NMO_MAX_MESHES)
    return whole.Reject(1, "mesh count {} exceeds the sanity cap {}", header.meshCount, NMO_MAX_MESHES);

  // Clause 10: a minor version this build has never heard of loads. That is the whole of the
  // additive-versioning mechanism -- reserved fields and undefined flag bits are ignorable by
  // construction (5.13), so there is nothing to do here but say so.
  if (header.versionMinor > NMO_VERSION_MINOR)
    DebugTrace("{}: minor version {} is newer than {}; the sections this build knows still load\n", label, header.versionMinor,
               NMO_VERSION_MINOR);

#if defined(_DEBUG)
  // Written by tools and checked by tools and debug builds; a release loader may skip it (5.3).
  if (header.payloadCrc32 != 0)
  {
    const std::uint32_t actual = Crc32(bytes.subspan(header.headerBytes));
    if (actual != header.payloadCrc32)
      return whole.Reject(1, "payload CRC {:#010x} does not match the stored {:#010x}", actual, header.payloadCrc32);
  }
#endif

  // Clause 2: the mesh directory, in 64-bit arithmetic before a single byte of it is trusted.
  const std::uint64_t directoryEnd = static_cast<std::uint64_t>(header.headerBytes) + sizeof(NmoMeshRef) * header.meshCount;
  if (directoryEnd > bytes.size())
    return whole.Reject(2, "mesh directory of {} entries escapes the file", header.meshCount);
  if (header.meshCount == 0)
    return whole.Reject(2, "the file carries no mesh");

  std::vector<NmoMeshRef> directory(header.meshCount);
  for (std::uint32_t index = 0; index < header.meshCount; ++index)
  {
    NmoMeshRef& ref = directory[index];
    std::memcpy(&ref, bytes.data() + header.headerBytes + sizeof(NmoMeshRef) * index, sizeof(ref));
    if (ref.offsetBytes % BUFFER_ALIGNMENT != 0)
      return whole.Reject(2, "mesh {} blob at {} is not 16-aligned", index, ref.offsetBytes);
    const std::uint64_t end = static_cast<std::uint64_t>(ref.offsetBytes) + ref.lengthBytes;
    if (ref.offsetBytes < directoryEnd || end > bytes.size())
      return whole.Reject(2, "mesh {} window {}..{} escapes [{}, {})", index, ref.offsetBytes, end, directoryEnd, bytes.size());
  }

  // Mesh 0 is the hull. A file carrying more than one is legal and this engine has no consumer for
  // a second, so the extras are a diagnostic rather than a rejection.
  if (header.meshCount > 1)
    DebugTrace("{}: {} meshes; drawing the first\n", label, header.meshCount);

  const Cursor blob(bytes.subspan(directory[0].offsetBytes, directory[0].lengthBytes), label);
  if (blob.SizeBytes() < sizeof(NmoMeshHeader))
    return blob.Reject(3, "a {}-byte mesh blob cannot hold a mesh header", blob.SizeBytes());

  MeshView mesh;
  if (!ReadMeshBlob(blob, mesh))
    return false;
  if (!ValidateMesh(blob, mesh))
    return false;

  MeshData loaded;
  if (!Expand(blob, mesh, loaded))
    return false;
  if (loaded.verts.empty())
  {
    DebugTrace("{}: holds no triangles\n", label);
    return false;
  }

  std::uint32_t exhausts = 0;
  std::uint32_t navLights = 0;
  std::uint32_t guns = 0;
  for (const MeshMarker& marker : loaded.markers)
  {
    if (marker.kind == MarkerKind::Exhaust)
      ++exhausts;
    else if (marker.kind == MarkerKind::NavLight)
      ++navLights;
    else if (marker.kind == MarkerKind::Gun)
      ++guns;
  }
  DebugTrace("{}: {} tris, {:.1f} x {:.1f} x {:.1f}, {} markers ({} exhaust, {} nav, {} gun)\n", label, loaded.verts.size() / 3,
             loaded.boundsMax.x - loaded.boundsMin.x, loaded.boundsMax.y - loaded.boundsMin.y, loaded.boundsMax.z - loaded.boundsMin.z,
             loaded.markers.size(), exhausts, navLights, guns);

  _outMesh = std::move(loaded);
  return true;
}
} // namespace Neuron
