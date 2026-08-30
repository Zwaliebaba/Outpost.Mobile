#pragma once

#include "BodyDesc.h"

#include <cstdint>

namespace Outpost
{
// One row, and it used to be six. The other five were worlds: a terran, the source's green and blue
// classic, an ice world with caps from twenty degrees, a desert and a dead moon. They went with the
// generated planet (Design/Decisions/0026) -- what a *world* looks like is a picture now, and only
// the rocks are still generated. Their ramps are still in Assets\Terrain if one is wanted back.
enum class BodyClass : std::uint8_t
{
  Asteroid
};

inline constexpr std::uint32_t BODY_CLASS_COUNT = 1;

// What a class of world is, to the game. This is the half of the feature that is content: the
// engine's BodyField and BodyMeshBuilder take numbers and know nothing about a terran world, and
// "terran uses LandscapeEarth with caps from 75 degrees of latitude" is a sentence only Outpost is
// allowed to say (AGENTS.md 2, Design/Archive/PlanetRenderer.md 4).
//
// The ramp is a file name and not a std::wstring, because a constexpr table cannot hold one. The
// composition root builds the path with TERRAIN_DIR at load, the way it does for fonts and icons.
struct BodyClassSpec
{
  const wchar_t* ramp; // under Terrain\, or nullptr for the builder's fallback grey
};

inline constexpr BodyClassSpec BODY_CLASSES[BODY_CLASS_COUNT] = {
  {L"LandscapeMine2.dds"}, // Asteroid
};

static_assert(std::size(BODY_CLASSES) == BODY_CLASS_COUNT, "the body class table has drifted from the enum");

// Everything else a BodyDesc holds, drawn from Pcg32(_seed) inside the BODY_* ranges
// (Design/Archive/PlanetRenderer.md 5.4, 9).
//
// The draws happen in one fixed order -- ellipsoid, lumpiness, height scale, tiles, craters, tilt --
// so a seed means one body forever. That is the same rule BodyField's constructor obeys one level
// down, and for the same reason: a server that one day sends a class and a seed sends a world.
[[nodiscard]] Neuron::BodyDesc RandomBody(std::uint64_t _seed, float _radiusMetres);
} // namespace Outpost
