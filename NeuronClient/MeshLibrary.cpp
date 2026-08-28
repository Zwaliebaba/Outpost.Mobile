#include "pch.h"
#include "MeshLibrary.h"

namespace Neuron
{
MeshHandle MeshLibrary::Load(GpuDevice& _gpu, SceneRenderer& _renderer, const std::wstring& _dir, const std::wstring& _name)
{
  Entry entry;
  if (!ObjParser::Load(_dir, _name, entry.data))
  {
    DebugTrace(L"mesh {} could not be loaded\n", _name);
    return INVALID_MESH;
  }

  entry.mesh = _renderer.UploadMesh(_gpu, entry.data.verts);
  m_entries.push_back(std::move(entry));
  return m_entries.back().mesh;
}

bool MeshLibrary::Has(MeshHandle _mesh) const noexcept
{
  for (const Entry& entry : m_entries)
  {
    if (entry.mesh == _mesh)
      return true;
  }
  return false;
}

const MeshData& MeshLibrary::Data(MeshHandle _mesh) const
{
  for (const Entry& entry : m_entries)
  {
    if (entry.mesh == _mesh)
      return entry.data;
  }
  return m_empty;
}
} // namespace Neuron
