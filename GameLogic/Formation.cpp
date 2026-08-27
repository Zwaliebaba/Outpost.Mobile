#include "pch.h"
#include "Formation.h"

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
} // namespace Game
