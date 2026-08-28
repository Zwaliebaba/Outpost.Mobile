#pragma once

#include "RenderTypes.h"

#include <d3d12.h>

namespace Neuron
{
// The D3D12 descriptor boilerplate every pipeline in the client needs, in one place rather than
// re-typed per file. Deliberately not a wrapper layer: these return the SDK's own structs and the
// call sites go on using the SDK's API directly.

[[nodiscard]] D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE _type) noexcept;
[[nodiscard]] D3D12_RESOURCE_DESC BufferDesc(std::uint64_t _bytes) noexcept;
[[nodiscard]] D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* _resource, D3D12_RESOURCE_STATES _before,
                                                D3D12_RESOURCE_STATES _after) noexcept;

// Serialises and creates a root signature, reporting the validation message on failure.
[[nodiscard]] GpuPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* _device, const D3D12_ROOT_SIGNATURE_DESC& _desc,
                                                              const char* _what);

// A pipeline state description filled in with the settings every pass in this renderer shares --
// solid fill, no culling, one render target in the back-buffer format, no multisampling -- so each
// pass only spells the handful of fields where it actually differs.
[[nodiscard]] D3D12_GRAPHICS_PIPELINE_STATE_DESC DefaultPipelineDesc() noexcept;
} // namespace Neuron
