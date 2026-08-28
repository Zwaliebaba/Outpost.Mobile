#include "pch.h"
#include "Collision.h"

namespace Game
{
namespace
{
// Below this a segment is a point and a distance is zero. Chosen against metres: hulls are between
// one and two hundred and fifty metres, so nothing real lands here.
constexpr float DEGENERATE_METRES = 1e-4f;
constexpr float DEGENERATE_METRES_SQUARED = DEGENERATE_METRES * DEGENERATE_METRES;

// The fallback direction when the closest points coincide -- two capsules whose segments cross or
// lie along one another, and, at the limit, two hulls spawned on the same spot.
//
// Preferring the line of centres is not cosmetic. A direction picked from the ids is reproducible
// but arbitrary, and an arbitrary push does not guarantee progress: a bundle of parallel hulls
// shoved in random directions random-walks instead of unpacking, and a hundred of them measurably
// fails to separate at all. The line of centres always increases the centre distance, so every tick
// is progress. It is also exactly negated when the pair is evaluated from the other side, which the
// gather requires.
void DegenerateNormal(ShipId _idA, ShipId _idB, float _centreOffsetX, float _centreOffsetZ, float& _outX, float& _outZ) noexcept
{
  const float centreDistanceSquared = _centreOffsetX * _centreOffsetX + _centreOffsetZ * _centreOffsetZ;
  if (centreDistanceSquared > DEGENERATE_METRES_SQUARED)
  {
    const float centreDistance = std::sqrt(centreDistanceSquared);
    _outX = _centreOffsetX / centreDistance;
    _outZ = _centreOffsetZ / centreDistance;
    return;
  }

  // Truly concentric, so there is nothing left to derive a direction from but identity.
  const ShipId low = (_idA < _idB) ? _idA : _idB;
  const ShipId high = (_idA < _idB) ? _idB : _idA;

  std::uint32_t mix = low * 0x9E3779B1u;
  mix ^= (high * 0x85EBCA77u) + 0x9E3779B9u + (mix << 6) + (mix >> 2);
  mix ^= mix >> 15;

  constexpr float TWO_PI = 6.28318530718f;
  const float angle = static_cast<float>(mix & 1023u) * (TWO_PI / 1024.0f);
  // Canonically from the low id towards the high one, then flipped so it points from B towards A.
  const float sign = (_idA == high) ? 1.0f : -1.0f;
  _outX = std::sin(angle) * sign;
  _outZ = std::cos(angle) * sign;
}

[[nodiscard]] float Saturate(float _value) noexcept
{
  return (_value < 0.0f) ? 0.0f : ((_value > 1.0f) ? 1.0f : _value);
}
} // namespace

Contact CapsuleContact(const Capsule& _a, const Capsule& _b, ShipId _idA, ShipId _idB) noexcept
{
  const float reach = _a.radiusMetres + _b.radiusMetres;
  float closestX = _a.centreX - _b.centreX;
  float closestZ = _a.centreZ - _b.centreZ;

  // Circle against circle is a handful of operations against roughly thirty for segment against
  // segment, and it is the overwhelmingly common pair.
  if (_a.halfLengthMetres > DEGENERATE_METRES || _b.halfLengthMetres > DEGENERATE_METRES)
  {
    // Ericson's segment-segment closest approach, in the plane. p is the start of each segment and
    // d its full extent; s and t are the parameters of the closest points along them.
    const float p1x = _a.centreX - _a.axisX * _a.halfLengthMetres;
    const float p1z = _a.centreZ - _a.axisZ * _a.halfLengthMetres;
    const float d1x = _a.axisX * 2.0f * _a.halfLengthMetres;
    const float d1z = _a.axisZ * 2.0f * _a.halfLengthMetres;
    const float p2x = _b.centreX - _b.axisX * _b.halfLengthMetres;
    const float p2z = _b.centreZ - _b.axisZ * _b.halfLengthMetres;
    const float d2x = _b.axisX * 2.0f * _b.halfLengthMetres;
    const float d2z = _b.axisZ * 2.0f * _b.halfLengthMetres;

    const float rx = p1x - p2x;
    const float rz = p1z - p2z;
    const float lengthA = d1x * d1x + d1z * d1z;
    const float lengthB = d2x * d2x + d2z * d2z;
    const float projectB = d2x * rx + d2z * rz;
    const float projectA = d1x * rx + d1z * rz;
    const float between = d1x * d2x + d1z * d2z;

    float s = 0.0f;
    float t = 0.0f;
    if (lengthA <= DEGENERATE_METRES_SQUARED && lengthB <= DEGENERATE_METRES_SQUARED)
    {
      // Both degenerate: two circles after all.
    }
    else if (lengthA <= DEGENERATE_METRES_SQUARED)
    {
      t = Saturate(projectB / lengthB);
    }
    else if (lengthB <= DEGENERATE_METRES_SQUARED)
    {
      s = Saturate(-projectA / lengthA);
    }
    else
    {
      const float denominator = lengthA * lengthB - between * between;
      // Parallel segments leave s free, so pin it to the start of A and let t follow. Any point on
      // the overlap is equally close, and picking a fixed one is what keeps this reproducible.
      s = (denominator > DEGENERATE_METRES_SQUARED) ? Saturate((between * projectB - projectA * lengthB) / denominator) : 0.0f;
      t = (between * s + projectB) / lengthB;
      if (t < 0.0f)
      {
        t = 0.0f;
        s = Saturate(-projectA / lengthA);
      }
      else if (t > 1.0f)
      {
        t = 1.0f;
        s = Saturate((between - projectA) / lengthA);
      }
    }

    closestX = (p1x + d1x * s) - (p2x + d2x * t);
    closestZ = (p1z + d1z * s) - (p2z + d2z * t);
  }

  const float distanceSquared = closestX * closestX + closestZ * closestZ;
  if (distanceSquared >= reach * reach)
    return {};

  Contact contact;
  contact.touching = true;
  if (distanceSquared <= DEGENERATE_METRES_SQUARED)
  {
    // Concentric. Never a random direction and never an uninitialised one: the pair has to unwind
    // the same way on a rerun.
    DegenerateNormal(_idA, _idB, _a.centreX - _b.centreX, _a.centreZ - _b.centreZ, contact.normalX, contact.normalZ);
    contact.overlapMetres = reach;
    return contact;
  }

  const float distance = std::sqrt(distanceSquared);
  contact.overlapMetres = reach - distance;
  contact.normalX = closestX / distance;
  contact.normalZ = closestZ / distance;
  return contact;
}

SeparationShares SeparationSharesFor(float _authorityA, bool _immovableA, float _authorityB, bool _immovableB) noexcept
{
  if (_immovableA && _immovableB)
    return {};
  if (_immovableA)
    return {0.0f, 1.0f};
  if (_immovableB)
    return {1.0f, 0.0f};

  const float total = _authorityA + _authorityB;
  if (total <= 0.0f)
    return {0.5f, 0.5f};
  return {_authorityB / total, _authorityA / total};
}
} // namespace Game
