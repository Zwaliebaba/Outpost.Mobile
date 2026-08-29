#include "pch.h"
#include "ShipExplosion.h"

using namespace DirectX;
using namespace Neuron;

namespace Outpost
{
namespace
{
constexpr float TWO_PI = 6.28318530717958647692f;

// A ship four times the reference size gets a burst four times as wide, and the day a fighter dies
// it gets a small one. Clamped at both ends because a Structure is 90 m across and a Stargate is
// bigger still, and neither should throw sprites a kilometre.
[[nodiscard]] float HullScaleOf(const XMFLOAT3& _halfExtents) noexcept
{
  const float largest = std::max({_halfExtents.x, _halfExtents.y, _halfExtents.z});
  return std::clamp(largest / EXPLOSION_REFERENCE_HALF_SIZE, 0.25f, 8.0f);
}
} // namespace

void ShipExplosion::Start(const Spawn& _spawn, SpriteParticles& _particles)
{
  m_rng.Seed(_spawn.seed);
  if (_spawn.mesh == nullptr)
    return;

  const float hullScale = HullScaleOf(_spawn.halfExtents);
  const XMFLOAT3 pos(_spawn.world._41, _spawn.world._42, _spawn.world._43);
  const XMVECTOR shipVel = XMLoadFloat3(&_spawn.velMetresPerSec);

  // Location::Bang(pos, range, damage) with the source's own arguments, kept as named numbers so
  // the counts below read the way the original does.
  constexpr float RANGE = EXPLOSION_INTENSITY;
  constexpr float DAMAGE = RANGE / 4.0f;

  // --- Building::Destroy step A: the hull, three times over -------------------------------------
  // Three copies triple the debris density for nothing, and they tumble differently because they
  // draw from the same generator one after another. That is the source's trick and the reason the
  // count is three rather than one.
  MeshShatter::Desc desc;
  desc.lifetimeSec = EXPLOSION_FRAGMENT_LIFETIME_SEC;
  desc.radialSpeedPerSec = EXPLOSION_FRAGMENT_RADIAL_SPEED;
  desc.maxAngVelRadPerSec = EXPLOSION_FRAGMENT_MAX_ANG_VEL;
  desc.frictionCoef = EXPLOSION_FRAGMENT_FRICTION;
  desc.rotFrictionCoef = EXPLOSION_FRAGMENT_ROT_FRICTION;
  desc.minCircumferenceMetres = EXPLOSION_FRAGMENT_MIN_CIRCUMFERENCE * hullScale;
  desc.fraction = EXPLOSION_HULL_FRACTION;
  desc.maxFragments = static_cast<std::uint32_t>(EXPLOSION_FRAGMENT_CAP);
  // A shard is the colour of the panel it came off, which is the hull's own draw mix and not the
  // raw vertex colour: ScenePS lerps the same way round (design 13).
  desc.tintColour = XMFLOAT3(SHIP_COLOUR.r, SHIP_COLOUR.g, SHIP_COLOUR.b);
  desc.tintMix = SHIP_MATERIAL_MIX;

  std::uint32_t dropped = 0;
  for (MeshShatter& shatter : m_shatters)
    dropped += shatter.Spawn(*_spawn.mesh, _spawn.world, _spawn.velMetresPerSec, desc, m_rng);
  if (dropped > 0)
    DebugTrace("explosion: {} triangles past the {}-fragment cap\n", dropped, desc.maxFragments);

  // --- Location::Bang, the visual half ----------------------------------------------------------
  // World up is the plume's axis. A banked hull's own up would tilt it a few degrees and nobody
  // would see it.
  const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
  const XMVECTOR centre = XMLoadFloat3(&pos);
  const std::uint32_t droppedBefore = _particles.Dropped(); // the pool's count is cumulative; this death's share is the difference

  const int baseCores = static_cast<int>(RANGE * DAMAGE * 0.01f); // 25 at intensity 100
  const int numCores = baseCores + static_cast<int>(m_rng.Below(static_cast<std::uint32_t>(baseCores) + 1u));
  const XMVECTOR corePos = XMVectorAdd(centre, XMVectorScale(up, RANGE * EXPLOSION_CORE_LIFT * hullScale));
  XMFLOAT3 emitPos;
  XMStoreFloat3(&emitPos, corePos);
  for (int i = 0; i < numCores; ++i)
  {
    // Named draws rather than three arguments: the order a compiler evaluates arguments in is
    // unspecified, so the same seed would throw the same fireball differently on two compilers.
    const float speedX = m_rng.Signed(EXPLOSION_CORE_SPEED_XZ);
    const float speedUp = EXPLOSION_CORE_SPEED_UP_MIN + m_rng.Float01() * EXPLOSION_CORE_SPEED_UP_RANGE;
    const float speedZ = m_rng.Signed(EXPLOSION_CORE_SPEED_XZ);
    const XMVECTOR spread = XMVectorSet(speedX, speedUp, speedZ, 0.0f);
    XMFLOAT3 vel;
    XMStoreFloat3(&vel, XMVectorAdd(XMVectorScale(spread, hullScale), shipVel));
    const float size = (EXPLOSION_CORE_SIZE_MIN + m_rng.Float01() * EXPLOSION_CORE_SIZE_RANGE) * hullScale;
    _particles.Emit(SpriteType::Core, emitPos, vel, size, m_rng);
  }

  const int numDebris = std::clamp(static_cast<int>(RANGE * DAMAGE * 0.01f), EXPLOSION_DEBRIS_MIN, EXPLOSION_DEBRIS_MAX);
  const XMVECTOR debrisPos = XMVectorAdd(centre, XMVectorScale(up, RANGE * EXPLOSION_DEBRIS_LIFT * hullScale));
  XMStoreFloat3(&emitPos, debrisPos);
  for (int i = 0; i < numDebris; ++i)
  {
    // A ring in the ground plane, tilted upward by a random amount before it is normalised -- which
    // is what makes the debris climb as it spreads instead of sliding along the floor.
    XMFLOAT3 direction;
    if constexpr (EXPLOSION_PLUME_ALONG_UP)
    {
      const float angle = TWO_PI * static_cast<float>(i) / static_cast<float>(numDebris);
      direction = XMFLOAT3(std::cos(angle), EXPLOSION_DEBRIS_UP_MIN + m_rng.Float01() * EXPLOSION_DEBRIS_UP_RANGE, std::sin(angle));
    }
    else
    {
      // Uniform on the unit sphere, written out here rather than added to Pcg32: the generator
      // carries the distributions its callers agree on, and exactly one caller wants this one
      // (slice 1, 3).
      const float y = m_rng.Signed(1.0f);
      const float phi = m_rng.Float01() * TWO_PI;
      const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
      direction = XMFLOAT3(radius * std::cos(phi), y, radius * std::sin(phi));
    }

    const float speed = (EXPLOSION_DEBRIS_SPEED_MIN + m_rng.Float01() * EXPLOSION_DEBRIS_SPEED_RANGE) * hullScale;
    XMFLOAT3 vel;
    XMStoreFloat3(&vel, XMVectorAdd(XMVectorScale(XMVector3Normalize(XMLoadFloat3(&direction)), speed), shipVel));
    const float size = (EXPLOSION_DEBRIS_SIZE_MIN + m_rng.Float01() * EXPLOSION_DEBRIS_SIZE_RANGE) * hullScale;
    _particles.Emit(SpriteType::Debris, emitPos, vel, size, m_rng);
  }

  // --- Building::Destroy step D: its own cores, thrown in every direction ------------------------
  const int extraCores = static_cast<int>(EXPLOSION_INTENSITY / 4.0f);
  for (int i = 0; i < extraCores; ++i)
  {
    const float speedX = m_rng.Signed(EXPLOSION_EXTRA_CORE_SPEED);
    const float speedY = m_rng.Signed(EXPLOSION_EXTRA_CORE_SPEED);
    const float speedZ = m_rng.Signed(EXPLOSION_EXTRA_CORE_SPEED);
    const XMVECTOR spread = XMVectorSet(speedX, speedY, speedZ, 0.0f);
    XMFLOAT3 vel;
    XMStoreFloat3(&vel, XMVectorAdd(XMVectorScale(spread, hullScale), shipVel));
    _particles.Emit(SpriteType::Core, pos, vel, EXPLOSION_EXTRA_CORE_SIZE * hullScale, m_rng);
  }

  if (_particles.Dropped() > droppedBefore)
    DebugTrace("explosion: the particle pool refused {} of this death's emissions\n", _particles.Dropped() - droppedBefore);
}

bool ShipExplosion::Advance(float _dtSec)
{
  bool finished = true;
  for (MeshShatter& shatter : m_shatters)
    finished = shatter.Advance(_dtSec) && finished; // every shatter is advanced, not just up to the first live one
  return finished;
}

bool ShipExplosion::Finished() const noexcept
{
  for (const MeshShatter& shatter : m_shatters)
  {
    if (shatter.AgeSec() < shatter.Description().lifetimeSec)
      return false;
  }
  return true;
}

void ShipExplosion::BuildFragments(std::vector<FxVertex>& _out) const
{
  for (const MeshShatter& shatter : m_shatters)
    shatter.Build(_out);
}

std::uint32_t ShipExplosion::FragmentCount() const noexcept
{
  std::uint32_t count = 0;
  for (const MeshShatter& shatter : m_shatters)
    count += shatter.FragmentCount();
  return count;
}
} // namespace Outpost
