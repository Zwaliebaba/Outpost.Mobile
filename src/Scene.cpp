#include "Scene.h"
#include "Tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>

using namespace DirectX;

namespace
{

// Change these three names to fly different hulls; everything else adapts to the mesh bounds.
const wchar_t* const SHIP_MESHES[] = {L"Bomber", L"Corvette", L"Frigate"};

struct LoadedMesh
{
  std::vector<SceneVertex> verts;
  float minY = 0.0f;
};

std::string ReadWholeFile(const std::wstring& _path)
{
  HANDLE file = CreateFileW(_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    DebugPrintf("cannot open %S\n", _path.c_str());
    return {};
  }
  LARGE_INTEGER size = {};
  GetFileSizeEx(file, &size);
  std::string text(size_t(size.QuadPart), '\0');
  DWORD read = 0;
  ReadFile(file, text.data(), DWORD(text.size()), &read, nullptr);
  text.resize(read);
  CloseHandle(file);
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
    {
      end = _text.size();
    }
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
    {
      tokens.push_back(_line.substr(start, i - start));
    }
  }
  return tokens;
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
      materials[current] = XMFLOAT3(0.7f, 0.7f, 0.7f);
    }
    else if (StartsWith(line, "Kd ") && !current.empty())
    {
      const std::vector<std::string_view> tokens = SplitTokens(line);
      if (tokens.size() >= 4)
      {
        materials[current] = XMFLOAT3(float(std::atof(std::string(tokens[1]).c_str())), float(std::atof(std::string(tokens[2]).c_str())),
                                      float(std::atof(std::string(tokens[3]).c_str())));
      }
    }
  }
  return materials;
}

// OBJ is right-handed and these hulls point their bow at -Z. Negating Z converts to the
// left-handed render basis (east, up, north) and lands the bow on +Z in one step.
bool LoadObj(const std::wstring& _dir, const std::wstring& _name, LoadedMesh& _out)
{
  const std::string text = ReadWholeFile(_dir + _name + L".obj");
  if (text.empty())
  {
    return false;
  }
  const std::unordered_map<std::string, XMFLOAT3> materials = LoadMaterials(_dir + _name + L".mtl");

  std::vector<XMFLOAT3> positions;
  XMFLOAT3 colour(0.7f, 0.7f, 0.7f);
  float minY = 1e30f;
  int badFaces = 0;

  for (const std::string_view line : SplitLines(text))
  {
    if (StartsWith(line, "v "))
    {
      const std::vector<std::string_view> tokens = SplitTokens(line);
      if (tokens.size() >= 4)
      {
        const float x = float(std::atof(std::string(tokens[1]).c_str()));
        const float y = float(std::atof(std::string(tokens[2]).c_str()));
        const float z = float(std::atof(std::string(tokens[3]).c_str()));
        positions.push_back(XMFLOAT3(x, y, -z));
        minY = std::min(minY, y);
      }
    }
    else if (StartsWith(line, "usemtl "))
    {
      const auto found = materials.find(std::string(line.substr(7)));
      colour = (found != materials.end()) ? found->second : XMFLOAT3(0.7f, 0.7f, 0.7f);
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
          ok = index >= 1 && size_t(index) <= positions.size();
          if (ok)
          {
            triangle[i] = positions[size_t(index) - 1];
          }
        }
        if (!ok)
        {
          ++badFaces;
          continue;
        }
        for (const XMFLOAT3& p : triangle)
        {
          _out.verts.push_back(SceneVertex{p.x, p.y, p.z, colour.x, colour.y, colour.z});
        }
      }
    }
  }

  if (_out.verts.size() % 3 != 0)
  {
    _out.verts.resize(_out.verts.size() - _out.verts.size() % 3);
  }
  _out.minY = (minY < 1e29f) ? minY : 0.0f;
  DebugPrintf("%S: %zu tris, minY %.2f%s\n", _name.c_str(), _out.verts.size() / 3, _out.minY,
              badFaces ? " (skipped malformed faces)" : "");
  return !_out.verts.empty();
}

// A unit quad in the ground plane; the shader draws the grid on it procedurally so grid spacing
// stays a live tuning value with no vertex rebuild.
std::vector<SceneVertex> BuildGroundQuad()
{
  const float h = 0.5f;
  return {
      SceneVertex{-h, 0.0f, -h, 1.0f, 1.0f, 1.0f}, SceneVertex{-h, 0.0f, h, 1.0f, 1.0f, 1.0f}, SceneVertex{h, 0.0f, h, 1.0f, 1.0f, 1.0f},
      SceneVertex{-h, 0.0f, -h, 1.0f, 1.0f, 1.0f}, SceneVertex{h, 0.0f, h, 1.0f, 1.0f, 1.0f},  SceneVertex{h, 0.0f, -h, 1.0f, 1.0f, 1.0f},
  };
}

} // namespace

std::wstring FindDataRoot()
{
  wchar_t exePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  std::wstring dir(exePath);
  const size_t lastSlash = dir.find_last_of(L'\\');
  dir = (lastSlash == std::wstring::npos) ? L"." : dir.substr(0, lastSlash);

  for (int up = 0; up < 8; ++up)
  {
    const DWORD attributes = GetFileAttributesW((dir + L"\\GameData\\Meshes").c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
      return dir + L"\\";
    }
    const size_t slash = dir.find_last_of(L'\\');
    if (slash == std::wstring::npos)
    {
      break;
    }
    dir = dir.substr(0, slash);
  }
  DebugPrintf("no GameData folder above the executable; falling back to the working directory\n");
  return L".\\";
}

void Scene::Init(Gfx& _gfx)
{
  const std::wstring meshDir = FindDataRoot() + L"GameData\\Meshes\\";

  const int shipCount = int(std::size(SHIP_MESHES));
  for (int i = 0; i < shipCount; ++i)
  {
    LoadedMesh loaded;
    if (!LoadObj(meshDir, SHIP_MESHES[i], loaded))
    {
      continue;
    }
    Ship ship;
    ship.mesh = _gfx.UploadMesh(loaded.verts);
    ship.restY = -loaded.minY;
    ship.headingRad = 0.0f;
    ship.posWorld = XMFLOAT3((float(i) - float(shipCount - 1) * 0.5f) * g_tuning.startSpacing, 0.0f, 0.0f);
    m_ships.push_back(ship);
  }

  m_groundMesh = _gfx.UploadMesh(BuildGroundQuad());
}

void Scene::Render(Gfx& _gfx)
{
  // Stage 1 has no pointer input yet, so the camera reads straight from the tuning values; stage 3
  // takes the wheel and drag over yaw and distance.
  m_yawRad = XMConvertToRadians(g_tuning.cameraYawDeg);
  m_pitchRad = XMConvertToRadians(std::clamp(g_tuning.cameraPitchDeg, 5.0f, 89.0f));
  m_distance = std::clamp(g_tuning.cameraDistance, g_tuning.cameraMinZoom, g_tuning.cameraMaxZoom);
  m_cameraTarget.y = g_tuning.cameraTargetHeight;

  const float cosPitch = std::cos(m_pitchRad);
  const XMVECTOR target = XMLoadFloat3(&m_cameraTarget);
  const XMVECTOR offset = XMVectorSet(std::sin(m_yawRad) * cosPitch, std::sin(m_pitchRad), -std::cos(m_yawRad) * cosPitch, 0.0f);
  const XMVECTOR eye = XMVectorMultiplyAdd(offset, XMVectorReplicate(m_distance), target);
  XMStoreFloat3(&m_cameraEye, eye);

  const float aspect = float(_gfx.m_widthPx) / float(std::max(1u, _gfx.m_heightPx));
  const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
  const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(g_tuning.cameraFovDeg), aspect,
                                                 std::max(0.01f, g_tuning.cameraNearPlane), g_tuning.cameraFarPlane);

  SceneFrame frame = {};
  XMStoreFloat4x4(&frame.viewProj, view * proj);
  frame.lightDir = XMFLOAT3(g_tuning.lightDirX, g_tuning.lightDirY, g_tuning.lightDirZ);
  frame.ambient = g_tuning.ambientLevel;
  frame.gridColour = Rgba{g_tuning.gridColourR, g_tuning.gridColourG, g_tuning.gridColourB, g_tuning.gridStrength};
  frame.gridSpacing = g_tuning.gridSpacing;
  frame.gridLineWidthPx = g_tuning.gridLineWidthPx;
  frame.gridFadeDistance = g_tuning.gridFadeDistance;
  frame.cameraPos = m_cameraEye;
  _gfx.BeginScene(frame);

  // The ground follows the camera in XZ, so its edge never comes into view.
  XMFLOAT4X4 world;
  const float groundSize = std::max(1.0f, g_tuning.groundSize);
  XMStoreFloat4x4(&world, XMMatrixScaling(groundSize, 1.0f, groundSize) *
                              XMMatrixTranslation(m_cameraTarget.x, 0.0f, m_cameraTarget.z));
  _gfx.DrawMesh(m_groundMesh, world, Rgba{g_tuning.groundColourR, g_tuning.groundColourG, g_tuning.groundColourB, 1.0f}, 0.0f, true);

  const float scale = std::max(0.01f, g_tuning.shipScale);
  for (const Ship& ship : m_ships)
  {
    XMStoreFloat4x4(&world, XMMatrixScaling(scale, scale, scale) * XMMatrixRotationY(ship.headingRad) *
                                XMMatrixTranslation(ship.posWorld.x, ship.posWorld.y + ship.restY * scale, ship.posWorld.z));
    _gfx.DrawMesh(ship.mesh, world, Rgba{g_tuning.shipColourR, g_tuning.shipColourG, g_tuning.shipColourB, 1.0f}, g_tuning.shipMaterialMix,
                  false);
  }
}
