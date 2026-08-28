#pragma once

#include "GpuDevice.h"
#include "RenderTypes.h"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace Neuron
{
// One picture the overlay pass can draw whole: a HUD icon, a logo, a cursor. Like a font atlas it
// is a coverage mask and not colour -- the pixel shader multiplies it by the vertex colour -- so
// one file serves every tint the HUD draws it in, and it queues in the same pipeline as the text
// around it rather than needing a second one kept in step.
class ScreenImage
{
public:
  // Reads _fileName through FileSys, records the upload into the device's command list, and writes
  // the shader resource view to _srv. Reports false and traces on a file that cannot be read: a
  // missing icon is a diagnostic, and whatever is queued on it simply does not draw.
  [[nodiscard]] bool Load(GpuDevice& _gpu, const std::wstring& _fileName, D3D12_CPU_DESCRIPTOR_HANDLE _srv);

  // Releases the staging buffer, which has to outlive Load because the copy has only been recorded.
  void DiscardStaging() noexcept
  {
    m_staging = nullptr;
  }

  [[nodiscard]] bool Ready() const noexcept
  {
    return m_texture.get() != nullptr;
  }
  [[nodiscard]] std::uint32_t WidthPx() const noexcept
  {
    return m_widthPx;
  }
  [[nodiscard]] std::uint32_t HeightPx() const noexcept
  {
    return m_heightPx;
  }

private:
  GpuPtr<ID3D12Resource> m_texture;
  GpuPtr<ID3D12Resource> m_staging;
  std::uint32_t m_widthPx = 0;
  std::uint32_t m_heightPx = 0;
};
} // namespace Neuron
