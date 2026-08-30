#pragma once

namespace Neuron
{
// A Win32 HANDLE closed by CloseHandle, owned by a unique_ptr. Only handles of that family belong
// here: an NCrypt handle is a ULONG_PTR and closes with NCryptFreeObject, so DevCertificate.cpp
// carries its own closer rather than widening this one.
struct HandleCloser
{
  void operator()(HANDLE _handle) const noexcept
  {
    if (_handle)
      CloseHandle(_handle);
  }
};

using ScopedHandle = std::unique_ptr<void, HandleCloser>;

// CreateFile2 reports failure as INVALID_HANDLE_VALUE, which is not null and would be closed as if
// it were a handle. Folding it to null here is what lets a ScopedHandle be tested with `if`.
[[nodiscard]] inline HANDLE SafeHandle(HANDLE _handle) noexcept
{
  return (_handle == INVALID_HANDLE_VALUE) ? nullptr : _handle;
}
} // namespace Neuron
