#include "pch.h"
#include "MeshShatter.h"

using namespace DirectX;

namespace Neuron
{
namespace
{
// Two vertices closer together than this are the same vertex, and the triangle between them has no
// area and no normal. Squared, so the test is a dot product.
constexpr float DEGENERATE_EDGE_SQ = 1e-12f;

[[nodiscard]] XMFLOAT3 Lerp(const XMFLOAT3& _from, const XMFLOAT3& _to, float _t) noexcept
{
  return XMFLOAT3(_from.x + (_to.x - _from.x) * _t, _from.y + (_to.y - _from.y) * _t, _from.z + (_to.z - _from.z) * _t);
}
} // namespace

std::uint32_t MeshShatter::Spawn(const MeshData& _mesh, const XMFLOAT4X4& _world, const XMFLOAT3& _inheritedVelMetresPerSec,
                                 const Desc& _desc, Pcg32& _rng)
{
  m_fragments.clear();
  m_desc = _desc;
  m_ageSec = 0.0f;

  for (int i = 0; i < TUMBLER_COUNT; ++i)
  {
    XMStoreFloat3x3(&m_tumblers[i].rot, XMMatrixIdentity());
    // Three statements and not three arguments: the order a compiler evaluates a function's
    // arguments in is unspecified, so three draws inside one XMFLOAT3 is three draws in whichever
    // order the compiler felt like, and two compilers that disagree hand one seed two different
    // tumbles. Measured rather than feared -- the same shape in BodyField built two different worlds
    // under gcc and clang before its draws were named.
    const float pitch = _rng.Signed(_desc.maxAngVelRadPerSec);
    const float yaw = _rng.Signed(_desc.maxAngVelRadPerSec);
    const float roll = _rng.Signed(_desc.maxAngVelRadPerSec);
    m_tumblers[i].angVelRadPerSec = XMFLOAT3(pitch, yaw, roll);
  }

  const XMMATRIX world = XMLoadFloat4x4(&_world);
  const XMVECTOR inherited = XMLoadFloat3(&_inheritedVelMetresPerSec);

  // The hull's origin sits at its base, not in the middle of it (MeshData::RestY), so shattering
  // about the origin would throw the whole wreck upward. The bounds centre goes through the same
  // world matrix as the vertices do.
  const XMFLOAT3 meshCentre = _mesh.BoundsCentre();
  const XMVECTOR worldCentre = XMVector3TransformCoord(XMLoadFloat3(&meshCentre), world);

  const std::size_t triangles = _mesh.verts.size() / 3;
  m_fragments.reserve(std::min(static_cast<std::size_t>(_desc.maxFragments), triangles));

  std::uint32_t dropped = 0;
  for (std::size_t triangle = 0; triangle < triangles; ++triangle)
  {
    // The thinning draw comes first and is not a drop: a caller asking for half the triangles gets
    // half of them, and the returned count stays what it says it is -- what the cap took.
    if (_desc.fraction < 1.0f && _rng.Float01() >= _desc.fraction)
      continue;

    const MeshVertex& a = _mesh.verts[triangle * 3 + 0];
    const MeshVertex& b = _mesh.verts[triangle * 3 + 1];
    const MeshVertex& c = _mesh.verts[triangle * 3 + 2];

    const XMFLOAT3 localA(a.px, a.py, a.pz);
    const XMFLOAT3 localB(b.px, b.py, b.pz);
    const XMFLOAT3 localC(c.px, c.py, c.pz);
    const XMVECTOR p0 = XMVector3TransformCoord(XMLoadFloat3(&localA), world);
    const XMVECTOR p1 = XMVector3TransformCoord(XMLoadFloat3(&localB), world);
    const XMVECTOR p2 = XMVector3TransformCoord(XMLoadFloat3(&localC), world);

    const XMVECTOR e0 = XMVectorSubtract(p1, p0);
    const XMVECTOR e1 = XMVectorSubtract(p2, p1);
    const XMVECTOR e2 = XMVectorSubtract(p0, p2);
    const float lengthSq0 = XMVectorGetX(XMVector3LengthSq(e0));
    const float lengthSq1 = XMVectorGetX(XMVector3LengthSq(e1));
    const float lengthSq2 = XMVectorGetX(XMVector3LengthSq(e2));
    if (lengthSq0 < DEGENERATE_EDGE_SQ || lengthSq1 < DEGENERATE_EDGE_SQ || lengthSq2 < DEGENERATE_EDGE_SQ)
      continue;

    // Measured after the world transform, so the threshold is in metres however the hull is scaled.
    const float circumference = std::sqrt(lengthSq0) + std::sqrt(lengthSq1) + std::sqrt(lengthSq2);
    if (circumference < _desc.minCircumferenceMetres)
      continue;

    const XMVECTOR centroid = XMVectorScale(XMVectorAdd(XMVectorAdd(p0, p1), p2), 1.0f / 3.0f);
    const XMVECTOR r0 = XMVectorSubtract(p0, centroid);
    const XMVECTOR r1 = XMVectorSubtract(p1, centroid);
    const XMVECTOR r2 = XMVectorSubtract(p2, centroid);

    // Three distinct but collinear vertices survive the edge test above and have no normal at all;
    // normalising that zero would light the shard black rather than fail.
    const XMVECTOR cross = XMVector3Cross(XMVectorSubtract(r0, r1), XMVectorSubtract(r1, r2));
    if (XMVectorGetX(XMVector3LengthSq(cross)) < DEGENERATE_EDGE_SQ)
      continue;

    if (m_fragments.size() >= _desc.maxFragments)
    {
      ++dropped;
      continue;
    }

    Fragment fragment;
    XMStoreFloat3(&fragment.pos, centroid);
    XMStoreFloat3(&fragment.velMetresPerSec,
                  XMVectorAdd(XMVectorScale(XMVectorSubtract(centroid, worldCentre), _desc.radialSpeedPerSec), inherited));
    XMStoreFloat3(&fragment.v0, r0);
    XMStoreFloat3(&fragment.v1, r1);
    XMStoreFloat3(&fragment.v2, r2);
    XMStoreFloat3(&fragment.normal, XMVector3Normalize(cross));

    // The importer gives every vertex of a face its material's colour, so the three agree and the
    // first is the panel's colour rather than a corner of a gradient that does not exist. The sign
    // of the normal does not matter: the shader faces it towards the eye, as the scene pass does.
    fragment.colour = Lerp(_desc.tintColour, XMFLOAT3(a.r, a.g, a.b), _desc.tintMix);
    fragment.tumbler = static_cast<std::uint8_t>(_rng.Below(static_cast<std::uint32_t>(TUMBLER_COUNT)));
    m_fragments.push_back(fragment);
  }

  return dropped;
}

bool MeshShatter::Advance(float _dtSec)
{
  m_ageSec += _dtSec;

  for (int i = 0; i < TUMBLER_COUNT; ++i)
  {
    Tumbler& tumbler = m_tumblers[i];
    const XMMATRIX delta = XMMatrixRotationRollPitchYaw(tumbler.angVelRadPerSec.x * _dtSec, tumbler.angVelRadPerSec.y * _dtSec,
                                                        tumbler.angVelRadPerSec.z * _dtSec);
    // Post-multiplied: the step composes onto the tumble already accumulated, so a fragment keeps
    // turning about the axis it started on instead of precessing.
    XMStoreFloat3x3(&tumbler.rot, XMMatrixMultiply(XMLoadFloat3x3(&tumbler.rot), delta));

    const float rotDrag = 1.0f - _dtSec * m_desc.rotFrictionCoef;
    tumbler.angVelRadPerSec =
      XMFLOAT3(tumbler.angVelRadPerSec.x * rotDrag, tumbler.angVelRadPerSec.y * rotDrag, tumbler.angVelRadPerSec.z * rotDrag);
  }

  // Clamped at 1 so that a step long enough to remove all the speed removes exactly that and does
  // not turn the drag into a reverse thrust.
  //
  // The drag is a fraction of the speed per second and does not scale with the speed itself, which
  // is what the acceptance test in the work order (slice 2 7, "friction slows, and never reverses")
  // requires and what SpriteParticles does too. The design's pseudocode (5.3) has an extra |vel|
  // factor; with it, a fragment at 100 m/s and this friction stops dead in one second, which is
  // both the clamp case the same test pins separately and not a drag anyone would tune.
  const float drag = 1.0f - std::min(1.0f, m_desc.frictionCoef * _dtSec);
  const XMVECTOR gravityStep = XMVectorScale(XMLoadFloat3(&m_desc.gravityMetresPerSec2), _dtSec);

  for (Fragment& fragment : m_fragments)
  {
    const XMVECTOR vel = XMLoadFloat3(&fragment.velMetresPerSec);
    XMStoreFloat3(&fragment.pos, XMVectorAdd(XMLoadFloat3(&fragment.pos), XMVectorScale(vel, _dtSec)));
    XMStoreFloat3(&fragment.velMetresPerSec, XMVectorAdd(XMVectorScale(vel, drag), gravityStep));
  }

  return m_ageSec >= m_desc.lifetimeSec;
}

void MeshShatter::Build(std::vector<FxVertex>& _out) const
{
  const float alpha = std::clamp(1.0f - m_ageSec / m_desc.lifetimeSec, 0.0f, 1.0f);

  XMMATRIX rotations[TUMBLER_COUNT];
  for (int i = 0; i < TUMBLER_COUNT; ++i)
    rotations[i] = XMLoadFloat3x3(&m_tumblers[i].rot);

  // One reserve for the whole shatter; the loop below allocates nothing.
  _out.reserve(_out.size() + m_fragments.size() * 3);

  for (const Fragment& fragment : m_fragments)
  {
    const XMMATRIX& rot = rotations[fragment.tumbler];
    const XMVECTOR pos = XMLoadFloat3(&fragment.pos);

    XMFLOAT3 normal;
    XMStoreFloat3(&normal, XMVector3TransformNormal(XMLoadFloat3(&fragment.normal), rot));

    const XMFLOAT3* corners[3] = {&fragment.v0, &fragment.v1, &fragment.v2};
    constexpr float US[3] = {0.0f, 0.0f, 1.0f};
    constexpr float VS[3] = {0.0f, 1.0f, 1.0f};

    for (int i = 0; i < 3; ++i)
    {
      XMFLOAT3 world;
      XMStoreFloat3(&world, XMVectorAdd(XMVector3TransformNormal(XMLoadFloat3(corners[i]), rot), pos));

      _out.push_back(
        FxVertex::Make(world, normal, XMFLOAT4(fragment.colour.x, fragment.colour.y, fragment.colour.z, alpha), XMFLOAT2(US[i], VS[i])));
    }
  }
}
} // namespace Neuron
