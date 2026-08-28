#include "pch.h"
#include "ScreenImage.h"

#include "GpuHelpers.h"

namespace Neuron
{
bool ScreenImage::Load(GpuDevice& _gpu, const std::wstring& _fileName, D3D12_CPU_DESCRIPTOR_HANDLE _srv)
{
  DdsImage image;
  if (!DdsImage::Load(_fileName, image))
    return false;

  ByteBuffer coverage;
  if (!CoverageOf(image, coverage))
  {
    DebugTrace(L"image {} is not an uncompressed 8-bit surface\n", _fileName);
    return false;
  }

  UploadCoverageTexture(_gpu, image.widthPx, image.heightPx, coverage, _srv, m_texture, m_staging);
  m_widthPx = image.widthPx;
  m_heightPx = image.heightPx;
  return true;
}
} // namespace Neuron
