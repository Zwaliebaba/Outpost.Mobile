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
  byte_buffer_t ReadAllBytes(const std::wstring& _fullName)
  {
    ScopedHandle hFile(safe_handle(CreateFile2(_fullName.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, nullptr)));
    if (!hFile)
      return {};

    // Get the file size.
    FILE_STANDARD_INFO fileInfo;
    if (!GetFileInformationByHandleEx(hFile.get(), FileStandardInfo, &fileInfo, sizeof(fileInfo)))
      return {};

    // File is too big for 32-bit allocation, so reject read.
    if (fileInfo.EndOfFile.HighPart > 0)
      return {};

    // Create enough space for the file data.
    byte_buffer_t data(fileInfo.EndOfFile.LowPart);

    // Read the data in.
    DWORD bytesRead = 0;

    if (!::ReadFile(hFile.get(), data.data(), fileInfo.EndOfFile.LowPart, &bytesRead, nullptr))
      return {};

    if (bytesRead < fileInfo.EndOfFile.LowPart)
      return {};

    return data;
  }

  bool WriteAllBytes(const std::wstring& _fullName, const void* _data, size_t _size)
  {
    ScopedHandle hFile(safe_handle(CreateFile2(_fullName.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr)));
    if (!hFile)
      return false;

    DWORD bytesWritten = 0;
    if (!::WriteFile(hFile.get(), _data, static_cast<DWORD>(_size), &bytesWritten, nullptr))
      return false;

    return bytesWritten == static_cast<DWORD>(_size);
  }

  std::wstring Utf8ToWide(std::string_view _text)
  {
    if (_text.empty())
      return {};

    const int size = ::MultiByteToWideChar(CP_UTF8, 0, _text.data(), static_cast<int>(_text.size()), nullptr, 0);
    if (size <= 0)
      return {};

    std::wstring result(static_cast<size_t>(size), L'\0');
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

    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, _text.data(), static_cast<int>(_text.size()), result.data(), size, nullptr, nullptr);

    return result;
  }
}

std::wstring FileSys::ResolvePath(const std::wstring& _fileName)
{
  if (IsAbsolute(_fileName))
    return _fileName;

  return m_homeDir + _fileName;
}

byte_buffer_t BinaryFile::ReadFile(const std::wstring& _fileName)
{
  return ReadAllBytes(ResolvePath(_fileName));
}

bool BinaryFile::WriteFile(const std::wstring& _fileName, const byte_buffer_t& _data)
{
  return WriteAllBytes(ResolvePath(_fileName), _data.data(), _data.size());
}

std::wstring TextFile::ReadFile(const std::wstring& _fileName)
{
  return Utf8ToWide(ReadFileA(_fileName));
}

std::string TextFile::ReadFileA(const std::wstring& _fileName)
{
  const byte_buffer_t data = ReadAllBytes(ResolvePath(_fileName));

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
