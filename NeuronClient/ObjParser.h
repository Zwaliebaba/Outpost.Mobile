#pragma once

#include "MeshData.h"

#include <string>

namespace Neuron
{
// Wavefront OBJ/MTL, hand-written, tolerant. Content errors are diagnostics, never crashes: a
// malformed face is counted and skipped, a missing material falls back to grey, and a file that
// cannot be read reports false rather than throwing.
//
// The importer negates Z. OBJ is right-handed and these hulls point their bow at -Z; negating
// converts to the left-handed render basis (east, up, north) and lands the bow on +Z in one step.
class ObjParser
{
public:
  // The material name whose faces are clustered into MeshData::attachPoints.
  static constexpr const char* ATTACH_MATERIAL = "thruster";

  // _dir is relative to the asset root unless it carries a drive or a root; FileSys resolves it.
  // _name carries no extension: ".obj" and ".mtl" are appended.
  [[nodiscard]] static bool Load(const std::wstring& _dir, const std::wstring& _name, MeshData& _outMesh);
};
} // namespace Neuron
