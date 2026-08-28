#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <DirectXMath.h>

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
  Rgba gridColour; // a = how far a grid line pulls away from the ground colour
  float gridSpacing;
  float gridLineWidthPx;
  float gridFadeDistance;
  DirectX::XMFLOAT3 cameraPos;
};

struct TextVertex
{
  float xPx, yPx;
  float u, v;
  Rgba colour;
};
} // namespace Neuron
