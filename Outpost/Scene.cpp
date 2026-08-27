#include "pch.h"
#include "Scene.h"

using namespace DirectX;

namespace
{
// Camera framing and feel.
constexpr float CAMERA_MIN_ZOOM                = 40.f;
constexpr float CAMERA_MAX_ZOOM                = 900.f;
constexpr float CAMERA_TARGET_HEIGHT           = 3.f;
constexpr float CAMERA_FOV_DEG                 = 45.f;
constexpr float CAMERA_NEAR_PLANE              = 0.5f;
constexpr float CAMERA_FAR_PLANE               = 8000.f;
constexpr float CAMERA_PAN_SPEED               = 1.f;
constexpr float CAMERA_FOLLOW_HALF_LIFE        = 0.18f;
constexpr float CAMERA_LEAD_FACTOR             = 0.35f;
constexpr float CAMERA_SHAKE_AMPLITUDE         = 2.5f;
constexpr float CAMERA_SHAKE_DECAY_HALF_LIFE   = 0.15f;
constexpr float CAMERA_SHAKE_FREQUENCY_HZ      = 22.f;
constexpr float CAMERA_ROTATE_SPEED_DEG_PER_PX = 0.35f;
constexpr float CAMERA_ZOOM_STEP_FACTOR        = 1.12f;

// Selection rings and hover highlight.
constexpr float SEL_RING_FADE_IN_MS            = 140.f;
constexpr float SEL_RING_FADE_OUT_MS           = 180.f;
constexpr float SEL_RING_SCALE_OVERSHOOT       = 1.35f;
constexpr float SEL_OVERSHOOT_SETTLE_HALF_LIFE = 0.09f;
constexpr float SEL_HOVER_HIGHLIGHT_STRENGTH   = 0.45f;
constexpr float SEL_HOVER_RING_ALPHA           = 0.35f;
constexpr float SEL_HOVER_RESPONSE_HALF_LIFE   = 0.06f;
constexpr float SEL_RING_THICKNESS             = 0.16f;
constexpr float SEL_RING_RADIUS_SCALE          = 1.25f;
constexpr float SEL_RING_COLOUR_R              = 0.35f;
constexpr float SEL_RING_COLOUR_G              = 0.95f;
constexpr float SEL_RING_COLOUR_B              = 0.55f;
constexpr float SEL_RING_COLOUR_A              = 0.9f;

// Order markers.
constexpr float MARKER_EXPAND_MS               = 160.f;
constexpr float MARKER_PULSE_COUNT             = 3.f;
constexpr float MARKER_PULSE_PERIOD_MS         = 320.f;
constexpr float MARKER_LIFETIME_MS             = 1400.f;
constexpr float MARKER_FADE_OUT_MS             = 260.f;
constexpr float MARKER_RADIUS                  = 9.f;
constexpr float MARKER_THICKNESS               = 0.18f;
constexpr float MARKER_PULSE_SCALE             = 0.18f;
constexpr float MARKER_COLOUR_R                = 0.95f;
constexpr float MARKER_COLOUR_G                = 0.78f;
constexpr float MARKER_COLOUR_B                = 0.28f;
constexpr float MARKER_COLOUR_A                = 0.9f;

// Ship motion.
constexpr float MOTION_MAX_SPEED               = 34.f;
constexpr float MOTION_ACCELERATION            = 26.f;
constexpr float MOTION_DECELERATION            = 34.f;
constexpr float MOTION_TURN_RATE_DEG_PER_SEC   = 70.f;
constexpr float MOTION_TURN_ACCELERATION       = 240.f;
constexpr float MOTION_ARRIVAL_RADIUS          = 3.5f;
constexpr float MOTION_STOP_DAMPING_HALF_LIFE  = 0.16f;
constexpr float MOTION_FORMATION_SPACING       = 34.f;
constexpr float MOTION_FORMATION_SHAPE         = 1.f;

// Banking into turns.
constexpr float BANK_MAX_ANGLE_DEG             = 28.f;
constexpr float BANK_RESPONSE_HALF_LIFE        = 0.14f;
constexpr float BANK_RETURN_HALF_LIFE          = 0.3f;

// Thruster glow and trails.
constexpr float THRUSTER_IDLE_INTENSITY        = 0.12f;
constexpr float THRUSTER_MAX_INTENSITY         = 1.f;
constexpr float THRUSTER_RESPONSE_HALF_LIFE    = 0.1f;
constexpr float THRUSTER_TRAIL_LENGTH          = 18.f;
constexpr float THRUSTER_TRAIL_FADE            = 0.55f;
constexpr float THRUSTER_GLOW_RADIUS           = 6.f;
constexpr float THRUSTER_GLOW_FALLOFF          = 2.2f;

// Pointer thresholds.
constexpr float INPUT_DRAG_THRESHOLD_PX        = 6.f;
constexpr float INPUT_TAP_MAX_DURATION_MS      = 320.f;
constexpr float INPUT_DOUBLE_TAP_WINDOW_MS     = 300.f;
constexpr float INPUT_PICK_PADDING             = 1.15f;

// Scene colours, lighting and ground grid.
constexpr float SKY_COLOUR_R                   = 0.043f;
constexpr float SKY_COLOUR_G                   = 0.051f;
constexpr float SKY_COLOUR_B                   = 0.063f;
constexpr float GROUND_COLOUR_R                = 0.075f;
constexpr float GROUND_COLOUR_G                = 0.082f;
constexpr float GROUND_COLOUR_B                = 0.094f;
constexpr float GRID_COLOUR_R                  = 0.28f;
constexpr float GRID_COLOUR_G                  = 0.36f;
constexpr float GRID_COLOUR_B                  = 0.44f;
constexpr float GRID_STRENGTH                  = 0.55f;
constexpr float GRID_SPACING                   = 20.f;
constexpr float GRID_LINE_WIDTH_PX             = 1.3f;
constexpr float GRID_FADE_DISTANCE             = 900.f;
constexpr float GROUND_SIZE                    = 4000.f;
constexpr float LIGHT_DIR_X                    = -0.42f;
constexpr float LIGHT_DIR_Y                    = 0.78f;
constexpr float LIGHT_DIR_Z                    = -0.46f;
constexpr float AMBIENT_LEVEL                  = 0.3f;
constexpr float SHIP_COLOUR_R                  = 0.55f;
constexpr float SHIP_COLOUR_G                  = 0.6f;
constexpr float SHIP_COLOUR_B                  = 0.66f;
constexpr float SELECTED_COLOUR_R              = 0.35f;
constexpr float SELECTED_COLOUR_G              = 0.95f;
constexpr float SELECTED_COLOUR_B              = 0.66f;
constexpr float SHIP_MATERIAL_MIX              = 0.55f;
constexpr float SHIP_SCALE                     = 1.f;
constexpr float SHIP_HOVER_HEIGHT              = 4.f;
constexpr float START_SPACING                  = 55.f;

// Change these three names to fly different hulls; everything else adapts to the mesh bounds.
const std::wstring SHIP_MESHES[] = {L"Bomber", L"Corvette", L"Frigate"};

struct LoadedMesh
{
  std::vector<SceneVertex> verts;
  XMFLOAT3 boundsMin{0.0f, 0.0f, 0.0f};
  XMFLOAT3 boundsMax{0.0f, 0.0f, 0.0f};
  std::vector<XMFLOAT3> thrusterLocals; // one centroid per exhaust nozzle
};

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
        materials[current] = XMFLOAT3(static_cast<float>(std::atof(std::string(tokens[1]).c_str())),
                                      static_cast<float>(std::atof(std::string(tokens[2]).c_str())),
                                      static_cast<float>(std::atof(std::string(tokens[3]).c_str())));
      }
    }
  }
  return materials;
}

// Every nozzle on a hull is written with the same "thruster" material, so the material alone
// cannot say how many exhausts there are or where each one sits. Single-link clustering can: the
// faces of one nozzle touch each other, and separate nozzles sit well apart. The link distance
// comes from the median thruster edge rather than from the hull bounds, so it means the same thing
// on a bomber as on a carrier.
std::vector<XMFLOAT3> ClusterThrusters(const std::vector<XMFLOAT3>& _faceCentroids, std::vector<float> _edgeLengths)
{
  if (_faceCentroids.empty())
    return {};

  // Degenerate faces (the exporter writes a few) contribute zero-length edges and would drag the
  // median to nothing, so they are dropped. Their centroids still sit on a nozzle, so the faces
  // themselves stay in.
  std::erase_if(_edgeLengths, [](float _e)
  {
    return _e <= 1e-6f;
  });
  if (_edgeLengths.empty())
    return {_faceCentroids[0]}; // nothing to measure with: take the lot as one exhaust
  const size_t middle = _edgeLengths.size() / 2;
  std::nth_element(_edgeLengths.begin(), _edgeLengths.begin() + static_cast<ptrdiff_t>(middle), _edgeLengths.end());
  const float linkDistance = _edgeLengths[middle] * 1.5f;
  const float linkDistanceSq = linkDistance * linkDistance;

  // Union-find over the face centroids. A few hundred thruster faces on the biggest hull, once at
  // load, so the quadratic pass costs nothing worth avoiding.
  const int count = static_cast<int>(_faceCentroids.size());
  std::vector<int> parent(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i)
    parent[static_cast<size_t>(i)] = i;
  const auto find = [&parent](int _i)
  {
    while (parent[static_cast<size_t>(_i)] != _i)
    {
      parent[static_cast<size_t>(_i)] = parent[static_cast<size_t>(parent[size_t(_i)])];
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

  // Averaged in first-seen order, so the nozzle list comes out the same on every load.
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
    sums[static_cast<size_t>(slot)] = XMFLOAT3(sums[static_cast<size_t>(slot)].x + p.x, sums[static_cast<size_t>(slot)].y + p.y,
                                               sums[static_cast<size_t>(slot)].z + p.z);
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

// OBJ is right-handed and these hulls point their bow at -Z. Negating Z converts to the
// left-handed render basis (east, up, north) and lands the bow on +Z in one step.
bool LoadObj(const std::wstring& _dir, const std::wstring& _name, LoadedMesh& _out)
{
  const std::string text = ReadWholeFile(_dir + _name + L".obj");
  if (text.empty())
    return false;
  const std::unordered_map<std::string, XMFLOAT3> materials = LoadMaterials(_dir + _name + L".mtl");

  std::vector<XMFLOAT3> positions;
  XMFLOAT3 colour(0.7f, 0.7f, 0.7f);
  XMFLOAT3 boundsMin(1e30f, 1e30f, 1e30f);
  XMFLOAT3 boundsMax(-1e30f, -1e30f, -1e30f);
  bool onThruster = false;
  std::vector<XMFLOAT3> thrusterFaces; // one centroid per thruster-material triangle
  std::vector<float> thrusterEdges;    // and their edge lengths, for the clustering distance
  int badFaces = 0;

  for (const std::string_view line : SplitLines(text))
  {
    if (StartsWith(line, "v "))
    {
      const std::vector<std::string_view> tokens = SplitTokens(line);
      if (tokens.size() >= 4)
      {
        const float x = static_cast<float>(std::atof(std::string(tokens[1]).c_str()));
        const float y = static_cast<float>(std::atof(std::string(tokens[2]).c_str()));
        const float z = static_cast<float>(std::atof(std::string(tokens[3]).c_str()));
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
      onThruster = material == "thruster"; // every hull in Assets names its engines this way
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
          _out.verts.push_back(SceneVertex{p.x, p.y, p.z, colour.x, colour.y, colour.z});
        if (onThruster)
        {
          thrusterFaces.push_back(XMFLOAT3((triangle[0].x + triangle[1].x + triangle[2].x) / 3.0f,
                                           (triangle[0].y + triangle[1].y + triangle[2].y) / 3.0f,
                                           (triangle[0].z + triangle[1].z + triangle[2].z) / 3.0f));
          for (int edge = 0; edge < 3; ++edge)
          {
            const XMFLOAT3& a = triangle[edge];
            const XMFLOAT3& b = triangle[(edge + 1) % 3];
            thrusterEdges.push_back(std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)));
          }
        }
      }
    }
  }

  if (_out.verts.size() % 3 != 0)
    _out.verts.resize(_out.verts.size() - _out.verts.size() % 3);
  if (boundsMin.x > boundsMax.x)
  {
    boundsMin = XMFLOAT3(0.0f, 0.0f, 0.0f);
    boundsMax = XMFLOAT3(0.0f, 0.0f, 0.0f);
  }
  _out.boundsMin = boundsMin;
  _out.boundsMax = boundsMax;
  _out.thrusterLocals = ClusterThrusters(thrusterFaces, std::move(thrusterEdges));
  DebugTrace(L"{}: {} tris, {:.1f} x {:.1f} x {:.1f}, {} exhaust{}{}\n", _name, _out.verts.size() / 3, boundsMax.x - boundsMin.x,
             boundsMax.y - boundsMin.y, boundsMax.z - boundsMin.z, _out.thrusterLocals.size(), _out.thrusterLocals.size() == 1 ? L"" : L"s",
             badFaces ? L" (skipped malformed faces)" : L"");
  return !_out.verts.empty();
}

// A unit quad in the ground plane; the shader draws the grid on it procedurally so grid spacing
// stays a constant with no vertex rebuild.
std::vector<SceneVertex> BuildGroundQuad()
{
  constexpr float h = 0.5f;
  return {SceneVertex{-h, 0.0f, -h, 1.0f, 1.0f, 1.0f}, SceneVertex{-h, 0.0f, h, 1.0f, 1.0f, 1.0f},
          SceneVertex{h, 0.0f, h, 1.0f, 1.0f, 1.0f}, SceneVertex{-h, 0.0f, -h, 1.0f, 1.0f, 1.0f}, SceneVertex{h, 0.0f, h, 1.0f, 1.0f, 1.0f},
          SceneVertex{h, 0.0f, -h, 1.0f, 1.0f, 1.0f},};
}

// ---------------------------------------------------------------------------------------------
// Feel maths. Everything that eases uses the half-life form, never a per-frame constant, so the
// same half-life produces the same motion at any frame rate.

float HalfLifeBlend(float _dtSec, float _halfLifeSec)
{
  return (_halfLifeSec <= 0.0f) ? 1.0f : 1.0f - std::exp2(-_dtSec / _halfLifeSec);
}

float MoveTowards(float _current, float _target, float _maxDelta)
{
  const float delta = _target - _current;
  if (std::fabs(delta) <= _maxDelta)
    return _target;
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
  const float lane = static_cast<float>(_slot) - static_cast<float>(_count - 1) * 0.5f;
  switch (_shape)
  {
  case 0: // line abreast
    return XMFLOAT2(lane * _spacing, 0.0f);
  case 2: // box
  {
    const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(float(_count)))));
    const int row = _slot / columns;
    const int column = _slot % columns;
    const int inRow = std::min(columns, _count - row * columns);
    return XMFLOAT2((static_cast<float>(column) - static_cast<float>(inRow - 1) * 0.5f) * _spacing, -static_cast<float>(row) * _spacing);
  }
  case 3: // circle
  {
    if (_count < 2)
      return XMFLOAT2(0.0f, 0.0f);
    const float angle = XM_2PI * static_cast<float>(_slot) / static_cast<float>(_count);
    const float radius = _spacing * static_cast<float>(_count) / XM_2PI;
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
  bool cameraDrag = false; // held with the second or third button
  bool inGesture = false;  // part of a two-finger gesture, so its release means nothing
};

PointerTrack g_pointers[MAX_POINTERS];
int64_t g_qpcFrequency = 1;
int64_t g_lastGroundTapQpc = 0;
float g_lastGroundTapXPx = 0.0f;
float g_lastGroundTapYPx = 0.0f;

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
      return &track;
  }
  return nullptr;
}

int ActiveTouchCount()
{
  int count = 0;
  for (const PointerTrack& track : g_pointers)
    count += (track.active && track.isTouch) ? 1 : 0;
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
      continue;
    if (!_a)
      _a = &track;
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
  return static_cast<float>(double(_toQpc - _fromQpc) / double(g_qpcFrequency) * 1000.0);
}
} // namespace

void Scene::Init(Gfx& _gfx)
{
  LARGE_INTEGER frequency = {};
  QueryPerformanceFrequency(&frequency);
  g_qpcFrequency = frequency.QuadPart;

  const std::wstring meshDir = L"Meshes\\";
  constexpr int shipCount = static_cast<int>(std::size(SHIP_MESHES));
  for (int i = 0; i < shipCount; ++i)
  {
    LoadedMesh loaded;
    if (!LoadObj(meshDir, SHIP_MESHES[i], loaded))
      continue;
    Ship ship;
    ship.mesh = _gfx.UploadMesh(loaded.verts);
    ship.restY = -loaded.boundsMin.y;
    ship.pickCentre = XMFLOAT3((loaded.boundsMin.x + loaded.boundsMax.x) * 0.5f, (loaded.boundsMin.y + loaded.boundsMax.y) * 0.5f,
                               (loaded.boundsMin.z + loaded.boundsMax.z) * 0.5f);
    ship.halfExtents = XMFLOAT3(std::max(0.5f, (loaded.boundsMax.x - loaded.boundsMin.x) * 0.5f),
                                std::max(0.5f, (loaded.boundsMax.y - loaded.boundsMin.y) * 0.5f),
                                std::max(0.5f, (loaded.boundsMax.z - loaded.boundsMin.z) * 0.5f));
    ship.thrusterLocals = loaded.thrusterLocals;
    ship.trail.assign(ship.thrusterLocals.size() * TRAIL_SAMPLES, XMFLOAT3(0.0f, 0.0f, 0.0f));
    ship.posWorld = XMFLOAT3((static_cast<float>(i) - static_cast<float>(shipCount - 1) * 0.5f) * START_SPACING, 0.0f, 0.0f);
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
    ship.selected = false;
}

int Scene::SelectedCount() const
{
  int count = 0;
  for (const Ship& ship : m_ships)
    count += ship.selected ? 1 : 0;
  return count;
}

// ---------------------------------------------------------------------------------------------
// Camera and projection

void Scene::UpdateCamera()
{
  const float yaw = XMConvertToRadians(m_cameraYawDeg);
  const float pitch = XMConvertToRadians(std::clamp(m_cameraPitchDeg, 5.0f, 89.0f));
  const float distance = std::clamp(m_cameraDistance, CAMERA_MIN_ZOOM, CAMERA_MAX_ZOOM);
  m_cameraTarget.y = CAMERA_TARGET_HEIGHT;

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

  const float aspect = static_cast<float>(m_viewWidthPx) / static_cast<float>(std::max(1u, m_viewHeightPx));
  const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
  const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(std::clamp(CAMERA_FOV_DEG, 5.0f, 170.0f)), aspect,
                                                 std::max(0.01f, CAMERA_NEAR_PLANE), std::max(1.0f, CAMERA_FAR_PLANE));
  const XMMATRIX viewProj = view * proj;
  XMStoreFloat4x4(&m_viewProj, viewProj);
  XMStoreFloat4x4(&m_invViewProj, XMMatrixInverse(nullptr, viewProj));
}

void Scene::ScreenRay(float _xPx, float _yPx, XMFLOAT3& _origin, XMFLOAT3& _direction) const
{
  const float ndcX = (_xPx / static_cast<float>(m_viewWidthPx)) * 2.0f - 1.0f;
  const float ndcY = 1.0f - (_yPx / static_cast<float>(m_viewHeightPx)) * 2.0f;
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
    return false;
  const float t = -origin.y / direction.y;
  _point = XMFLOAT3(origin.x + direction.x * t, 0.0f, origin.z + direction.z * t);
  return true;
}

bool Scene::WorldToScreen(const XMFLOAT3& _world, float& _xPx, float& _yPx) const
{
  const XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&_world), XMLoadFloat4x4(&m_viewProj));
  const float w = XMVectorGetW(clip);
  if (w <= 1e-4f) // behind the eye
    return false;
  _xPx = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * static_cast<float>(m_viewWidthPx);
  _yPx = (0.5f - XMVectorGetY(clip) / w * 0.5f) * static_cast<float>(m_viewHeightPx);
  return true;
}

// Ray against each hull's oriented bounding box. A sphere would be far too loose on a hull three
// times longer than it is wide.
int Scene::PickShip(float _xPx, float _yPx) const
{
  XMFLOAT3 origin;
  XMFLOAT3 direction;
  ScreenRay(_xPx, _yPx, origin, direction);
  const float scale = std::max(0.01f, SHIP_SCALE);
  const float padding = std::max(1.0f, INPUT_PICK_PADDING);

  int best = -1;
  float bestT = 1e30f;
  for (int i = 0; i < static_cast<int>(m_ships.size()); ++i)
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
        std::swap(t1, t2);
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
  for (int i = 0; i < static_cast<int>(m_ships.size()); ++i)
  {
    if (m_ships[i].selected)
      chosen.push_back(i);
  }
  if (chosen.empty())
    return;

  // Point the formation along the ordered facing, or along the way the group is about to travel.
  float heading = _facingRad;
  if (!_hasFacing)
  {
    float centreX = 0.0f;
    float centreZ = 0.0f;
    for (const int index : chosen)
    {
      centreX += m_ships[static_cast<size_t>(index)].posWorld.x;
      centreZ += m_ships[static_cast<size_t>(index)].posWorld.z;
    }
    centreX /= static_cast<float>(chosen.size());
    centreZ /= static_cast<float>(chosen.size());
    const float dx = _point.x - centreX;
    const float dz = _point.z - centreZ;
    heading = (dx * dx + dz * dz > 1e-4f) ? std::atan2(dx, dz) : m_ships[static_cast<size_t>(chosen[0])].headingRad;
  }

  // Hand out slots in the order the ships already lie across the formation, so they do not have to
  // cross each other on the way in.
  const float rightX = std::cos(heading);
  const float rightZ = -std::sin(heading);
  std::sort(chosen.begin(), chosen.end(), [&](int _a, int _b)
  {
    return m_ships[static_cast<size_t>(_a)].posWorld.x * rightX + m_ships[static_cast<size_t>(_a)].posWorld.z * rightZ < m_ships[static_cast
             <size_t>(_b)].posWorld.x * rightX + m_ships[static_cast<size_t>(_b)].posWorld.z * rightZ;
  });

  OrderMarker marker;
  marker.posWorld = XMFLOAT3(_point.x, 0.0f, _point.z);
  marker.facingRad = heading;
  marker.hasFacing = _hasFacing;
  m_markers.push_back(marker);

  const int count = static_cast<int>(chosen.size());
  const int shape = std::clamp(static_cast<int>(MOTION_FORMATION_SHAPE + 0.5f), 0, 3);
  const float spacing = std::max(0.0f, MOTION_FORMATION_SPACING);
  const float cosH = std::cos(heading);
  const float sinH = std::sin(heading);

  for (int slot = 0; slot < count; ++slot)
  {
    const XMFLOAT2 local = FormationOffset(slot, count, shape, spacing);
    Ship& ship = m_ships[static_cast<size_t>(chosen[size_t(slot)])];
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
      m_ships[static_cast<size_t>(hit)].selected = !m_ships[static_cast<size_t>(hit)].selected;
    else
    {
      ClearSelection();
      m_ships[static_cast<size_t>(hit)].selected = true;
    }
    g_lastGroundTapQpc = 0; // tapping a hull does not begin a double tap
    return;
  }

  const bool doubleTap = g_lastGroundTapQpc != 0 && ElapsedMs(g_lastGroundTapQpc, _qpc) <= INPUT_DOUBLE_TAP_WINDOW_MS && Distance2D(
                           g_lastGroundTapXPx, g_lastGroundTapYPx, _xPx, _yPx) <= INPUT_DRAG_THRESHOLD_PX * 3.0f;
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
    return;
  XMFLOAT3 point;
  if (RayToGround(_xPx, _yPx, point))
    IssueMoveOrder(point, false, 0.0f);
}

void Scene::FinishBoxSelect(float _x0Px, float _y0Px, float _x1Px, float _y1Px, bool _additive)
{
  const float left = std::min(_x0Px, _x1Px);
  const float right = std::max(_x0Px, _x1Px);
  const float top = std::min(_y0Px, _y1Px);
  const float bottom = std::max(_y0Px, _y1Px);
  if (!_additive)
    ClearSelection();
  const float scale = std::max(0.01f, SHIP_SCALE);
  for (Ship& ship : m_ships)
  {
    const XMFLOAT3 centre(ship.posWorld.x, ship.posWorld.y + ship.halfExtents.y * scale, ship.posWorld.z);
    float xPx = 0.0f;
    float yPx = 0.0f;
    if (WorldToScreen(centre, xPx, yPx) && xPx >= left && xPx <= right && yPx >= top && yPx <= bottom)
      ship.selected = true;
  }
}

void Scene::FinishOrderDrag(float _x0Px, float _y0Px, float _x1Px, float _y1Px)
{
  XMFLOAT3 from;
  XMFLOAT3 to;
  if (!RayToGround(_x0Px, _y0Px, from))
    return;
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
    return;

  if ((_track.buttons & 0x2u) != 0) // second button: orbit
  {
    _scene.m_cameraYawDeg = std::remainder(_scene.m_cameraYawDeg + dx * CAMERA_ROTATE_SPEED_DEG_PER_PX, 360.0f);
    _scene.m_cameraPitchDeg = std::clamp(_scene.m_cameraPitchDeg - dy * CAMERA_ROTATE_SPEED_DEG_PER_PX, 5.0f, 89.0f);
    _scene.UpdateCamera();
    return;
  }

  XMFLOAT3 before;
  XMFLOAT3 after;
  if (_scene.RayToGround(_track.prevXPx, _track.prevYPx, before) && _scene.RayToGround(_track.xPx, _track.yPx, after))
  {
    _scene.m_cameraGoal.x -= (after.x - before.x) * CAMERA_PAN_SPEED;
    _scene.m_cameraGoal.z -= (after.z - before.z) * CAMERA_PAN_SPEED;
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

  _scene.m_cameraDistance =
    std::clamp(_scene.m_cameraDistance * (g_gestureSpreadPx / spread), CAMERA_MIN_ZOOM, CAMERA_MAX_ZOOM);
  const float twistDeg = XMConvertToDegrees(std::remainder(angle - g_gestureAngleRad, XM_2PI));
  _scene.m_cameraYawDeg = std::remainder(_scene.m_cameraYawDeg + twistDeg, 360.0f);
  _scene.UpdateCamera();

  XMFLOAT3 before;
  XMFLOAT3 after;
  if (_scene.RayToGround(g_gestureCentroidXPx, g_gestureCentroidYPx, before) && _scene.RayToGround(centroidX, centroidY, after))
  {
    _scene.m_cameraGoal.x -= (after.x - before.x) * CAMERA_PAN_SPEED;
    _scene.m_cameraGoal.z -= (after.z - before.z) * CAMERA_PAN_SPEED;
    _scene.UpdateCamera();
  }

  g_gestureCentroidXPx = centroidX;
  g_gestureCentroidYPx = centroidY;
  g_gestureSpreadPx = spread;
  g_gestureAngleRad = angle;
}
} // namespace

Rgba Scene::SkyColour() const
{
  return Rgba{SKY_COLOUR_R, SKY_COLOUR_G, SKY_COLOUR_B, 1.0f};
}

// ---------------------------------------------------------------------------------------------
// Pointer handling. One path for mouse and touch: WM_POINTER gives both, and the second and third
// buttons drive the camera so a single contact is always free for selection and orders.

void Scene::ApplyPointerEvent(const PointerEvent& _event)
{
  if (_event.kind == PointerEvent::Kind::Wheel)
  {
    const float step = std::pow(std::max(1.001f, CAMERA_ZOOM_STEP_FACTOR), static_cast<float>(-_event.wheelNotches));
    m_cameraDistance = std::clamp(m_cameraDistance * step, CAMERA_MIN_ZOOM, CAMERA_MAX_ZOOM);
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
      return;
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
    return;
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
      return;

    if (!track->dragging && Distance2D(track->startXPx, track->startYPx, track->xPx, track->yPx) >= INPUT_DRAG_THRESHOLD_PX)
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
    g_gestureActive = false;
  if (finished.inGesture || finished.cameraDrag)
    return;

  if (finished.dragging)
  {
    if (finished.boxSelecting)
      FinishBoxSelect(finished.startXPx, finished.startYPx, _event.xPx, _event.yPx, _event.shift);
    else
      FinishOrderDrag(finished.startXPx, finished.startYPx, _event.xPx, _event.yPx);
    return;
  }
  if (ElapsedMs(finished.downQpc, _event.timestampQpc) <= INPUT_TAP_MAX_DURATION_MS)
    HandleTap(_event.xPx, _event.yPx, _event.shift, _event.timestampQpc);
}

void Scene::Update(uint32_t _viewWidthPx, uint32_t _viewHeightPx)
{
  m_viewWidthPx = std::max(1u, _viewWidthPx);
  m_viewHeightPx = std::max(1u, _viewHeightPx);
  UpdateCamera(); // picking needs matrices that match what was on screen when the pointer moved
  for (const PointerEvent& event : m_pendingEvents)
    ApplyPointerEvent(event);
  m_pendingEvents.clear();
  UpdateCamera(); // input may have moved it again
}

// ---------------------------------------------------------------------------------------------
// One fixed 60 Hz tick. Turn towards the target at a limited rate, drive forward along the facing,
// slow down in time to stop on the point. No pathfinding and no avoidance, by design.

void Scene::Step()
{
  const float maxSpeed = std::max(0.0f, MOTION_MAX_SPEED);
  const float acceleration = std::max(0.01f, MOTION_ACCELERATION);
  const float deceleration = std::max(0.01f, MOTION_DECELERATION);
  const float maxTurnRate = XMConvertToRadians(std::max(0.0f, MOTION_TURN_RATE_DEG_PER_SEC));
  const float turnAcceleration = XMConvertToRadians(std::max(1.0f, MOTION_TURN_ACCELERATION));
  const float arrivalRadius = std::max(0.01f, MOTION_ARRIVAL_RADIUS);
  const float stopBlend = HalfLifeBlend(SIM_DT, MOTION_STOP_DAMPING_HALF_LIFE);

  const float scale = std::max(0.01f, SHIP_SCALE);
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
        ship.order = ship.orderHasFacing ? OrderState::Aligning : OrderState::Idle;
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
        ship.order = OrderState::Idle;
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
      ship.speed = MoveTowards(ship.speed, desiredSpeed, (desiredSpeed > ship.speed ? acceleration : deceleration) * SIM_DT);
    else
    {
      ship.speed -= ship.speed * stopBlend; // half-life damping down to a standstill
      if (std::fabs(ship.speed) < 0.01f)
        ship.speed = 0.0f;
    }

    ship.posWorld.x += std::sin(ship.headingRad) * ship.speed * SIM_DT;
    ship.posWorld.z += std::cos(ship.headingRad) * ship.speed * SIM_DT;

    // What the thruster glow and trail are driven by.
    ship.accelSample = (ship.speed - speedBefore) / SIM_DT;

    // One sample per nozzle per tick, so trailLength means the same thing whatever the frame rate.
    const float cosH = std::cos(ship.headingRad);
    const float sinH = std::sin(ship.headingRad);
    ship.trailHead = (ship.trailHead + 1) % TRAIL_SAMPLES;
    for (size_t nozzle = 0; nozzle < ship.thrusterLocals.size(); ++nozzle)
    {
      const XMFLOAT3& local = ship.thrusterLocals[nozzle];
      ship.trail[nozzle * TRAIL_SAMPLES + static_cast<size_t>(ship.trailHead)] = XMFLOAT3(
        ship.posWorld.x + (local.x * cosH + local.z * sinH) * scale,
        ship.posWorld.y + SHIP_HOVER_HEIGHT + (ship.restY + local.y) * scale,
        ship.posWorld.z + (-local.x * sinH + local.z * cosH) * scale);
    }
    ship.trailCount = std::min(ship.trailCount + 1, TRAIL_SAMPLES);
  }
}

// ---------------------------------------------------------------------------------------------
// Feedback. Real time rather than sim time: none of it feeds back into Step, so the simulation
// stays fixed-step and deterministic while the look of it runs as fast as the swapchain does.

// A ring that overshoots is a spring, so use one. The peak overshoot fixes the damping ratio and
// the settle half-life then fixes the frequency, which makes both values mean exactly what
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
  const float maxTurnRate = XMConvertToRadians(std::max(1.0f, MOTION_TURN_RATE_DEG_PER_SEC));
  const float maxBank = XMConvertToRadians(BANK_MAX_ANGLE_DEG);
  const float accelReference = std::max(0.01f, MOTION_ACCELERATION);

  for (int i = 0; i < static_cast<int>(m_ships.size()); ++i)
  {
    Ship& ship = m_ships[static_cast<size_t>(i)];

    // Selection ring: a straight ramp on the tuned duration for the fade, and a spring for the
    // scale so it sails past and settles back.
    const float rampSec = std::max(0.001f, (ship.selected ? SEL_RING_FADE_IN_MS : SEL_RING_FADE_OUT_MS) * 0.001f);
    ship.ringFade = MoveTowards(ship.ringFade, ship.selected ? 1.0f : 0.0f, dt / rampSec);
    // The spring chases the selected state directly rather than the fade ramp, so the peak really
    // is ringScaleOvershoot and the two knobs stay independent: one shapes alpha, one shapes size.
    SpringTowards(ship.ringScale, ship.ringScaleVel, ship.selected ? 1.0f : 0.0f, SEL_RING_SCALE_OVERSHOOT,
                  SEL_OVERSHOOT_SETTLE_HALF_LIFE, dt);
    ship.ringScale = std::max(0.0f, ship.ringScale);

    // Hover.
    ship.hoverAmount += ((i == m_hoverShip ? 1.0f : 0.0f) - ship.hoverAmount) * HalfLifeBlend(dt, SEL_HOVER_RESPONSE_HALF_LIFE);

    // Bank into the turn, proportional to angular velocity. Rolling starboard down in a starboard
    // turn means a negative roll, since a positive rotation about +Z lifts the starboard side.
    const float bankTarget = -std::clamp(ship.turnRateRadPerSec / maxTurnRate, -1.0f, 1.0f) * maxBank;
    const bool goingIn = std::fabs(bankTarget) > std::fabs(ship.bankRad);
    const float bankHalfLife = goingIn ? BANK_RESPONSE_HALF_LIFE : BANK_RETURN_HALF_LIFE;
    ship.bankRad += (bankTarget - ship.bankRad) * HalfLifeBlend(dt, bankHalfLife);

    // Thrusters follow acceleration, not speed: they flare on the way up to cruise and go quiet
    // once the ship is coasting.
    const float drive = std::clamp(ship.accelSample / accelReference, 0.0f, 1.0f);
    const float idle = THRUSTER_IDLE_INTENSITY;
    const float thrusterTarget = idle + (THRUSTER_MAX_INTENSITY - idle) * drive;
    ship.thrusterIntensity += (thrusterTarget - ship.thrusterIntensity) * HalfLifeBlend(dt, THRUSTER_RESPONSE_HALF_LIFE);
  }

  // Order markers age out on their own.
  const float markerLifeSec = std::max(0.05f, MARKER_LIFETIME_MS * 0.001f);
  for (OrderMarker& marker : m_markers)
    marker.ageSec += dt;
  std::erase_if(m_markers, [markerLifeSec](const OrderMarker& _marker)
  {
    return _marker.ageSec >= markerLifeSec;
  });

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
      continue;
    ++movingCount;
    centreX += ship.posWorld.x;
    centreZ += ship.posWorld.z;
    leadX += std::sin(ship.headingRad) * ship.speed;
    leadZ += std::cos(ship.headingRad) * ship.speed;
  }
  if (movingCount > 0)
  {
    m_cameraGoal.x = centreX / static_cast<float>(movingCount);
    m_cameraGoal.z = centreZ / static_cast<float>(movingCount);
    leadX = leadX / static_cast<float>(movingCount) * CAMERA_LEAD_FACTOR;
    leadZ = leadZ / static_cast<float>(movingCount) * CAMERA_LEAD_FACTOR;
  }

  const float follow = HalfLifeBlend(dt, CAMERA_FOLLOW_HALF_LIFE);
  m_cameraTarget.x += (m_cameraGoal.x + leadX - m_cameraTarget.x) * follow;
  m_cameraTarget.z += (m_cameraGoal.z + leadZ - m_cameraTarget.z) * follow;

  // Shake: two detuned sines per axis so it never reads as a single wobble, decaying on its own
  // half-life, thrown across the view rather than along the world axes.
  m_shakeTimeSec += dt;
  m_shakeAmount -= m_shakeAmount * HalfLifeBlend(dt, CAMERA_SHAKE_DECAY_HALF_LIFE);
  if (m_shakeAmount < 0.001f)
  {
    m_shakeAmount = 0.0f;
    m_shakeOffset = XMFLOAT3(0.0f, 0.0f, 0.0f);
  }
  else
  {
    const float w = XM_2PI * std::max(0.1f, CAMERA_SHAKE_FREQUENCY_HZ);
    const float t = m_shakeTimeSec;
    const float swing = m_shakeAmount * CAMERA_SHAKE_AMPLITUDE;
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
  frame.lightDir = XMFLOAT3(LIGHT_DIR_X, LIGHT_DIR_Y, LIGHT_DIR_Z);
  frame.ambient = AMBIENT_LEVEL;
  frame.gridColour = Rgba{GRID_COLOUR_R, GRID_COLOUR_G, GRID_COLOUR_B, GRID_STRENGTH};
  frame.gridSpacing = GRID_SPACING;
  frame.gridLineWidthPx = GRID_LINE_WIDTH_PX;
  frame.gridFadeDistance = GRID_FADE_DISTANCE;
  frame.cameraPos = m_cameraEye;
  _gfx.BeginScene(frame);

  XMFLOAT4X4 world;
  const float groundSize = std::max(1.0f, GROUND_SIZE);
  XMStoreFloat4x4(&world, XMMatrixScaling(groundSize, 1.0f, groundSize) * XMMatrixTranslation(m_cameraTarget.x, 0.0f, m_cameraTarget.z));
  _gfx.DrawMesh(m_groundMesh, world, Rgba{GROUND_COLOUR_R, GROUND_COLOUR_G, GROUND_COLOUR_B, 1.0f}, 0.0f, true);

  const float scale = std::max(0.01f, SHIP_SCALE);
  const Rgba plain{SHIP_COLOUR_R, SHIP_COLOUR_G, SHIP_COLOUR_B, 1.0f};
  const Rgba picked{SELECTED_COLOUR_R, SELECTED_COLOUR_G, SELECTED_COLOUR_B, 1.0f};

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
                          XMMatrixRotationY(heading) * XMMatrixTranslation(x, y + SHIP_HOVER_HEIGHT, z);
    XMStoreFloat4x4(&world, hull);

    Rgba tint = ship.selected ? picked : plain;
    const float lift = ship.hoverAmount * SEL_HOVER_HIGHLIGHT_STRENGTH;
    tint = Rgba{tint.r + (1.0f - tint.r) * lift, tint.g + (1.0f - tint.g) * lift, tint.b + (1.0f - tint.b) * lift, 1.0f};
    _gfx.DrawMesh(ship.mesh, world, tint, SHIP_MATERIAL_MIX, false);
  }

  DrawFeedback(_gfx, _alpha);

  // Screen-space feedback for whatever the pointer is in the middle of. The proper rings and
  // markers arrive at stage 4.
  if (m_boxActive)
  {
    const Rgba edge{SEL_RING_COLOUR_R, SEL_RING_COLOUR_G, SEL_RING_COLOUR_B, SEL_RING_COLOUR_A};
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
    const Rgba line{MARKER_COLOUR_R, MARKER_COLOUR_G, MARKER_COLOUR_B, MARKER_COLOUR_A};
    _gfx.DrawScreenLine(m_orderX0Px, m_orderY0Px, m_orderX1Px, m_orderY1Px, 2.0f, line);
  }
}

// ---------------------------------------------------------------------------------------------
// The overlay pass: selection and hover rings on the ground, order markers, thruster glow and
// trail in the air. All of it is the same unit quad shaped by the decal shader.

void Scene::DrawFeedback(Gfx& _gfx, float _alpha)
{
  _gfx.BeginDecals(m_viewProj, m_cameraEye);

  const float scale = std::max(0.01f, SHIP_SCALE);
  constexpr float decalLiftY = 0.2f; // clear of the ground quad so the two cannot z-fight
  XMFLOAT4X4 world;

  // --- selection and hover rings --------------------------------------------------------------
  const Rgba ringColour{SEL_RING_COLOUR_R, SEL_RING_COLOUR_G, SEL_RING_COLOUR_B, SEL_RING_COLOUR_A};
  for (const Ship& ship : m_ships)
  {
    const float hullRadius = std::max(ship.halfExtents.x, ship.halfExtents.z) * scale;
    const float x = ship.prevPos.x + (ship.posWorld.x - ship.prevPos.x) * _alpha;
    const float z = ship.prevPos.z + (ship.posWorld.z - ship.prevPos.z) * _alpha;

    if (ship.ringFade > 0.002f && ship.ringScale > 0.002f)
    {
      const float radius = hullRadius * SEL_RING_RADIUS_SCALE * ship.ringScale;
      XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) * XMMatrixTranslation(x, decalLiftY, z));
      _gfx.DrawDecal(m_groundMesh, world, Rgba{ringColour.r, ringColour.g, ringColour.b, ringColour.a * ship.ringFade},
                     SEL_RING_THICKNESS, 0.0f);
    }

    // The hover ring sits just outside the selection ring so both are readable at once.
    if (ship.hoverAmount > 0.002f)
    {
      const float radius = hullRadius * SEL_RING_RADIUS_SCALE * 1.12f;
      XMStoreFloat4x4(&world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) * XMMatrixTranslation(x, decalLiftY, z));
      _gfx.DrawDecal(m_groundMesh, world, Rgba{ringColour.r, ringColour.g, ringColour.b, SEL_HOVER_RING_ALPHA * ship.hoverAmount},
                     SEL_RING_THICKNESS * 0.7f, 0.0f);
    }
  }

  // --- order markers --------------------------------------------------------------------------
  const Rgba markerColour{MARKER_COLOUR_R, MARKER_COLOUR_G, MARKER_COLOUR_B, MARKER_COLOUR_A};
  const float expandSec = std::max(0.001f, MARKER_EXPAND_MS * 0.001f);
  const float pulseSec = std::max(0.001f, MARKER_PULSE_PERIOD_MS * 0.001f);
  const float lifeSec = std::max(0.05f, MARKER_LIFETIME_MS * 0.001f);
  const float fadeSec = std::max(0.001f, MARKER_FADE_OUT_MS * 0.001f);
  const int pulseCount = std::max(0, static_cast<int>(MARKER_PULSE_COUNT + 0.5f));

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
    if (sincePulseStart > 0.0f && pulseIndex < static_cast<float>(pulseCount))
    {
      const float withinPulse = pulseIndex - std::floor(pulseIndex);
      beat = std::sin(withinPulse * XM_PI); // 0 at each pulse boundary, 1 in the middle
    }

    const float radius = std::max(0.1f, MARKER_RADIUS) * eased * (1.0f + beat * MARKER_PULSE_SCALE);
    const float alpha = markerColour.a * fade * (0.72f + beat * 0.28f);
    XMStoreFloat4x4(
      &world, XMMatrixScaling(radius * 2.0f, 1.0f, radius * 2.0f) * XMMatrixTranslation(marker.posWorld.x, decalLiftY, marker.posWorld.z));
    _gfx.DrawDecal(m_groundMesh, world, Rgba{markerColour.r, markerColour.g, markerColour.b, alpha}, MARKER_THICKNESS, 0.10f);

    // Each pulse also throws a ripple outwards, which is what makes the count readable.
    if (beat > 0.0f)
    {
      const float withinPulse = pulseIndex - std::floor(pulseIndex);
      const float rippleRadius = radius * (1.0f + withinPulse * 1.1f);
      XMStoreFloat4x4(
        &world, XMMatrixScaling(rippleRadius * 2.0f, 1.0f, rippleRadius * 2.0f) * XMMatrixTranslation(
                  marker.posWorld.x, decalLiftY, marker.posWorld.z));
      _gfx.DrawDecal(m_groundMesh, world, Rgba{markerColour.r, markerColour.g, markerColour.b, alpha * (1.0f - withinPulse) * 0.7f},
                     MARKER_THICKNESS * 0.6f, 0.0f);
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
  const float glowRadius = std::max(0.1f, THRUSTER_GLOW_RADIUS) * scale;
  const float trailLength = std::max(0.0f, THRUSTER_TRAIL_LENGTH);
  const float trailFade = std::max(0.01f, THRUSTER_TRAIL_FADE);

  for (const Ship& ship : m_ships)
  {
    if (ship.thrusterLocals.empty() || ship.thrusterIntensity <= 0.002f)
      continue;
    const Rgba glowColour{SELECTED_COLOUR_R, SELECTED_COLOUR_G, SELECTED_COLOUR_B, ship.thrusterIntensity};

    // Every exhaust gets its own glow and its own trail: a bomber flying with three nozzles lays
    // down three ribbons, and they fan apart through a turn because the outboard ones sweep wider.
    for (size_t nozzle = 0; nozzle < ship.thrusterLocals.size(); ++nozzle)
    {
      const XMFLOAT3* const samples = ship.trail.data() + nozzle * TRAIL_SAMPLES;

      // Newest sample first, walking back along the path until trailLength runs out. The trail
      // follows the path the nozzle actually took, so it curves through a turn.
      float travelled = 0.0f;
      XMFLOAT3 previous = samples[ship.trailHead];
      for (int step = 0; step < ship.trailCount; ++step)
      {
        const int index = ((ship.trailHead - step) % TRAIL_SAMPLES + TRAIL_SAMPLES) % TRAIL_SAMPLES;
        const XMFLOAT3 point = samples[index];
        if (step > 0)
        {
          travelled += Distance2D(previous.x, previous.z, point.x, point.z);
          if (travelled >= trailLength)
            break;
        }
        previous = point;

        const float along = (trailLength > 0.0f) ? travelled / trailLength : 1.0f;
        const float taper = std::pow(std::max(0.0f, 1.0f - along), trailFade);
        const float radius = glowRadius * (step == 0 ? 1.0f : taper * 0.8f);
        const float alpha = glowColour.a * (step == 0 ? 1.0f : taper * 0.55f);
        if (alpha <= 0.002f || radius <= 0.001f)
          continue;

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
        _gfx.DrawGlow(m_groundMesh, billboard, Rgba{glowColour.r, glowColour.g, glowColour.b, alpha}, THRUSTER_GLOW_FALLOFF);

        if (trailLength <= 0.0f)
          break;
      }
    }
  }
}
