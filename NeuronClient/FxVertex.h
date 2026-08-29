#pragma once

namespace Neuron
{
// One vertex format for both effect passes -- tumbling hull fragments and camera-facing sprites.
// Colour and alpha arrive already curved: the fade over a fragment's life and the zero alpha the
// darkening sprite blend needs are computed on the CPU, so the two effect shaders do no fading and
// differ only in how they sample their texture.
//
// A sprite writes a zero normal and the sprite shader never reads it. Carrying the field anyway is
// what lets one input layout, one vertex buffer and one ring serve both passes.
struct FxVertex
{
  float px, py, pz; // world position -- there is no world matrix; the CPU build is what places it
  float nx, ny, nz; // world normal
  float r, g, b, a;
  float u, v;
};

// The input layout in FxRenderer spells these offsets by hand, which is the only thing that could
// disagree with this struct.
static_assert(sizeof(FxVertex) == 48, "FxVertex is padded; the effect input layout's offsets are wrong");
} // namespace Neuron
