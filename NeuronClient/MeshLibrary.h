#pragma once

#include "SceneRenderer.h"

#include "MeshData.h"

#include <string>
#include <vector>

namespace Neuron
{
// Loads meshes once and keeps what the rest of the client needs to know about them: the GPU handle
// to draw, and the CPU-side bounds and attach points to pick and to place effects with.
//
// Kept out of the renderer because the renderer's job ends at "draw this handle", and out of the
// game because a mesh is not a game concept. It is the seam where content becomes something both
// can use.
class MeshLibrary
{
public:
  struct Entry
  {
    MeshHandle mesh = INVALID_MESH;
    MeshData data;
  };

  // Loads _dir/_name.obj (and its .mtl) and uploads it. Returns INVALID_MESH and logs on a mesh
  // that cannot be read or is empty -- a missing asset is a diagnostic, not a crash.
  MeshHandle Load(GpuDevice& _gpu, SceneRenderer& _renderer, const std::wstring& _dir, const std::wstring& _name);

  [[nodiscard]] const MeshData& Data(MeshHandle _mesh) const;
  [[nodiscard]] bool Has(MeshHandle _mesh) const noexcept;
  [[nodiscard]] std::uint32_t Count() const noexcept { return static_cast<std::uint32_t>(m_entries.size()); }
  [[nodiscard]] const Entry& At(std::uint32_t _index) const { return m_entries[_index]; }

private:
  std::vector<Entry> m_entries;
  MeshData m_empty;
};
} // namespace Neuron
