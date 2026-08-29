#pragma once

#include "WorldPos.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>

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

// Which way a formation faces when the order did not say: along the way the group is about to
// travel, from its own centre to the destination.
//
// One definition, called by both halves. The server orients the formation with it and the client
// orients the order marker with it, from the same ship positions and the same tapped point, so the
// two cannot disagree about which way an order points. Nothing is predicted here -- it is the same
// arithmetic on the same inputs, and the client has all of them in its snapshot
// (Design/Collision-slice-2b.md 2.5).
[[nodiscard]] float FormationHeading(std::span<const WorldPos> _shipPositions, const WorldPos& _destination,
                                     float _fallbackHeadingRad) noexcept;
} // namespace Game
