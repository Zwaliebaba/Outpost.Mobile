#pragma once

#include "UniverseLayout.h"
#include "UniversePos.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Game
{
// Where the systems are. A galaxy laid out as a lattice of candidate cells, from a seed, as a pure
// function -- static content both halves of the game read rather than a thing one half tells the
// other about.
//
// It is UniverseLayout.h's argument one level up, and it is here for that header's reason (ADR
// 0037): the server half spawns a station and a gate at every system, the client half marks them,
// so it is content both binaries need, and content living in one executable is in the wrong one the
// day there are two. It also buys the layout a test suite, which the executable layer does not have.
//
// Nothing in the simulation knows a galaxy exists. There is no record, no collision and no tick
// cost at a cell nobody occupies; what the simulation eventually sees is the ordinary spawn input
// this produces -- positions and seeds (Design/Archive/Universe.md 3).

// Half the height of a hex row, as a fraction of the lattice pitch: sqrt(3)/2.
//
// Spelled as a literal rather than computed, because it decides where every system in the galaxy
// is and a value that came out of a std::sqrt would make that depend on a library rather than on
// this file. The digits are more than a float holds, so the compiler rounds them once and the same
// way everywhere.
inline constexpr float HEX_ROW_SPACING = 0.86602540378443864676f;

// sqrt(2), for the same reason and spelled the same way: it is the diagonal of the jitter square,
// and it is what MinimumStarSeparationMetres is proved with.
inline constexpr float ROOT_TWO = 1.41421356237309504880f;

// No pin authored this system: the lattice drew it.
inline constexpr std::uint32_t INVALID_PIN_INDEX = 0xFFFFFFFFu;

// One system: where its star is, which lattice cell put it there, and the seed everything inside it
// grows from.
//
// The cell is carried because it is the system's *name* -- the one identifier that survives a
// density change, where an index into the systems array does not (Design/Archive/Universe.md 3.1). A save
// file and a shard handoff both want that name, and recovering it from a position afterwards would
// be arithmetic against the jitter.
struct SystemSite
{
  UniversePos starPos;
  std::int32_t cellQ = 0;
  std::int32_t cellR = 0;
  std::uint64_t systemSeed = 0;

  // Which authored pin placed this system, or INVALID_PIN_INDEX if the lattice drew it. An index
  // into the pin span the layout was given, which is the caller's own content.
  std::uint32_t pin = INVALID_PIN_INDEX;
};

// One gate link, as a pair of indices into SystemSite's array.
//
// Indices rather than cells, because a link is only meaningful beside the layout that produced it
// and an index is what every consumer wants. Always systemA < systemB, and the list is in ascending
// order of the pair, so two galaxies with the same systems have the same link list whatever order
// the loop that found them ran in.
struct GateLink
{
  std::uint32_t systemA = 0;
  std::uint32_t systemB = 0;
};

// A system whose place and contents are authored rather than drawn.
//
// It carries a whole SystemDesc because that is what "authored" means: the pinned system is laid
// out from the description its author wrote, not from one the galaxy drew for it. The shipped pin
// is the starting system, and what it holds is what the game boots into today.
struct SystemPin
{
  std::int32_t cellQ = 0;
  std::int32_t cellR = 0;
  std::uint64_t systemSeed = 0;
  SystemDesc desc;
};

// The bounds a galaxy is drawn within.
//
// The defaults are the shipped numbers (Design/Archive/Universe.md 10), not placeholders, and that is
// load-bearing for the same reason SystemDesc's are: the separation and ceiling bounds below are
// provable in GameLogicTests only if the values they are proved against are the ones the game
// actually ships.
struct GalaxyDesc
{
  // How far out the lattice goes. Ring 0 is the origin cell alone; ring k adds 6k cells.
  std::uint32_t ringCount = 5;

  // The distance between two adjacent lattice cells: 16 sectors. Far past every radius that
  // exists -- the widest gather is 655 m and a ghost zone is 700 -- so no interaction, no interest
  // set and no path island can reach out of the system it belongs to.
  float latticePitchMetres = 131072.0f;

  // How far a star may wander from its lattice point, as a fraction of the pitch, PER AXIS. Bounded
  // by requirement rather than by taste, exactly as PLANET_BEARING_JITTER is: it is what makes
  // MinimumStarSeparationMetres a construction instead of a hope, and no rejection loop is needed
  // for the layout to be both deterministic and non-overlapping.
  //
  // Per axis is the word that costs something. The jitter is a square and not a disc -- two
  // independent draws, which is the cheap and deterministic way to do it -- so a star's worst
  // displacement is its diagonal, cellJitter * sqrt(2) * pitch, and the separation below is
  // proved against that rather than against the edge. Design/Archive/Universe.md 3.4 originally said 0.30
  // here and proved (1 - 2 * jitter); that arithmetic is a disc's, the suite caught it, and 0.20 is
  // what holds the separation the design asked for (Design/Archive/Universe-slice-1.md 7).
  float cellJitter = 0.20f;

  // What fraction of the candidate cells hold a system. A knob rather than a count, because a
  // count would have to be reached by rejecting draws and the whole layout would then depend on
  // how many times it rolled.
  float density = 0.55f;

  // The planet count band an unpinned system draws from, inclusive at both ends.
  std::uint32_t minPlanetCount = 2;
  std::uint32_t maxPlanetCount = 5;

  // How far from its star a gate stands, on the bearing toward the system it leads to.
  //
  // Outside the widest orbit (6 500 m), so a gate never stands in a planet's lap -- and bounded
  // above by the path grid's ceiling, which is what actually decides it. A system's static span is
  // twice this plus two grid margins, and PathIslands DECLINES to build past
  // PATH_GRID_MAX_CELLS_PER_AXIS *quietly*: the symptom is ships that stop routing, a long way from
  // here. Design/Archive/Universe.md 10 specified 8 000, which is 532 cells against a ceiling of 512
  // (Design/Archive/Universe-slice-3.md 7).
  //
  // It lives in this struct rather than in the composition root precisely so the bound is a
  // GameLogicTests assertion instead of a hope: the executable layer has no suite
  // (ADR 0037's argument, re-run).
  float gateRingMetres = 7000.0f;

  // The orbit and radius bands an unpinned system's planets are drawn within. Its planetCount and
  // its pin fields are not read: a galaxy's system draws its own count, and pinFirstPlanet is a
  // property of an authored system rather than of a drawn one.
  SystemDesc systemBounds;
};

struct GalaxyLayout
{
  // Where the lattice is anchored: the position of cell (0, 0) before any jitter.
  UniversePos origin;

  std::vector<SystemSite> systems;
  std::vector<GateLink> links;
};

// How many candidate cells a lattice of _ringCount rings has: 1 + 3k(k + 1).
[[nodiscard]] constexpr std::uint32_t GalaxyCellCount(std::uint32_t _ringCount) noexcept
{
  return 1u + 3u * _ringCount * (_ringCount + 1u);
}

// The closest two stars in a galaxy drawn to this description can ever be.
//
// (1 - 2 * sqrt(2) * cellJitter) * pitch, and it is a construction rather than a sample: the
// closest two lattice points are one pitch apart -- every other pair is at least sqrt(3) pitches --
// and each of the two stars may close that gap by at most the diagonal of its own jitter square.
//
// The sqrt(2) is the whole content of this function. Without it the bound is a disc's, it is not
// the one this layout actually holds, and it reads as true for exactly as long as nobody measures
// it.
//
// A function rather than a comment because the test that proves the bound and any caller that
// relies on it must not each state the arithmetic and drift.
[[nodiscard]] constexpr float MinimumStarSeparationMetres(const GalaxyDesc& _desc) noexcept
{
  return (1.0f - 2.0f * ROOT_TWO * _desc.cellJitter) * _desc.latticePitchMetres;
}

// The galaxy, from a seed.
//
// A pure function of its arguments: one Neuron::Pcg32(_seed) walked over the candidate cells in one
// fixed spiral order -- ring 0 outward, each ring entered at (k, 0) -- drawing per cell, in one
// fixed order: occupancy, jitter x, jitter z, system seed.
//
// **Every cell takes all four draws, occupied or not, pinned or not.** That is LayOutSystem's rule
// about pinFirstPlanet, and it buys more here than it does there: because a cell's draws belong to
// the cell rather than to the census, raising the density reveals cells without moving, reseeding
// or removing any system a lower density already had. A galaxy can therefore be retuned without
// being rerolled, which is what lets density be a balance knob rather than a new universe.
//
// A pin overwrites what its cell drew -- it is occupied whatever the density said, it sits exactly
// on its lattice point, and it carries its authored seed -- and it never skips a draw, because a
// flag that shifted the stream would make one seed mean two galaxies.
//
// The links are the relative neighborhood graph over the systems (LinkGates below).
//
// Boot-time only, like LayOutSystem: the result is then ordinary spawn input, so the replay
// contract never sees the randomness that produced it.
[[nodiscard]] GalaxyLayout LayOutGalaxy(std::uint64_t _seed, const UniversePos& _origin, const GalaxyDesc& _desc,
                                        std::span<const SystemPin> _pins);

// One system of a galaxy, laid out from its own seed.
//
// A pinned system is laid out from the description its pin authored, through LayOutSystem, so the
// starting system is exactly the system the game boots into today. An unpinned one draws its planet
// count from its own seed and then lays its planets out over the same generator, through the loop
// LayOutSystem uses -- one stream, so a system is a system rather than a count and some planets
// that happen to share a seed (Design/Archive/Universe.md 3.3).
//
// _pins must be the same span _site came out of. A site whose pin index is not in it is laid out as
// though it were drawn, which is the fail-closed direction: a system with planets in it beats a
// read past the end of somebody's table.
[[nodiscard]] SystemLayout LayOutGalaxySystem(const SystemSite& _site, const GalaxyDesc& _desc, std::span<const SystemPin> _pins);

// Which system a point is in: the index of the nearest star.
//
// Nearest rather than "inside a radius", and the difference is that this function always has an
// answer. A radius leaves a band between systems where nothing is in anything, and a caller in that
// band -- a camera panned into the void, a fleet mid-crossing -- would have to invent a rule of its
// own. Stars are at least MinimumStarSeparationMetres apart by construction, so inside a system the
// nearest star is the obvious one and no threshold has to be tuned to say so.
//
// Ties keep the lower index, which is stable rather than arbitrary: the comparison is strict, so a
// point exactly between two stars belongs to the earlier one on every machine and every run.
//
// Returns 0 on an empty galaxy, which cannot arise from LayOutGalaxy -- ring 0 is always laid --
// but is the fail-closed answer for a caller holding a default-constructed layout.
//
// Here rather than in the client because it is a question about the galaxy, not about a view: a
// server deciding which system a position belongs to asks exactly this, and the client's copy would
// have been the second opinion ADR 0037 exists to prevent. It is also the half of the client's
// scenery machinery that a suite can actually reach (Design/Archive/Universe-slice-4b.md 4).
[[nodiscard]] std::uint32_t SystemAt(std::span<const SystemSite> _systems, const UniversePos& _at) noexcept;

// Where the gate that leads to _to stands, inside _from's system.
//
// On the bearing from one star to the other, at gateRingMetres. Both ends derive from the same two
// positions, so the pair faces each other without either being told where the other put its gate --
// which is what lets a client and a server place gates independently and agree.
[[nodiscard]] UniversePos GateSite(const SystemSite& _from, const SystemSite& _to, const GalaxyDesc& _desc) noexcept;

// The heading a gate faces: away from its own star, along the road. A fleet arriving through it is
// set down on this bearing, which is what keeps arrivals clear of the structure and pointed into
// the system rather than back at the door.
[[nodiscard]] float GateHeadingRad(const SystemSite& _from, const SystemSite& _to) noexcept;

// The gate links over a set of systems: the relative neighborhood graph.
//
// A and B are linked unless some third system C is strictly closer to both -- max(d(A,C), d(B,C))
// < d(A,B). Chosen for a theorem rather than for a look: this graph contains the minimum spanning
// tree, so the galaxy is connected for every seed by construction, while staying sparse enough that
// bridges exist and a cut link can genuinely divide the map (ADR 0055).
//
// Exposed beside LayOutGalaxy, which calls it, so that a test can put systems where it wants them
// and ask what the rule says -- the graph is where the map's strategy lives, and it should be
// answerable without a lattice in the way.
void LinkGates(std::span<const SystemSite> _systems, std::vector<GateLink>& _outLinks);
} // namespace Game
