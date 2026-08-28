#pragma once

#include "SimTuning.h"

#include <DirectXMath.h>

#include <cstdint>

namespace Game
{
// What a hull is, to the simulation. There was no size information in here at all before this
// table: the only extent anywhere was the view's mesh bounds, which is presentation data, sits in
// the client half, and is absent on a headless server. Collision needs a size, so the size has to
// live where the simulation can see it (Design/Collision.md 5).
//
// It is worth its own phase without collision anywhere near it. SimTuning.h gave an Interceptor
// and a Carrier identical acceleration and identical turn rate -- a 30:1 hull handled as though it
// were 1:1 -- and this is the table that stops it.
enum class HullId : std::uint32_t
{
  Interceptor,
  Bomber,
  Corvette,
  Miner,
  Frigate,
  Hauler,
  Battleship,
  Carrier,
  Stargate,
  Structure
};

inline constexpr std::uint32_t HULL_COUNT = 10;

struct HullSpec
{
  // Collision shape: a segment of half-length L along the hull's forward axis, inflated by radius
  // r. L == 0 is a circle, so a circle is the degenerate capsule rather than a second shape.
  float capsuleRadiusMetres = 1.115f;
  float capsuleHalfLengthMetres = 2.390f;

  float maxSpeedMetresPerSec = 34.0f;
  float accelerationMetresPerSec2 = 30.0f;
  float decelerationMetresPerSec2 = 38.0f;

  // maxTurnRateRadPerSec, not turnRateRadPerSec: ShipState already has a turnRateRadPerSec and it
  // means live angular velocity. Two fields with the same name and different meanings in one
  // simulation is a bug waiting for a tired afternoon.
  float maxTurnRateRadPerSec = 3.4907f;        // 200 deg/s
  float turnAccelerationRadPerSec2 = 12.2173f; // 700 deg/s^2

  // The share of a contact correction a hull takes is the *other* hull's authority over the sum, so
  // a high authority is a hull that holds its line. Authored rather than derived from radius, so
  // that a hull which is small but immovable in intent -- a picket, a deployed turret -- is
  // expressible without a special case (Design/Collision.md 9).
  float avoidanceAuthority = 0.6f;

  // How many neighbours this hull tracks. Per-hull rather than global because a Carrier
  // legitimately watches more than an Interceptor, and the scratch buffer is sized to the table
  // maximum either way. It is in the replay contract: truncation changes the answer.
  std::uint32_t neighbourCap = 8;

  bool immovable = false; // structures: they take no correction, and traffic is projected out of them
  bool collidable = true; // a Stargate is flown through on purpose (Design/Collision.md 18.2)

  [[nodiscard]] constexpr float BoundingRadiusMetres() const noexcept
  {
    return capsuleHalfLengthMetres + capsuleRadiusMetres;
  }

  // Time to reverse course plus time to stop -- the hull's own agility, rather than a magic
  // constant needing separate tuning per hull. Guarded against a zero turn rate because a
  // default-constructed spec must survive this call.
  [[nodiscard]] constexpr float BrakeHorizonSec() const noexcept
  {
    const float turn = (maxTurnRateRadPerSec > 0.0f) ? DirectX::XM_PI / maxTurnRateRadPerSec : AVOID_HORIZON_MAX_SEC;
    const float brake = (decelerationMetresPerSec2 > 0.0f) ? maxSpeedMetresPerSec / decelerationMetresPerSec2 : 0.0f;
    return turn + brake;
  }

  // What avoidance scores against, and what sizes the neighbour query. Capped, because the
  // uncapped form gives a Carrier a 2.7 km query circle sixty times a second -- which makes the
  // neighbour cap meaningless and forces ghost zones tens of kilometres wide. The honest
  // consequence is that a capital cannot fully avoid on local steering alone; it needs a planned
  // route, which is what phase 7 is for (Design/Collision.md 10).
  [[nodiscard]] constexpr float AvoidHorizonSec() const noexcept
  {
    const float derived = BrakeHorizonSec();
    return (derived < AVOID_HORIZON_MAX_SEC) ? derived : AVOID_HORIZON_MAX_SEC;
  }
};

// Radii and half-lengths start from the shipped meshes -- r is half the beam, L is half the length
// beyond it -- but they are authored values, not derived ones. Deriving them at load time would put
// the collision hull in the renderer's reach, would drag antennae and wingtips into it, and would
// make the simulation depend on content it has to be able to run without. They are free to be tuned
// inward of the visual hull; the mesh fit is where they start, not what they are.
//
// Speeds keep the largest hull slower than the smallest rather than spreading in both directions.
// The tunnelling margin (Collision.md 11) is set by the fastest hull against the smallest capsule
// radius, so slowing the capitals costs nothing there while giving the fleet the speed spread the
// design asks for.
inline constexpr HullSpec HULL_SPECS[HULL_COUNT] = {
  // r         L        speed  accel  decel  turn rad/s  turn accel  auth   K   immov  collide
  {1.115f, 2.390f, 34.0f, 30.0f, 38.0f, 3.4907f, 12.2173f, 0.6f, 8, false, true},   // Interceptor
  {8.705f, 0.000f, 30.0f, 24.0f, 30.0f, 2.0944f, 7.3304f, 1.2f, 8, false, true},    // Bomber
  {8.595f, 4.515f, 30.0f, 20.0f, 26.0f, 1.2217f, 4.1888f, 1.6f, 10, false, true},   // Corvette
  {11.400f, 5.800f, 24.0f, 12.0f, 16.0f, 0.7854f, 2.7053f, 2.4f, 8, false, true},   // Miner
  {10.405f, 11.910f, 28.0f, 14.0f, 18.0f, 0.5236f, 1.8326f, 3.0f, 12, false, true}, // Frigate
  {23.105f, 5.605f, 22.0f, 9.0f, 12.0f, 0.3840f, 1.3090f, 3.6f, 10, false, true},   // Hauler
  {21.640f, 18.735f, 24.0f, 8.0f, 11.0f, 0.2094f, 0.7330f, 5.0f, 14, false, true},  // Battleship
  {39.670f, 67.870f, 20.0f, 5.0f, 7.0f, 0.0873f, 0.3142f, 9.0f, 16, false, true},   // Carrier
  {131.610f, 0.000f, 0.0f, 30.0f, 38.0f, 3.4907f, 12.2173f, 1.0f, 4, true, false},  // Stargate
  {251.180f, 0.590f, 0.0f, 30.0f, 38.0f, 3.4907f, 12.2173f, 1.0f, 4, true, true},   // Structure
};

// An unknown hull id resolves to a working ship rather than to an inert one or to memory past the
// end of the table. Content is a diagnostic, never a crash (AGENTS.md 5).
[[nodiscard]] constexpr const HullSpec& HullSpecOf(std::uint32_t _hullId) noexcept
{
  return HULL_SPECS[(_hullId < HULL_COUNT) ? _hullId : 0];
}

[[nodiscard]] constexpr const HullSpec& HullSpecOf(HullId _hullId) noexcept
{
  return HullSpecOf(static_cast<std::uint32_t>(_hullId));
}

// Derived from the table rather than restated beside it, so adding a hull cannot leave one of them
// stale. These size the neighbour query (Collision.md 10) and the tunnelling gate (11).
[[nodiscard]] constexpr float SmallestCapsuleRadiusMetres() noexcept
{
  float smallest = HULL_SPECS[0].capsuleRadiusMetres;
  for (const HullSpec& spec : HULL_SPECS)
  {
    if (spec.collidable && spec.capsuleRadiusMetres < smallest)
      smallest = spec.capsuleRadiusMetres;
  }
  return smallest;
}

[[nodiscard]] constexpr float FastestHullSpeedMetresPerSec() noexcept
{
  float fastest = 0.0f;
  for (const HullSpec& spec : HULL_SPECS)
  {
    if (spec.maxSpeedMetresPerSec > fastest)
      fastest = spec.maxSpeedMetresPerSec;
  }
  return fastest;
}

[[nodiscard]] constexpr float LargestMobileBoundingRadiusMetres() noexcept
{
  float largest = 0.0f;
  for (const HullSpec& spec : HULL_SPECS)
  {
    if (!spec.immovable && spec.collidable && spec.BoundingRadiusMetres() > largest)
      largest = spec.BoundingRadiusMetres();
  }
  return largest;
}

[[nodiscard]] constexpr float LargestStaticBoundingRadiusMetres() noexcept
{
  float largest = 0.0f;
  for (const HullSpec& spec : HULL_SPECS)
  {
    if (spec.immovable && spec.collidable && spec.BoundingRadiusMetres() > largest)
      largest = spec.BoundingRadiusMetres();
  }
  return largest;
}

[[nodiscard]] constexpr std::uint32_t LargestNeighbourCap() noexcept
{
  std::uint32_t largest = 0;
  for (const HullSpec& spec : HULL_SPECS)
  {
    if (spec.neighbourCap > largest)
      largest = spec.neighbourCap;
  }
  return largest;
}

// How wide a circle this hull has to ask the index for.
//
// The first term is avoidance: what can close on it inside its own look-ahead. The second is
// separation against architecture, which the first does not cover -- a Structure's centre sits
// 251 m from its own skin, so a hull that only queried its avoidance radius could be inside one
// and never see it. The larger of the two is the honest answer and it is the Carrier's 647 m,
// which is what keeps a region's ghost zone a sane width (Collision.md 10).
[[nodiscard]] constexpr float QueryRadiusMetres(const HullSpec& _hull) noexcept
{
  const float ownRadius = _hull.BoundingRadiusMetres();
  const float closing = (_hull.maxSpeedMetresPerSec + FastestHullSpeedMetresPerSec()) * _hull.AvoidHorizonSec();
  const float avoid = closing + ownRadius + LargestMobileBoundingRadiusMetres();
  const float separate = ownRadius + LargestStaticBoundingRadiusMetres() + SEPARATION_QUERY_MARGIN_METRES;
  return (avoid > separate) ? avoid : separate;
}
} // namespace Game
