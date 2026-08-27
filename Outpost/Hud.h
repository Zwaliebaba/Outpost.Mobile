#pragma once

#include "TextRenderer.h"

#include <cstdint>

namespace Outpost
{
// The debug readout. Small on purpose and separate on purpose: it is the thing that grows every
// week, and it should grow somewhere that is not the frame loop.
class Hud
{
public:
  struct Stats
  {
    float fps = 0.0f;
    float frameMs = 0.0f;
    std::uint64_t tick = 0;
    int selectedCount = 0;
    std::uint32_t shipCount = 0;
    float timeScale = 1.0f;
  };

  void Draw(Neuron::TextRenderer& _text, const Stats& _stats, float _dpiScale) const;
};
} // namespace Outpost
