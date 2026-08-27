#include "pch.h"
#include "Hud.h"

namespace Outpost
{
namespace
{
constexpr float HUD_MARGIN_PX = 12.0f;
constexpr float HUD_TOP_PX = 10.0f;
inline constexpr Neuron::Rgba HUD_COLOUR{0.78f, 0.87f, 0.96f, 1.0f};
} // namespace

void Hud::Draw(Neuron::TextRenderer& _text, const Stats& _stats, float _dpiScale) const
{
  char line[512] = {};
  std::snprintf(line, sizeof(line), "fps       %6.1f\nframe     %6.2f ms\nsim tick  %6llu\nselected  %6d of %u\ntime      %5.2fx",
                static_cast<double>(_stats.fps), static_cast<double>(_stats.frameMs),
                static_cast<unsigned long long>(_stats.tick), _stats.selectedCount, _stats.shipCount,
                static_cast<double>(_stats.timeScale));
  _text.DrawTextLine(HUD_MARGIN_PX * _dpiScale, HUD_TOP_PX * _dpiScale, _dpiScale, HUD_COLOUR, line);
}
} // namespace Outpost
