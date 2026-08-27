#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace Neuron
{
// One vertex format for everything the scene pass draws. No normal: the pixel shader takes the face
// normal from screen-space derivatives, which is exactly the flat shading this game wants, and one
// format means one input layout and one pipeline for hulls, ground and decals alike.
struct MeshVertex
{
  float px, py, pz;
  float r, g, b;
};

// A mesh as it comes off disk: triangle soup, its bounds, and whatever attachment points the
// importer recovered. Deliberately free of graphics API types, so a loader, a test and a tool can
// all hold one without a device.
struct MeshData
{
  std::vector<MeshVertex> verts;
  DirectX::XMFLOAT3 boundsMin{0.0f, 0.0f, 0.0f};
  DirectX::XMFLOAT3 boundsMax{0.0f, 0.0f, 0.0f};

  // One centroid per cluster of faces carrying the attachment material — an exhaust nozzle on a
  // hull, a hardpoint later. A hull carries anywhere from one to a dozen.
  std::vector<DirectX::XMFLOAT3> attachPoints;

  [[nodiscard]] DirectX::XMFLOAT3 BoundsCentre() const noexcept
  {
    return DirectX::XMFLOAT3((boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f,
                             (boundsMin.z + boundsMax.z) * 0.5f);
  }

  // Never zero on any axis: a flat mesh would otherwise pick as an infinitely thin slab.
  [[nodiscard]] DirectX::XMFLOAT3 HalfExtents() const noexcept
  {
    const float x = (boundsMax.x - boundsMin.x) * 0.5f;
    const float y = (boundsMax.y - boundsMin.y) * 0.5f;
    const float z = (boundsMax.z - boundsMin.z) * 0.5f;
    return DirectX::XMFLOAT3(x > 0.5f ? x : 0.5f, y > 0.5f ? y : 0.5f, z > 0.5f ? z : 0.5f);
  }

  // How far the mesh has to be lifted for its lowest vertex to rest on y = 0.
  [[nodiscard]] float RestY() const noexcept { return -boundsMin.y; }

  [[nodiscard]] bool Empty() const noexcept { return verts.empty(); }
};
} // namespace Neuron
