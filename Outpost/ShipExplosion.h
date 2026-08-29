#pragma once

#include "ViewTuning.h"

#include "FxVertex.h"
#include "MeshData.h"
#include "MeshShatter.h"
#include "SpriteParticles.h"

#include "Pcg32.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Outpost
{
// One dead ship's effect: the recipe that turns a hull and a place into three shatters, a fireball
// and a ring of smoking debris.
//
// This is the game half of the explosion, and it is here rather than in NeuronClient for the reason
// the whole library boundary exists (AGENTS.md 2): a mesh that breaks into triangles and a pool of
// camera-facing sprites are things a second game would want unchanged, and "twenty-five smoke
// puffs in a ring and three copies of the hull" is not. It reads no engine internals -- it calls
// MeshShatter::Spawn and SpriteParticles::Emit with numbers out of ViewTuning.h.
//
// It is a port of Building::Destroy plus the visual half of Location::Bang from Interstellar
// Outpost, with the source's constants read as metres and seconds and scaled by one number per
// ship (Design/SpaceshipExplosion.md 3).
class ShipExplosion
{
public:
  struct Spawn
  {
    const Neuron::MeshData* mesh = nullptr;
    DirectX::XMFLOAT4X4 world{};                         // the hull's last drawn matrix, bank and hover included
    DirectX::XMFLOAT3 velMetresPerSec{0.0f, 0.0f, 0.0f}; // what the ship was carrying when it died
    DirectX::XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f};     // the mesh's, for hullScale
    std::uint64_t seed = 0;                              // the ship's handle mixed with the tick it died on

    // Whether this death leaves a blast front on the ground. A parameter rather than something the
    // effect works out for itself: the station that exists cannot be destroyed, and whatever can
    // destroy one is the thing that knows it should get a ring -- not a size threshold guessing on
    // its behalf.
    bool shockRing = false;
  };

  // Runs once, in the frame the ship vanished. Particles go into the shared pool and outlive this
  // object; the shatters do not, and are what Advance ages.
  void Start(const Spawn& _spawn, Neuron::SpriteParticles& _particles);

  // Ages every shatter. Returns what Finished() would, so a caller with one explosion needs no
  // second call.
  bool Advance(float _dtSec);

  // True once every shatter has reached its lifetime and the object can be dropped.
  [[nodiscard]] bool Finished() const noexcept;

  void BuildFragments(std::vector<Neuron::FxVertex>& _out) const;

  // The blast front, if this death asked for one. This class holds no device, so it reports the
  // ring and whoever owns the decal pass draws it -- the same division as the fragments. It passes
  // well before the shatters have finished fading.
  [[nodiscard]] bool HasShockRing() const noexcept;
  [[nodiscard]] const DirectX::XMFLOAT3& ShockRingCentre() const noexcept
  {
    return m_shockCentre;
  }
  [[nodiscard]] float ShockRingRadiusMetres() const noexcept;
  [[nodiscard]] float ShockRingAlpha() const noexcept;

  [[nodiscard]] std::uint32_t FragmentCount() const noexcept;

private:
  // Fixed rather than a vector: the number of copies is a tuning constant, not a runtime answer.
  std::array<Neuron::MeshShatter, EXPLOSION_HULL_COPIES> m_shatters;
  Neuron::Pcg32 m_rng;

  DirectX::XMFLOAT3 m_shockCentre{0.0f, 0.0f, 0.0f};
  float m_shockMaxRadiusMetres = 0.0f; // zero when this death asked for no ring, which is the test for having one
  float m_shockAgeSec = 0.0f;
};
} // namespace Outpost
