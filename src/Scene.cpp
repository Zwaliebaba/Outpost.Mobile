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
  DirectX::XMFLOAT3 boundsMin{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 boundsMax{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 thrusterLocal{0.0f, 0.0f, 0.0f}; // centroid of the thruster-material faces
  bool hasThruster = false;
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
  XMFLOAT3 boundsMin(1e30f, 1e30f, 1e30f);
  XMFLOAT3 boundsMax(-1e30f, -1e30f, -1e30f);
  bool onThruster = false;
  XMFLOAT3 thrusterSum(0.0f, 0.0f, 0.0f);
  int thrusterVerts = 0;
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
        boundsMin = XMFLOAT3(std::min(boundsMin.x, x), std::min(boundsMin.y, y), std::min(boundsMin.z, -z));
        boundsMax = XMFLOAT3(std::max(boundsMax.x, x), std::max(boundsMax.y, y), std::max(boundsMax.z, -z));
      }
    }
    else if (StartsWith(line, "usemtl "))
    {
      const std::string material(line.substr(7));
      const auto found = materials.find(material);
      colour = (found != materials.end()) ? found->second : XMFLOAT3(0.7f, 0.7f, 0.7f);
      onThruster = material == "thruster"; // every hull in GameData names its engines this way
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
          if (onThruster)
          {
            thrusterSum = XMFLOAT3(thrusterSum.x + p.x, thrusterSum.y + p.y, thrusterSum.z + p.z);
            ++thrusterVerts;
          }
        }
      }
    }
  }

  if (_out.verts.size() % 3 != 0)
  {
    _out.verts.resize(_out.verts.size() - _out.verts.size() % 3);
  }
  if (boundsMin.x > boundsMax.x)
  {
    boundsMin = XMFLOAT3(0.0f, 0.0f, 0.0f);
    boundsMax = XMFLOAT3(0.0f, 0.0f, 0.0f);
  }
  _out.boundsMin = boundsMin;
  _out.boundsMax = boundsMax;
  _out.hasThruster = thrusterVerts > 0;
  if (_out.hasThruster)
  {
    _out.thrusterLocal = XMFLOAT3(thrusterSum.x / float(thrusterVerts), thrusterSum.y / float(thrusterVerts),
                                  thrusterSum.z / float(thrusterVerts));
  }
  DebugPrintf("%S: %zu tris, %.1f x %.1f x %.1f%s\n", _name.c_str(), _out.verts.size() / 3, boundsMax.x - boundsMin.x,
              boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z, badFaces ? " (skipped malformed faces)" : "");
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

// ---------------------------------------------------------------------------------------------
// Feel maths. Everything that eases uses the half-life form, never a per-frame constant, so the
// same tuning value produces the same motion at any frame rate.

float HalfLifeBlend(float _dtSec, float _halfLifeSec)
{
  return (_halfLifeSec <= 0.0f) ? 1.0f : 1.0f - std::exp2(-_dtSec / _halfLifeSec);
}

float MoveTowards(float _current, float _target, float _maxDelta)
{
  const float delta = _target - _current;
  if (std::fabs(delta) <= _maxDelta)
  {
    return _target;
  }
  return _current + ((delta > 0.0f) ? _maxDelta : -_maxDelta);
}

float Distance2D(float _ax, float _ay, float _bx, float _by)
{
  const float dx = _bx - _ax;
  const float dy = _by - _ay;
  return std::sqrt(dx * dx + dy * dy);
}

// Slot offsets in formation space: x to starboard, y forward.
XMFLOAT2 FormationOffset(int _slot, int _count, int _shape, float _spacing)
{
  const float lane = float(_slot) - float(_count - 1) * 0.5f;
  switch (_shape)
  {
  case 0: // line abreast
    return XMFLOAT2(lane * _spacing, 0.0f);
  case 2: // box
  {
    const int columns = std::max(1, int(std::ceil(std::sqrt(float(_count)))));
    const int row = _slot / columns;
    const int column = _slot % columns;
    const int inRow = std::min(columns, _count - row * columns);
    return XMFLOAT2((float(column) - float(inRow - 1) * 0.5f) * _spacing, -float(row) * _spacing);
  }
  case 3: // circle
  {
    if (_count < 2)
    {
      return XMFLOAT2(0.0f, 0.0f);
    }
    const float angle = XM_2PI * float(_slot) / float(_count);
    const float radius = _spacing * float(_count) / XM_2PI;
    return XMFLOAT2(std::sin(angle) * radius, std::cos(angle) * radius);
  }
  default: // 1: wedge
    return XMFLOAT2(lane * _spacing, -std::fabs(lane) * _spacing * 0.8f);
  }
}

// ---------------------------------------------------------------------------------------------
// Pointer bookkeeping. One entry per contact, so a mouse and several fingers share one path.

constexpr int MAX_POINTERS = 4;

struct PointerTrack
{
  uint32_t id = 0;
  bool active = false;
  bool isTouch = false;
  uint32_t buttons = 0;
  float startXPx = 0.0f, startYPx = 0.0f;
  float xPx = 0.0f, yPx = 0.0f;
  float prevXPx = 0.0f, prevYPx = 0.0f;
  int64_t downQpc = 0;
  bool dragging = false;
  bool boxSelecting = false;
  bool cameraDrag = false;    // held with the second or third button
  bool inGesture = false;     // part of a two-finger gesture, so its release means nothing
};

PointerTrack g_pointers[MAX_POINTERS];
int64_t g_qpcFrequency = 1;
int64_t g_lastGroundTapQpc = 0;
float g_lastGroundTapXPx = 0.0f;
float g_lastGroundTapYPx = 0.0f;
bool g_cameraMoved = false; // pushes the camera values back onto the Tuner sliders when the drag ends

// Two-finger gesture, remembered between updates so pan, pinch and twist all read clean deltas.
bool g_gestureActive = false;
float g_gestureCentroidXPx = 0.0f;
float g_gestureCentroidYPx = 0.0f;
float g_gestureSpreadPx = 0.0f;
float g_gestureAngleRad = 0.0f;

PointerTrack* FindTrack(uint32_t _id)
{
  for (PointerTrack& track : g_pointers)
  {
    if (track.active && track.id == _id)
    {
      return &track;
    }
  }
  return nullptr;
}

int ActiveTouchCount()
{
  int count = 0;
  for (const PointerTrack& track : g_pointers)
  {
    count += (track.active && track.isTouch) ? 1 : 0;
  }
  return count;
}

// The two oldest live touches, in slot order.
bool TwoTouches(PointerTrack*& _a, PointerTrack*& _b)
{
  _a = nullptr;
  _b = nullptr;
  for (PointerTrack& track : g_pointers)
  {
    if (!track.active || !track.isTouch)
    {
      continue;
    }
    if (!_a)
    {
      _a = &track;
    }
    else if (!_b)
    {
      _b = &track;
      return true;
    }
  }
  return false;
}

float ElapsedMs(int64_t _fromQpc, int64_t _toQpc)
{
  return float(double(_toQpc - _fromQpc) / double(g_qpcFrequency) * 1000.0);
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
  LARGE_INTEGER frequency = {};
  QueryPerformanceFrequency(&frequency);
  g_qpcFrequency = frequency.QuadPart;

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
    ship.restY = -loaded.boundsMin.y;
    ship.pickCentre = XMFLOAT3((loaded.boundsMin.x + loaded.boundsMax.x) * 0.5f, (loaded.boundsMin.y + loaded.boundsMax.y) * 0.5f,
                               (loaded.boundsMin.z + loaded.boundsMax.z) * 0.5f);
    ship.halfExtents = XMFLOAT3(std::max(0.5f, (loaded.boundsMax.x - loaded.boundsMin.x) * 0.5f),
                                std::max(0.5f, (loaded.boundsMax.y - loaded.boundsMin.y) * 0.5f),
                                std::max(0.5f, (loaded.boundsMax.z - loaded.boundsMin.z) * 0.5f));
    ship.thrusterLocal = loaded.thrusterLocal;
    ship.hasThruster = loaded.hasThruster;
    ship.posWorld = XMFLOAT3((float(i) - float(shipCount - 1) * 0.5f) * g_tuning.startSpacing, 0.0f, 0.0f);
    ship.prevPos = ship.posWorld;
    m_ships.push_back(ship);
  }

  m_groundMesh = _gfx.UploadMesh(BuildGroundQuad());
}

void Scene::QueuePointerEvent(const PointerEvent& _event)
{
  m_pendingEvents.push_back(_event);
}

void Scene::ClearSelection()
{
  for (Ship& ship : m_ships)
  {
    ship.selected = false;
  }
}

int Scene::SelectedCount() const
{
  int count = 0;
  for (const Ship& ship : m_ships)
  {
    count += ship.selected ? 1 : 0;
  }
  return count;
}

// ---------------------------------------------------------------------------------------------
// Camera and projection

void Scene::UpdateCamera()
{
  const float yaw = XMConvertToRadians(g_tuning.cameraYawDeg);
  const float pitch = XMConvertToRadians(std::clamp(g_tuning.cameraPitchDeg, 5.0f, 89.0f));
  const float distance = std::clamp(g_tuning.cameraDistance, g_tuning.cameraMinZoom, g_tuning.cameraMaxZoom);
  m_cameraTarget.y = g_tuning.cameraTargetHeight;

  const float cosPitch = std::cos(pitch);
  const XMFLOAT3 shaken(m_cameraTarget.x + m_shakeOffset.x, m_cameraTarget.y + m_shakeOffset.y, m_cameraTarget.z + m_shakeOffset.z);
  const XMVECTOR target = XMLoadFloat3(&shaken);
  const XMVECTOR offset = XMVectorSet(std::sin(yaw) * cosPitch, std::sin(pitch), -std::cos(yaw) * cosPitch, 0.0f);
  const XMVECTOR eye = XMVectorMultiplyAdd(offset, XMVectorReplicate(distance), target);
  XMStoreFloat3(&m_cameraEye, eye);

  const XMVECTOR forward = XMVector3Normalize(XMVectorNegate(offset));
  const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
  XMStoreFloat3(&m_cameraRight, right);
  XMStoreFloat3(&m_cameraUp, XMVector3Cross(forward, right));

  const float aspect = float(m_viewWidthPx) / float(std::max(1u, m_viewHeightPx));
  const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
  const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(std::clamp(g_tuning.cameraFovDeg, 5.0f, 170.0f)), aspect,
                                                 std::max(0.01f, g_tuning.cameraNearPlane), std::max(1.0f, g_tuning.cameraFarPlane));
  const XMMATRIX viewProj = view * proj;
  XMStoreFloat4x4(&m_viewProj, viewProj);
  XMStoreFloat4x4(&m_invViewProj, XMMatrixInverse(nullptr, viewProj));
}

void Scene::ScreenRay(float _xPx, float _yPx, XMFLOAT3& _origin, XMFLOAT3& _direction) const
{
  const float ndcX = (_xPx / float(m_viewWidthPx)) * 2.0f - 1.0f;
  const float ndcY = 1.0f - (_yPx / float(m_viewHeightPx)) * 2.0f;
  const XMMATRIX inverse = XMLoadFloat4x4(&m_invViewProj);
  const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverse);
  const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverse);
  XMStoreFloat3(&_origin, nearPoint);
  XMStoreFloat3(&_direction, XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint)));
}

bool Scene::RayToGround(float _xPx, float _yPx, XMFLOAT3& _point) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  ScreenRay(_xPx, _yPx, origin, direction);
  if (direction.y > -1e-5f) // at or above the horizon
  {
    return false;
  }
  const float t = -origin.y / direction.y;
  _point = XMFLOAT3(origin.x + direction.x * t, 0.0f, origin.z + direction.z * t);
  return true;
}

bool Scene::WorldToScreen(const XMFLOAT3& _world, float& _xPx, float& _yPx) const
{
  const XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&_world), XMLoadFloat4x4(&m_viewProj));
  const float w = XMVectorGetW(clip);
  if (w <= 1e-4f) // behind the eye
  {
    return false;
  }
  _xPx = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * float(m_viewWidthPx);
  _yPx = (0.5f - XMVectorGetY(clip) / w * 0.5f) * float(m_viewHeightPx);
  return true;
}

// Ray against each hull's oriented bounding box. A sphere would be far too loose on a hull three
// times longer than it is wide.
int Scene::PickShip(float _xPx, float _yPx) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  ScreenRay(_xPx, _yPx, origin, direction);
  const float scale = std::max(0.01f, g_tuning.shipScale);
  const float padding = std::max(1.0f, g_tuning.inputPickPadding);

  int best = -1;
  float bestT = 1e30f;
  for (int i = 0; i < int(m_ships.size()); ++i)
  {
    const Ship& ship = m_ships[i];
    const float cosH = std::cos(ship.headingRad);
    const float sinH = std::sin(ship.headingRad);

    const XMFLOAT3 centre(ship.posWorld.x + (ship.pickCentre.x * cosH + ship.pickCentre.z * sinH) * scale,
                          ship.posWorld.y + (ship.restY + ship.pickCentre.y) * scale,
                          ship.posWorld.z + (-ship.pickCentre.x * sinH + ship.pickCentre.z * cosH) * scale);

    // Into hull space: to the centre, undo the heading, undo the scale.
    const float rx = origin.x - centre.x;
    const float rz = origin.z - centre.z;
    const float localOrigin[3] = {(rx * cosH - rz * sinH) / scale, (origin.y - centre.y) / scale, (rx * sinH + rz * cosH) / scale};
    const float localDir[3] = {(direction.x * cosH - direction.z * sinH) / scale, direction.y / scale,
                               (direction.x * sinH + direction.z * cosH) / scale};
    const float extent[3] = {ship.halfExtents.x * padding, ship.halfExtents.y * padding, ship.halfExtents.z * padding};

    float tMin = 0.0f;
    float tMax = 1e30f;
    bool hit = true;
    for (int axis = 0; axis < 3 && hit; ++axis)
    {
      if (std::fabs(localDir[axis]) < 1e-8f)
      {
        hit = std::fabs(localOrigin[axis]) <= extent[axis];
        continue;
      }
      float t1 = (-extent[axis] - localOrigin[axis]) / localDir[axis];
      float t2 = (extent[axis] - localOrigin[axis]) / localDir[axis];
      if (t1 > t2)
      {
        std::swap(t1, t2);
      }
      tMin = std::max(tMin, t1);
      tMax = std::min(tMax, t2);
      hit = tMax >= tMin;
    }
    if (hit && tMin < bestT)
    {
      bestT = tMin;
      best = i;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------------------------
// Orders

void Scene::IssueMoveOrder(const XMFLOAT3& _point, bool _hasFacing, float _facingRad)
{
  std::vector<int> chosen;
  for (int i = 0; i < int(m_ships.size()); ++i)
  {
    if (m_ships[i].selected)
    {
      chosen.push_back(i);
    }
  }
  if (chosen.empty())
  {
    return;
  }

  // Point the formation along the ordered facing, or along the way the group is about to travel.
  float heading = _facingRad;
  if (!_hasFacing)
  {
    float centreX = 0.0f;
    float centreZ = 0.0f;
    for (const int index : chosen)
    {
      centreX += m_ships[size_t(index)].posWorld.x;
      centreZ += m_ships[size_t(index)].posWorld.z;
    }
    centreX /= float(chosen.size());
    centreZ /= float(chosen.size());
    const float dx = _point.x - centreX;
    const float dz = _point.z - centreZ;
    heading = (dx * dx + dz * dz > 1e-4f) ? std::atan2(dx, dz) : m_ships[size_t(chosen[0])].headingRad;
  }

  // Hand out slots in the order the ships already lie across the formation, so they do not have to
  // cross each other on the way in.
  const float rightX = std::cos(heading);
  const float rightZ = -std::sin(heading);
  std::sort(chosen.begin(), chosen.end(),
            [&](int _a, int _b)
            {
              return m_ships[size_t(_a)].posWorld.x * rightX + m_ships[size_t(_a)].posWorld.z * rightZ <
                     m_ships[size_t(_b)].posWorld.x * rightX + m_ships[size_t(_b)].posWorld.z * rightZ;
            });

  OrderMarker marker;
  marker.posWorld = XMFLOAT3(_point.x, 0.0f, _point.z);
  marker.facingRad = heading;
  marker.hasFacing = _hasFacing;
  m_markers.push_back(marker);

  const int count = int(chosen.size());
  const int shape = std::clamp(int(g_tuning.motionFormationShape + 0.5f), 0, 3);
  const float spacing = std::max(0.0f, g_tuning.motionFormationSpacing);
  const float cosH = std::cos(heading);
  const float sinH = std::sin(heading);

  for (int slot = 0; slot < count; ++slot)
  {
    const XMFLOAT2 local = FormationOffset(slot, count, shape, spacing);
    Ship& ship = m_ships[size_t(chosen[size_t(slot)])];
    ship.orderPos = XMFLOAT3(_point.x + local.x * cosH + local.y * sinH, 0.0f, _point.z - local.x * sinH + local.y * cosH);
    ship.orderFacingRad = heading;
    ship.orderHasFacing = _hasFacing;
    ship.order = OrderState::Moving;
  }
}

void Scene::HandleTap(float _xPx, float _yPx, bool _shift, int64_t _qpc)
{
  const int hit = PickShip(_xPx, _yPx);
  if (hit >= 0)
  {
    if (_shift)
    {
      m_ships[size_t(hit)].selected = !m_ships[size_t(hit)].selected;
    }
    else
    {
      ClearSelection();
      m_ships[size_t(hit)].selected = true;
    }
    g_lastGroundTapQpc = 0; // tapping a hull does not begin a double tap
    return;
  }

  const bool doubleTap = g_lastGroundTapQpc != 0 && ElapsedMs(g_lastGroundTapQpc, _qpc) <= g_tuning.inputDoubleTapWindowMs &&
                         Distance2D(g_lastGroundTapXPx, g_lastGroundTapYPx, _xPx, _yPx) <= g_tuning.inputDragThresholdPx * 3.0f;
  g_lastGroundTapQpc = _qpc;
  g_lastGroundTapXPx = _xPx;
  g_lastGroundTapYPx = _yPx;

  // Double tapping empty ground is how a selection is dropped, since a single tap with a selection
  // is already a move order.
  if (doubleTap)
  {
    ClearSelection();
    return;
  }
  if (SelectedCount() == 0)
  {
    return;
  }
  XMFLOAT3 point;
  if (RayToGround(_xPx, _yPx, point))
  {
    IssueMoveOrder(point, false, 0.0f);
  }
}

void Scene::FinishBoxSelect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, bool _additive)
{
  const float left = std::min(_x0Px, _x1Px);
  const float right = std::max(_x0Px, _x1Px);
  const float top = std::min(_y0Px, _y1Px);
  const float bottom = std::max(_y0Px, _y1Px);
  if (!_additive)
  {
    ClearSelection();
  }
  const float scale = std::max(0.01f, g_tuning.shipScale);
  for (Ship& ship : m_ships)
  {
    const XMFLOAT3 centre(ship.posWorld.x, ship.posWorld.y + ship.halfExtents.y * scale, ship.posWorld.z);
    float xPx = 0.0f;
    float yPx = 0.0f;
    if (WorldToScreen(centre, xPx, yPx) && xPx >= left && xPx <= right && yPx >= top && yPx <= bottom)
    {
      ship.selected = true;
    }
  }
}

void Scene::FinishOrderDrag(float _x0Px, float _y0Px, float _x1Px, float _y1Px)
{
  XMFLOAT3 from;
  XMFLOAT3 to;
  if (!RayToGround(_x0Px, _y0Px, from))
  {
    return;
  }
  if (!RayToGround(_x1Px, _y1Px, to))
  {
    IssueMoveOrder(from, false, 0.0f);
    return;
  }
  const float dx = to.x - from.x;
  const float dz = to.z - from.z;
  const bool hasFacing = (dx * dx + dz * dz) > 1.0f;
  IssueMoveOrder(from, hasFacing, hasFacing ? std::atan2(dx, dz) : 0.0f);
}

namespace
{

// Second button orbits, third button pans. Panning unprojects both pointer positions onto the
// ground so the world stays stuck to the finger, which makes panSpeed a plain multiplier on that.
void ApplyCameraDrag(Scene& _scene, const PointerTrack& _track)
{
  const float dx = _track.xPx - _track.prevXPx;
  const float dy = _track.yPx - _track.prevYPx;
  if (dx == 0.0f && dy == 0.0f)
  {
    return;
  }
  g_cameraMoved = true;

  if ((_track.buttons & 0x2u) != 0) // second button: orbit
  {
    g_tuning.cameraYawDeg = std::remainder(g_tuning.cameraYawDeg + dx * g_tuning.cameraRotateSpeedDegPerPx, 360.0f);
    g_tuning.cameraPitchDeg = std::clamp(g_tuning.cameraPitchDeg - dy * g_tuning.cameraRotateSpeedDegPerPx, 5.0f, 89.0f);
    _scene.UpdateCamera();
    return;
  }

  XMFLOAT3 before;
  XMFLOAT3 after;
  if (_scene.RayToGround(_track.prevXPx, _track.prevYPx, before) && _scene.RayToGround(_track.xPx, _track.yPx, after))
  {
    _scene.m_cameraGoal.x -= (after.x - before.x) * g_tuning.cameraPanSpeed;
    _scene.m_cameraGoal.z -= (after.z - before.z) * g_tuning.cameraPanSpeed;
    _scene.UpdateCamera();
  }
}

// Two fingers: the centroid pans, the spread zooms, the twist orbits. Cheap enough to be worth it,
// since without it a tablet has no camera control at all.
void ApplyTwoFingerGesture(Scene& _scene, const PointerTrack& _first, const PointerTrack& _second)
{
  const float centroidX = (_first.xPx + _second.xPx) * 0.5f;
  const float centroidY = (_first.yPx + _second.yPx) * 0.5f;
  const float spread = std::max(1.0f, Distance2D(_first.xPx, _first.yPx, _second.xPx, _second.yPx));
  const float angle = std::atan2(_second.yPx - _first.yPx, _second.xPx - _first.xPx);

  if (!g_gestureActive)
  {
    g_gestureActive = true;
    g_gestureCentroidXPx = centroidX;
    g_gestureCentroidYPx = centroidY;
    g_gestureSpreadPx = spread;
    g_gestureAngleRad = angle;
    return;
  }

  g_tuning.cameraDistance =
      std::clamp(g_tuning.cameraDistance * (g_gestureSpreadPx / spread), g_tuning.cameraMinZoom, g_tuning.cameraMaxZoom);
  const float twistDeg = XMConvertToDegrees(std::remainder(angle - g_gestureAngleRad, XM_2PI));
  g_tuning.cameraYawDeg = std::remainder(g_tuning.cameraYawDeg + twistDeg, 360.0f);
  _scene.UpdateCamera();

  XMFLOAT3 before;
  XMFLOAT3 after;
  if (_scene.RayToGround(g_gestureCentroidXPx, g_gestureCentroidYPx, before) && _scene.RayToGround(centroidX, centroidY, after))
  {
    _scene.m_cameraGoal.x -= (after.x - before.x) * g_tuning.cameraPanSpeed;
    _scene.m_cameraGoal.z -= (after.z - before.z) * g_tuning.cameraPanSpeed;
    _scene.UpdateCamera();
  }

  g_gestureCentroidXPx = centroidX;
  g_gestureCentroidYPx = centroidY;
  g_gestureSpreadPx = spread;
  g_gestureAngleRad = angle;
  g_cameraMoved = true;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Pointer handling. One path for mouse and touch: WM_POINTER gives both, and the second and third
// buttons drive the camera so a single contact is always free for selection and orders.

void Scene::ApplyPointerEvent(const PointerEvent& _event)
{
  if (_event.kind == PointerEvent::Kind::Wheel)
  {
    const float step = std::pow(std::max(1.001f, g_tuning.cameraZoomStepFactor), float(-_event.wheelNotches));
    g_tuning.cameraDistance = std::clamp(g_tuning.cameraDistance * step, g_tuning.cameraMinZoom, g_tuning.cameraMaxZoom);
    g_cameraMoved = true;
    return;
  }

  if (_event.kind == PointerEvent::Kind::Down)
  {
    PointerTrack* slot = FindTrack(_event.pointerId);
    if (!slot)
    {
      for (PointerTrack& candidate : g_pointers)
      {
        if (!candidate.active)
        {
          slot = &candidate;
          break;
        }
      }
    }
    if (!slot)
    {
      return;
    }
    *slot = PointerTrack{};
    slot->id = _event.pointerId;
    slot->active = true;
    slot->isTouch = _event.isTouch;
    slot->buttons = _event.buttons;
    slot->startXPx = slot->xPx = slot->prevXPx = _event.xPx;
    slot->startYPx = slot->yPx = slot->prevYPx = _event.yPx;
    slot->downQpc = _event.timestampQpc;
    slot->cameraDrag = (_event.buttons & 0x6u) != 0; // second or third button
    g_gestureActive = false;                         // a new contact restarts any gesture
    return;
  }

  PointerTrack* track = FindTrack(_event.pointerId);
  if (!track)
  {
    return;
  }
  track->prevXPx = track->xPx;
  track->prevYPx = track->yPx;
  track->xPx = _event.xPx;
  track->yPx = _event.yPx;
  track->buttons = _event.buttons;

  if (_event.kind == PointerEvent::Kind::Update)
  {
    m_hoverShip = PickShip(_event.xPx, _event.yPx);

    PointerTrack* first = nullptr;
    PointerTrack* second = nullptr;
    if (TwoTouches(first, second))
    {
      first->inGesture = true;
      second->inGesture = true;
      ApplyTwoFingerGesture(*this, *first, *second);
      return;
    }
    g_gestureActive = false;

    if (track->cameraDrag)
    {
      ApplyCameraDrag(*this, *track);
      return;
    }
    if ((_event.buttons & 1u) == 0) // hovering, nothing held
    {
      return;
    }

    if (!track->dragging && Distance2D(track->startXPx, track->startYPx, track->xPx, track->yPx) >= g_tuning.inputDragThresholdPx)
    {
      track->dragging = true;
      // With nothing selected a drag bands a box; with a selection it lays down a move order and
      // its final facing. Shift forces the box either way.
      track->boxSelecting = _event.shift || SelectedCount() == 0;
    }
    if (track->dragging)
    {
      m_boxActive = track->boxSelecting;
      m_orderDragActive = !track->boxSelecting;
      m_boxX0Px = m_orderX0Px = track->startXPx;
      m_boxY0Px = m_orderY0Px = track->startYPx;
      m_boxX1Px = m_orderX1Px = track->xPx;
      m_boxY1Px = m_orderY1Px = track->yPx;
    }
    return;
  }

  // Release.
  const PointerTrack finished = *track;
  track->active = false;
  m_boxActive = false;
  m_orderDragActive = false;
  if (ActiveTouchCount() < 2)
  {
    g_gestureActive = false;
  }
  bool anyStillDown = false;
  for (const PointerTrack& other : g_pointers)
  {
    anyStillDown = anyStillDown || other.active;
  }
  if (!anyStillDown && g_cameraMoved)
  {
    g_cameraMoved = false;
    TuningRefreshWindow(); // the sliders should show where the drag left the camera
  }
  if (finished.inGesture || finished.cameraDrag)
  {
    return;
  }

  if (finished.dragging)
  {
    if (finished.boxSelecting)
    {
      FinishBoxSelect(finished.startXPx, finished.startYPx, _event.xPx, _event.yPx, _event.shift);
    }
    else
    {
      FinishOrderDrag(finished.startXPx, finished.startYPx, _event.xPx, _event.yPx);
    }
    return;
  }
  if (ElapsedMs(finished.downQpc, _event.timestampQpc) <= g_tuning.inputTapMaxDurationMs)
  {
    HandleTap(_event.xPx, _event.yPx, _event.shift, _event.timestampQpc);
  }
}

void Scene::Update(uint32_t _viewWidthPx, uint32_t _viewHeightPx)
{
  m_viewWidthPx = std::max(1u, _viewWidthPx);
  m_viewHeightPx = std::max(1u, _viewHeightPx);
  UpdateCamera(); // picking needs matrices that match what was on screen when the pointer moved
  for (const PointerEvent& event : m_pendingEvents)
  {
    ApplyPointerEvent(event);
  }
  m_pendingEvents.clear();
  UpdateCamera(); // input may have moved it again
}

// ---------------------------------------------------------------------------------------------
// One fixed 60 Hz tick. Turn towards the target at a limited rate, drive forward along the facing,
// slow down in time to stop on the point. No pathfinding and no avoidance, by design.

void Scene::Step()
{
  const float maxSpeed = std::max(0.0f, g_tuning.motionMaxSpeed);
  const float acceleration = std::max(0.01f, g_tuning.motionAcceleration);
  const float deceleration = std::max(0.01f, g_tuning.motionDeceleration);
  const float maxTurnRate = XMConvertToRadians(std::max(0.0f, g_tuning.motionTurnRateDegPerSec));
  const float turnAcceleration = XMConvertToRadians(std::max(1.0f, g_tuning.motionTurnAcceleration));
  const float arrivalRadius = std::max(0.01f, g_tuning.motionArrivalRadius);
  const float stopBlend = HalfLifeBlend(SIM_DT, g_tuning.motionStopDampingHalfLife);

  const float scale = std::max(0.01f, g_tuning.shipScale);
  for (Ship& ship : m_ships)
  {
    ship.prevPos = ship.posWorld;
    ship.prevHeading = ship.headingRad;
    const float speedBefore = ship.speed;

    float desiredHeading = ship.headingRad;
    float desiredSpeed = 0.0f;

    if (ship.order == OrderState::Moving)
    {
      const float dx = ship.orderPos.x - ship.posWorld.x;
      const float dz = ship.orderPos.z - ship.posWorld.z;
      const float distance = std::sqrt(dx * dx + dz * dz);
      if (distance <= arrivalRadius)
      {
        ship.order = ship.orderHasFacing ? OrderState::Aligning : OrderState::Idle;
      }
      else
      {
        desiredHeading = std::atan2(dx, dz);
        // Never faster than can still be shed before the point.
        desiredSpeed = std::min(maxSpeed, std::sqrt(2.0f * deceleration * (distance - arrivalRadius)));
      }
    }
    else if (ship.order == OrderState::Aligning)
    {
      desiredHeading = ship.orderFacingRad;
      if (std::fabs(XMScalarModAngle(desiredHeading - ship.headingRad)) < 0.02f && std::fabs(ship.speed) < 0.05f)
      {
        ship.order = OrderState::Idle;
      }
    }

    // Angular velocity accelerates towards whatever closes the error, capped both by the turn rate
    // and by what can still be brought to rest inside the angle that is left.
    const float headingError = XMScalarModAngle(desiredHeading - ship.headingRad);
    const float settleRate = std::sqrt(2.0f * turnAcceleration * std::fabs(headingError));
    float targetRate = std::clamp(headingError / SIM_DT, -maxTurnRate, maxTurnRate);
    targetRate = std::clamp(targetRate, -settleRate, settleRate);
    ship.turnRateRadPerSec = MoveTowards(ship.turnRateRadPerSec, targetRate, turnAcceleration * SIM_DT);
    ship.headingRad = XMScalarModAngle(ship.headingRad + ship.turnRateRadPerSec * SIM_DT);

    // Only drive hard while roughly pointed the right way, so ships arc round instead of pivoting
    // on the spot and then snapping into motion.
    desiredSpeed *= std::max(0.0f, std::cos(headingError));

    if (ship.order == OrderState::Moving)
    {
      ship.speed = MoveTowards(ship.speed, desiredSpeed, (desiredSpeed > ship.speed ? acceleration : deceleration) * SIM_DT);
    }
    else
    {
      ship.speed -= ship.speed * stopBlend; // half-life damping down to a standstill
      if (std::fabs(ship.speed) < 0.01f)
      {
        ship.speed = 0.0f;
      }
    }

    ship.posWorld.x += std::sin(ship.headingRad) * ship.speed * SIM_DT;
    ship.posWorld.z += std::cos(ship.headingRad) * ship.speed * SIM_DT;

    // What the thruster glow and trail are driven by.
    ship.accelSample = (ship.speed - speedBefore) / SIM_DT;

    // One trail sample per tick, so trailLength means the same thing whatever the frame rate.
    const float cosH = std::cos(ship.headingRad);
    const float sinH = std::sin(ship.headingRad);
    ship.trailHead = (ship.trailHead + 1) % TRAIL_SAMPLES;
    ship.trail[ship.trailHead] =
        XMFLOAT3(ship.posWorld.x + (ship.thrusterLocal.x * cosH + ship.thrusterLocal.z * sinH) * scale,
                 ship.posWorld.y + g_tuning.shipHoverHeight + (ship.restY + ship.thrusterLocal.y) * scale,
                 ship.posWorld.z + (-ship.thrusterLocal.x * sinH + ship.thrusterLocal.z * cosH) * scale);
    ship.trailCount = std::min(ship.trailCount + 1, TRAIL_SAMPLES);
  }
}


// ---------------------------------------------------------------------------------------------
// Feedback. Real time rather than sim time: none of it feeds back into Step, so the simulation
// stays fixed-step and deterministic while the look of it runs as fast as the swapchain does.

// A ring that overshoots is a spring, so use one. The peak overshoot fixes the damping ratio and
// the settle half-life then fixes the frequency, which makes both tuning values mean exactly what
// they say rather than being two knobs on the same vague curve.
void SpringTowards(float& _value, float& _velocity, float _target, float _overshoot, float _settleHalfLifeSec, float _dtSec)
{
  const float overshoot = std::clamp(_overshoot - 1.0f, 0.002f, 0.95f);
  const float logOvershoot = std::log(overshoot);
  const float damping = -logOvershoot / std::sqrt(XM_PI * XM_PI + logOvershoot * logOvershoot);
  // The envelope decays as exp(-damping * omega * t), so this omega puts its half-life exactly on
  // the tuned one. Capped against the step size so dragging the slider to nothing cannot blow up.
  float omega = 0.6931472f / (std::max(0.001f, _settleHalfLifeSec) * std::max(0.05f, damping));
  omega = std::min(omega, 0.8f / std::max(1e-5f, _dtSec));

  _velocity += (omega * omega * (_target - _value) - 2.0f * damping * omega * _velocity) * _dtSec;
  _value += _velocity * _dtSec;
}

void Scene::TriggerCameraShake()
{
  m_shakeAmount = 1.0f;
}

void Scene::UpdateFeedback(float _dtSec)
{
  const float dt = std::clamp(_dtSec, 0.0f, 0.1f);
  const float shipScale = std::max(0.01f, g_tuning.shipScale);
  const float maxTurnRate = XMConvertToRadians(std::max(1.0f, g_tuning.motionTurnRateDegPerSec));
  const float maxBank = XMConvertToRadians(g_tuning.bankMaxAngleDeg);
  const float accelReference = std::max(0.01f, g_tuning.motionAcceleration);

  for (int i = 0; i < int(m_ships.size()); ++i)
  {
    Ship& ship = m_ships[size_t(i)];

    // Selection ring: a straight ramp on the tuned duration for the fade, and a spring for the
    // scale so it sails past and settles back.
    const float rampSec = std::max(0.001f, (ship.selected ? g_tuning.selRingFadeInMs : g_tuning.selRingFadeOutMs) * 0.001f);
    ship.ringFade = MoveTowards(ship.ringFade, ship.selected ? 1.0f : 0.0f, dt / rampSec);
    // The spring chases the selected state directly rather than the fade ramp, so the peak really
    // is ringScaleOvershoot and the two knobs stay independent: one shapes alpha, one shapes size.
    SpringTowards(ship.ringScale, ship.ringScaleVel, ship.selected ? 1.0f : 0.0f, g_tuning.selRingScaleOvershoot,
                  g_tuning.selOvershootSettleHalfLife, dt);
    ship.ringScale = std::max(0.0f, ship.ringScale);

    // Hover.
    ship.hoverAmount += ((i == m_hoverShip ? 1.0f : 0.0f) - ship.hoverAmount) * HalfLifeBlend(dt, g_tuning.selHoverResponseHalfLife);

    // Bank into the turn, proportional to angular velocity. Rolling starboard down in a starboard
    // turn means a negative roll, since a positive rotation about +Z lifts the starboard side.
    const float bankTarget = -std::clamp(ship.turnRateRadPerSec / maxTurnRate, -1.0f, 1.0f) * maxBank;
    const bool goingIn = std::fabs(bankTarget) > std::fabs(ship.bankRad);
    const float bankHalfLife = goingIn ? g_tuning.bankResponseHalfLife : g_tuning.bankReturnHalfLife;
    ship.bankRad += (bankTarget - ship.bankRad) * HalfLifeBlend(dt, bankHalfLife);

    // Thrusters follow acceleration, not speed: they flare on the way up to cruise and go quiet
    // once the ship is coasting.
    const float drive = std::clamp(ship.accelSample / accelReference, 0.0f, 1.0f);
    const float idle = g_tuning.thrusterIdleIntensity;
    const float thrusterTarget = idle + (g_tuning.thrusterMaxIntensity - idle) * drive;
    ship.thrusterIntensity += (thrusterTarget - ship.thrusterIntensity) * HalfLifeBlend(dt, g_tuning.thrusterResponseHalfLife);
  }

  // Order markers age out on their own.
  const float markerLifeSec = std::max(0.05f, g_tuning.markerLifetimeMs * 0.001f);
  for (OrderMarker& marker : m_markers)
  {
    marker.ageSec += dt;
  }
  m_markers.erase(std::remove_if(m_markers.begin(), m_markers.end(),
                                 [markerLifeSec](const OrderMarker& _marker) { return _marker.ageSec >= markerLifeSec; }),
                  m_markers.end());

  // Camera. While a selection is under way the goal rides with it; otherwise it stays where
  // panning left it. The lead pushes ahead of where the group is going, and the target eases in
  // behind, which is the lag.
  float leadX = 0.0f;
  float leadZ = 0.0f;
  int movingCount = 0;
  float centreX = 0.0f;
  float centreZ = 0.0f;
  for (const Ship& ship : m_ships)
  {
    if (!ship.selected || ship.order != OrderState::Moving)
    {
      continue;
    }
    ++movingCount;
    centreX += ship.posWorld.x;
    centreZ += ship.posWorld.z;
    leadX += std::sin(ship.headingRad) * ship.speed;
    leadZ += std::cos(ship.headingRad) * ship.speed;
  }
  if (movingCount > 0)
  {
    m_cameraGoal.x = centreX / float(movingCount);
    m_cameraGoal.z = centreZ / float(movingCount);
    leadX = leadX / float(movingCount) * g_tuning.cameraLeadFactor;
    leadZ = leadZ / float(movingCount) * g_tuning.cameraLeadFactor;
  }

  const float follow = HalfLifeBlend(dt, g_tuning.cameraFollowHalfLife);
  m_cameraTarget.x += (m_cameraGoal.x + leadX - m_cameraTarget.x) * follow;
  m_cameraTarget.z += (m_cameraGoal.z + leadZ - m_cameraTarget.z) * follow;

  // Shake: two detuned sines per axis so it never reads as a single wobble, decaying on its own
  // half-life, thrown across the view rather than along the world axes.
  m_shakeTimeSec += dt;
  m_shakeAmount -= m_shakeAmount * HalfLifeBlend(dt, g_tuning.cameraShakeDecayHalfLife);
  if (m_shakeAmount < 0.001f)
  {
    m_shakeAmount = 0.0f;
    m_shakeOffset = XMFLOAT3(0.0f, 0.0f, 0.0f);
  }
  else
  {
    const float w = XM_2PI * std::max(0.1f, g_tuning.cameraShakeFrequencyHz);
    const float t = m_shakeTimeSec;
    const float swing = m_shakeAmount * g_tuning.cameraShakeAmplitude;
    const float acrossView = (std::sin(t * w) * 0.62f + std::sin(t * w * 1.71f) * 0.38f) * swing;
    const float upView = (std::sin(t * w * 1.31f) * 0.62f + std::sin(t * w * 2.13f) * 0.38f) * swing;
    m_shakeOffset = XMFLOAT3(m_cameraRight.x * acrossView + m_cameraUp.x * upView, m_cameraRight.y * acrossView + m_cameraUp.y * upView,
                             m_cameraRight.z * acrossView + m_cameraUp.z * upView);
  }

  UpdateCamera(); // everything above moved it
}

// ---------------------------------------------------------------------------------------------

void Scene::Render(Gfx& _gfx, float _alpha)
{
  SceneFrame frame = {};
  frame.viewProj = m_viewProj;
  frame.lightDir = XMFLOAT3(g_tuning.lightDirX, g_tuning.lightDirY, g_tuning.lightDirZ);
  frame.ambient = g_tuning.ambientLevel;
  frame.gridColour = Rgba{g_tuning.gridColourR, g_tuning.gridColourG, g_tuning.gridColourB, g_tuning.gridStrength};
  frame.gridSpacing = g_tuning.gridSpacing;
  frame.gridLineWidthPx = g_tuning.gridLineWidthPx;
  frame.gridFadeDistance = g_tuning.gridFadeDistance;
  frame.cameraPos = m_cameraEye;
  _gfx.BeginScene(frame);

  XMFLOAT4X4 world;
  const float groundSize = std::max(1.0f, g_tuning.groundSize);
  XMStoreFloat4x4(&world,
                  XMMatrixScaling(groundSize, 1.0f, groundSize) * XMMatrixTranslation(m_cameraTarget.x, 0.0f, m_cameraTarget.z));
  _gfx.DrawMesh(m_groundMesh, world, Rgba{g_tuning.groundColourR, g_tuning.groundColourG, g_tuning.groundColourB, 1.0f}, 0.0f, true);

  const float scale = std::max(0.01f, g_tuning.shipScale);
  const Rgba plain{g_tuning.shipColourR, g_tuning.shipColourG, g_tuning.shipColourB, 1.0f};
  const Rgba picked{g_tuning.selectedColourR, g_tuning.selectedColourG, g_tuning.selectedColourB, 1.0f};

  for (const Ship& ship : m_ships)
  {
    // Between ticks, so motion is smooth however fast the swapchain runs.
    const float x = ship.prevPos.x + (ship.posWorld.x - ship.prevPos.x) * _alpha;
    const float y = ship.prevPos.y + (ship.posWorld.y - ship.prevPos.y) * _alpha;
    const float z = ship.prevPos.z + (ship.posWorld.z - ship.prevPos.z) * _alpha;
    const float heading = ship.prevHeading + XMScalarModAngle(ship.headingRad - ship.prevHeading) * _alpha;

    // Roll about the hull's own mid-height axis, not its base, or a banked ship pivots on one
    // wingtip. shipHoverHeight is what keeps the low wing out of the ground while it does.
    const float rollAxisY = ship.pickCentre.y;
    const XMMATRIX hull = XMMatrixTranslation(0.0f, -rollAxisY, 0.0f) * XMMatrixRotationZ(ship.bankRad) *
                          XMMatrixTranslation(0.0f, rollAxisY + ship.restY, 0.0f) * XMMatrixScaling(scale, scale, scale) *
                          XMMatrixRotationY(heading) * XMMatrixTranslation(x, y + g_tuning.shipHoverHeight, z);
    XMStoreFloat4x4(&world, hull);

    Rgba tint = ship.selected ? picked : plain;
    const float lift = ship.hoverAmount * g_tuning.selHoverHighlightStrength;
    tint = Rgba{tint.r + (1.0f - tint.r) * lift, tint.g + (1.0f - tint.g) * lift, tint.b + (1.0f - tint.b) * lift, 1.0f};
    _gfx.DrawMesh(ship.mesh, world, tint, g_tuning.shipMaterialMix, false);
  }

  DrawFeedback(_gfx, _alpha);

  // Screen-space feedback for whatever the pointer is in the middle of. The proper rings and
  // markers arrive at stage 4.
  if (m_boxActive)
  {
    const Rgba edge{g_tuning.selRingColourR, g_tuning.selRingColourG, g_tuning.selRingColourB, g_tuning.selRingColourA};
    const Rgba fill{edge.r, edge.g, edge.b, edge.a * 0.12f};
    const float left = std::min(m_boxX0Px, m_boxX1Px);
    const float right = std::max(m_boxX0Px, m_boxX1Px);
    const float top = std::min(m_boxY0Px, m_boxY1Px);
    const float bottom = std::max(m_boxY0Px, m_boxY1Px);
    _gfx.DrawScreenRect(left, top, right, bottom, fill);
    _gfx.DrawScreenRect(left, top, right, top + 1.0f, edge);
    _gfx.DrawScreenRect(left, bottom - 1.0f, right, bottom, edge);
    _gfx.DrawScreenRect(left, top, left + 1.0f, bottom, edge);
    _gfx.DrawScreenRect(right - 1.0f, top, right, bottom, edge);
  }
  if (m_orderDragActive)
  {
    const Rgba line{g_tuning.markerColourR, g_tuning.markerColourG, g_tuning.markerColourB, g_tuning.markerColourA};
    _gfx.DrawScreenLine(m_orderX0Px, m_orderY0Px, m_orderX1Px, m_orderY1Px, 2.0f, line);
  }
}

// ---------------------------------------------------------------------------------------------
// The overlay pass: selection and hover rings on the ground, order markers, thruster glow and
// trail in the air. All of it is the same unit quad shaped by the decal shader.

void Scene::DrawFeedback(Gfx& _gfx, float _alpha)
{
  _gfx.BeginDecals(m_viewProj, m_cameraEye);

  const float scale = std::max(0.01f, g_tuning.shipScale);
  const float decalLiftY = 0.2f; // clear of the ground quad so the two cannot z-fight
  XMFLOAT4X4 world;

  // --- selection and hover rings --------------------------------------------------------------
  const Rgba ringColour{g_tuning.selRingColourR, g_tuning.selRingColourG, g_tuning.selRingColourB, g_tuning.selRingColourA};
  for (const Ship& ship : m_ships)
  {
    const float hullRadius = std::max(ship.halfExtents.x, ship.halfExtents.z) * scale;
    const float x = ship.prevPos.x + (ship.posWorld.x - ship.prevPos.x) * _alpha;
    const float z = ship.prevPos.z + (ship.posWorld.z - ship.prevPos.z) * _alpha;

    if (ship.ringFade > 0.002f && ship.ringScale > 0.002f)
    {
      const float radius = hullRadius * g_tuning.selRingRadiusScale * ship.ringScale;
      XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) * XMMatrixTranslation(x, decalLiftY, z));
      _gfx.DrawDecal(m_groundMesh, world, Rgba{ringColour.r, ringColour.g, ringColour.b, ringColour.a * ship.ringFade},
                     g_tuning.selRingThickness, 0.0f);
    }

    // The hover ring sits just outside the selection ring so both are readable at once.
    if (ship.hoverAmount > 0.002f)
    {
      const float radius = hullRadius * g_tuning.selRingRadiusScale * 1.12f;
      XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) * XMMatrixTranslation(x, decalLiftY, z));
      _gfx.DrawDecal(m_groundMesh, world, Rgba{ringColour.r, ringColour.g, ringColour.b, g_tuning.selHoverRingAlpha * ship.hoverAmount},
                     g_tuning.selRingThickness * 0.7f, 0.0f);
    }
  }

  // --- order markers --------------------------------------------------------------------------
  const Rgba markerColour{g_tuning.markerColourR, g_tuning.markerColourG, g_tuning.markerColourB, g_tuning.markerColourA};
  const float expandSec = std::max(0.001f, g_tuning.markerExpandMs * 0.001f);
  const float pulseSec = std::max(0.001f, g_tuning.markerPulsePeriodMs * 0.001f);
  const float lifeSec = std::max(0.05f, g_tuning.markerLifetimeMs * 0.001f);
  const float fadeSec = std::max(0.001f, g_tuning.markerFadeOutMs * 0.001f);
  const int pulseCount = std::max(0, int(g_tuning.markerPulseCount + 0.5f));

  for (const OrderMarker& marker : m_markers)
  {
    // Expands in, holds while it pulses, fades out at the end of its life.
    const float grow = std::clamp(marker.ageSec / expandSec, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - grow) * (1.0f - grow); // ease out
    const float fadeStart = std::max(0.0f, lifeSec - fadeSec);
    const float fade = (marker.ageSec <= fadeStart) ? 1.0f : std::clamp(1.0f - (marker.ageSec - fadeStart) / fadeSec, 0.0f, 1.0f);

    const float sincePulseStart = marker.ageSec - expandSec;
    const float pulseIndex = sincePulseStart / pulseSec;
    float beat = 0.0f;
    if (sincePulseStart > 0.0f && pulseIndex < float(pulseCount))
    {
      const float withinPulse = pulseIndex - std::floor(pulseIndex);
      beat = std::sin(withinPulse * XM_PI); // 0 at each pulse boundary, 1 in the middle
    }

    const float radius = std::max(0.1f, g_tuning.markerRadius) * eased * (1.0f + beat * g_tuning.markerPulseScale);
    const float alpha = markerColour.a * fade * (0.72f + beat * 0.28f);
    XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) *
                                XMMatrixTranslation(marker.posWorld.x, decalLiftY, marker.posWorld.z));
    _gfx.DrawDecal(m_groundMesh, world, Rgba{markerColour.r, markerColour.g, markerColour.b, alpha}, g_tuning.markerThickness, 0.10f);

    // Each pulse also throws a ripple outwards, which is what makes the count readable.
    if (beat > 0.0f)
    {
      const float withinPulse = pulseIndex - std::floor(pulseIndex);
      const float rippleRadius = radius * (1.0f + withinPulse * 1.1f);
      XMStoreFloat4x4(&world, XMMatrixScaling(rippleRadius * 2.0f, 1.0f, rippleRadius * 2.0f) *
                                  XMMatrixTranslation(marker.posWorld.x, decalLiftY, marker.posWorld.z));
      _gfx.DrawDecal(m_groundMesh, world, Rgba{markerColour.r, markerColour.g, markerColour.b, alpha * (1.0f - withinPulse) * 0.7f},
                     g_tuning.markerThickness * 0.6f, 0.0f);
    }

    // A pip out along the ordered facing, so a drag order shows which way the ships will end up.
    if (marker.hasFacing)
    {
      const float pip = radius * 0.28f;
      const float outX = marker.posWorld.x + std::sin(marker.facingRad) * radius * 1.5f;
      const float outZ = marker.posWorld.z + std::cos(marker.facingRad) * radius * 1.5f;
      XMStoreFloat4x4(&world, XMMatrixScaling(pip * 2.0f, 1.0f, pip * 2.0f) * XMMatrixTranslation(outX, decalLiftY, outZ));
      _gfx.DrawDecal(m_groundMesh, world, Rgba{markerColour.r, markerColour.g, markerColour.b, alpha}, 1.0f, 1.0f);
    }
  }

  // --- thruster glow and trail ----------------------------------------------------------------
  // Billboards: the unit quad lies in XZ, so putting the camera right vector in row 0 and the
  // camera up vector in row 2 turns it to face the eye wherever it is.
  const float glowRadius = std::max(0.1f, g_tuning.thrusterGlowRadius) * scale;
  const float trailLength = std::max(0.0f, g_tuning.thrusterTrailLength);
  const float trailFade = std::max(0.01f, g_tuning.thrusterTrailFade);

  for (const Ship& ship : m_ships)
  {
    if (!ship.hasThruster || ship.thrusterIntensity <= 0.002f)
    {
      continue;
    }
    const Rgba glowColour{g_tuning.selectedColourR, g_tuning.selectedColourG, g_tuning.selectedColourB, ship.thrusterIntensity};

    // Newest sample first, walking back along the path until trailLength runs out. The trail
    // follows the path the ship actually took, so it curves through a turn.
    float travelled = 0.0f;
    XMFLOAT3 previous = ship.trail[ship.trailHead];
    for (int step = 0; step < ship.trailCount; ++step)
    {
      const int index = ((ship.trailHead - step) % TRAIL_SAMPLES + TRAIL_SAMPLES) % TRAIL_SAMPLES;
      const XMFLOAT3 point = ship.trail[index];
      if (step > 0)
      {
        travelled += Distance2D(previous.x, previous.z, point.x, point.z);
        if (travelled >= trailLength)
        {
          break;
        }
      }
      previous = point;

      const float along = (trailLength > 0.0f) ? travelled / trailLength : 1.0f;
      const float taper = std::pow(std::max(0.0f, 1.0f - along), trailFade);
      const float radius = glowRadius * (step == 0 ? 1.0f : taper * 0.8f);
      const float alpha = glowColour.a * (step == 0 ? 1.0f : taper * 0.55f);
      if (alpha <= 0.002f || radius <= 0.001f)
      {
        continue;
      }

      XMFLOAT4X4 billboard;
      billboard._11 = m_cameraRight.x * radius * 2.0f;
      billboard._12 = m_cameraRight.y * radius * 2.0f;
      billboard._13 = m_cameraRight.z * radius * 2.0f;
      billboard._14 = 0.0f;
      billboard._21 = 0.0f;
      billboard._22 = 1.0f;
      billboard._23 = 0.0f;
      billboard._24 = 0.0f;
      billboard._31 = m_cameraUp.x * radius * 2.0f;
      billboard._32 = m_cameraUp.y * radius * 2.0f;
      billboard._33 = m_cameraUp.z * radius * 2.0f;
      billboard._34 = 0.0f;
      billboard._41 = point.x;
      billboard._42 = point.y;
      billboard._43 = point.z;
      billboard._44 = 1.0f;
      _gfx.DrawGlow(m_groundMesh, billboard, Rgba{glowColour.r, glowColour.g, glowColour.b, alpha}, g_tuning.thrusterGlowFalloff);

      if (trailLength <= 0.0f)
      {
        break;
      }
    }
  }
}
