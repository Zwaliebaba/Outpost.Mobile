#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace Game
{
enum class FormationShape : std::uint8_t
{
  LineAbreast,
  Wedge,
  Box,
  Circle
};

// Slot offsets in formation space: x to starboard, y forward. The caller rotates them onto the
// formation heading. Pure function of its arguments -- no state, no allocation, so it is the piece
// of the order pipeline that is trivially testable on its own.
[[nodiscard]] DirectX::XMFLOAT2 FormationOffset(int _slot, int _count, FormationShape _shape, float _spacing) noexcept;
} // namespace Game
