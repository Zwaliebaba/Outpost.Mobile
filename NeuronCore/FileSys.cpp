#include "pch.h"
#include "FileSys.h"

namespace
{
bool IsAbsolute(const std::wstring& _path)
{
  if (_path.size() >= 2 && _path[1] == L':')
    return true;

  return !_path.empty() && (_path[0] == L'\\' || _path[0] == L'/');
}

// Shared for writing too: a file the user is editing must not fail the read.
ByteBuffer ReadAllBytes(const std::wstring& _fullName)
{
  ScopedHandle file(SafeHandle(CreateFile2(_fullName.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, nullptr)));
  if (!file)
    return {};

  // Get the file size.
  FILE_STANDARD_INFO fileInfo;
  if (!GetFileInformationByHandleEx(file.get(), FileStandardInfo, &fileInfo, sizeof(fileInfo)))
    return {};

  // File is too big for 32-bit allocation, so reject read.
  if (fileInfo.EndOfFile.HighPart > 0)
    return {};

  // Create enough space for the file data.
  ByteBuffer data(fileInfo.EndOfFile.LowPart);

  // Read the data in.
  DWORD bytesRead = 0;

  if (!::ReadFile(file.get(), data.data(), fileInfo.EndOfFile.LowPart, &bytesRead, nullptr))
    return {};

  if (bytesRead < fileInfo.EndOfFile.LowPart)
    return {};

  return data;
}

bool WriteAllBytes(const std::wstring& _fullName, const void* _data, std::size_t _size, bool _flush = false)
{
  ScopedHandle file(SafeHandle(CreateFile2(_fullName.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr)));
  if (!file)
    return false;

  DWORD bytesWritten = 0;
  if (!::WriteFile(file.get(), _data, static_cast<DWORD>(_size), &bytesWritten, nullptr))
    return false;

  if (bytesWritten != static_cast<DWORD>(_size))
    return false;

  // Only the atomic path asks for this, and it is the half of "atomic" that the rename cannot
  // provide: a rename orders the DIRECTORY entry, not the file's contents, so without this a crash
  // just after the rename can leave the new name pointing at a file whose bytes never reached the
  // disk -- which is precisely the half-written save the temporary was there to prevent.
  return !_flush || ::FlushFileBuffers(file.get()) != FALSE;
}

std::wstring Utf8ToWide(std::string_view _text)
{
  if (_text.empty())
    return {};

  const int size = ::MultiByteToWideChar(CP_UTF8, 0, _text.data(), static_cast<int>(_text.size()), nullptr, 0);
  if (size <= 0)
    return {};

  std::wstring result(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, _text.data(), static_cast<int>(_text.size()), result.data(), size);

  return result;
}

std::string WideToUtf8(std::wstring_view _text)
{
  if (_text.empty())
    return {};

  const int size = ::WideCharToMultiByte(CP_UTF8, 0, _text.data(), static_cast<int>(_text.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0)
    return {};

  std::string result(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, _text.data(), static_cast<int>(_text.size()), result.data(), size, nullptr, nullptr);

  return result;
}
} // namespace

namespace Neuron
{
std::wstring FileSys::ResolvePath(const std::wstring& _fileName)
{
  if (IsAbsolute(_fileName))
    return _fileName;

  return sm_homeDir + _fileName;
}

ByteBuffer BinaryFile::ReadFile(const std::wstring& _fileName)
{
  return ReadAllBytes(ResolvePath(_fileName));
}

bool BinaryFile::WriteFile(const std::wstring& _fileName, const ByteBuffer& _data)
{
  return WriteAllBytes(ResolvePath(_fileName), _data.data(), _data.size());
}

bool FileSys::Exists(const std::wstring& _fileName)
{
  // The attributes rather than an open, so a file somebody else holds open still answers yes: the
  // question is whether the name is taken, not whether this process may read it this instant.
  return ::GetFileAttributesW(ResolvePath(_fileName).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool BinaryFile::WriteFileAtomic(const std::wstring& _fileName, const ByteBuffer& _data)
{
  const std::wstring target = ResolvePath(_fileName);

  // A sibling, so the rename below is within one volume -- MoveFileEx is only atomic there, and a
  // temporary in the system temp directory would silently become a copy-then-delete across volumes.
  const std::wstring temporary = target + L".tmp";

  if (!WriteAllBytes(temporary, _data.data(), _data.size(), true))
  {
    // A temporary nobody will ever finish is rubbish beside the real file, and the next attempt
    // overwrites it anyway; removing it keeps a failed write from looking like a pending one.
    ::DeleteFileW(temporary.c_str());
    return false;
  }

  // The one step that makes this worth doing: the target either names the old file or the new one,
  // and never a partial one, because a rename over an existing name is a single directory
  // operation. WRITE_THROUGH holds the call until that operation itself is on the disk.
  if (::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
  {
    ::DeleteFileW(temporary.c_str());
    return false;
  }

  return true;
}

std::wstring TextFile::ReadFile(const std::wstring& _fileName)
{
  return Utf8ToWide(ReadFileA(_fileName));
}

std::string TextFile::ReadFileA(const std::wstring& _fileName)
{
  const ByteBuffer data = ReadAllBytes(ResolvePath(_fileName));

  return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

bool TextFile::WriteFile(const std::wstring& _fileName, std::wstring_view _text)
{
  return WriteFileA(_fileName, WideToUtf8(_text));
}

bool TextFile::WriteFileA(const std::wstring& _fileName, std::string_view _text)
{
  return WriteAllBytes(ResolvePath(_fileName), _text.data(), _text.size());
}
} // namespace Neuron
