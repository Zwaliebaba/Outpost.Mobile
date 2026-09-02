#pragma once

#include "MeshData.h"

#include <string>

namespace Neuron
{
// Neuron Mesh Object version 2.0, read into the triangle soup every consumer in this client already
// holds. Design/Archive/NmoFormat.md is the format; NmoFile.h is its on-disk layout; section 5.12 is the
// validation list this reader implements clause by clause, in order, before it uses a single byte.
//
// Every count and every offset in the file is arithmetic that ends in a pointer, so all of it is
// done in 64 bits and none of it is trusted until it has been proved to lie inside the window it
// claims. A file that fails any clause is rejected with the clause traced and _outMesh untouched:
// content errors are diagnostics, never crashes, so nothing here repairs, asserts or throws
// (AGENTS.md 5).
//
// What it consumes: geometry, material base colours and the RaceTinted flag, mesh extents, the
// named submeshes the geometry is made of with their own extents and marker runs, and markers with
// the bone each one is bound to.
//
// What it validates and then deliberately skips: normals, UVs, emissive, skin buffers, bone tables
// and clips -- nothing in this engine poses a bone yet, and no hull in the game has one to pose:
// every shipped mesh is named rigid submeshes with no skeleton at all
// (Design/Archive/Combat-slice-3.md 2.6). A file carrying a rig this reader ignored is better than one it
// refused, and the day a rigged hull is authored the bytes are already proved.
class NmoReader
{
public:
  // _dir is relative to the asset root unless it carries a drive or a root; FileSys resolves it.
  // _name carries no extension: ".nmo" is appended.
  [[nodiscard]] static bool Load(const std::wstring& _dir, const std::wstring& _name, MeshData& _outMesh);
};
} // namespace Neuron
