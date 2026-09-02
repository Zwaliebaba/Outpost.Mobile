#include "pch.h"
#include "HullParts.h"

using namespace Neuron;

namespace Outpost
{
namespace
{
// The shortest signed way from _from to _to, in (-pi, pi]. Both are hull-frame bearings and a turret
// crossing the stern must not take the long way round to get there.
[[nodiscard]] float ShortestTurn(float _from, float _to) noexcept
{
  constexpr float TWO_PI = 6.28318530718f;
  float delta = _to - _from;
  while (delta > 3.14159265359f)
    delta -= TWO_PI;
  while (delta < -3.14159265359f)
    delta += TWO_PI;
  return delta;
}
} // namespace

std::vector<MuzzleView> ResolveMuzzles(const MeshData& _mesh, std::uint32_t _mountCount)
{
  // The suffixes a mount's muzzles may carry, in the order a turret alternates through them. The
  // NUL one first, because a single-muzzle mount is named `Gun0` with no letter at all.
  static constexpr char SUFFIXES[MAX_MOUNT_MUZZLES] = {'\0', 'A', 'B', 'C'};

  // Built by hand rather than formatted, so the name a hash is taken of is spelled out here and
  // nothing depends on a format string agreeing with the rule the art check enforces.
  static_assert(Game::MAX_MOUNTS < 10, "a mount index past nine needs two digits, and Gun<N> assumes one");

  std::vector<MuzzleView> muzzles;
  for (std::uint32_t mount = 0; mount < _mountCount && mount < Game::MAX_MOUNTS; ++mount)
  {
    for (const char suffix : SUFFIXES)
    {
      const char name[6] = {'G', 'u', 'n', static_cast<char>('0' + mount), suffix, '\0'};
      const std::uint32_t hash = NameHash(name);
      for (const MeshMarker& marker : _mesh.markers)
      {
        if (marker.kind == MarkerKind::Gun && marker.nameHash == hash)
          muzzles.push_back(MuzzleView{.mount = mount, .local = marker.position});
      }
    }
  }
  return muzzles;
}

std::vector<MountView> ResolveMounts(Game::HullId _hull, const MeshData& _mesh)
{
  std::vector<MountView> mounts;
  const Game::HullSpec& spec = Game::HullSpecOf(_hull);

  for (const MountArt& row : HULL_MOUNT_ART)
  {
    if (row.hull != _hull || row.mount >= spec.MountCount())
      continue;

    const Game::MountSpec& mount = spec.loadout.mount[row.mount];
    const Game::DeviceSpec& device = Game::DeviceSpecOf(mount.device);
    if (device.Fixed())
      continue; // a bow gun does not turn, whatever its art is called: the hull aims it

    MountView view;
    view.mount = row.mount;
    view.restRad = mount.bearingRad;
    view.aimRad = view.restRad;
    view.wantRad = view.restRad;
    view.arcHalfRad = mount.arcHalfRad;
    view.traverseRadPerSec = device.traverseRadPerSec;

    // The FIRST named part decides the pivot, and the rest turn about it. A turret and its barrels
    // pivot together on the turret's centre; giving each part its own centre would have the barrels
    // orbiting themselves.
    bool pivoted = false;
    for (const char* name : row.parts)
    {
      if (name == nullptr)
        break;
      const std::uint32_t hash = NameHash(name);
      const MeshRange range = _mesh.RangeOf(hash);
      if (range.vertexCount == 0)
        continue; // the art does not carry this part: a diagnostic, not a crash (Design/Archive/Combat.md 3.1)
      if (!pivoted)
      {
        view.pivot = _mesh.PivotOf(hash);
        pivoted = true;
      }
      if (view.partCount < MAX_MOUNT_PARTS)
        view.parts[view.partCount++] = range;
    }

    if (view.partCount > 0)
      mounts.push_back(view);
  }
  return mounts;
}

const MountView* FindMount(std::span<const MountView> _mounts, std::uint32_t _mount) noexcept
{
  for (const MountView& mount : _mounts)
  {
    if (mount.mount == _mount)
      return &mount;
  }
  return nullptr;
}

MountView* FindMount(std::span<MountView> _mounts, std::uint32_t _mount) noexcept
{
  for (MountView& mount : _mounts)
  {
    if (mount.mount == _mount)
      return &mount;
  }
  return nullptr;
}

float MountBearingToward(const MountView& _mount, float _localX, float _localZ) noexcept
{
  // Bearing in the hull frame, +Z forward and +X to starboard -- atan2(x, z), which is the same
  // convention SimTuning's mount bearings are authored in.
  const float wanted = std::atan2(_localX, _localZ);
  const float off = ShortestTurn(_mount.restRad, wanted);
  return _mount.restRad + std::clamp(off, -_mount.arcHalfRad, _mount.arcHalfRad);
}

void SlewMount(MountView& _mount, float _dtSec) noexcept
{
  if (_dtSec > 0.0f)
    _mount.holdSec = std::max(0.0f, _mount.holdSec - _dtSec);

  // Nothing held: back to where it was authored. The drift home is the same rate as the chase, so a
  // turret that swung to starboard takes as long to stow as it took to bear.
  const float goal = (_mount.holdSec > 0.0f) ? _mount.wantRad : _mount.restRad;

  const float step = _mount.traverseRadPerSec * std::max(0.0f, _dtSec);
  const float delta = ShortestTurn(_mount.aimRad, goal);
  if (step <= 0.0f)
    return; // a fixed mount, or a frame with no time in it
  _mount.aimRad += std::clamp(delta, -step, step);

  // Wrapped once, not looped: aimRad only ever moves by step, so one wrap is all it can need, and a
  // while loop here would be a place for a NaN to hang the frame.
  constexpr float TWO_PI = 6.28318530718f;
  if (_mount.aimRad > 3.14159265359f)
    _mount.aimRad -= TWO_PI;
  else if (_mount.aimRad < -3.14159265359f)
    _mount.aimRad += TWO_PI;
}
} // namespace Outpost
