#include "pch.h"
#include "GlowBillboards.h"

using namespace DirectX;

namespace Neuron
{
void BuildGlowBillboards(std::span<const GlowSample> _samples, const XMFLOAT3& _cameraRight, const XMFLOAT3& _cameraUp,
                         std::vector<FxVertex>& _out)
{
  const XMVECTOR right = XMLoadFloat3(&_cameraRight);
  const XMVECTOR up = XMLoadFloat3(&_cameraUp);

  // One reserve for the upper bound; nothing in the loop allocates.
  _out.reserve(_out.size() + _samples.size() * GLOW_VERTS_PER_SAMPLE);

  // The quad's four corners, as multiples of the radius along the camera's right and up. The uv
  // each carries is the same pair, which is what makes the pixel shader's length(uv) the distance
  // from the centre in radii -- 0 at the middle, 1 at an edge, and past 1 in the corners, where the
  // shader clips. That is the disc DecalPS drew inscribed in the quad it was given.
  constexpr float CORNER_X[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
  constexpr float CORNER_Y[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
  constexpr int TRIANGLES[GLOW_VERTS_PER_SAMPLE] = {0, 1, 2, 0, 2, 3};

  for (const GlowSample& sample : _samples)
  {
    if (sample.radiusMetres <= 0.001f || sample.colour.a <= 0.001f)
      continue;

    const XMVECTOR centre = XMLoadFloat3(&sample.posWorld);
    const XMVECTOR alongRight = XMVectorScale(right, sample.radiusMetres);
    const XMVECTOR alongUp = XMVectorScale(up, sample.radiusMetres);

    XMFLOAT3 corners[4];
    for (int corner = 0; corner < 4; ++corner)
    {
      const XMVECTOR offset = XMVectorAdd(XMVectorScale(alongRight, CORNER_X[corner]), XMVectorScale(alongUp, CORNER_Y[corner]));
      XMStoreFloat3(&corners[corner], XMVectorAdd(centre, offset));
    }

    // The normal is zero and the glow shader never reads it, exactly as a sprite does: carrying the
    // field is what lets one input layout and one ring serve every effect pass (FxVertex.h).
    const XMFLOAT3 noNormal(0.0f, 0.0f, 0.0f);
    const XMFLOAT4 colour(sample.colour.r, sample.colour.g, sample.colour.b, sample.colour.a);
    for (int at = 0; at < static_cast<int>(GLOW_VERTS_PER_SAMPLE); ++at)
    {
      const int corner = TRIANGLES[at];
      _out.push_back(FxVertex::Make(corners[corner], noNormal, colour, XMFLOAT2(CORNER_X[corner], CORNER_Y[corner])));
    }
  }
}
} // namespace Neuron
