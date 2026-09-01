#pragma once

#include <cstdint>

namespace Game
{
// What a mounted thing does when it cycles.
//
// A device is the tool; a mount (HullSpec.h) is the arm that carries one. They are two tables
// rather than one because a hull carries several mounts of the same device, and because the day a
// mining tool cycles on the same arm as a gun, only this table gains a row
// (Design/Combat.md 3.1, 12).
//
// Authored simulation content, and never read from a mesh at run time. The shipped hulls carry
// turret submeshes and the NMO format defines a Gun marker with a muzzle direction, but what the
// *simulation* needs of a hardpoint arrives here as numbers, for the reason the capsule table
// gives about size: content is the renderer's, and a headless server has none of it (ADR 0002).
enum class DeviceKind : std::uint8_t
{
  Gun,
  MiningTool
};

// Declared whole so the byte never renumbers, and MiningTool is reserved exactly as
// FleetOrderKind::Mine is: no row below uses it, and the fire pass skips any mount carrying one
// rather than guessing what extraction would mean. The mining design owns everything behind it.
enum class DeviceId : std::uint32_t
{
  LightGun,
  StrikeCannon,
  LightTurret,
  MediumTurret,
  HeavyTurret
};

inline constexpr std::uint32_t DEVICE_COUNT = 5;

struct DeviceSpec
{
  DeviceKind kind = DeviceKind::Gun;

  // To the target's *skin*, not its centre. With a 72:1 size ratio in the hull table, the centre
  // reading would put a Carrier out of a fighter's range while the fighter sat against its flank.
  float rangeMetres = 0.0f;

  // Ticks, never seconds off a clock (ADR 0045). Rate of fire is a property of the game, so it is
  // counted in the game's own unit.
  std::uint32_t cooldownTicks = 0;

  // Whole hull points. The damage path holds no float at all, which is what makes it bit-exact
  // under every summation order on every machine (ShipState::hullPoints).
  std::uint32_t damage = 0;

  // Zero is a FIXED mount, and that is the whole of the special case: a barrel bolted to a hull has
  // no slew to settle, so its firing gate is its arc alone and the hull's own turn is its traverse.
  // It is what makes a fighter fly attack runs without a line written for attack runs
  // (Design/Combat.md 4, 8).
  float traverseRadPerSec = 0.0f;

  [[nodiscard]] constexpr bool Fixed() const noexcept
  {
    return traverseRadPerSec <= 0.0f;
  }
};

// The starting table (Design/Combat.md 13). Every number here serves the five pacing targets that
// section states -- a fighter dead in about ten seconds under one peer and about one and a half
// under a focused fleet -- and is written to be measured against them in slice 5 rather than to be
// admired here. All of it is in the replay contract: change a row and a recorded battle ends on a
// different tick.
//
// The two fixed rows are the interesting ones. A fighter's gun and a bomber's cannon do not slew,
// so the hull is the turret: they must point to shoot, which is what turns a chase into a pass.
//
// The HeavyTurret's damage is 40 and not the 70 this table shipped with, and it was measured rather
// than argued (Design/Combat-slice-5.md 2.1). At 70 a Battleship put out 65.8 damage a second, which
// killed an Interceptor in 0.9 s and a Corvette in 3.6 s -- so a fleet sent against one lost its own
// damage faster than it could spend it, and the matchup had no middle: below about 3,000 hull points
// the fleet won in 45 s, above it the Battleship wiped all eight and stood. Forty makes the capital
// something a fleet grinds down over a minute and a quarter while losing ships doing it, which is
// what "a capital is an event, not a target" was supposed to mean.
inline constexpr DeviceSpec DEVICE_SPECS[DEVICE_COUNT] = {
  // kind                  range   cooldown  damage  traverse rad/s
  {DeviceKind::Gun, 160.0f, 30, 3, 0.0f},      // LightGun     -- 0.5 s, the fighter's bow gun
  {DeviceKind::Gun, 240.0f, 360, 90, 0.0f},    // StrikeCannon -- 6 s, the Bomber's whole argument
  {DeviceKind::Gun, 180.0f, 45, 5, 3.1416f},   // LightTurret  -- 180 deg/s: it tracks anything
  {DeviceKind::Gun, 280.0f, 90, 18, 1.0472f},  // MediumTurret -- 60 deg/s, the line weapon
  {DeviceKind::Gun, 420.0f, 240, 40, 0.3142f}, // HeavyTurret  -- 18 deg/s; 40 is measured, see below
};

// An unknown device resolves to a working one rather than to memory past the end of the table,
// which is HullSpecOf's rule and its reason: content is a diagnostic, never a crash (AGENTS.md 5).
[[nodiscard]] constexpr const DeviceSpec& DeviceSpecOf(std::uint32_t _deviceId) noexcept
{
  return DEVICE_SPECS[(_deviceId < DEVICE_COUNT) ? _deviceId : 0];
}

[[nodiscard]] constexpr const DeviceSpec& DeviceSpecOf(DeviceId _deviceId) noexcept
{
  return DeviceSpecOf(static_cast<std::uint32_t>(_deviceId));
}
} // namespace Game
