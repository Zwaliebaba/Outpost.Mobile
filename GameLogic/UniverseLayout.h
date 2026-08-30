#pragma once

#include "WorldPos.h"

#include <cstdint>
#include <vector>

namespace Game
{
// Where the places are. A solar system laid out as a star anchor and a ring of planet sites, from a
// seed, as a pure function -- static content both halves of the game read rather than a thing one
// half tells the other about.
//
// It is here and not in the executable for ADR 0008's reason, re-run rather than cited because the
// obvious precedent points the other way. BodyCatalogue -- what a planet looks like -- is client
// content and stays client content: a server has no business knowing what a world wears. A planet's
// *position* is not that. The server half spawns a station at every site and the client half marks
// them, so it is content both binaries need, and content living in one executable is in the wrong
// one the day there are two. It also buys the layout a test suite, which the executable layer does
// not have (Design/Archive/Stations.md 5.2).
//
// Nothing in the simulation knows a planet exists. There is no ship, no record and no collision at
// a site; ships fly through the place it marks, and its visual is kilometres below the plane
// regardless (ADR 0016). The station is the only thing at a planet the simulation knows about, and
// this header does not spawn it -- it says where it would go.

// One planet, and therefore one station.
struct PlanetSite
{
  WorldPos posWorld;          // where the planet -- and its station -- sit on the plane
  float radiusMetres = 0.0f;  // the body's visual radius: drawn by the client, ignored by the server
  float bearingRad = 0.0f;    // from the star, for anything that wants to face or frame it
  std::uint64_t bodySeed = 0; // what the client's BodyCatalogue generates the look from
};

struct SystemLayout
{
  WorldPos starPos;
  std::vector<PlanetSite> planets;
};

// The bounds a system is drawn within.
//
// The defaults are the shipped numbers (Design/Archive/Stations.md 5.3), not placeholders, and that is
// load-bearing: GameLogicTests cannot see ViewTuning.h, so the grid-ceiling bound below is provable
// here only if the values it is proved against are the ones the game actually ships. The
// composition root sets the pinned fields and nothing else.
struct SystemDesc
{
  std::uint32_t planetCount = 3;
  float minOrbitMetres = 2500.0f;
  float maxOrbitMetres = 6500.0f;
  float minRadiusMetres = 400.0f; // the client aligns these with its BODY_PLANET_* band
  float maxRadiusMetres = 1200.0f;

  // True: planet 0 takes the two fields below verbatim, in place of its drawn orbit and bearing.
  // It exists so that the carefully framed opening shot survives as content instead of being
  // superseded by a die roll.
  bool pinFirstPlanet = false;
  float firstPlanetBearingRad = 0.0f;
  float firstPlanetOrbitMetres = 0.0f;
};

// How far a bearing may wander from its slot, as a fraction of half a slot. Below 1 by requirement
// rather than by taste: at 1 the top of one slot and the bottom of the next are the same bearing
// and two planets can share it, which is the overlap the slotting exists to prevent. At a half,
// adjacent bearings are at least half a slot apart by construction, which is what makes the minimum
// separation in PlanetsKeepTheirDistance a proof rather than a sample.
inline constexpr float PLANET_BEARING_JITTER = 0.5f;

// The starting system, from a seed.
//
// A pure function of its arguments: one Neuron::Pcg32(_seed) drawing per planet in one fixed order
// -- orbit, radius, bearing jitter, body seed -- so a seed means one system forever, which is the
// rule BodyCatalogue::RandomBody states for a body. All four are drawn for every planet, a pinned
// one included: the pin overwrites what was drawn and never skips a draw, because a flag that
// shifted the stream would make one seed mean two systems.
//
// Bearings are not drawn free. Planet i sits at its slot, i * 2pi / planetCount, plus a jitter
// bounded to PLANET_BEARING_JITTER of half a slot either side, so the planets are spread by
// construction and no rejection loop is needed for the layout to be both deterministic and
// non-overlapping.
//
// Boot-time only. The result is then ordinary spawn input -- positions, not a generator -- so the
// replay contract never sees the randomness that produced it (Design/Archive/Stations.md 10).
[[nodiscard]] SystemLayout LayOutSystem(std::uint64_t _seed, const WorldPos& _starPos, const SystemDesc& _desc);
} // namespace Game
