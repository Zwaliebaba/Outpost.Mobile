#include "pch.h"
#include "GalaxyLayout.h"

#include "Pcg32.h"

#include <algorithm>
#include <cstddef>

namespace Game
{
namespace
{
// The six axial steps around a hex ring, in the order the walk takes them. Entering a ring at
// (k, 0) and applying these k times each closes it in exactly 6k cells.
//
// The order is part of what a seed means: it decides which cell each of the ring's draws lands on,
// so reordering this table reseeds every system outside ring 0 (Design/Universe-slice-1.md 4.1).
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
// (Design/Universe-slice-1.md 7): it is insurance against a map that is slightly wrong, not a fix
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
} // namespace Game
