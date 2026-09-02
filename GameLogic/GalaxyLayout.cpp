#include "pch.h"
#include "GalaxyLayout.h"

#include "Pcg32.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Game
{
namespace
{
// The six axial steps around a hex ring, in the order the walk takes them. Entering a ring at
// (k, 0) and applying these k times each closes it in exactly 6k cells.
//
// The order is part of what a seed means: it decides which cell each of the ring's draws lands on,
// so reordering this table reseeds every system outside ring 0 (Design/Archive/Universe-slice-1.md 4.1).
struct AxialStep
{
  std::int32_t q;
  std::int32_t r;
};

constexpr AxialStep RING_STEPS[6] = {{-1, 1}, {-1, 0}, {0, -1}, {1, -1}, {1, 0}, {0, 1}};

// Which pin, if any, authored this cell. A linear scan of a table with single digits of rows, which
// is free at boot and is the shape Universe::StationAt already uses at this scale.
[[nodiscard]] std::uint32_t PinAt(std::int32_t _q, std::int32_t _r, std::span<const SystemPin> _pins) noexcept
{
  for (std::size_t at = 0; at < _pins.size(); ++at)
  {
    if (_pins[at].cellQ == _q && _pins[at].cellR == _r)
      return static_cast<std::uint32_t>(at);
  }
  return INVALID_PIN_INDEX;
}

// The square of the distance between two stars, accumulated in double.
//
// The offsets are exact enough in float -- a galaxy spans about 1.8e6 m, where a float step is
// 0.125 m -- but their squares are not: 3.4e12 lands where a float step is 262 144, so two pairs
// differing by under about 8 cm at that range would compare equal.
//
// What that could and could not do is worth stating, because it decides how much this matters.
// The blocking test below is STRICT, so a spurious tie leaves a link alone: imprecision can only
// ever keep an edge, never cut one, and the connectivity the graph is chosen for is therefore safe
// in float too. What it could change is the shape of the map, by a link nobody meant. This runs
// once at boot over a table in the dozens, so the double costs nothing and removes the question --
// and, measured rather than assumed, the suite does NOT catch the float version
// (Design/Archive/Universe-slice-1.md 7): it is insurance against a map that is slightly wrong, not a fix
// for one that is broken.
//
// Symmetric exactly, which is what the connectivity theorem needs: OffsetX negates exactly under a
// swap, and squaring and adding are symmetric, so d2(a, b) and d2(b, a) are the same bits.
[[nodiscard]] double StarDistanceSquared(const SystemSite& _a, const SystemSite& _b) noexcept
{
  const double dx = static_cast<double>(OffsetX(_a.starPos, _b.starPos));
  const double dz = static_cast<double>(OffsetZ(_a.starPos, _b.starPos));
  return dx * dx + dz * dz;
}

// The same accumulation for a loose point, and for the same reason: a galaxy spans about 1.8e6 m,
// where the SQUARE of a distance lands in a part of the float range whose step is 262 144, so two
// stars differing by centimetres at that range would compare equal. Game::DistanceSquared is the
// float one and is right everywhere it is used -- inside a sector, where the numbers are small --
// and wrong here for the one reason this file already had to write down once.
[[nodiscard]] double PointDistanceSquared(const UniversePos& _a, const UniversePos& _b) noexcept
{
  const double dx = static_cast<double>(OffsetX(_a, _b));
  const double dz = static_cast<double>(OffsetZ(_a, _b));
  return dx * dx + dz * dz;
}
} // namespace

GalaxyLayout LayOutGalaxy(std::uint64_t _seed, const UniversePos& _origin, const GalaxyDesc& _desc, std::span<const SystemPin> _pins)
{
  GalaxyLayout layout;
  layout.origin = _origin;
  layout.systems.reserve(GalaxyCellCount(_desc.ringCount));

  // One generator for the whole galaxy, walked in one fixed order -- LayOutSystem's sentence about
  // its own generator, and its reason: one per ring, seeded from the ring index, would look
  // equivalent and would make what a cell draws depend on an arithmetic nobody wrote down.
  Neuron::Pcg32 rng(_seed);

  const float jitterMetres = _desc.cellJitter * _desc.latticePitchMetres;

  std::int32_t q = 0;
  std::int32_t r = 0;
  for (std::uint32_t ring = 0; ring <= _desc.ringCount; ++ring)
  {
    // Ring 0 is the origin cell alone; every other ring is entered at (k, 0) and closed by the six
    // steps. The cell count is the loop bound rather than a termination test, so a ring is walked
    // the same way whatever its contents.
    const std::uint32_t cellsInRing = (ring == 0u) ? 1u : 6u * ring;
    q = static_cast<std::int32_t>(ring);
    r = 0;

    std::uint32_t stepped = 0;
    for (std::uint32_t at = 0; at < cellsInRing; ++at)
    {
      // Every cell draws all four, occupied or not, pinned or not. Reordering these lines, or
      // guarding one of them behind the density or a pin, changes what every seed means -- which is
      // the one thing this function promises not to do, and the property that lets density be
      // retuned without rerolling the galaxy.
      const float occupyUnit = rng.Float01();
      const float jitterX = rng.Signed(jitterMetres);
      const float jitterZ = rng.Signed(jitterMetres);
      const std::uint64_t systemSeed = (static_cast<std::uint64_t>(rng.Next()) << 32u) | static_cast<std::uint64_t>(rng.Next());

      const std::uint32_t pin = PinAt(q, r, _pins);
      const bool pinned = pin != INVALID_PIN_INDEX;

      if (pinned || occupyUnit < _desc.density)
      {
        SystemSite site;
        site.cellQ = q;
        site.cellR = r;
        site.systemSeed = pinned ? _pins[pin].systemSeed : systemSeed;
        site.pin = pin;

        // The lattice point, then the jitter -- and a pinned system takes none, so it sits exactly
        // where its author put it and the framed opening shot is not moved by a die roll.
        const float latticeX = _desc.latticePitchMetres * (static_cast<float>(q) + 0.5f * static_cast<float>(r));
        const float latticeZ = _desc.latticePitchMetres * HEX_ROW_SPACING * static_cast<float>(r);

        // Through Translate rather than by writing localX, because localX is an offset inside a
        // sector and a galaxy anchored anywhere but a sector's corner would otherwise leave the
        // invariant behind -- LayOutSystem's rule, at the scale that actually crosses sectors.
        site.starPos = _origin;
        Translate(site.starPos, pinned ? latticeX : latticeX + jitterX, pinned ? latticeZ : latticeZ + jitterZ);

        layout.systems.push_back(site);
      }

      // Stepped after the cell is visited, so the ring's first cell is the one it was entered at.
      // Ring 0 never steps: it has one cell and six steps would walk it into ring 1.
      if (ring != 0u)
      {
        q += RING_STEPS[stepped / ring].q;
        r += RING_STEPS[stepped / ring].r;
        ++stepped;
      }
    }
  }

  LinkGates(layout.systems, layout.links);
  return layout;
}

SystemLayout LayOutGalaxySystem(const SystemSite& _site, const GalaxyDesc& _desc, std::span<const SystemPin> _pins)
{
  // An authored system is laid out from the description its author wrote, and nothing here draws
  // for it: that is what pinning means, and it is what makes the starting system the system the
  // game already boots into.
  if (_site.pin != INVALID_PIN_INDEX && _site.pin < _pins.size())
    return LayOutSystem(_site.systemSeed, _site.starPos, _pins[_site.pin].desc);

  SystemLayout layout;
  layout.starPos = _site.starPos;

  // One generator, and the count comes off the front of it. The planets then continue the same
  // stream, so a system is one draw sequence rather than a count and some planets that happen to
  // share a seed -- and LayOutPlanets is the shipped loop rather than a copy of it.
  Neuron::Pcg32 rng(_site.systemSeed);

  SystemDesc desc = _desc.systemBounds;
  desc.pinFirstPlanet = false; // a drawn system has no authored planet to hold still
  const std::uint32_t span = (_desc.maxPlanetCount >= _desc.minPlanetCount) ? _desc.maxPlanetCount - _desc.minPlanetCount + 1u : 1u;
  desc.planetCount = _desc.minPlanetCount + rng.Below(span);

  LayOutPlanets(rng, _site.starPos, desc, layout);
  return layout;
}

std::uint32_t SystemAt(std::span<const SystemSite> _systems, const UniversePos& _at) noexcept
{
  if (_systems.empty())
    return 0;

  // A linear scan over a table in the dozens. The lattice is regular enough that the cell could be
  // solved for arithmetically, and that solution would be wrong at exactly the point it mattered:
  // a drawn cell's jitter and a missing neighbour both move which star is nearest, so the cell a
  // point sits in and the system it belongs to are not the same question.
  std::uint32_t nearest = 0;
  double nearestSq = PointDistanceSquared(_at, _systems[0].starPos);
  for (std::size_t at = 1; at < _systems.size(); ++at)
  {
    // Strictly closer, so a tie keeps the lower index rather than taking the later one.
    const double distanceSq = PointDistanceSquared(_at, _systems[at].starPos);
    if (distanceSq < nearestSq)
    {
      nearestSq = distanceSq;
      nearest = static_cast<std::uint32_t>(at);
    }
  }
  return nearest;
}

UniversePos GateSite(const SystemSite& _from, const SystemSite& _to, const GalaxyDesc& _desc) noexcept
{
  const float bearing = GateHeadingRad(_from, _to);
  UniversePos site = _from.starPos;
  // Through Translate, because localX is an offset inside a sector and a system near a boundary
  // would otherwise leave the invariant behind -- LayOutPlanets' rule, unchanged.
  Translate(site, std::sin(bearing) * _desc.gateRingMetres, std::cos(bearing) * _desc.gateRingMetres);
  return site;
}

float GateHeadingRad(const SystemSite& _from, const SystemSite& _to) noexcept
{
  const float dx = OffsetX(_from.starPos, _to.starPos);
  const float dz = OffsetZ(_from.starPos, _to.starPos);
  // Two stars are at least MinimumStarSeparationMetres apart by construction, so this is never the
  // atan2(0, 0) whose answer is a platform's opinion rather than a value.
  return std::atan2(dx, dz);
}

void LinkGates(std::span<const SystemSite> _systems, std::vector<GateLink>& _outLinks)
{
  _outLinks.clear();
  const std::size_t count = _systems.size();

  // The relative neighborhood rule, stated directly: keep the pair unless something is closer to
  // both ends of it. O(n^3) over a table in the dozens, once, at boot -- the shape the rule is
  // written in matters more here than the exponent, because this is the map (ADR 0055).
  for (std::size_t a = 0; a < count; ++a)
  {
    for (std::size_t b = a + 1; b < count; ++b)
    {
      const double apart = StarDistanceSquared(_systems[a], _systems[b]);

      bool blocked = false;
      for (std::size_t c = 0; c < count && !blocked; ++c)
      {
        if (c == a || c == b)
          continue;

        // Strictly closer to BOTH, and strictly: a tie leaves the link, which is what keeps the
        // rule from cutting an edge on a coincidence and is what the connectivity theorem assumes.
        blocked = std::max(StarDistanceSquared(_systems[a], _systems[c]), StarDistanceSquared(_systems[b], _systems[c])) < apart;
      }

      if (!blocked)
        _outLinks.push_back(GateLink{static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)});
    }
  }
}

namespace
{
// How many lattice cells stand in column q of a hex of radius R, and how many stand in every column
// left of it. A hex column holds 2R+1-|q| cells, which is the only arithmetic the partition needs.
[[nodiscard]] std::uint32_t CellsInColumn(std::int32_t _q, std::uint32_t _ringCount) noexcept
{
  const std::int32_t ring = static_cast<std::int32_t>(_ringCount);
  const std::int32_t away = (_q < 0) ? -_q : _q;
  return (away > ring) ? 0u : static_cast<std::uint32_t>(2 * ring + 1 - away);
}

[[nodiscard]] std::uint32_t CellsLeftOfColumn(std::int32_t _q, std::uint32_t _ringCount) noexcept
{
  const std::int32_t ring = static_cast<std::int32_t>(_ringCount);
  std::uint32_t before = 0;
  for (std::int32_t column = -ring; column < _q && column <= ring; ++column)
    before += CellsInColumn(column, _ringCount);
  return before;
}
} // namespace

std::uint32_t MaxShardCount(const GalaxyDesc& _desc) noexcept
{
  // One shard per column is the most a band split can give, since a band is a whole number of
  // columns and a shard with no column has no systems.
  return 2u * _desc.ringCount + 1u;
}

std::uint32_t OccupiedShardCount(std::span<const SystemSite> _systems, const GalaxyDesc& _desc) noexcept
{
  if (_desc.shardCount <= 1u)
    return _systems.empty() ? 0u : 1u;

  // Distinct answers, counted by looking back rather than by marking a bitset over shards. The
  // bitset was one line shorter and was a std::vector<bool>, which allocates -- and this function is
  // noexcept, so clang-tidy refused it (bugprone-exception-escape, run 245). A shard count is
  // deployment configuration and a bound this function does not get to assume; the number of
  // SYSTEMS is bounded by the galaxy and is what the work is actually proportional to, so the
  // quadratic pass over 54 of them is both smaller than the allocation would have been and cannot
  // throw.
  std::uint32_t occupied = 0;
  for (std::size_t at = 0; at < _systems.size(); ++at)
  {
    const ShardId mine = ShardOfSystem(_systems[at], _desc);
    bool first = true;
    for (std::size_t before = 0; before < at && first; ++before)
      first = ShardOfSystem(_systems[before], _desc) != mine;
    occupied += first ? 1u : 0u;
  }
  return occupied;
}

ShardId ShardOfSystem(const SystemSite& _site, const GalaxyDesc& _desc) noexcept
{
  if (_desc.shardCount <= 1u)
    return 0;

  // Where this column starts in the lattice's own left-to-right cell order, scaled by the shard
  // count and divided by the total: the standard equal-mass cut, done in integers. Every cell of a
  // column lands in the same shard, so a shard is a contiguous band of columns.
  //
  // The multiplication is widened because cells * shardCount overflows a u32 for no galaxy anyone
  // will build, and the widening costs nothing to be sure of.
  const std::uint64_t total = GalaxyCellCount(_desc.ringCount);
  const std::uint64_t before = CellsLeftOfColumn(_site.cellQ, _desc.ringCount);
  const std::uint64_t shard = (before * _desc.shardCount) / total;

  // Clamped rather than trusted. A cell outside the lattice -- which LayOutGalaxy cannot produce and
  // a hand-built test can -- would otherwise index past the last shard.
  const std::uint64_t last = _desc.shardCount - 1u;
  return static_cast<ShardId>((shard < last) ? shard : last);
}
} // namespace Game
