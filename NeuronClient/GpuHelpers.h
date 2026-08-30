#pragma once

#include "RenderTypes.h"

#include "DdsImage.h"
#include "FileSys.h"

#include <d3d12.h>

#include <cstdint>
#include <span>

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

// The overlay pass draws coverage, not colour: a font atlas and a HUD icon are both one channel the
// pixel shader multiplies by the vertex colour. Coverage comes from the alpha channel where the
// file has one and from the luminance where it does not, so a mask authored white-on-black reads
// the same as one authored white-on-transparent -- assuming either turns the other into a solid
// block of ink. Reports false and traces on a surface that cannot be read on the CPU.
[[nodiscard]] bool CoverageOf(const DdsImage& _image, ByteBuffer& _outCoverage);

// Records the upload of one R8 texture into the device's command list and writes its view to _srv.
// _outStaging has to outlive the copy, which has only been recorded: release it after the list has
// run, never before.
class GpuDevice;
void UploadCoverageTexture(GpuDevice& _gpu, std::uint32_t _widthPx, std::uint32_t _heightPx, const ByteBuffer& _coverage,
                           D3D12_CPU_DESCRIPTOR_HANDLE _srv, GpuPtr<ID3D12Resource>& _outTexture, GpuPtr<ID3D12Resource>& _outStaging);

// The same, for one BGRA8 texture: _pixels is four bytes a texel, B G R A, rows tightly packed --
// what DdsImage::TopMipAsBgra hands back. Deliberately a second function rather than a format
// parameter on the one above: the coverage upload exists because the overlay draws coverage and
// says so at its call site, and this one exists because a sprite is colour. Two names, two intents.
void UploadColourTexture(GpuDevice& _gpu, std::uint32_t _widthPx, std::uint32_t _heightPx, const ByteBuffer& _pixels,
                         D3D12_CPU_DESCRIPTOR_HANDLE _srv, GpuPtr<ID3D12Resource>& _outTexture, GpuPtr<ID3D12Resource>& _outStaging);

// Records the copy of _bytes into a new DEFAULT-heap buffer, on the COPY queue: bracket it with
// GpuDevice::BeginCopies and SubmitCopies, not with BeginUploads. There is no transition to
// VERTEX_AND_CONSTANT_BUFFER any more and there must not be -- the buffer decays to COMMON when the
// copy queue's submission completes and the first draw promotes it out again for free, and a COPY
// list cannot express that barrier in any case (ADR 0044).
//
// _outStaging has to outlive the copy, which has only been recorded: release it once
// GpuDevice::CompletedCopyFence has reached the LastCopyFence of the batch, never before.
//
// This is the path for a buffer the GPU reads every frame and the CPU never touches again. A planet
// is seven megabytes the input assembler reads twice a frame, and in an upload heap that is system
// memory pulled across PCIe at every draw (Design/Archive/PlanetRenderer.md 7.1).
//
// `SceneRenderer::UploadMesh` used to be the counter-example and is now a caller. The argument that
// kept it on the upload-heap shortcut -- a few thousand triangles do not justify a staging copy --
// weighed the upload and not the reading, and it held only for the fleet it was written against.
// The hulls in this tree average 32 kB: a hundred ships is 0.3 GB/s of vertex fetch and the
// argument stands, five hundred is 1.4 and two thousand is 5.6, which does not
// (Design/MmoScalabilityReview.md G2).
void UploadStaticBuffer(GpuDevice& _gpu, std::span<const std::uint8_t> _bytes, GpuPtr<ID3D12Resource>& _outBuffer,
                        GpuPtr<ID3D12Resource>& _outStaging);
} // namespace Neuron
