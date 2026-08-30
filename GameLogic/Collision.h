#pragma once

#include "HullSpec.h"
#include "ShipState.h"

namespace Game
{
// The narrow phase, and nothing else.
//
// No mass, no momentum, no restitution, no impulses. This is an RTS: units nudge each other apart
// and give way. Rigid-body response would make a fleet behave like a break shot and would put an
// integrator's stability into the replay contract (Design/Archive/Collision.md 8).

// A capsule in the ground plane, in whatever local frame the caller is working in. Both ends of a
// pair are expressed in the same frame and the frame is always local -- never absolute world
// coordinates -- so this stays correct the day a sector boundary can sit between two hulls.
struct Capsule
{
  float centreX = 0.0f;
  float centreZ = 0.0f;
  float axisX = 0.0f; // unit vector along the hull's forward direction
  float axisZ = 1.0f;
  float halfLengthMetres = 0.0f; // 0 is a circle, which is the common case and has a fast path
  float radiusMetres = 1.0f;
};

struct Contact
{
  bool touching = false;
  float overlapMetres = 0.0f;
  float normalX = 0.0f; // unit, pointing from B towards A
  float normalZ = 0.0f;
};

// Closest approach between two capsules. The ids are not used for the geometry -- they are the
// tie-break for the one degenerate case, concentric hulls, where the closest points coincide and
// there is no normal to compute. That case has to produce the same direction on a rerun, and the
// two sides of the pair have to produce exactly opposite directions, or the gather in World's
// separation pass would push both ships the same way.
[[nodiscard]] Contact CapsuleContact(const Capsule& _a, const Capsule& _b, ShipId _idA, ShipId _idB) noexcept;

// How a contacting pair splits the correction between them.
//
// With a 31:1 mobile size ratio, a symmetric split is wrong and it looks wrong -- a Carrier
// shouldering aside for an Interceptor reads as a bug to anyone watching. The share a hull takes is
// the *other* hull's authority over the sum, so authority is a hull's tendency to hold its line.
// Both sides compute the same split from the same two numbers, so the pass stays order-independent
// and needs no arbitration step (Design/Archive/Collision.md 9).
struct SeparationShares
{
  float a = 0.0f;
  float b = 0.0f;
};

// Two immovable hulls -- structures authored overlapping, a turret placed against a station -- give
// a zero denominator. Neither can move, so the answer is to take no correction at all, and the
// guard is here rather than at the call site because a NaN discovered after it has propagated
// through a position and into a snapshot is not a debuggable failure.
[[nodiscard]] SeparationShares SeparationSharesFor(float _authorityA, bool _immovableA, float _authorityB, bool _immovableB) noexcept;

// A hull's authority as the passes actually use it. Spelled once here because both of them need the
// same number from the same rule: if avoidance and separation disagreed about who yields, a ship
// would steer aside and then be shoved back.
//
// A parked ship holds its station harder than one under way, which is what stops a fleet on station
// being walked off it by passing traffic. Formation drift is not a separate problem; it is this one
// with a different number (Design/Archive/Collision.md 9).
[[nodiscard]] float AvoidanceAuthorityOf(const HullSpec& _hull, OrderState _order) noexcept;
} // namespace Game
