#pragma once

#include "BodyDesc.h"
#include "RenderTypes.h"

#include <cstdint>

namespace Outpost
{
enum class BodyClass : std::uint8_t
{
  Terran,
  Classic,
  Ice,
  Desert,
  Barren,
  Asteroid
};

inline constexpr std::uint32_t BODY_CLASS_COUNT = 6;

// What a class of world is, to the game. This is the half of the feature that is content: the
// engine's BodyField and BodyMeshBuilder take numbers and know nothing about a terran world, and
// "terran uses LandscapeEarth with caps from 75 degrees of latitude" is a sentence only Outpost is
// allowed to say (AGENTS.md 2, Design/PlanetRenderer.md 4).
//
// The ramp is a file name and not a std::wstring, because a constexpr table cannot hold one. The
// composition root builds the path with TERRAIN_DIR at load, the way it does for fonts and icons.
struct BodyClassSpec
{
  const wchar_t* ramp;      // under Terrain\, or nullptr for the builder's fallback grey
  float polarStrength;      // how much of maxHeight a pole adds to the climate
  float capStart;           // |sin latitude| at which the cap begins; 1 means never
  bool wet;                 // the height outside a continent is below zero; the ocean itself is slice 5
  Neuron::Rgba oceanColour; // read by slice 5; named now so a row is complete rather than half a row
  bool asteroid;            // the asteroid parameter set of Design/PlanetRenderer.md 5.4
};

//                                                            ramp  polar   cap    wet    ocean colour                  asteroid
inline constexpr BodyClassSpec BODY_CLASSES[BODY_CLASS_COUNT] = {
  {L"LandscapeEarth.dds", 0.6f, 0.75f, true, {0.10f, 0.22f, 0.40f, 1.0f}, false},   // Terran: the garden world
  {L"LandscapeDefault.dds", 0.6f, 0.75f, true, {0.08f, 0.12f, 0.35f, 1.0f}, false}, // Classic: the source's green and blue
  {L"LandscapeIcecaps.dds", 1.0f, 0.35f, true, {0.30f, 0.34f, 0.42f, 1.0f}, false}, // Ice: caps from 20 degrees of latitude
  {L"LandscapeDesert.dds", 0.0f, 1.0f, false, {0.0f, 0.0f, 0.0f, 0.0f}, false},     // Desert: sand top to bottom, no caps
  {L"LandscapeMine.dds", 0.0f, 1.0f, false, {0.0f, 0.0f, 0.0f, 0.0f}, false},       // Barren: dead moons
  {L"LandscapeMine2.dds", 0.0f, 1.0f, false, {0.0f, 0.0f, 0.0f, 0.0f}, true},       // Asteroid
};

static_assert(std::size(BODY_CLASSES) == BODY_CLASS_COUNT, "the body class table has drifted from the enum");

// Clamps an unknown class to row zero rather than reading past the table, which is what HullSpecOf
// does and for the same reason: a bad id is a content mistake and should draw the wrong world, not
// crash the game.
[[nodiscard]] constexpr const BodyClassSpec& BodyClassOf(BodyClass _class) noexcept
{
  const std::uint32_t row = static_cast<std::uint32_t>(_class);
  return BODY_CLASSES[(row < BODY_CLASS_COUNT) ? row : 0];
}

// Everything else a BodyDesc holds, drawn from Pcg32(_seed) inside the BODY_* ranges
// (Design/PlanetRenderer.md 5.4, 9).
//
// The draws happen in one fixed order -- ellipsoid, lumpiness, height scale, tiles, craters, tilt --
// so a seed means one body forever. That is the same rule BodyField's constructor obeys one level
// down, and for the same reason: a server that one day sends a class and a seed sends a world.
[[nodiscard]] Neuron::BodyDesc RandomBody(std::uint64_t _seed, BodyClass _class, float _radiusMetres);
} // namespace Outpost
