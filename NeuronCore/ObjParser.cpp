#include "pch.h"
#include "ObjParser.h"

#include "FileSys.h"

#include <unordered_map>

using namespace DirectX;

namespace Neuron
{
namespace
{
constexpr float DEFAULT_R = 0.7f;
constexpr float DEFAULT_G = 0.7f;
constexpr float DEFAULT_B = 0.7f;
constexpr float DEGENERATE_EDGE = 1e-6f;
constexpr float LINK_DISTANCE_FACTOR = 1.5f; // multiple of the median attach-face edge

// Relative to the asset root; FileSys resolves it.
std::string ReadWholeFile(const std::wstring& _path)
{
  std::string text = TextFile::ReadFileA(_path);
  if (text.empty())
    DebugTrace(L"cannot open {}\n", _path);
  return text;
}

// Splits on newlines without copying; the caller gets views into _text.
std::vector<std::string_view> SplitLines(const std::string& _text)
{
  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start <= _text.size())
  {
    size_t end = _text.find('\n', start);
    if (end == std::string::npos)
      end = _text.size();
    size_t stop = end;
    while (stop > start && (_text[stop - 1] == '\r' || _text[stop - 1] == ' '))
    {
      --stop;
    }
    lines.push_back(std::string_view(_text).substr(start, stop - start));
    start = end + 1;
  }
  return lines;
}

bool StartsWith(std::string_view _line, const char* _prefix)
{
  const size_t n = std::strlen(_prefix);
  return _line.size() >= n && _line.compare(0, n, _prefix) == 0;
}

// "12/34/56", "12//56" and "12" all yield 12. Returns 0 on anything unparseable, which the caller
// treats as a bad face and skips -- content errors are diagnostics, never crashes.
int ParseFaceIndex(std::string_view _token)
{
  int value = 0;
  size_t i = 0;
  while (i < _token.size() && _token[i] >= '0' && _token[i] <= '9')
  {
    value = value * 10 + (_token[i] - '0');
    ++i;
  }
  return value;
}

std::vector<std::string_view> SplitTokens(std::string_view _line)
{
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < _line.size())
  {
    while (i < _line.size() && _line[i] == ' ')
    {
      ++i;
    }
    const size_t start = i;
    while (i < _line.size() && _line[i] != ' ')
    {
      ++i;
    }
    if (i > start)
      tokens.push_back(_line.substr(start, i - start));
  }
  return tokens;
}

float ParseFloat(std::string_view _token)
{
  return static_cast<float>(std::atof(std::string(_token).c_str()));
}

std::unordered_map<std::string, XMFLOAT3> LoadMaterials(const std::wstring& _path)
{
  std::unordered_map<std::string, XMFLOAT3> materials;
  const std::string text = ReadWholeFile(_path);
  std::string current;
  for (const std::string_view line : SplitLines(text))
  {
    if (StartsWith(line, "newmtl "))
    {
      current.assign(line.substr(7));
      materials[current] = XMFLOAT3(DEFAULT_R, DEFAULT_G, DEFAULT_B);
    }
    else if (StartsWith(line, "Kd ") && !current.empty())
    {
      const std::vector<std::string_view> tokens = SplitTokens(line);
      if (tokens.size() >= 4)
        materials[current] = XMFLOAT3(ParseFloat(tokens[1]), ParseFloat(tokens[2]), ParseFloat(tokens[3]));
    }
  }
  return materials;
}

// Every nozzle on a hull is written with the same attachment material, so the material alone cannot
// say how many there are or where each one sits. Single-link clustering can: the faces of one
// nozzle touch each other, and separate nozzles sit well apart. The link distance comes from the
// median attach-face edge rather than from the hull bounds, so it means the same thing on a bomber
// as on a carrier.
std::vector<XMFLOAT3> ClusterAttachPoints(const std::vector<XMFLOAT3>& _faceCentroids, std::vector<float> _edgeLengths)
{
  if (_faceCentroids.empty())
    return {};

  // Degenerate faces (the exporter writes a few) contribute zero-length edges and would drag the
  // median to nothing, so they are dropped. Their centroids still sit on a nozzle, so the faces
  // themselves stay in.
  std::erase_if(_edgeLengths, [](float _e) { return _e <= DEGENERATE_EDGE; });
  if (_edgeLengths.empty())
    return {_faceCentroids[0]}; // nothing to measure with: take the lot as one cluster
  const size_t middle = _edgeLengths.size() / 2;
  std::nth_element(_edgeLengths.begin(), _edgeLengths.begin() + static_cast<ptrdiff_t>(middle), _edgeLengths.end());
  const float linkDistance = _edgeLengths[middle] * LINK_DISTANCE_FACTOR;
  const float linkDistanceSq = linkDistance * linkDistance;

  // Union-find over the face centroids. A few hundred attach faces on the biggest hull, once at
  // load, so the quadratic pass costs nothing worth avoiding.
  const int count = static_cast<int>(_faceCentroids.size());
  std::vector<int> parent(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i)
    parent[static_cast<size_t>(i)] = i;
  const auto find = [&parent](int _i)
  {
    while (parent[static_cast<size_t>(_i)] != _i)
    {
      parent[static_cast<size_t>(_i)] = parent[static_cast<size_t>(parent[static_cast<size_t>(_i)])];
      _i = parent[static_cast<size_t>(_i)];
    }
    return _i;
  };
  for (int i = 0; i < count; ++i)
  {
    for (int j = i + 1; j < count; ++j)
    {
      const XMFLOAT3& a = _faceCentroids[static_cast<size_t>(i)];
      const XMFLOAT3& b = _faceCentroids[static_cast<size_t>(j)];
      const float dx = a.x - b.x;
      const float dy = a.y - b.y;
      const float dz = a.z - b.z;
      if (dx * dx + dy * dy + dz * dz <= linkDistanceSq)
      {
        const int rootI = find(i);
        const int rootJ = find(j);
        if (rootI != rootJ)
          parent[static_cast<size_t>(rootI)] = rootJ;
      }
    }
  }

  // Averaged in first-seen order, so the list comes out the same on every load.
  std::vector<int> slotOfRoot(static_cast<size_t>(count), -1);
  std::vector<XMFLOAT3> sums;
  std::vector<int> counts;
  for (int i = 0; i < count; ++i)
  {
    int& slot = slotOfRoot[static_cast<size_t>(find(i))];
    if (slot < 0)
    {
      slot = static_cast<int>(sums.size());
      sums.push_back(XMFLOAT3(0.0f, 0.0f, 0.0f));
      counts.push_back(0);
    }
    const XMFLOAT3& p = _faceCentroids[static_cast<size_t>(i)];
    sums[static_cast<size_t>(slot)] =
      XMFLOAT3(sums[static_cast<size_t>(slot)].x + p.x, sums[static_cast<size_t>(slot)].y + p.y, sums[static_cast<size_t>(slot)].z + p.z);
    ++counts[static_cast<size_t>(slot)];
  }

  std::vector<XMFLOAT3> centres;
  centres.reserve(sums.size());
  for (size_t slot = 0; slot < sums.size(); ++slot)
  {
    const float n = static_cast<float>(counts[slot]);
    centres.push_back(XMFLOAT3(sums[slot].x / n, sums[slot].y / n, sums[slot].z / n));
  }
  return centres;
}
} // namespace

bool ObjParser::Load(const std::wstring& _dir, const std::wstring& _name, MeshData& _outMesh)
{
  const std::string text = ReadWholeFile(_dir + _name + L".obj");
  if (text.empty())
    return false;
  const std::unordered_map<std::string, XMFLOAT3> materials = LoadMaterials(_dir + _name + L".mtl");

  std::vector<XMFLOAT3> positions;
  XMFLOAT3 colour(DEFAULT_R, DEFAULT_G, DEFAULT_B);
  XMFLOAT3 boundsMin(1e30f, 1e30f, 1e30f);
  XMFLOAT3 boundsMax(-1e30f, -1e30f, -1e30f);
  bool onAttachMaterial = false;
  std::vector<XMFLOAT3> attachFaces; // one centroid per attach-material triangle
  std::vector<float> attachEdges;    // and their edge lengths, for the clustering distance
  int badFaces = 0;

  for (const std::string_view line : SplitLines(text))
  {
    if (StartsWith(line, "v "))
    {
      const std::vector<std::string_view> tokens = SplitTokens(line);
      if (tokens.size() >= 4)
      {
        const float x = ParseFloat(tokens[1]);
        const float y = ParseFloat(tokens[2]);
        const float z = ParseFloat(tokens[3]);
        positions.push_back(XMFLOAT3(x, y, -z));
        boundsMin = XMFLOAT3(std::min(boundsMin.x, x), std::min(boundsMin.y, y), std::min(boundsMin.z, -z));
        boundsMax = XMFLOAT3(std::max(boundsMax.x, x), std::max(boundsMax.y, y), std::max(boundsMax.z, -z));
      }
    }
    else if (StartsWith(line, "usemtl "))
    {
      const std::string material(line.substr(7));
      const auto found = materials.find(material);
      colour = (found != materials.end()) ? found->second : XMFLOAT3(DEFAULT_R, DEFAULT_G, DEFAULT_B);
      onAttachMaterial = material == ATTACH_MATERIAL;
    }
    else if (StartsWith(line, "f "))
    {
      const std::vector<std::string_view> tokens = SplitTokens(line);
      if (tokens.size() < 4)
      {
        ++badFaces;
        continue;
      }
      for (size_t corner = 2; corner + 1 < tokens.size(); ++corner)
      {
        const size_t fan[3] = {1, corner, corner + 1};
        XMFLOAT3 triangle[3] = {};
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i)
        {
          const int index = ParseFaceIndex(tokens[fan[i]]);
          ok = index >= 1 && static_cast<size_t>(index) <= positions.size();
          if (ok)
            triangle[i] = positions[static_cast<size_t>(index) - 1];
        }
        if (!ok)
        {
          ++badFaces;
          continue;
        }
        for (const XMFLOAT3& p : triangle)
          _outMesh.verts.push_back(MeshVertex{p.x, p.y, p.z, colour.x, colour.y, colour.z});
        if (onAttachMaterial)
        {
          attachFaces.push_back(XMFLOAT3((triangle[0].x + triangle[1].x + triangle[2].x) / 3.0f,
                                         (triangle[0].y + triangle[1].y + triangle[2].y) / 3.0f,
                                         (triangle[0].z + triangle[1].z + triangle[2].z) / 3.0f));
          for (int edge = 0; edge < 3; ++edge)
          {
            const XMFLOAT3& a = triangle[edge];
            const XMFLOAT3& b = triangle[(edge + 1) % 3];
            attachEdges.push_back(std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)));
          }
        }
      }
    }
  }

  if (_outMesh.verts.size() % 3 != 0)
    _outMesh.verts.resize(_outMesh.verts.size() - _outMesh.verts.size() % 3);
  if (boundsMin.x > boundsMax.x)
  {
    boundsMin = XMFLOAT3(0.0f, 0.0f, 0.0f);
    boundsMax = XMFLOAT3(0.0f, 0.0f, 0.0f);
  }
  _outMesh.boundsMin = boundsMin;
  _outMesh.boundsMax = boundsMax;
  _outMesh.attachPoints = ClusterAttachPoints(attachFaces, std::move(attachEdges));
  DebugTrace(L"{}: {} tris, {:.1f} x {:.1f} x {:.1f}, {} attach point{}{}\n", _name, _outMesh.verts.size() / 3, boundsMax.x - boundsMin.x,
             boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z, _outMesh.attachPoints.size(),
             _outMesh.attachPoints.size() == 1 ? L"" : L"s", badFaces ? L" (skipped malformed faces)" : L"");
  return !_outMesh.verts.empty();
}
} // namespace Neuron
