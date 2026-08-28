#pragma once

#include "WorldPos.h"

#include <cstdint>

namespace Game
{
using ShipId = std::uint32_t;
inline constexpr ShipId INVALID_SHIP_ID = 0xFFFFFFFFu;

// A reference to a ship that survives a despawn.
//
// ShipId is a dense array index, which is what makes iteration cheap and is why it stays. The cost
// is that despawn moves the last ship into the freed slot, so any id stored across a tick boundary
// silently retargets to a stranger -- and the place that hurts is a snapshot delta baseline, where
// the symptom is not a crash but one ship smoothly interpolating into a different ship while a
// player watches (Design/Collision.md 6).
//
// So: anything that stores a reference across a tick boundary, or sends one over a wire, stores a
// ShipHandle; anything iterating within a tick uses the index. Resolving is one indexed load and a
// compare, and a stale handle resolves to nothing rather than to a stranger.
//
// The field is a slot rather than the ship's index because the design's shorter form -- generation
// alongside the index itself -- only makes the *despawned* ship's handles safe. The ship that
// swap-and-pop moved has a live handle naming an index that is now past the end, so it would
// resolve to nothing too, and a weapon would drop its target because something unrelated died. The
// slot is stable for the ship's life and costs one more indirection.
struct ShipHandle
{
  std::uint32_t slot = 0;
  std::uint32_t generation = 0; // bumped on despawn; 0 is never issued, so a default handle is null
};

[[nodiscard]] inline bool operator==(ShipHandle _a, ShipHandle _b) noexcept
{
  return _a.slot == _b.slot && _a.generation == _b.generation;
}

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
// between two ticks rather than sampling a half-stepped state. They are also the start-of-tick
// snapshot every neighbour query reads, which is what makes the tick order-independent -- see
// World::Step.
struct ShipState
{
  WorldPos posWorld;
  float headingRad = 0.0f; // 0 points north (+Z); forward is (sin h, 0, cos h)
  float speed = 0.0f;      // metres per second along the facing
  float turnRateRadPerSec = 0.0f;

  WorldPos prevPos;
  float prevHeading = 0.0f;

  OrderState order = OrderState::Idle;
  WorldPos orderPos;
  float orderFacingRad = 0.0f;
  bool orderHasFacing = false;

  // The heading the avoidance pass committed to last tick. It is the one piece of state the
  // steering carries between ticks, and it is what stops a plain per-tick argmax chattering when
  // two candidate headings score within noise of each other. Simulation state, so it goes over the
  // wire like everything else here (Design/Collision.md 10).
  float avoidHeadingRad = 0.0f;

  // Last tick's acceleration. Simulation output, read by the view to drive thruster response --
  // which is why it is here and not derived per frame: per frame it would be zero on every frame
  // that did not happen to land on a tick.
  float accelSample = 0.0f;

  // Which hull this ship uses. The simulation resolves it to a HullSpec; the view resolves it to a
  // mesh, and neither knows about the other's table.
  std::uint32_t hullId = 0;
};
} // namespace Game
