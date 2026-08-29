#include "pch.h"
#include "SpriteParticles.h"

using namespace DirectX;

namespace Neuron
{
namespace
{
[[nodiscard]] const SpriteTypeSpec& Spec(SpriteType _type) noexcept
{
  return SPRITE_TYPES[static_cast<std::size_t>(_type)];
}

// The colour is chosen once, here, and then never moves: over its life a particle changes only in
// alpha. Two draws of the same type therefore differ, and neither shifts hue as it fades.
[[nodiscard]] SpriteParticles::Particle MakeParticle(SpriteType _type, const XMFLOAT3& _pos, const XMFLOAT3& _vel, float _size,
                                                     Pcg32& _rng) noexcept
{
  const SpriteTypeSpec& spec = Spec(_type);
  const float mix = _rng.Float01();

  SpriteParticles::Particle particle;
  particle.pos = _pos;
  particle.velMetresPerSec = _vel;
  particle.colour =
    XMFLOAT3(spec.colourA.x + (spec.colourB.x - spec.colourA.x) * mix, spec.colourA.y + (spec.colourB.y - spec.colourA.y) * mix,
             spec.colourA.z + (spec.colourB.z - spec.colourA.z) * mix);
  particle.size = _size;
  particle.ageSec = 0.0f;
  particle.type = _type;
  return particle;
}
} // namespace

void SpriteParticles::Init(const Desc& _desc)
{
  m_capacity = _desc.capacity;
  m_particles.clear();
  m_particles.reserve(m_capacity);
  m_scratch.clear();
  m_dropped = 0;
}

void SpriteParticles::Emit(SpriteType _type, const XMFLOAT3& _pos, const XMFLOAT3& _velMetresPerSec, float _size, Pcg32& _rng)
{
  // The draw happens either way, so that a pool which happens to be full does not shift the stream
  // every later particle is picked from -- a dropped smoke puff must not change the fireball.
  const Particle particle = MakeParticle(_type, _pos, _velMetresPerSec, _size, _rng);
  if (m_particles.size() >= m_capacity)
  {
    ++m_dropped;
    return;
  }
  m_particles.push_back(particle);
}

void SpriteParticles::Advance(float _dtSec, Pcg32& _rng)
{
  m_scratch.clear();

  for (std::size_t i = 0; i < m_particles.size();)
  {
    Particle& particle = m_particles[i];
    const SpriteTypeSpec& spec = Spec(particle.type);

    particle.ageSec += _dtSec;
    if (particle.ageSec >= spec.lifeSec)
    {
      // Swap-remove keeps the pool dense, so nothing iterates over holes. Order is not promised and
      // nothing depends on it: the particles are unsorted at draw time too.
      particle = m_particles.back();
      m_particles.pop_back();
      continue;
    }

    const XMVECTOR vel = XMLoadFloat3(&particle.velMetresPerSec);
    XMStoreFloat3(&particle.pos, XMVectorAdd(XMLoadFloat3(&particle.pos), XMVectorScale(vel, _dtSec)));
    XMStoreFloat3(&particle.velMetresPerSec, XMVectorScale(vel, 1.0f - _dtSec * spec.friction));

    if (particle.type == SpriteType::Debris && _rng.Float01() < SMOKE_RATE_PER_SEC * _dtSec)
    {
      XMFLOAT3 smokeVel;
      XMStoreFloat3(&smokeVel, XMVectorScale(XMLoadFloat3(&particle.velMetresPerSec), 1.0f / SMOKE_VELOCITY_DIVISOR));
      m_scratch.push_back(MakeParticle(SpriteType::Smoke, particle.pos, smokeVel, particle.size / SMOKE_SIZE_DIVISOR, _rng));
    }

    ++i;
  }

  for (const Particle& smoke : m_scratch)
  {
    if (m_particles.size() >= m_capacity)
    {
      ++m_dropped;
      continue;
    }
    m_particles.push_back(smoke);
  }
}

void SpriteParticles::Build(SpriteBlend _blend, const XMFLOAT3& _cameraRight, const XMFLOAT3& _cameraUp, std::vector<FxVertex>& _out) const
{
  const XMVECTOR right = XMLoadFloat3(&_cameraRight);
  const XMVECTOR up = XMLoadFloat3(&_cameraUp);

  // One reserve for the upper bound; nothing in the loop allocates.
  _out.reserve(_out.size() + m_particles.size() * 6);

  for (const Particle& particle : m_particles)
  {
    const SpriteTypeSpec& spec = Spec(particle.type);
    if (spec.blend != _blend)
      continue;

    const float fadeStartSec = FADE_START_FRACTION * spec.lifeSec;
    float alpha = PEAK_ALPHA;
    if (particle.ageSec >= fadeStartSec)
      alpha = PEAK_ALPHA * (1.0f - (particle.ageSec - fadeStartSec) / (spec.lifeSec - fadeStartSec));
    alpha = std::clamp(alpha, 0.0f, PEAK_ALPHA);

    float r = particle.colour.x;
    float g = particle.colour.y;
    float b = particle.colour.z;
    float a = alpha;
    if (_blend == SpriteBlend::Dark)
    {
      // Deliberate, and it looks like a bug: with a source alpha of zero the source term vanishes
      // and the frame becomes dest * (1 - src.rgb), so a light sprite *darkens* what is behind it.
      // That destination term is what draws. Give this an alpha and the smoke turns into fog.
      // Design/SpaceshipExplosion.md 7.
      const float fade = alpha / PEAK_ALPHA;
      r *= fade;
      g *= fade;
      b *= fade;
      a = 0.0f;
    }

    // A 45-degree diamond rather than an axis-aligned quad: it is what hides the square silhouette
    // of a 16x16 sprite, and it is the source effect's own corner order.
    const float half = particle.size / 16.0f;
    const XMVECTOR centre = XMLoadFloat3(&particle.pos);
    const XMVECTOR offsets[4] = {XMVectorScale(up, -half), XMVectorScale(right, half), XMVectorScale(up, half),
                                 XMVectorScale(right, -half)};
    constexpr float US[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    constexpr float VS[4] = {0.0f, 0.0f, 1.0f, 1.0f};

    XMFLOAT3 corners[4];
    for (int i = 0; i < 4; ++i)
      XMStoreFloat3(&corners[i], XMVectorAdd(centre, offsets[i]));

    constexpr int TRIANGLES[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i)
    {
      const int corner = TRIANGLES[i];
      _out.push_back(FxVertex::Make(corners[corner], XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT4(r, g, b, a), XMFLOAT2(US[corner], VS[corner])));
    }
  }
}

void SpriteParticles::Clear() noexcept
{
  m_particles.clear();
  m_scratch.clear();
  m_dropped = 0;
}
} // namespace Neuron
