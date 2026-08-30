#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>

namespace Neuron
{
// The swapchain and depth formats every pipeline in this renderer is built against. They are here
// rather than in GpuDevice because a pipeline state has to name them and a pipeline does not
// otherwise need the device object.
inline constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;

// COM lifetimes are RAII through C++/WinRT, never raw Release() calls and never WRL's ComPtr.
// Create with Thing(IID_PPV_ARGS(thing.put())); query with thing.try_as<IOther>(), which returns
// null on failure. put() asserts the pointer is empty rather than silently releasing what was
// there, so release before refilling.
template <typename T> using GpuPtr = winrt::com_ptr<T>;

struct Rgba
{
  float r, g, b, a;
};

// A mesh that has been uploaded and can be drawn. Handed out by index rather than by pointer, so
// the renderer can rehouse its buffers without invalidating anything the game is holding.
// One ship in an instanced draw: where it is and what colour it takes. Both were root constants
// before, which is exactly why every ship needed its own draw (Design/Archive/MmoScalabilityReview.md G2).
//
// The layout is the input layout: four rows of the world matrix and a tint, five R32G32B32A32_FLOAT
// elements at input slot 1. Scene.hlsli's VsInstance spells the same thing from the other side, and
// the static_assert below is what stops the two drifting.
struct MeshInstance
{
  DirectX::XMFLOAT4X4 world;
  // rgb is the flying faction's livery, multiplied into the surfaces the model declared RaceTinted
  // and into nothing else; w is a highlight lift in 0..1 applied after lighting. It carried a base
  // colour and a material mix before liveries, which is exactly the kind of drift a comment is for:
  // the shape did not change and the meaning did (Design/Archive/NmoFormat.md 5.5, Design/Decisions/0036).
  float tint[4];
};
static_assert(sizeof(MeshInstance) == 80, "MeshInstance is padded; the instanced input layout's offsets are wrong");
static_assert(offsetof(MeshInstance, tint) == 64, "MeshInstance::tint moved; the instanced input layout spells 64");

struct GpuMesh
{
  GpuPtr<ID3D12Resource> vb;
  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  std::uint32_t vertexCount = 0;
};

using MeshHandle = std::uint32_t;
inline constexpr MeshHandle INVALID_MESH = 0xFFFFFFFFu;

// Set once per frame, before any DrawMesh.
struct SceneFrame
{
  DirectX::XMFLOAT4X4 viewProj;
  DirectX::XMFLOAT3 lightDir; // towards the light; normalised in the shader
  float ambient;
  DirectX::XMFLOAT3 cameraPos;
};

struct TextVertex
{
  float xPx, yPx;
  float u, v;
  Rgba colour;
};
} // namespace Neuron
