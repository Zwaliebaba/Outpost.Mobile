#pragma once

#include "FxVertex.h"
#include "RenderTypes.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Neuron
{
// Camera-facing glow quads, built on the CPU into the effect's vertex ring.
//
// A glow used to be one draw each: SceneRenderer::DrawGlow put the billboard's matrix in a root
// constant, so N glows were N draws. A thruster trail is one glow per sample per nozzle, which put
// a bomber at up to 96 draws and a hundred of them past nine thousand
// (Design/Archive/MmoScalabilityReview.md G1). Built here instead, the whole frame's glows are one draw.
//
// The shape is still decided per pixel, by FxGlowPS, and it is DecalPS's glow arithmetic unchanged
// -- what moved is where the quad's local coordinate comes from. DecalVS derived it from the unit
// quad it was drawing; here it is written into the vertex, because there is no per-glow matrix any
// more and no unit quad to derive it from.
struct GlowSample
{
  DirectX::XMFLOAT3 posWorld;
  float radiusMetres = 0.0f; // the glow's half-extent: it fills a quad 2 x radius on a side
  Rgba colour;
};

// Six vertices, two triangles, one quad. Spelled here because it is what a caller sizing a reserve
// or checking headroom against FxRenderer::MAX_FX_VERTS needs, and a caller should not count it.
inline constexpr std::uint32_t GLOW_VERTS_PER_SAMPLE = 6;

// Appends six vertices per sample to _out, in world space, facing the camera.
//
// The corners are centre +- right * radius +- up * radius, and each carries its own corner in uv as
// (+-1, +-1) -- exactly the range DecalVS's `i.pos.xz * 2.0` produced over the unit quad, so the
// falloff the pixel shader computes from it is the falloff that shipped.
//
// A sample with no radius or no alpha contributes nothing: the old path returned early on both
// (SceneRenderer::DrawGlow's own guard), and dropping them here keeps that and keeps the ring clear
// of quads that would draw nothing.
void BuildGlowBillboards(std::span<const GlowSample> _samples, const DirectX::XMFLOAT3& _cameraRight, const DirectX::XMFLOAT3& _cameraUp,
                         std::vector<FxVertex>& _out);
} // namespace Neuron
