#pragma once

#include <cstdint>

namespace Neuron
{
// One pointer message, decoded from WM_POINTER* and queued rather than acted on immediately: the
// frame loop drains the queue once the camera matrices are current, so a pick tests against the
// matrices that were on screen when the contact happened.
//
// WM_POINTER covers mouse, pen and touch with one path, which is the whole reason for using it:
// the same build works on a desktop and on a tablet with no second code path.
struct PointerEvent
{
  enum class Kind : std::uint8_t
  {
    Down,
    Update,
    Up,
    Wheel
  };

  Kind kind = Kind::Update;
  std::uint32_t pointerId = 0;
  float xPx = 0.0f;
  float yPx = 0.0f;
  std::uint32_t buttons = 0; // bit 0 first (left / touch contact), bit 1 second, bit 2 third
  bool isTouch = false;
  bool shift = false;
  std::int32_t wheelNotches = 0;
  std::int64_t timestampQpc = 0; // when the hardware reported the contact, where it says

  static constexpr std::uint32_t BUTTON_FIRST = 0x1u;
  static constexpr std::uint32_t BUTTON_SECOND = 0x2u;
  static constexpr std::uint32_t BUTTON_THIRD = 0x4u;
  static constexpr std::uint32_t BUTTON_CAMERA = BUTTON_SECOND | BUTTON_THIRD;
};
} // namespace Neuron
