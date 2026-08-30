#include "pch.h"

// The C++ half of Design/NmoFormat.md section 12: one deliberately corrupted file per section 5.12
// clause over the same golden bytes the Python codec is tested against.
//
// Assets\NmoFixture.nmo is committed rather than generated at test time. That is the narrow,
// deliberate exception to "nothing generated is committed" recorded as decision D3 in section 15 of
// the design: Tools/NmoFixture.py is its generator and its diff is its review, so regenerating the
// file and comparing it byte for byte is part of the acceptance of any slice that touches it. The
// alternative was a python dependency in the VS suite, which is worse.
//
// Every corruption locates its field through the header rather than at a hard-coded absolute
// offset, so these tests keep testing the rule they name when the fixture changes.

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DirectX;

namespace NeuronClientTests
{
namespace
{
constexpr wchar_t FIXTURE_NAME[] = L"NmoFixture";
constexpr wchar_t CORRUPT_NAME[] = L"NmoCorrupt"; // every rejection test writes its own copy here

constexpr std::uint32_t FIXTURE_TRIANGLES = 16; // 12 hull + 4 turret
constexpr std::uint32_t FIXTURE_HULL_VERTICES = 36;
constexpr std::uint32_t FIXTURE_TURRET_VERTICES = 12;
constexpr std::uint32_t FIXTURE_MARKERS = 6;

template <typename T> T Peek(const Neuron::ByteBuffer& _bytes, std::size_t _offset)
{
  T value = {};
  std::memcpy(&value, _bytes.data() + _offset, sizeof(T));
  return value;
}

template <typename T> void Poke(Neuron::ByteBuffer& _bytes, std::size_t _offset, const T& _value)
{
  std::memcpy(_bytes.data() + _offset, &_value, sizeof(T));
}

// A String is a uint32 length, that many bytes, and zero padding to the next 4 (design 5.1).
std::size_t AfterString(const Neuron::ByteBuffer& _bytes, std::size_t _at)
{
  const std::uint32_t lengthBytes = Peek<std::uint32_t>(_bytes, _at);
  return _at + sizeof(std::uint32_t) + lengthBytes + ((4 - lengthBytes % 4) % 4);
}

Neuron::ByteBuffer ReadFixture()
{
  return Neuron::BinaryFile::ReadFile(std::wstring(FIXTURE_NAME) + L".nmo");
}

std::size_t BlobStart(const Neuron::ByteBuffer& _bytes)
{
  return Peek<Neuron::NmoMeshRef>(_bytes, sizeof(Neuron::NmoFileHeader)).offsetBytes;
}

Neuron::NmoMeshHeader MeshHeader(const Neuron::ByteBuffer& _bytes)
{
  return Peek<Neuron::NmoMeshHeader>(_bytes, BlobStart(_bytes));
}

// File offset of submesh _index's record.
std::size_t SubMeshAt(const Neuron::ByteBuffer& _bytes, std::uint32_t _index)
{
  return BlobStart(_bytes) + MeshHeader(_bytes).subMeshesOffset + _index * sizeof(Neuron::NmoSubMesh);
}

// File offset of bone _index's NmoBone, walking the name strings that separate the records.
std::size_t BoneAt(const Neuron::ByteBuffer& _bytes, std::uint32_t _sectionOffset, std::uint32_t _index)
{
  std::size_t at = BlobStart(_bytes) + _sectionOffset;
  for (std::uint32_t bone = 0; bone < _index; ++bone)
  {
    at = AfterString(_bytes, at) + sizeof(Neuron::NmoBone);
  }
  return AfterString(_bytes, at);
}

// File offset of marker _index's name string. The NmoMarker sits two strings further on.
std::size_t MarkerNameAt(const Neuron::ByteBuffer& _bytes, std::uint32_t _sectionOffset, std::uint32_t _index)
{
  std::size_t at = BlobStart(_bytes) + _sectionOffset;
  for (std::uint32_t marker = 0; marker < _index; ++marker)
  {
    at = AfterString(_bytes, AfterString(_bytes, at)) + sizeof(Neuron::NmoMarker);
  }
  return at;
}

std::size_t MarkerAt(const Neuron::ByteBuffer& _bytes, std::uint32_t _sectionOffset, std::uint32_t _index)
{
  return AfterString(_bytes, AfterString(_bytes, MarkerNameAt(_bytes, _sectionOffset, _index)));
}

// The turret's only clip, whose record is name / NmoClip / tracks / keys (design 5.9).
std::size_t TurretClipAt(const Neuron::ByteBuffer& _bytes)
{
  const Neuron::NmoSubMesh turret = Peek<Neuron::NmoSubMesh>(_bytes, SubMeshAt(_bytes, 1));
  return AfterString(_bytes, BlobStart(_bytes) + turret.clipsOffset);
}

// A corrupted payload would fail the CRC before it reached the clause under test, so every patch
// clears it -- exactly as the Python suite's clear_crc does, and for the same reason.
void ClearCrc(Neuron::ByteBuffer& _bytes)
{
  Neuron::NmoFileHeader header = Peek<Neuron::NmoFileHeader>(_bytes, 0);
  header.payloadCrc32 = 0;
  Poke(_bytes, 0, header);
}

// Writes _bytes beside the fixture and asserts the reader refuses them and leaves the mesh alone.
void ExpectRejected(Neuron::ByteBuffer& _bytes, const wchar_t* _what, bool _clearCrc = true)
{
  if (_clearCrc)
    ClearCrc(_bytes);
  Assert::IsTrue(Neuron::BinaryFile::WriteFile(std::wstring(CORRUPT_NAME) + L".nmo", _bytes), _what);

  Neuron::MeshData mesh;
  Assert::IsFalse(Neuron::NmoReader::Load(L"", CORRUPT_NAME, mesh), _what);
  Assert::IsTrue(mesh.Empty(), L"a rejected file left vertices behind");
  Assert::IsTrue(mesh.markers.empty(), L"a rejected file left markers behind");
}

// --- a file built rather than corrupted ---------------------------------------------------------
// Two of the rules cannot be reached by bending one field of the fixture: an FNV-1a collision is a
// pair of names found by search and will not be the length of any name the fixture holds, and no
// two names in one fixture table share a padded footprint, so neither can be renamed onto the
// other in place. Both get the smallest legal file that carries them instead.

struct Writer
{
  Neuron::ByteBuffer bytes;

  std::size_t At() const
  {
    return bytes.size();
  }

  void Raw(const void* _data, std::size_t _sizeBytes)
  {
    const std::uint8_t* from = static_cast<const std::uint8_t*>(_data);
    bytes.insert(bytes.end(), from, from + _sizeBytes);
  }

  template <typename T> void Put(const T& _value)
  {
    Raw(&_value, sizeof(T));
  }

  void Text(std::string_view _text)
  {
    Put(static_cast<std::uint32_t>(_text.size()));
    Raw(_text.data(), _text.size());
    while (bytes.size() % 4 != 0)
    {
      bytes.push_back(0);
    }
  }

  void Align(std::size_t _to)
  {
    while (bytes.size() % _to != 0)
    {
      bytes.push_back(0);
    }
  }
};

struct MinimalNmo
{
  std::vector<std::string> markerNames;
  std::vector<std::string> boneNames;
  std::vector<std::uint32_t> clipTrackBones; // one keyless SRT track per entry, in this order
};

// One mesh, one material, one submesh, one triangle, plus whatever names and tracks the caller
// wants. Everything else is the smallest thing the format allows.
Neuron::ByteBuffer MakeMinimalNmo(const MinimalNmo& _spec)
{
  Writer blob;
  Neuron::NmoMeshHeader header = {};
  blob.Put(header); // rewritten once the offsets are known

  header.nameOffset = static_cast<std::uint32_t>(blob.At());
  blob.Text("Probe");

  header.materialCount = 1;
  header.materialsOffset = static_cast<std::uint32_t>(blob.At());
  blob.Text("Skin");
  Neuron::NmoMaterial material = {};
  material.baseColour = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
  blob.Put(material);
  for (std::uint32_t slot = 0; slot < Neuron::NMO_TEXTURE_SLOTS; ++slot)
  {
    blob.Text("");
  }

  header.subMeshCount = 1;
  header.subMeshesOffset = static_cast<std::uint32_t>(blob.At());
  Neuron::NmoSubMesh sub = {};
  const std::size_t subMeshAt = blob.At();
  blob.Put(sub);

  sub.nameOffset = static_cast<std::uint32_t>(blob.At());
  blob.Text("Part");

  sub.markerCount = static_cast<std::uint32_t>(_spec.markerNames.size());
  sub.markersOffset = _spec.markerNames.empty() ? 0 : static_cast<std::uint32_t>(blob.At());
  for (const std::string& name : _spec.markerNames)
  {
    blob.Text(name);
    blob.Text("Gun");
    Neuron::NmoMarker marker = {};
    marker.orientation = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    marker.scale = 1.0f;
    marker.parentBone = Neuron::NMO_NO_BONE;
    marker.colour = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    blob.Put(marker);
  }

  header.boneCount = static_cast<std::uint32_t>(_spec.boneNames.size());
  header.bonesOffset = _spec.boneNames.empty() ? 0 : static_cast<std::uint32_t>(blob.At());
  for (const std::string& name : _spec.boneNames)
  {
    blob.Text(name);
    Neuron::NmoBone bone = {};
    bone.parentIndex = Neuron::NMO_NO_PARENT;
    bone.meshBoneIndex = Neuron::NMO_NO_BONE;
    blob.Put(bone);
  }

  header.clipCount = _spec.clipTrackBones.empty() ? 0 : 1;
  header.clipsOffset = _spec.clipTrackBones.empty() ? 0 : static_cast<std::uint32_t>(blob.At());
  if (!_spec.clipTrackBones.empty())
  {
    blob.Text("Idle");
    Neuron::NmoClip clip = {};
    clip.trackCount = static_cast<std::uint32_t>(_spec.clipTrackBones.size());
    blob.Put(clip);
    for (const std::uint32_t bone : _spec.clipTrackBones)
    {
      Neuron::NmoSrtTrack track = {};
      track.boneIndex = bone; // no keys: a track that only says which bone it is about
      blob.Put(track);
    }
  }

  blob.Align(16);
  header.indexBufferCount = 1;
  header.indexBuffersOffset = static_cast<std::uint32_t>(blob.At());
  Neuron::NmoBufferHeader indexHeader = {};
  indexHeader.format = static_cast<std::uint32_t>(Neuron::NmoIndexFormat::U16);
  indexHeader.strideBytes = sizeof(std::uint16_t);
  indexHeader.elementCount = 3;
  blob.Put(indexHeader);
  for (std::uint16_t index = 0; index < 3; ++index)
  {
    blob.Put(index);
  }
  blob.Align(16);

  header.vertexBufferCount = 1;
  header.vertexBuffersOffset = static_cast<std::uint32_t>(blob.At());
  Neuron::NmoBufferHeader vertexHeader = {};
  vertexHeader.format = static_cast<std::uint32_t>(Neuron::NmoVertexFormat::Standard);
  vertexHeader.strideBytes = sizeof(Neuron::NmoVertex);
  vertexHeader.elementCount = 3;
  blob.Put(vertexHeader);
  const XMFLOAT3 corners[3] = {XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT3(1.0f, 0.0f, 0.0f)};
  for (const XMFLOAT3& corner : corners)
  {
    Neuron::NmoVertex vertex = {};
    vertex.position = corner;
    vertex.normal = XMFLOAT3(0.0f, 0.0f, -1.0f);
    vertex.colour = 0xFFFFFFFFu;
    blob.Put(vertex);
  }
  blob.Align(16);

  header.extentsOffset = static_cast<std::uint32_t>(blob.At());
  Neuron::NmoMeshExtents extents = {};
  extents.boxMax = XMFLOAT3(1.0f, 1.0f, 0.0f);
  extents.centre = XMFLOAT3(0.5f, 0.5f, 0.0f);
  extents.radius = 1.0f;
  blob.Put(extents);
  blob.Align(16);

  sub.primitiveCount = 1;
  sub.vertexCount = 3;
  Poke(blob.bytes, subMeshAt, sub);
  Poke(blob.bytes, 0, header);

  Writer file;
  Neuron::NmoFileHeader fileHeader = {};
  fileHeader.magic = Neuron::NMO_FILE_MAGIC;
  fileHeader.versionMajor = Neuron::NMO_VERSION_MAJOR;
  fileHeader.versionMinor = Neuron::NMO_VERSION_MINOR;
  fileHeader.headerBytes = sizeof(Neuron::NmoFileHeader);
  fileHeader.meshCount = 1;
  file.Put(fileHeader);
  Neuron::NmoMeshRef ref = {};
  file.Put(ref);
  file.Align(16);
  ref.offsetBytes = static_cast<std::uint32_t>(file.At());
  ref.lengthBytes = static_cast<std::uint32_t>(blob.bytes.size());
  file.Raw(blob.bytes.data(), blob.bytes.size());
  Poke(file.bytes, sizeof(Neuron::NmoFileHeader), ref);

  fileHeader.fileBytes = static_cast<std::uint32_t>(file.bytes.size());
  fileHeader.payloadCrc32 = 0; // legal, and not computed: the reader is required to skip a zero
  Poke(file.bytes, 0, fileHeader);
  return file.bytes;
}
} // namespace

TEST_CLASS(NmoReaderTests)
{
public:
  // Runs before every test. No other suite in this project reads a file from disk, so the working
  // directory is proved by TheFixtureIsWhereTheSuiteExpectsIt below rather than assumed here: if
  // vstest runs from somewhere else, one test says so instead of thirty failing anonymously.
  TEST_METHOD_INITIALIZE(PointAtTheOutputAssets)
  {
    Neuron::FileSys::SetHomeDirectory(L"."); // appends \Assets\, the same shape the app uses
  }

  // --- the working directory ------------------------------------------------------------------

  TEST_METHOD(TheFixtureIsWhereTheSuiteExpectsIt)
  {
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh),
                   L"Assets\\NmoFixture.nmo did not load: check the working directory and the project's CopyToOutputDirectory");
  }

  TEST_METHOD(AMissingMeshFailsClosed)
  {
    // Content errors are diagnostics, never crashes. A mesh that is not there reports false and
    // leaves the output alone; it does not throw, and it does not half-fill the mesh.
    Neuron::MeshData mesh;
    Assert::IsFalse(Neuron::NmoReader::Load(L"", L"NoSuchHullExists", mesh), L"a missing mesh reported success");
    Assert::IsTrue(mesh.Empty(), L"a failed load left vertices behind");
  }

  // --- what the fixture is supposed to arrive as -----------------------------------------------

  TEST_METHOD(TheFixtureLoadsItsTriangles)
  {
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh), L"the fixture did not load");
    Assert::AreEqual(static_cast<std::size_t>(FIXTURE_TRIANGLES) * 3, mesh.verts.size(), L"triangle count");
  }

  TEST_METHOD(BoundsComeFromTheFilesExtents)
  {
    // Not recomputed: the writer measured them over the same vertices, and clause 3 has already
    // proved the section is there.
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh), L"the fixture did not load");
    Assert::AreEqual(-2.0f, mesh.boundsMin.x, 1e-5f, L"bounds min x");
    Assert::AreEqual(0.0f, mesh.boundsMin.y, 1e-5f, L"bounds min y");
    Assert::AreEqual(-4.0f, mesh.boundsMin.z, 1e-5f, L"bounds min z");
    Assert::AreEqual(2.0f, mesh.boundsMax.x, 1e-5f, L"bounds max x");
    Assert::AreEqual(3.0f, mesh.boundsMax.y, 1e-5f, L"bounds max y");
    Assert::AreEqual(4.0f, mesh.boundsMax.z, 1e-5f, L"bounds max z");
  }

  TEST_METHOD(EveryMarkerArrivesWithItsKindColourScaleAndParams)
  {
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh), L"the fixture did not load");
    Assert::AreEqual(static_cast<std::size_t>(FIXTURE_MARKERS), mesh.markers.size(), L"marker count");

    // Submesh then file order: the hull's five, then the turret's one.
    Assert::IsTrue(mesh.markers[0].kind == Neuron::MarkerKind::Exhaust, L"marker 0 kind");
    Assert::IsTrue(mesh.markers[2].kind == Neuron::MarkerKind::NavLight, L"marker 2 kind");
    Assert::IsTrue(mesh.markers[4].kind == Neuron::MarkerKind::Gun, L"marker 4 kind");
    Assert::IsTrue(mesh.markers[5].kind == Neuron::MarkerKind::Gun, L"the turret's marker follows the hull's");

    const Neuron::MeshMarker& navPort = mesh.markers[2];
    Assert::AreEqual(-2.0f, navPort.position.x, 1e-5f, L"nav light position");
    Assert::AreEqual(0.25f, navPort.scale, 1e-5f, L"nav light scale");
    Assert::AreEqual(1.0f, navPort.colour.x, 1e-5f, L"nav light red");
    Assert::AreEqual(0.1f, navPort.colour.y, 1e-5f, L"nav light green");
    Assert::AreEqual(2.0f, navPort.param0, 1e-5f, L"nav light blink period");
    Assert::AreEqual(0.5f, mesh.markers[3].param1, 1e-5f, L"nav light blink phase");
  }

  TEST_METHOD(MarkerNamesArriveHashedAndDistinct)
  {
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh), L"the fixture did not load");
    for (std::size_t index = 0; index < mesh.markers.size(); ++index)
    {
      Assert::AreNotEqual(0u, mesh.markers[index].nameHash, L"a marker arrived without a name hash");
      for (std::size_t other = 0; other < index; ++other)
      {
        Assert::AreNotEqual(mesh.markers[other].nameHash, mesh.markers[index].nameHash, L"two markers share a hash");
      }
    }
  }

  TEST_METHOD(TheFlaggedMarkerIsRaceTintedAndTheOtherIsNot)
  {
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh), L"the fixture did not load");
    Assert::IsTrue(mesh.markers[0].raceTinted, L"ExhaustPort carries MARKER_FLAG_RACE_TINTED and did not arrive tinted");
    Assert::IsFalse(mesh.markers[1].raceTinted, L"ExhaustStarboard is unflagged and arrived tinted");
  }

  TEST_METHOD(TheFlaggedMaterialsVerticesCarryRace)
  {
    // The Gunship's GlowStripe is RaceTinted and HullPlate is not, so the two submeshes come out
    // with 1 and 0 in the channel. It is exactly 0 or 1 because the flag is per material and a
    // triangle belongs to one submesh: nothing interpolates across a material seam.
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh), L"the fixture did not load");
    std::size_t ownPaint = 0;
    std::size_t liveried = 0;
    for (const Neuron::MeshVertex& vertex : mesh.verts)
    {
      if (vertex.race == 0.0f)
        ++ownPaint;
      else if (vertex.race == 1.0f)
        ++liveried;
    }
    Assert::AreEqual(static_cast<std::size_t>(FIXTURE_HULL_VERTICES), ownPaint, L"vertices painted by the model");
    Assert::AreEqual(static_cast<std::size_t>(FIXTURE_TURRET_VERTICES), liveried, L"vertices painted by the faction");
  }

  TEST_METHOD(VertexColourIsModulatedByTheMaterial)
  {
    // MeshVertex.rgb = NmoVertex.colour.rgb / 255 * material.baseColour.rgb. The hull's vertices
    // are 0xFF8C9AA8 under a (0.55, 0.6, 0.66) base, which lands all three channels on one grey.
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh), L"the fixture did not load");
    const float expected = (168.0f / 255.0f) * 0.55f;
    Assert::AreEqual(expected, mesh.verts[0].r, 1e-4f, L"red");
    Assert::AreEqual((154.0f / 255.0f) * 0.6f, mesh.verts[0].g, 1e-4f, L"green");
    Assert::AreEqual((140.0f / 255.0f) * 0.66f, mesh.verts[0].b, 1e-4f, L"blue");
  }

  TEST_METHOD(AttachPointsHoldOneEntryPerExhaustInFileOrder)
  {
    // The bridge that lets the loader switch without WorldView changing. Slice 4 deletes it.
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", FIXTURE_NAME, mesh), L"the fixture did not load");
    Assert::AreEqual(static_cast<std::size_t>(2), mesh.attachPoints.size(), L"one attach point per Exhaust marker");
    Assert::AreEqual(-1.0f, mesh.attachPoints[0].x, 1e-5f, L"the port nozzle comes first");
    Assert::AreEqual(1.0f, mesh.attachPoints[1].x, 1e-5f, L"the starboard nozzle comes second");
  }

  // --- clause 1: identification -----------------------------------------------------------------

  TEST_METHOD(RejectsBadMagic)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Poke(bytes, 0, static_cast<std::uint32_t>(0x33454950u)); // "PIE3"
    ExpectRejected(bytes, L"a file whose magic is not NMO2 loaded");
  }

  TEST_METHOD(RejectsMajorVersionOne)
  {
    // 1.x is the Interstellar Outpost dialect: it must fail in one comparison rather than
    // half-parse into a confusing material error (design 3).
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoFileHeader header = Peek<Neuron::NmoFileHeader>(bytes, 0);
    header.versionMajor = 1;
    Poke(bytes, 0, header);
    ExpectRejected(bytes, L"a version 1 file loaded");
  }

  TEST_METHOD(RejectsHeaderBytesThatAreNotThirtyTwo)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoFileHeader header = Peek<Neuron::NmoFileHeader>(bytes, 0);
    header.headerBytes = 16;
    Poke(bytes, 0, header);
    ExpectRejected(bytes, L"a file with a 16-byte header loaded");
  }

  TEST_METHOD(RejectsFileBytesThatDisagreeWithTheFile)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoFileHeader header = Peek<Neuron::NmoFileHeader>(bytes, 0);
    header.fileBytes -= 4;
    Poke(bytes, 0, header);
    ExpectRejected(bytes, L"a file whose stated size is wrong loaded");
  }

  TEST_METHOD(RejectsATruncatedTail)
  {
    // The cheapest real-world corruption there is: an interrupted copy.
    Neuron::ByteBuffer bytes = ReadFixture();
    bytes.resize(bytes.size() - 8);
    ExpectRejected(bytes, L"a truncated file loaded");
  }

  TEST_METHOD(RejectsAMeshCountOverTheSanityCap)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoFileHeader header = Peek<Neuron::NmoFileHeader>(bytes, 0);
    header.meshCount = 100000;
    Poke(bytes, 0, header);
    ExpectRejected(bytes, L"a mesh count past the cap loaded");
  }

#if defined(_DEBUG)
  TEST_METHOD(RejectsAPayloadThatFailsTheCrc)
  {
    // Checked by tools and debug builds; a release loader may skip it (design 5.3), so this test
    // only exists in the configuration that does the check.
    Neuron::ByteBuffer bytes = ReadFixture();
    bytes.back() ^= 0xFFu;
    ExpectRejected(bytes, L"a file whose payload does not match its CRC loaded", false);
  }
#endif

  // --- clause 2: the mesh directory --------------------------------------------------------------

  TEST_METHOD(RejectsAMeshBlobThatIsNotSixteenAligned)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoMeshRef ref = Peek<Neuron::NmoMeshRef>(bytes, sizeof(Neuron::NmoFileHeader));
    ref.offsetBytes += 4;
    Poke(bytes, sizeof(Neuron::NmoFileHeader), ref);
    ExpectRejected(bytes, L"an unaligned mesh blob loaded");
  }

  TEST_METHOD(RejectsAMeshWindowThatEscapesTheFile)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoMeshRef ref = Peek<Neuron::NmoMeshRef>(bytes, sizeof(Neuron::NmoFileHeader));
    ref.lengthBytes = static_cast<std::uint32_t>(bytes.size());
    Poke(bytes, sizeof(Neuron::NmoFileHeader), ref);
    ExpectRejected(bytes, L"a mesh window reaching past the end loaded");
  }

  TEST_METHOD(RejectsAnOffsetAndLengthThatWrapInThirtyTwoBits)
  {
    // The reason every window is measured in 64 bits: offset + length wraps to a small number here,
    // and a naive 32-bit bounds check would wave it through.
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoMeshRef ref = Peek<Neuron::NmoMeshRef>(bytes, sizeof(Neuron::NmoFileHeader));
    ref.lengthBytes = 0xFFFFFF00u;
    Poke(bytes, sizeof(Neuron::NmoFileHeader), ref);
    ExpectRejected(bytes, L"a mesh window whose length wraps loaded");
  }

  // --- clause 3: offsets, strings, sections -------------------------------------------------------

  TEST_METHOD(RejectsAStringLongerThanTheCap)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Poke(bytes, BlobStart(bytes) + MeshHeader(bytes).nameOffset, static_cast<std::uint32_t>(2000));
    ExpectRejected(bytes, L"a string past MAX_STRING_BYTES loaded");
  }

  TEST_METHOD(RejectsInvalidUtf8InAName)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = BlobStart(bytes) + MeshHeader(bytes).nameOffset + sizeof(std::uint32_t);
    bytes[at] = 0xFFu;
    bytes[at + 1] = 0xFEu;
    ExpectRejected(bytes, L"a name that is not UTF-8 loaded");
  }

  TEST_METHOD(RejectsACountWithNoSectionBehindIt)
  {
    // Presence is a count and 0 means absent, so a count with a zero offset is malformed rather
    // than empty (design 5.1).
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoMeshHeader header = MeshHeader(bytes);
    header.materialsOffset = 0;
    Poke(bytes, BlobStart(bytes), header);
    ExpectRejected(bytes, L"a material count with no section loaded");
  }

  TEST_METHOD(RejectsAFacetSectionThatEscapesTheBlob)
  {
    // The facet section carries no length of its own, so "exactly primitiveCount ids" is the take
    // that measures primitiveCount * 4 against the blob, and this is that take failing.
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoMeshRef ref = Peek<Neuron::NmoMeshRef>(bytes, sizeof(Neuron::NmoFileHeader));
    Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    hull.facetsOffset = ref.lengthBytes - 4;
    Poke(bytes, SubMeshAt(bytes, 0), hull);
    ExpectRejected(bytes, L"a facet section running off the end of the blob loaded");
  }

  // --- clause 4: buffer formats and pairing --------------------------------------------------------

  TEST_METHOD(RejectsAnUnknownBufferFormat)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoBufferHeader buffer = Peek<Neuron::NmoBufferHeader>(bytes, BlobStart(bytes) + MeshHeader(bytes).vertexBuffersOffset);
    buffer.format = 77;
    Poke(bytes, BlobStart(bytes) + MeshHeader(bytes).vertexBuffersOffset, buffer);
    ExpectRejected(bytes, L"an unknown vertex format loaded");
  }

  TEST_METHOD(RejectsAStrideThatContradictsItsFormat)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = BlobStart(bytes) + MeshHeader(bytes).indexBuffersOffset;
    Neuron::NmoBufferHeader buffer = Peek<Neuron::NmoBufferHeader>(bytes, at);
    buffer.strideBytes = 3; // U16 says 2
    Poke(bytes, at, buffer);
    ExpectRejected(bytes, L"a stride that does not match its format loaded");
  }

  TEST_METHOD(RejectsASkinBufferCountThatIsNeitherZeroNorTheVertexBufferCount)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoMeshHeader header = MeshHeader(bytes);
    header.skinBufferCount = 2; // the mesh has one vertex buffer
    Poke(bytes, BlobStart(bytes), header);
    ExpectRejected(bytes, L"a mismatched skin buffer count loaded");
  }

  // --- clause 5: submesh ranges ---------------------------------------------------------------------

  TEST_METHOD(RejectsAMaterialIndexOutOfRange)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    hull.materialIndex = 9;
    Poke(bytes, SubMeshAt(bytes, 0), hull);
    ExpectRejected(bytes, L"a submesh naming a material that is not there loaded");
  }

  TEST_METHOD(RejectsAnIndexRangePastItsIndexBuffer)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    hull.facetsOffset = 0; // the facet ids are counted per triangle and would fail first
    hull.primitiveCount = 1000;
    Poke(bytes, SubMeshAt(bytes, 0), hull);
    ExpectRejected(bytes, L"an index range past its buffer loaded");
  }

  TEST_METHOD(RejectsAMinVertexBelowItsBaseVertex)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    hull.baseVertex = 5;
    Poke(bytes, SubMeshAt(bytes, 0), hull);
    ExpectRejected(bytes, L"a minVertex below its baseVertex loaded");
  }

  TEST_METHOD(RejectsABiasedIndexOutsideTheVertexWindow)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    hull.vertexCount = 4; // the hull's indices reach 23
    Poke(bytes, SubMeshAt(bytes, 0), hull);
    ExpectRejected(bytes, L"an index outside the declared vertex window loaded");
  }

  TEST_METHOD(RejectsAVertexBufferIndexOutOfRange)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    hull.vertexBufferIndex = 3;
    Poke(bytes, SubMeshAt(bytes, 0), hull);
    ExpectRejected(bytes, L"a submesh naming a vertex buffer that is not there loaded");
  }

  // --- clause 6: bone tables ------------------------------------------------------------------------

  TEST_METHOD(RejectsABoneParentedForward)
  {
    // parentIndex < ownIndex makes cycles impossible and pose evaluation one forward loop.
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = BoneAt(bytes, MeshHeader(bytes).bonesOffset, 0);
    Neuron::NmoBone bone = Peek<Neuron::NmoBone>(bytes, at);
    bone.parentIndex = 1;
    Poke(bytes, at, bone);
    ExpectRejected(bytes, L"a bone parented onto a later bone loaded");
  }

  TEST_METHOD(RejectsAMeshBoneCarryingAnAlias)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = BoneAt(bytes, MeshHeader(bytes).bonesOffset, 0);
    Neuron::NmoBone bone = Peek<Neuron::NmoBone>(bytes, at);
    bone.meshBoneIndex = 0; // mesh-scope records carry -1
    Poke(bytes, at, bone);
    ExpectRejected(bytes, L"a mesh-scope bone carrying an alias loaded");
  }

  TEST_METHOD(RejectsAnAliasOfAMissingMeshBone)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    const std::size_t at = BoneAt(bytes, hull.bonesOffset, 0);
    Neuron::NmoBone bone = Peek<Neuron::NmoBone>(bytes, at);
    bone.meshBoneIndex = 5; // the mesh skeleton has two
    Poke(bytes, at, bone);
    ExpectRejected(bytes, L"a palette entry aliasing a bone that is not there loaded");
  }

  TEST_METHOD(RejectsDuplicateBoneNamesInOneTable)
  {
    MinimalNmo spec;
    spec.boneNames = {"Root", "Root"};
    Neuron::ByteBuffer bytes = MakeMinimalNmo(spec);
    ExpectRejected(bytes, L"two bones with one name loaded");
  }

  // --- clause 7: clips -------------------------------------------------------------------------------

  TEST_METHOD(RejectsAClipThatEndsBeforeItStarts)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = TurretClipAt(bytes);
    Neuron::NmoClip clip = Peek<Neuron::NmoClip>(bytes, at);
    clip.endSeconds = -1.0f;
    Poke(bytes, at, clip);
    ExpectRejected(bytes, L"a clip ending before it starts loaded");
  }

  TEST_METHOD(RejectsAClipKeyingABoneOutsideItsScope)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = TurretClipAt(bytes) + sizeof(Neuron::NmoClip);
    Neuron::NmoSrtTrack track = Peek<Neuron::NmoSrtTrack>(bytes, at);
    track.boneIndex = 2; // the turret's local table has two bones
    Poke(bytes, at, track);
    ExpectRejected(bytes, L"a clip keying a bone outside its scope loaded");
  }

  TEST_METHOD(RejectsTracksThatAreNotStrictlyIncreasingByBone)
  {
    // Both of the fixture's clips carry exactly one track, so the ordering rule cannot be reached
    // by bending one field of it: it takes a second track, and this is the smallest file with one.
    MinimalNmo spec;
    spec.boneNames = {"Root", "Barrel"};
    spec.clipTrackBones = {1, 0}; // the second track goes backwards
    Neuron::ByteBuffer bytes = MakeMinimalNmo(spec);
    ExpectRejected(bytes, L"a clip whose tracks do not increase by bone loaded");
  }

  TEST_METHOD(RejectsAClipClaimingMoreTracksThanItHolds)
  {
    // The framing half of the same rule: the count is what the reader walks, so a count one too
    // high reads a track out of bytes that are keys, and everything after it is misread.
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = TurretClipAt(bytes);
    Neuron::NmoClip clip = Peek<Neuron::NmoClip>(bytes, at);
    clip.trackCount = 2;
    Poke(bytes, at, clip);
    ExpectRejected(bytes, L"a clip claiming a track it does not hold loaded");
  }

  TEST_METHOD(RejectsKeysThatAreNotStrictlyIncreasingInTime)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t keys = TurretClipAt(bytes) + sizeof(Neuron::NmoClip) + sizeof(Neuron::NmoSrtTrack);
    Neuron::NmoRotationKey key = Peek<Neuron::NmoRotationKey>(bytes, keys + sizeof(Neuron::NmoRotationKey));
    key.timeSeconds = 0.0f; // the key before it is already at 0
    Poke(bytes, keys + sizeof(Neuron::NmoRotationKey), key);
    ExpectRejected(bytes, L"keys that do not advance in time loaded");
  }

  // --- clause 8: skinning ------------------------------------------------------------------------------

  TEST_METHOD(RejectsASkinBoneIndexOutsideItsScope)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = BlobStart(bytes) + MeshHeader(bytes).skinBuffersOffset + sizeof(Neuron::NmoBufferHeader);
    Neuron::NmoSkinVertex skin = Peek<Neuron::NmoSkinVertex>(bytes, at);
    skin.boneIndex[0] = 3; // the hull's palette holds one entry
    Poke(bytes, at, skin);
    ExpectRejected(bytes, L"a skin vertex riding a bone outside its scope loaded");
  }

  TEST_METHOD(RejectsWeightsThatDoNotSumToOne)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = BlobStart(bytes) + MeshHeader(bytes).skinBuffersOffset + sizeof(Neuron::NmoBufferHeader);
    Neuron::NmoSkinVertex skin = Peek<Neuron::NmoSkinVertex>(bytes, at);
    skin.boneWeight[0] = 0.5f;
    Poke(bytes, at, skin);
    ExpectRejected(bytes, L"weights that do not sum to one loaded");
  }

  TEST_METHOD(RejectsWeightsThatAreNotDescending)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const std::size_t at = BlobStart(bytes) + MeshHeader(bytes).skinBuffersOffset + sizeof(Neuron::NmoBufferHeader);
    Neuron::NmoSkinVertex skin = Peek<Neuron::NmoSkinVertex>(bytes, at);
    skin.boneWeight[0] = 0.0f;
    skin.boneWeight[1] = 1.0f;
    Poke(bytes, at, skin);
    ExpectRejected(bytes, L"weights that are not sorted descending loaded");
  }

  TEST_METHOD(RejectsOneVertexSkinnedByTwoBoneScopes)
  {
    // The rule that keeps a later slice's skinning honest: two scopes over one vertex are two
    // answers to "which palette does this skin against".
    Neuron::ByteBuffer bytes = ReadFixture();
    Neuron::NmoSubMesh turret = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 1));
    turret.minVertex = 0;
    turret.vertexCount = 36;
    Poke(bytes, SubMeshAt(bytes, 1), turret);
    ExpectRejected(bytes, L"a vertex skinned by two different bone scopes loaded");
  }

  // --- clause 9: markers ---------------------------------------------------------------------------------

  TEST_METHOD(RejectsAMarkerRidingABoneOutsideItsScope)
  {
    Neuron::ByteBuffer bytes = ReadFixture();
    const Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    const std::size_t at = MarkerAt(bytes, hull.markersOffset, 0);
    Neuron::NmoMarker marker = Peek<Neuron::NmoMarker>(bytes, at);
    marker.parentBone = 7; // the hull's palette holds one entry
    Poke(bytes, at, marker);
    ExpectRejected(bytes, L"a marker riding a bone outside its scope loaded");
  }

  TEST_METHOD(RejectsDuplicateMarkerNamesInOneSubMesh)
  {
    // "BowGun" and "NavPort" occupy the same padded footprint, so one can be renamed onto the other
    // without moving a byte of anything that follows.
    Neuron::ByteBuffer bytes = ReadFixture();
    const Neuron::NmoSubMesh hull = Peek<Neuron::NmoSubMesh>(bytes, SubMeshAt(bytes, 0));
    const std::size_t at = MarkerNameAt(bytes, hull.markersOffset, 4);
    Assert::AreEqual(static_cast<std::uint32_t>(6), Peek<std::uint32_t>(bytes, at), L"the fixture's fifth marker is no longer BowGun");
    Poke(bytes, at, static_cast<std::uint32_t>(7));
    std::memcpy(bytes.data() + at + sizeof(std::uint32_t), "NavPort", 7);
    bytes[at + sizeof(std::uint32_t) + 7] = 0;
    ExpectRejected(bytes, L"two markers with one name loaded");
  }

  TEST_METHOD(RejectsTwoMarkerNamesThatCollideUnderFnv1a)
  {
    // Names are hashed and never stored, so a collision is one marker as far as every consumer is
    // concerned. "costarring" and "liquid" are the shortest known FNV-1a 32 collision; both hash to
    // 0x5F5E77D2. A colliding pair has to be found by search and will not be the length of anything
    // the fixture holds, so this is the one test that builds a file rather than corrupting one.
    MinimalNmo spec;
    spec.markerNames = {"costarring", "liquid"};
    Neuron::ByteBuffer bytes = MakeMinimalNmo(spec);
    ExpectRejected(bytes, L"two marker names that hash alike loaded");
  }

  // --- MeshData's own rules ---------------------------------------------------------------------
  // These two came from ObjParserTests, which was deleted with the parser. Neither ever mentioned
  // ObjParser: they guard the degenerate-extent rule every consumer of a mesh divides by or scales
  // with, and they belong with whichever reader fills those bounds.

  TEST_METHOD(EmptyBoundsAreUsable)
  {
    const Neuron::MeshData mesh;
    const XMFLOAT3 extents = mesh.HalfExtents();
    Assert::IsTrue(extents.x > 0.0f && extents.y > 0.0f && extents.z > 0.0f, L"an empty mesh has a zero extent");
    Assert::AreEqual(0.0f, mesh.RestY(), 1e-5f, L"an empty mesh does not rest on the ground plane");
  }

  TEST_METHOD(BoundsGiveTheCentreAndTheLift)
  {
    Neuron::MeshData mesh;
    mesh.boundsMin = XMFLOAT3(-2.0f, -3.0f, -10.0f);
    mesh.boundsMax = XMFLOAT3(2.0f, 5.0f, 10.0f);

    const XMFLOAT3 centre = mesh.BoundsCentre();
    Assert::AreEqual(0.0f, centre.x, 1e-5f, L"centre x");
    Assert::AreEqual(1.0f, centre.y, 1e-5f, L"centre y");
    Assert::AreEqual(0.0f, centre.z, 1e-5f, L"centre z");

    const XMFLOAT3 extents = mesh.HalfExtents();
    Assert::AreEqual(2.0f, extents.x, 1e-5f, L"half extent x");
    Assert::AreEqual(4.0f, extents.y, 1e-5f, L"half extent y");
    Assert::AreEqual(10.0f, extents.z, 1e-5f, L"half extent z");

    Assert::AreEqual(3.0f, mesh.RestY(), 1e-5f, L"the lift does not put the lowest vertex on y = 0");
  }

  TEST_METHOD(AMinimalFileWithDistinctNamesStillLoads)
  {
    // The control for the two tests above: the builder makes a file the reader accepts, so their
    // rejections are the names and not the construction.
    MinimalNmo spec;
    spec.markerNames = {"costarring", "solid"};
    spec.boneNames = {"Root", "Barrel"};
    spec.clipTrackBones = {0, 1};
    Neuron::ByteBuffer bytes = MakeMinimalNmo(spec);
    Assert::IsTrue(Neuron::BinaryFile::WriteFile(std::wstring(CORRUPT_NAME) + L".nmo", bytes), L"could not write the built file");
    Neuron::MeshData mesh;
    Assert::IsTrue(Neuron::NmoReader::Load(L"", CORRUPT_NAME, mesh), L"a well-formed built file was rejected");
    Assert::AreEqual(static_cast<std::size_t>(3), mesh.verts.size(), L"the built file's triangle");
    Assert::AreEqual(static_cast<std::size_t>(2), mesh.markers.size(), L"the built file's markers");
  }
};
} // namespace NeuronClientTests
