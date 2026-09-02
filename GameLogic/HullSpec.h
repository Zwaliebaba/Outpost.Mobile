#pragma once

#include "DeviceSpec.h"
#include "SimTuning.h"

#include <DirectXMath.h>

#include <cstdint>

namespace Game
{
// What a hull is, to the simulation. There was no size information in here at all before this
// table: the only extent anywhere was the view's mesh bounds, which is presentation data, sits in
// the client half, and is absent on a headless server. Collision needs a size, so the size has to
// live where the simulation can see it (Design/Archive/Collision.md 5).
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

// How many mounts one hull may carry. One more than the largest authored loadout below (the
// Battleship's five), and a capacity rather than a behavior: raising it changes the size of the
// mount table and no recorded outcome. The assert under the hull table is what makes exceeding it
// a compile error instead of a silent truncation.
inline constexpr std::uint32_t MAX_MOUNTS = 6;

// Where a hull carries a device, in hull frame: 0 is dead ahead and positive is to starboard,
// which is headingRad's own convention read from the bow rather than from north.
struct MountSpec
{
  float bearingRad = 0.0f;
  float arcHalfRad = 0.0f; // half-width of the cone this mount may bear through
  DeviceId device = DeviceId::LightGun;
};

// A hull's whole armament, as one value, so the rows of HULL_SPECS stay readable and the mounts
// stay in the same table as everything else about a hull. Two tables indexed by hull id is exactly
// what the derived functions below exist to avoid: adding a hull must not be able to leave a second
// table stale.
struct MountLoadout
{
  MountSpec mount[MAX_MOUNTS]{};
  std::uint32_t count = 0;
};

// A fixed gun points where the hull points, so its arc is narrow and the hull does the aiming; a
// turret sweeps almost all the way round and keeps a blind spot astern, which is what makes
// approaching from directly behind a real approach rather than a preference.
inline constexpr float MOUNT_ARC_BOW_RAD = 0.3491f;    // +/- 20 degrees
inline constexpr float MOUNT_ARC_CANNON_RAD = 0.1745f; // +/- 10 degrees
inline constexpr float MOUNT_ARC_TURRET_RAD = 2.6180f; // +/- 150 degrees

// The authored loadouts (Design/Combat.md 13). Named rather than written into the rows below
// because a five-mount Battleship inside a positional aggregate is a line nobody can read, and
// because these are the values an artist and a designer actually talk about.
inline constexpr MountLoadout LOADOUT_NONE{};
inline constexpr MountLoadout LOADOUT_INTERCEPTOR{{{0.0f, MOUNT_ARC_BOW_RAD, DeviceId::LightGun}}, 1};
inline constexpr MountLoadout LOADOUT_BOMBER{{{0.0f, MOUNT_ARC_CANNON_RAD, DeviceId::StrikeCannon}}, 1};
inline constexpr MountLoadout LOADOUT_CORVETTE{
  {{0.0f, MOUNT_ARC_TURRET_RAD, DeviceId::LightTurret}, {3.1416f, MOUNT_ARC_TURRET_RAD, DeviceId::LightTurret}}, 2};
inline constexpr MountLoadout LOADOUT_FRIGATE{
  {{0.0f, MOUNT_ARC_TURRET_RAD, DeviceId::MediumTurret}, {3.1416f, MOUNT_ARC_TURRET_RAD, DeviceId::MediumTurret}}, 2};
inline constexpr MountLoadout LOADOUT_BATTLESHIP{{{0.0f, MOUNT_ARC_TURRET_RAD, DeviceId::HeavyTurret},
                                                  {2.0944f, MOUNT_ARC_TURRET_RAD, DeviceId::HeavyTurret},
                                                  {-2.0944f, MOUNT_ARC_TURRET_RAD, DeviceId::HeavyTurret},
                                                  {1.5708f, MOUNT_ARC_TURRET_RAD, DeviceId::LightTurret},
                                                  {-1.5708f, MOUNT_ARC_TURRET_RAD, DeviceId::LightTurret}},
                                                 5};
inline constexpr MountLoadout LOADOUT_CARRIER{{{0.0f, MOUNT_ARC_TURRET_RAD, DeviceId::LightTurret},
                                               {1.5708f, MOUNT_ARC_TURRET_RAD, DeviceId::LightTurret},
                                               {3.1416f, MOUNT_ARC_TURRET_RAD, DeviceId::LightTurret},
                                               {-1.5708f, MOUNT_ARC_TURRET_RAD, DeviceId::LightTurret}},
                                              4};

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
  // expressible without a special case (Design/Archive/Collision.md 9).
  float avoidanceAuthority = 0.6f;

  // How many neighbours this hull tracks. Per-hull rather than global because a Carrier
  // legitimately watches more than an Interceptor, and the scratch buffer is sized to the table
  // maximum either way. It is in the replay contract: truncation changes the answer.
  std::uint32_t neighbourCap = 8;

  bool immovable = false; // structures: they take no correction, and traffic is projected out of them
  bool collidable = true; // a Stargate is flown through on purpose (Design/Archive/Collision.md 18.2)

  // Whether this hull answers an attack. A fleet's defense turns its combatants on whoever was
  // stated to have shot at it and leaves the rest flying the order they were given, so this is what
  // decides which half of a mixed fleet reacts (Design/Archive/Fleets.md 6.5, 7.2).
  //
  // Authored rather than derived, for avoidanceAuthority's reason: a hull that is armed but precious
  // -- a Q-ship, an armed hauler -- has to stay expressible, and a flag inferred from a weapon table
  // that does not exist yet would be a guess with nothing able to disagree with it. In the replay
  // contract like every other column here: it decides who turns.
  bool combatant = false;

  // What this hull can take before it stops existing, and ZERO MEANS INDESTRUCTIBLE -- the
  // degenerate reading this table already uses for a zero capsule half-length and an order speed cap
  // of zero. It is how Design/Archive/Stations.md 8.5's standing rule is implemented, and it is a
  // property of the hull rather than of the faction: "however it models damage, a Vanguard station's
  // is discarded" is one column here, with no station special case anywhere in the fire pass.
  std::uint32_t maxHullPoints = 0;

  // What this hull shoots with, and from where. Authored beside the size and the speed for
  // avoidanceAuthority's reason -- a hull that is armed but precious has to stay expressible -- and
  // in the replay contract like every other column: it decides who dies.
  MountLoadout loadout;

  [[nodiscard]] constexpr std::uint32_t MountCount() const noexcept
  {
    return loadout.count;
  }

  // What the neighbour query has to reach for this hull's guns to have anything to shoot at, and
  // what a mount's own envelope is measured against. Derived from the loadout rather than restated
  // beside it, for the reason every other derived function in this file gives.
  [[nodiscard]] constexpr float LongestMountRangeMetres() const noexcept
  {
    float longest = 0.0f;
    for (std::uint32_t at = 0; at < loadout.count; ++at)
    {
      const float range = DeviceSpecOf(loadout.mount[at].device).rangeMetres;
      if (range > longest)
        longest = range;
    }
    return longest;
  }

  // The shortest range among the mounts that can actually traverse, which is where a pursuit holds
  // so that every turret bears. Zero when the hull has none: a bow-fixed hull takes no stand-off at
  // all and is sent at its target, because its aiming is done by flying (Design/Combat.md 8).
  [[nodiscard]] constexpr float ShortestTurretRangeMetres() const noexcept
  {
    float shortest = 0.0f;
    for (std::uint32_t at = 0; at < loadout.count; ++at)
    {
      const DeviceSpec& device = DeviceSpecOf(loadout.mount[at].device);
      if (device.Fixed())
        continue;
      if (shortest == 0.0f || device.rangeMetres < shortest)
        shortest = device.rangeMetres;
    }
    return shortest;
  }

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
  // route, which is what phase 7 is for (Design/Archive/Collision.md 10).
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
  // Hull points are measured against Design/Combat.md 13's pacing targets rather than chosen: the
  // Battleship's 3,800 is what puts a mixed eight-fleet's kill at 81.6 s against a 90 s target, and
  // it is sharply bounded on both sides -- 2,400 gives 35 s and 4,000 lets the Battleship win
  // outright (Design/Combat-slice-5.md 2.1). Retune it and the fight changes character, not degree.
  //
  // r         L        speed  accel  decel  turn rad/s  turn accel  auth   K   immov  collide  fights  hull pts  loadout
  {1.115f, 2.390f, 34.0f, 30.0f, 38.0f, 3.4907f, 12.2173f, 0.6f, 8, false, true, true, 60, LOADOUT_INTERCEPTOR},   // Interceptor
  {8.705f, 0.000f, 30.0f, 24.0f, 30.0f, 2.0944f, 7.3304f, 1.2f, 8, false, true, true, 150, LOADOUT_BOMBER},        // Bomber
  {8.595f, 4.515f, 30.0f, 20.0f, 26.0f, 1.2217f, 4.1888f, 1.6f, 10, false, true, true, 240, LOADOUT_CORVETTE},     // Corvette
  {11.400f, 5.800f, 24.0f, 12.0f, 16.0f, 0.7854f, 2.7053f, 2.4f, 8, false, true, false, 200, LOADOUT_NONE},        // Miner
  {10.405f, 11.910f, 28.0f, 14.0f, 18.0f, 0.5236f, 1.8326f, 3.0f, 12, false, true, true, 520, LOADOUT_FRIGATE},    // Frigate
  {23.105f, 5.605f, 22.0f, 9.0f, 12.0f, 0.3840f, 1.3090f, 3.6f, 10, false, true, false, 420, LOADOUT_NONE},        // Hauler
  {21.640f, 18.735f, 24.0f, 8.0f, 11.0f, 0.2094f, 0.7330f, 5.0f, 14, false, true, true, 3800, LOADOUT_BATTLESHIP}, // Battleship
  {39.670f, 67.870f, 20.0f, 5.0f, 7.0f, 0.0873f, 0.3142f, 9.0f, 16, false, true, true, 5200, LOADOUT_CARRIER},     // Carrier
  {131.610f, 0.000f, 0.0f, 30.0f, 38.0f, 3.4907f, 12.2173f, 1.0f, 4, true, false, false, 0, LOADOUT_NONE},         // Stargate
  {251.180f, 0.590f, 0.0f, 30.0f, 38.0f, 3.4907f, 12.2173f, 1.0f, 4, true, true, false, 0, LOADOUT_NONE},          // Structure
};

// Two things a reader should not have to check by eye, and which a table edit can silently break.
//
// The Miner and the Hauler are deliberately unarmed and deliberately durable: they are what a
// fleet's combatants are defending, and Design/Archive/Fleets.md 7.2 has them carry on flying their
// orders through a fight rather than fleeing it. The mining design arms the Miner with tools on
// these same mounts (Design/Combat.md 12), which is why it is a loadout of none rather than a hull
// that cannot carry one.
[[nodiscard]] constexpr bool EveryLoadoutFitsItsMounts() noexcept
{
  for (const HullSpec& spec : HULL_SPECS)
  {
    if (spec.loadout.count > MAX_MOUNTS)
      return false;
  }
  return true;
}

// Nothing that cannot move may shoot, and nothing that cannot move may die. A station that shot
// back is a design nobody has written, and a station that could be destroyed drags a ledger, a
// garrison and a docked fleet's manifest behind it (Design/Combat.md 7.2, 14). Both are one column
// each today, and this is what keeps them that way.
[[nodiscard]] constexpr bool NoImmovableHullIsArmed() noexcept
{
  for (const HullSpec& spec : HULL_SPECS)
  {
    if (spec.immovable && spec.loadout.count != 0)
      return false;
  }
  return true;
}

[[nodiscard]] constexpr bool NoImmovableHullIsDestructible() noexcept
{
  for (const HullSpec& spec : HULL_SPECS)
  {
    if (spec.immovable && spec.maxHullPoints != 0)
      return false;
  }
  return true;
}

static_assert(EveryLoadoutFitsItsMounts(), "a hull carries more mounts than MAX_MOUNTS holds; raise the constant or cut the loadout");
static_assert(NoImmovableHullIsArmed(), "an immovable hull is armed, and no pass in this simulation knows how to aim a building");
static_assert(NoImmovableHullIsDestructible(), "an immovable hull can be destroyed, which no design has yet said what to do about");

// Where a pursuit holds so that its guns bear (Design/Combat.md 8).
//
// Read off the shortest range among the mounts that can traverse, so a hull with several turrets
// holds where ALL of them reach rather than where its longest one does. Zero for a hull whose
// mounts are all fixed, and zero for a hull with no mounts at all: both are sent at the target
// itself, the first because its aiming is done by flying and the second because shadowing at
// contact is what it did before this design existed.
[[nodiscard]] constexpr float EngageStandoffMetres(const HullSpec& _hull) noexcept
{
  return ENGAGE_STANDOFF_FRACTION * _hull.ShortestTurretRangeMetres();
}

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

// When two pieces of architecture belong to the same island, and therefore to one path grid: their
// surfaces are closer together than the widest corridor any hull could need. Derived rather than
// invented -- twice the largest mobile hull's bounding radius plus its clearance margin, because a
// ship's centre has to stay that far clear of each surface to pass between them. A wider gap is one
// the straight-line test flies through on its own; a narrower one is a wall A* has to find its way
// around (Design/Archive/RegionalPathfinding.md 3.2).
//
// In the replay contract, and near the top of it: this decides the partition, the partition decides
// which grid a route is planned in, and the grid decides the route. It is here rather than in
// SimTuning.h because it is read off the hull table, which is the same contract by another route.
[[nodiscard]] constexpr float IslandGapMetres() noexcept
{
  return 2.0f * (LargestMobileBoundingRadiusMetres() + PATH_CLEARANCE_MARGIN_METRES);
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

// How close a hull has to get before its order is done, and how far apart the slots of a formation
// containing it must sit. Both scale with the hull, and the constraint between them -- an arrival
// radius must fit well inside half a slot spacing, or ships arrive in each other's positions -- is
// what the margin in SimTuning.h exists to guarantee (Design/Archive/Collision.md 13).
[[nodiscard]] constexpr float ArrivalRadiusMetres(const HullSpec& _hull) noexcept
{
  const float scaled = ARRIVAL_RADIUS_FRACTION * _hull.BoundingRadiusMetres();
  return (scaled > ARRIVAL_RADIUS_MIN_METRES) ? scaled : ARRIVAL_RADIUS_MIN_METRES;
}

// Where a docking ship is sent: DOCK_CAPTURE_METRES clear of the two hulls' skins, measured centre
// to centre.
//
// Derived per pair rather than flat, for the reason DOCK_CAPTURE_METRES gives: the hull table spans
// a 3.4 m Interceptor and a 107 m Carrier against a 251 m station, and no single number is outside
// the big pair's no-go band while still being a dock rather than a postcode for the small one.
// Sixty metres clear of touching, for every pair in the table, by construction.
[[nodiscard]] constexpr float DockApproachRangeMetres(const HullSpec& _station, const HullSpec& _ship) noexcept
{
  return _station.BoundingRadiusMetres() + _ship.BoundingRadiusMetres() + DOCK_CAPTURE_METRES;
}

// How close a ship has to get to a station before it is inside.
//
// The approach range plus the tolerance the ship stops within, and that second term is not a fudge:
// it is what makes *arriving* and *docking* the same event. A ship is declared arrived when it is
// within ArrivalRadiusMetres of its destination, in any direction -- so a ship sent to a point
// exactly on the capture boundary can settle just outside it, go Idle, be re-aimed at the same
// point it is already at, and sit there for ever. Measured, before this term existed: a Corvette
// parked at 328.66 m against a 324.88 m boundary and never docked.
//
// Adding the arrival radius makes the implication hold by construction rather than by luck, for
// every hull -- including the Carrier, whose 37 m tolerance is more than half the capture slack and
// which no fixed margin inside the boundary could have absorbed (Design/Archive/Stations-slice-3.md 2.2).
//
// A ship does not *stop* here. It stops existing, which is why the constant behind it is in the
// contract (Design/Archive/Stations.md 7.3).
[[nodiscard]] constexpr float DockRangeMetres(const HullSpec& _station, const HullSpec& _ship) noexcept
{
  return DockApproachRangeMetres(_station, _ship) + ArrivalRadiusMetres(_ship);
}

// How close a fleet member has to be to a gate before the fleet may cross: GATE_CAPTURE_METRES
// clear of the two hulls' skins, measured centre to centre.
//
// DockApproachRangeMetres' shape and its argument, at fleet grain (Design/Archive/Universe-slice-2.md 7).
[[nodiscard]] constexpr float GateRangeMetres(const HullSpec& _gate, const HullSpec& _ship) noexcept
{
  return _gate.BoundingRadiusMetres() + _ship.BoundingRadiusMetres() + GATE_CAPTURE_METRES;
}

[[nodiscard]] constexpr float SlotSpacingMetres(float _largestBoundingRadiusMetres) noexcept
{
  return 2.0f * _largestBoundingRadiusMetres * FORMATION_SPACING_MARGIN;
}

// The look-ahead one hull needs against one particular neighbour.
//
// The per-hull horizon above is about the ship's own agility -- time to reverse course plus time to
// stop -- and that is the right quantity against something its own size. It is badly wrong against
// something far larger. An Interceptor's 1.8 s look-ahead carries it 61 m, while clearing a
// Carrier's 107 m hull needs 119 m of lateral room, so every reachable heading scores as equally
// doomed, the danger term cancels out of the comparison entirely, and the fighter flies straight
// into the capital. Measured before this term existed: 2.6 m of interpenetration and no starboard
// break at all.
//
// So take the longer of the two, still under the cap that keeps region ghost zones sane.
[[nodiscard]] constexpr float ThreatHorizonSec(const HullSpec& _hull, float _neighbourRadiusMetres, float _speedMetresPerSec) noexcept
{
  const float clearance = _hull.BoundingRadiusMetres() + _neighbourRadiusMetres + AVOID_MARGIN_METRES;
  const float toClear = AVOID_CLEARANCE_LEAD * clearance / ((_speedMetresPerSec > 1.0f) ? _speedMetresPerSec : 1.0f);
  const float wanted = (toClear > _hull.AvoidHorizonSec()) ? toClear : _hull.AvoidHorizonSec();
  return (wanted < AVOID_HORIZON_MAX_SEC) ? wanted : AVOID_HORIZON_MAX_SEC;
}

// The largest and fastest things a query could have to find.
//
// The hull table's own maxima are the ceiling, and that ceiling is what sizes a region's ghost zone.
// What is actually in a universe at a given moment is usually far less -- a skirmish between fighters
// pays the Carrier's 655 m because a Carrier exists in the table, not because one is there
// (Design/Archive/MmoScalabilityReview.md U2). A gather asks for the second; region sizing asks for the
// first; they are the same arithmetic over different numbers, which is why there is one function.
struct NeighbourhoodExtent
{
  float largestMobileRadiusMetres = 0.0f;
  float largestStaticRadiusMetres = 0.0f;
  float fastestSpeedMetresPerSec = 0.0f;
};

[[nodiscard]] constexpr NeighbourhoodExtent WholeHullTableExtent() noexcept
{
  return NeighbourhoodExtent{.largestMobileRadiusMetres = LargestMobileBoundingRadiusMetres(),
                             .largestStaticRadiusMetres = LargestStaticBoundingRadiusMetres(),
                             .fastestSpeedMetresPerSec = FastestHullSpeedMetresPerSec()};
}

// How wide a circle this hull has to ask the index for, against a stated neighbourhood.
//
// The first term is avoidance: what can close on it inside its own look-ahead. The second is
// separation, which the first does not cover -- a Structure's centre sits 251 m from its own skin,
// so a hull that only queried its avoidance radius could be inside one and never see it. The larger
// of the two is the honest answer (Collision.md 10).
//
// The separation term takes the widest thing present of *either* kind. On the hull table as it
// stands that is the Structure's 251 m and the value is unchanged, but a universe with no architecture
// in it must still separate its ships from each other, and reading only the static maximum there
// would return a radius narrower than a pair of Carriers need.
[[nodiscard]] constexpr float QueryRadiusMetres(const HullSpec& _hull, const NeighbourhoodExtent& _extent) noexcept
{
  const float ownRadius = _hull.BoundingRadiusMetres();
  // The same horizon the avoidance pass will use against the largest thing that can move, so a hull
  // is never asked to steer around something the query never showed it.
  const float horizon = ThreatHorizonSec(_hull, _extent.largestMobileRadiusMetres, _hull.maxSpeedMetresPerSec);
  const float closing = (_hull.maxSpeedMetresPerSec + _extent.fastestSpeedMetresPerSec) * horizon;
  // AVOID_MARGIN_METRES is part of this sum because it is part of the clearance ThreatAlong tests
  // against: a neighbour whose closest approach lands inside own + other + margin still scores, so a
  // query that stopped at own + other would be exactly that margin short of finding it.
  //
  // It was missing until slice 11 and nothing showed, because the largest mobile radius in the table
  // (a Carrier's 107 m) buys an Interceptor far more slack than eight metres. Narrowing this to what
  // is present is what took the slack away and made the omission visible -- an 8 m band at the edge
  // of every query in which a neighbour existed and was not returned.
  const float avoid = closing + ownRadius + _extent.largestMobileRadiusMetres + AVOID_MARGIN_METRES;
  const float widest = (_extent.largestStaticRadiusMetres > _extent.largestMobileRadiusMetres) ? _extent.largestStaticRadiusMetres
                                                                                               : _extent.largestMobileRadiusMetres;
  const float separate = ownRadius + widest + SEPARATION_QUERY_MARGIN_METRES;
  const float reach = (avoid > separate) ? avoid : separate;

  // The third term is gunnery, and it is here for the reason the second one is: the first does not
  // cover it. A mount ranges to a target's *skin*, so finding one takes the range plus the widest
  // skin that can be wearing it, and a target the query never returned is a target no mount can see.
  //
  // Against the whole hull table it never binds -- a Battleship queries 620 m and its heaviest gun
  // reaches 528 -- which is exactly why it had to be written down rather than assumed. The maxima
  // narrow to what is *present* (Design/Archive/MmoScalabilityReview.md U2), and a skirmish of
  // Interceptors alone narrows this query to 137.1 m while a LightGun reaches 163.5 m: 26 m in which
  // a fighter's guns out-range its senses, in the only kind of fight the game opens with.
  const float gunnery = (_hull.LongestMountRangeMetres() > 0.0f)
                          ? _hull.LongestMountRangeMetres() + _extent.largestMobileRadiusMetres + GUNNERY_QUERY_MARGIN_METRES
                          : 0.0f;
  return (gunnery > reach) ? gunnery : reach;
}

// The table's worst case: the Carrier's 655 m, and the floor on how wide a region's ghost zone has
// to be. A universe-layout number, not a per-tick one.
[[nodiscard]] constexpr float QueryRadiusMetres(const HullSpec& _hull) noexcept
{
  return QueryRadiusMetres(_hull, WholeHullTableExtent());
}

// How far apart these two hulls can be and still matter to each other. This is what lets a candidate
// be rejected before its record is built, so it has to be at least as far as any consumer of the
// neighbour list can reach, and it is derived from the furthest-reaching one rather than guessed.
//
// That consumer is Movement.cpp's ThreatAlong, which ignores a neighbour whose closest approach
// stays outside `own + other + AVOID_MARGIN_METRES` or falls beyond the horizon. The pair can close
// at most their combined top speeds over that horizon, so this sum is exactly the distance from
// which one can still come to matter to the other. Separation and blocking both need mere capsule
// overlap, which is well inside it.
//
// The immovable case is *not* special. A Structure has no speed, but a fighter closing on one at
// 34 m/s has to see it from most of 500 m away or it never begins the turn -- writing this as a
// separation problem, which is what its 251 m radius makes it look like, cuts that to 115 m and the
// fighter flies into the station. Measured, not reasoned: the shortcut was written and the numbers
// said otherwise.
//
// It is never wider than the query that found the pair, because the query's numbers are maxima over
// what is present and these are a member of that set. GameLogicTests checks that over every ordered
// pair and every subset of the hull table rather than leaving it as an argument.
[[nodiscard]] constexpr float PairRelevanceRadiusMetres(const HullSpec& _hull, const HullSpec& _other) noexcept
{
  const float ownRadius = _hull.BoundingRadiusMetres();
  const float otherRadius = _other.BoundingRadiusMetres();
  const float horizon = ThreatHorizonSec(_hull, otherRadius, _hull.maxSpeedMetresPerSec);
  const float closing = (_hull.maxSpeedMetresPerSec + _other.maxSpeedMetresPerSec) * horizon;
  const float reach = closing + ownRadius + otherRadius + AVOID_MARGIN_METRES;
  const float separate = ownRadius + otherRadius + SEPARATION_QUERY_MARGIN_METRES;
  const float widest = (reach > separate) ? reach : separate;

  // Gunnery, against this particular neighbour, and it is not optional tidiness. This filter is
  // what decides whether a candidate becomes a Neighbour record at all, so a query widened for the
  // guns without widening this one would find a target and then throw it away before any mount
  // could see it. The gather clamps this to the query that found the pair, so widening it can only
  // ever keep a candidate the query already returned.
  const float gunnery =
    (_hull.LongestMountRangeMetres() > 0.0f) ? _hull.LongestMountRangeMetres() + otherRadius + GUNNERY_QUERY_MARGIN_METRES : 0.0f;
  return (gunnery > widest) ? gunnery : widest;
}
} // namespace Game
