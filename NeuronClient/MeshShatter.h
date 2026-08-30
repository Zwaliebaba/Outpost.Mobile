#pragma once

#include "FxVertex.h"
#include "MeshData.h"

#include "Pcg32.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace Neuron
{
// One shattered mesh: every triangle of a hull detached, thrown outward from the mesh centre,
// tumbling and fading over its lifetime.
//
// It holds no device and includes no graphics header. Spawn takes a MeshData and a world matrix,
// Advance takes a dtSec, and Build writes FxVertex into a caller's vector -- which is what lets the
// whole of the effect's arithmetic be decided by tests, with the D3D12 in one other file.
//
// Fragments share five tumbles rather than carrying one each. That is the source effect's own
// trick, and it is what keeps a fifteen-hundred-fragment shatter at five rotation matrices per
// frame instead of fifteen hundred.
class MeshShatter
{
public:
  static constexpr int TUMBLER_COUNT = 5;

  struct Tumbler
  {
    DirectX::XMFLOAT3X3 rot;
    DirectX::XMFLOAT3 angVelRadPerSec;
  };

  struct Fragment
  {
    DirectX::XMFLOAT3 pos;             // world-space centroid
    DirectX::XMFLOAT3 velMetresPerSec; //
    DirectX::XMFLOAT3 v0, v1, v2;      // vertices relative to the centroid, world orientation, untumbled
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT3 colour; // lerp(vertexColour, desc.livery * vertexColour, vertexRace), fixed at spawn
    std::uint8_t tumbler = 0; // 0..TUMBLER_COUNT-1
  };

  struct Desc
  {
    float lifetimeSec = 5.0f;
    float radialSpeedPerSec = 3.0f;      // vel = (centroid - centre) * this
    float maxAngVelRadPerSec = 4.0f;     // per axis, uniform in +-
    float frictionCoef = 0.05f;          // linear drag
    float rotFrictionCoef = 0.2f;        // angular drag
    float minCircumferenceMetres = 6.0f; // smaller triangles are skipped
    float fraction = 1.0f;               // keep each triangle with this probability
    std::uint32_t maxFragments = 500;
    DirectX::XMFLOAT3 gravityMetresPerSec2{0.0f, 0.0f, 0.0f};

    // Fragment colour = lerp(vertexColour, livery * vertexColour, vertexRace), which is exactly the
    // line ScenePS runs, so a shard is the colour the panel it came off was being drawn in. A shard
    // off a grey hull plate stays grey; a shard off a liveried panel keeps the faction's paint. The
    // default is white, which leaves every vertex colour as it stands.
    DirectX::XMFLOAT3 livery{1.0f, 1.0f, 1.0f};
  };

  // Shatters _mesh, placed by _world, into fragments carrying _inheritedVelMetresPerSec on top of
  // their radial throw -- so a wreck keeps drifting the way the ship was going.
  //
  // Returns how many triangles passed rejection and arrived after maxFragments, so the caller can
  // trace it: a hull with more faces than the cap loses the difference silently otherwise.
  std::uint32_t Spawn(const MeshData& _mesh, const DirectX::XMFLOAT4X4& _world, const DirectX::XMFLOAT3& _inheritedVelMetresPerSec,
                      const Desc& _desc, Pcg32& _rng);

  // True once the shatter has reached its lifetime and should be dropped.
  bool Advance(float _dtSec);

  void Build(std::vector<FxVertex>& _out) const;

  [[nodiscard]] std::uint32_t FragmentCount() const noexcept
  {
    return static_cast<std::uint32_t>(m_fragments.size());
  }
  [[nodiscard]] float AgeSec() const noexcept
  {
    return m_ageSec;
  }
  [[nodiscard]] const Desc& Description() const noexcept
  {
    return m_desc;
  }
  [[nodiscard]] const Tumbler& TumblerAt(int _index) const noexcept
  {
    return m_tumblers[_index];
  }
  [[nodiscard]] const Fragment& FragmentAt(std::uint32_t _index) const noexcept
  {
    return m_fragments[_index];
  }

private:
  std::vector<Fragment> m_fragments;
  Tumbler m_tumblers[TUMBLER_COUNT] = {};
  Desc m_desc;
  float m_ageSec = 0.0f;
};
} // namespace Neuron
