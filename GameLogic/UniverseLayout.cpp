#include "pch.h"
#include "UniverseLayout.h"

#include "Pcg32.h"

#include <DirectXMath.h>

#include <cmath>

namespace Game
{
namespace
{
[[nodiscard]] float Between(float _low, float _high, float _unit) noexcept
{
  return _low + _unit * (_high - _low);
}
} // namespace

SystemLayout LayOutSystem(std::uint64_t _seed, const UniversePos& _starPos, const SystemDesc& _desc)
{
  SystemLayout layout;
  layout.starPos = _starPos;

  // One generator for the whole system, drawn in one fixed order. Two of them -- one per planet,
  // seeded from the index -- would look equivalent and would not be: the stream a planet gets would
  // then depend on an arithmetic nobody wrote down, and a system is a system rather than three
  // independent bodies that happen to share a star.
  Neuron::Pcg32 rng(_seed);
  LayOutPlanets(rng, _starPos, _desc, layout);
  return layout;
}

void LayOutPlanets(Neuron::Pcg32& _rng, const UniversePos& _starPos, const SystemDesc& _desc, SystemLayout& _outLayout)
{
  if (_desc.planetCount == 0)
    return;

  _outLayout.planets.reserve(_outLayout.planets.size() + _desc.planetCount);

  const float slotRad = DirectX::XM_2PI / static_cast<float>(_desc.planetCount);
  const float jitterRad = slotRad * 0.5f * PLANET_BEARING_JITTER;

  for (std::uint32_t at = 0; at < _desc.planetCount; ++at)
  {
    // Every draw happens for every planet, pinned or not. Reordering these four lines, or guarding
    // one of them behind pinFirstPlanet, changes what every seed means -- which is the one thing
    // this function promises not to do.
    const float orbitUnit = _rng.Float01();
    const float radiusUnit = _rng.Float01();
    const float bearingJitterRad = _rng.Signed(jitterRad);
    const std::uint64_t bodySeed = (static_cast<std::uint64_t>(_rng.Next()) << 32u) | static_cast<std::uint64_t>(_rng.Next());

    PlanetSite site;
    site.radiusMetres = Between(_desc.minRadiusMetres, _desc.maxRadiusMetres, radiusUnit);
    site.bodySeed = bodySeed;
    site.bearingRad = static_cast<float>(at) * slotRad + bearingJitterRad;
    float orbitMetres = Between(_desc.minOrbitMetres, _desc.maxOrbitMetres, orbitUnit);

    if (at == 0 && _desc.pinFirstPlanet)
    {
      site.bearingRad = _desc.firstPlanetBearingRad;
      orbitMetres = _desc.firstPlanetOrbitMetres;
    }

    // Through Translate rather than by writing localX, because localX is an offset inside a sector
    // and a system laid out near a boundary would otherwise leave the invariant behind -- the same
    // rule PatrolRingPoint follows for the same reason.
    site.posUniverse = _starPos;
    Translate(site.posUniverse, std::sin(site.bearingRad) * orbitMetres, std::cos(site.bearingRad) * orbitMetres);

    _outLayout.planets.push_back(site);
  }
}
} // namespace Game
