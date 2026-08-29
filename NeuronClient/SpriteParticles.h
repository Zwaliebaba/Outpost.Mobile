#pragma once

#include "FxVertex.h"

#include "Pcg32.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace Neuron
{
enum class SpriteType : std::uint8_t
{
  Core,
  Debris,
  Smoke
};

// Two blends, and the difference between them is the whole reason the effect reads as fire and
// smoke rather than two clouds of pink. Additive brightens what is behind it; Dark darkens it,
// through a destination blend of one-minus-source-colour and a vertex alpha of zero (FxRenderer,
// Design/SpaceshipExplosion.md 7).
enum class SpriteBlend : std::uint8_t
{
  Dark,
  Additive
};

struct SpriteTypeSpec
{
  float lifeSec;
  float size; // the quad's half-size is size / 16, the source effect's convention
  float friction;
  DirectX::XMFLOAT3 colourA; // 0..1; a particle picks once between A and B at emission
  DirectX::XMFLOAT3 colourB;
  SpriteBlend blend;
};

// Indexed by SpriteType. A fourth kind of particle is a row here and a name in the enum; the table
// is constexpr data rather than a file because this tree has no configuration file (AGENTS.md).
inline constexpr SpriteTypeSpec SPRITE_TYPES[] = {
  // Core: the fireball. Additive, short, and large.
  {2.0f,
   150.0f,
   0.2f,
   {200.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f},
   {255.0f / 255.0f, 120.0f / 255.0f, 120.0f / 255.0f},
   SpriteBlend::Additive},
  // Debris: a lump thrown out in a ring, shedding smoke behind it as it goes.
  {6.0f,
   40.0f,
   0.2f,
   {200.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f},
   {250.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f},
   SpriteBlend::Dark},
  // Smoke: emitted by Debris, never directly by a recipe. No friction: it hangs where it was left.
  {5.0f,
   200.0f,
   0.0f,
   {100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f},
   {200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f},
   SpriteBlend::Dark},
};

// A fixed-capacity pool of camera-facing billboards, shared by every explosion on screen.
//
// Fixed rather than growing on purpose: a pool that grows turns a bad frame into a worse one, and
// what overflows here is smoke, which is the right thing to lose. An Emit that finds the pool full
// drops the particle and counts it, and the count is readable, because a silent cap reads to
// whoever is looking at it as "everything fitted".
//
// Device-free, like MeshShatter: it takes a dtSec and writes FxVertex into a caller's vector.
class SpriteParticles
{
public:
  // Reproduces the source's `frand() < 0.5 * dt * 10` -- five puffs a second behind each debris.
  static constexpr float SMOKE_RATE_PER_SEC = 5.0f;
  static constexpr float PEAK_ALPHA = 90.0f / 255.0f;
  static constexpr float FADE_START_FRACTION = 0.75f;
  static constexpr float SMOKE_VELOCITY_DIVISOR = 5.0f;
  static constexpr float SMOKE_SIZE_DIVISOR = 1.5f;

  struct Desc
  {
    std::uint32_t capacity = 4096;
  };

  struct Particle
  {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 velMetresPerSec;
    DirectX::XMFLOAT3 colour; // picked once at emission and never changed; only alpha moves
    float size = 0.0f;
    float ageSec = 0.0f;
    SpriteType type = SpriteType::Core;
  };

  void Init(const Desc& _desc);

  void Emit(SpriteType _type, const DirectX::XMFLOAT3& _pos, const DirectX::XMFLOAT3& _velMetresPerSec, float _size, Pcg32& _rng);

  void Advance(float _dtSec, Pcg32& _rng);

  // Emits only the particles whose type carries _blend, so the renderer makes two calls and gets
  // two disjoint sets, one per pipeline.
  void Build(SpriteBlend _blend, const DirectX::XMFLOAT3& _cameraRight, const DirectX::XMFLOAT3& _cameraUp,
             std::vector<FxVertex>& _out) const;

  void Clear() noexcept;

  [[nodiscard]] std::uint32_t Count() const noexcept
  {
    return static_cast<std::uint32_t>(m_particles.size());
  }
  [[nodiscard]] std::uint32_t Capacity() const noexcept
  {
    return m_capacity;
  }
  // Emissions refused for want of room since Init or Clear.
  [[nodiscard]] std::uint32_t Dropped() const noexcept
  {
    return m_dropped;
  }
  [[nodiscard]] const Particle& At(std::uint32_t _index) const noexcept
  {
    return m_particles[_index];
  }

private:
  std::vector<Particle> m_particles;
  // Smoke a debris particle sheds lands here during Advance and is appended after the sweep. A
  // fixed pool does not make emitting mid-iteration safe, it only changes the failure from an
  // invalidated iterator into a silent overwrite. Member rather than local, so a warm frame
  // allocates nothing.
  std::vector<Particle> m_scratch;
  std::uint32_t m_capacity = 0;
  std::uint32_t m_dropped = 0;
};
} // namespace Neuron
