#include "pch.h"
#include "Formation.h"

#include <cmath>

using namespace DirectX;

namespace Game
{
XMFLOAT2 FormationOffset(int _slot, int _count, FormationShape _shape, float _spacing) noexcept
{
  const float lane = static_cast<float>(_slot) - static_cast<float>(_count - 1) * 0.5f;
  switch (_shape)
  {
  case FormationShape::LineAbreast:
    return XMFLOAT2(lane * _spacing, 0.0f);
  case FormationShape::Box:
  {
    const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<float>(_count)))));
    const int row = _slot / columns;
    const int column = _slot % columns;
    const int inRow = std::min(columns, _count - row * columns);
    return XMFLOAT2((static_cast<float>(column) - static_cast<float>(inRow - 1) * 0.5f) * _spacing, -static_cast<float>(row) * _spacing);
  }
  case FormationShape::Circle:
  {
    if (_count < 2)
      return XMFLOAT2(0.0f, 0.0f);
    const float angle = XM_2PI * static_cast<float>(_slot) / static_cast<float>(_count);
    const float radius = _spacing * static_cast<float>(_count) / XM_2PI;
    return XMFLOAT2(std::sin(angle) * radius, std::cos(angle) * radius);
  }
  case FormationShape::Wedge:
  default:
    return XMFLOAT2(lane * _spacing, -std::fabs(lane) * _spacing * 0.8f);
  }
}

float FormationHeading(std::span<const UniversePos> _shipPositions, const UniversePos& _destination, float _fallbackHeadingRad) noexcept
{
  if (_shipPositions.empty())
    return _fallbackHeadingRad;

  // The centroid is accumulated as offsets from the first ship rather than by averaging fields,
  // so a group straddling a sector boundary has a centre between its ships and not a sector away.
  UniversePos centre = _shipPositions[0];
  float centreX = 0.0f;
  float centreZ = 0.0f;
  for (const UniversePos& position : _shipPositions)
  {
    centreX += OffsetX(centre, position);
    centreZ += OffsetZ(centre, position);
  }
  Translate(centre, centreX / static_cast<float>(_shipPositions.size()), centreZ / static_cast<float>(_shipPositions.size()));

  const float dx = OffsetX(centre, _destination);
  const float dz = OffsetZ(centre, _destination);
  return (dx * dx + dz * dz > 1e-4f) ? std::atan2(dx, dz) : _fallbackHeadingRad;
}
} // namespace Game
