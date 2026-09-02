#pragma once

#include "GalaxyLayout.h"
#include "HullSpec.h"
#include "ShipState.h"
#include "Universe.h"
#include "UniverseLayout.h"

#include <DirectXMath.h>

#include <cstdint>

namespace Game
{
// The shipped universe: what exists at tick zero, and where.
//
// This is content, and until slice 5b it lived in Outpost/ViewTuning.h beside the camera's framing
// numbers, because the composition root was the only thing that authored a universe. It is here now
// for ADR 0037's reason, applied to the thing that reason was written about: content that lives in
// one executable is in the wrong one the day there are two, and there are two -- the tool that
// writes a universe and the program that runs it (ADR 0058).
//
// It is emphatically NOT presentation. Every value below is a simulation noun -- a hull id, a
// faction, a position, a seed -- and the day this header names a texture or a colour it has drifted
// back into the layer it came from. What a Bomber LOOKS like is still the client's.
//
// The values are unchanged from what the game shipped with. A universe generated from this header
// is byte-identical to the one genesis used to build at boot, which is the property that made the
// move safe to make (Design/Archive/Universe-slice-5b.md 8).

// --- the starting system -------------------------------------------------------------------------

// Where the pinned first planet sits. It is a framing decision that became a layout one: the numbers
// were chosen so the world fills the lower third of the opening shot at the camera's default pitch,
// and the layout has to honour them exactly or the framing is gone. A depth in metres rather than
// radii, so the seeded radius cannot move the angle on the screen.
inline constexpr float HOME_PLANET_BEARING_DEG = -23.0f;
inline constexpr float HOME_PLANET_ORBIT_METRES = 3500.0f;

// The starting system's seed. Its own, kept apart from whatever seeds the client's *looks*, because
// a debug key that rerolled how a world looks must never move where it is: the system's station
// derives from its first planet's site, so a moved site is a moved universe.
inline constexpr std::uint64_t HOME_SYSTEM_SEED = 0x53797331ull; // "Sys1"

// Every other SystemDesc field keeps its default on purpose -- the defaults are the shipped numbers,
// and GameLogicTests proves the grid-ceiling bound against them, so an author who overrode the orbit
// band would be shipping a system the suite never saw (UniverseLayout.h).
inline constexpr SystemDesc STARTING_SYSTEM{.pinFirstPlanet = true,
                                            .firstPlanetBearingRad = HOME_PLANET_BEARING_DEG * (DirectX::XM_PI / 180.0f),
                                            .firstPlanetOrbitMetres = HOME_PLANET_ORBIT_METRES};

// --- the galaxy ------------------------------------------------------------------------------------

// Where every other system is, from one seed (LayOutGalaxy, ADR 0055).
inline constexpr std::uint64_t STARTING_GALAXY_SEED = 0x46726F6E74696572ull; // "Frontier"
inline constexpr GalaxyDesc STARTING_GALAXY{};

// The one authored place in the galaxy: the starting system, at the lattice origin, laid out from the
// seed and the description the game has always booted on. The galaxy grows around the scene the
// player already knows; nothing about home changes but the map it sits on (Design/Archive/Universe.md 3.2).
inline constexpr SystemPin HOME_PIN{.cellQ = 0, .cellR = 0, .systemSeed = HOME_SYSTEM_SEED, .desc = STARTING_SYSTEM};
inline constexpr SystemPin GALAXY_PINS[]{HOME_PIN};

// --- the player's fleet ----------------------------------------------------------------------------

inline constexpr HullId STARTING_FLEET[] = {HullId::Bomber, HullId::Corvette, HullId::Frigate};
inline constexpr float STARTING_FLEET_SPACING_METRES = 55.0f;

// --- the Vanguard's garrison -----------------------------------------------------------------------

// What every station of the galaxy launches when it is attacked, and how
// (Design/Archive/Stations.md 8.2).
inline constexpr HullId VANGUARD_PROTECTOR_HULL = HullId::Corvette;
inline constexpr std::uint32_t VANGUARD_PROTECTOR_COMPLEMENT = 3;
inline constexpr std::uint32_t VANGUARD_LAUNCH_EVERY_TICKS = 90;
inline constexpr std::uint32_t VANGUARD_TARGET_CAP = 4;

// Where a system's one Vanguard station stands: on the first planet's bearing, pulled in to this
// fraction of its orbit. One station per system rather than one per planet keeps the opening scene
// legible, and the pull puts home's station about 1,575 m out -- inside the 2 km interest radius,
// so the first frame holds it as a live record beside the fleet instead of only a mark 3.5 km away.
// The client's minimap mark derives from this same function (OutpostApp::MarkLocalStations), which
// is what keeps the picture and the row in agreement (ADR 0055's rule, applied to a station).
inline constexpr float VANGUARD_STATION_ORBIT_FRACTION = 0.45f;

[[nodiscard]] inline UniversePos VanguardStationSite(const SystemLayout& _system)
{
  // No shipped layout is planetless (minPlanetCount is 2), but a default-constructed layout should
  // place a station at the star rather than read past an end.
  if (_system.planets.empty())
    return _system.starPos;
  return Lerp(_system.starPos, _system.planets.front().posUniverse, VANGUARD_STATION_ORBIT_FRACTION);
}

// --- the hostile base ------------------------------------------------------------------------------

// The station sits 1,202 m out on the diagonal: inside the 2,000 m interest radius, so the base is
// subscribed from the first update and the overview shows red immediately, and well inside the
// minimap's half-range, so it has an edge to be seen against. The farthest patrol point is 1,602 m
// out, still inside both (Design/Archive/Hostiles.md 6).
//
// The ring clears the station's 251.77 m skin by 148 m, and its chords clear the station's centre by
// 386 m against the 263 m an Interceptor needs -- so the legs plan straight and the station never
// even scores as a threat. PatrolTests spells these same five numbers, and the two must agree.
inline constexpr float HOSTILE_BASE_EAST_METRES = 850.0f;
inline constexpr float HOSTILE_BASE_NORTH_METRES = 850.0f;
inline constexpr float HOSTILE_PATROL_RING_METRES = 400.0f;
inline constexpr float HOSTILE_PATROL_CRUISE_MPS = 10.0f; // 29 % of an Interceptor's maximum: a lap in about 4.2 minutes
inline constexpr std::uint32_t HOSTILE_PATROL_COUNT = 3;

// The hull both ends of every gate are spawned on, named once because it is spawned in two places:
// SpawnGates builds a whole galaxy into one universe, and BuildStartingGalaxy builds each end into
// the universe of the shard its system belongs to, which no single-universe function can do. The
// name is what keeps the two in step -- and it exists because they fell out of step: the hull moved
// from Structure to Stargate on one path and not the other, and the partitioned build went on
// spawning doors that looked like stations (Design/CrossShard-slice-1.md).
inline constexpr HullId GATE_HULL = HullId::Stargate;

// Everything above, spawned into _outUniverse: the player's fleet in slot 1, a Vanguard station in
// every system, a gate at each end of every link, and the hostile base with its patrol.
//
// A pure function of its arguments, called once, before the first tick. Nothing here draws -- the
// randomness is all upstream in _galaxy, which the caller laid out from a seed it named -- so two
// callers handed the same galaxy produce the same universe to the byte, which is what lets a tool
// write one and a program run it (ADR 0058).
//
// _outUniverse is expected to be empty. It is not cleared: a caller that wants a fresh universe
// makes one, and a function that silently emptied whatever it was handed would be a function that
// could delete a loaded save.
//
// _shard is what identities are minted under (ADR 0047). Zero for the shipped single-shard game; a
// dedicated server generating its own region passes its own, and nothing else here changes.
void BuildStartingUniverse(const GalaxyLayout& _galaxy, ShardId _shard, Universe& _outUniverse);

// The same galaxy, partitioned: one universe per shard, each holding the systems ShardOfSystem gives
// it (Design/CrossShard-slice-1.md).
//
// **All of them at once, and that is the whole reason this function exists rather than a loop over
// the one above.** A gate carries the EntityId of the gate it leads to (ADR 0056), and for a link
// whose ends fall on two shards that identity is minted in the OTHER universe. Building the shards
// one at a time could not name it, and predicting it would mean a second copy of the spawn order --
// which is the drift ADR 0058 exists to refuse. Holding every shard at once makes the far end's
// identity a thing to be read rather than computed, exactly as the single-shard build already reads
// it across its own two passes.
//
// _outShards is expected to be _desc.shardCount empty universes, and each is configured with its own
// index as its shard id. The player's fleet and the Vandal base go to whichever shard holds the home
// system, since both stand in it; every other shard boots with stations and gates and nothing else.
//
// A gate whose far end is on another shard is spawned and its row points at that shard's entity, so
// EntityShardOf(destination) != Shard() is true of it -- which is the test the handoff design uses
// for "this gate leads out" (Design/CrossShard.md 3). Nothing here acts on that; StepJumps is
// unchanged and a fleet ordered through such a gate strands nobody, which is the behaviour ADR 0056
// already gives a gate whose destination does not resolve.
void BuildStartingGalaxy(const GalaxyLayout& _galaxy, const GalaxyDesc& _desc, std::span<Universe> _outShards);
} // namespace Game
