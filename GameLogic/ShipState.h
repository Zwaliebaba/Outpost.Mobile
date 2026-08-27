#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace Game
{
using ShipId = std::uint32_t;
inline constexpr ShipId INVALID_SHIP_ID = 0xFFFFFFFFu;

enum class OrderState : std::uint8_t
{
  Idle,
  Moving,  // steering towards orderPos
  Aligning // arrived; turning onto the ordered facing
};

// One ship, as the simulation sees it. Everything here is advanced only in World::Step, and there
// is nothing in it a renderer needs that a snapshot could not carry over a wire.
//
// prevPos/prevHeading are the values from the tick before, kept so that the view can interpolate
// between two ticks rather than sampling a half-stepped state. They are output, not input: Step
// writes them and never reads them.
struct ShipState
{
  DirectX::XMFLOAT3 posWorld{0.0f, 0.0f, 0.0f};
  float headingRad = 0.0f; // 0 points north (+Z); forward is (sin h, 0, cos h)
  float speed = 0.0f;      // metres per second along the facing
  float turnRateRadPerSec = 0.0f;

  DirectX::XMFLOAT3 prevPos{0.0f, 0.0f, 0.0f};
  float prevHeading = 0.0f;

  OrderState order = OrderState::Idle;
  DirectX::XMFLOAT3 orderPos{0.0f, 0.0f, 0.0f};
  float orderFacingRad = 0.0f;
  bool orderHasFacing = false;

  // Last tick's acceleration. Simulation output, read by the view to drive thruster response --
  // which is why it is here and not derived per frame: per frame it would be zero on every frame
  // that did not happen to land on a tick.
  float accelSample = 0.0f;

  // Which hull this ship uses. The simulation never resolves it to a mesh; the view does.
  std::uint32_t hullId = 0;
};
} // namespace Game
