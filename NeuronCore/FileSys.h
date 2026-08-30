#pragma once

namespace Neuron
{
using ByteBuffer = std::vector<std::uint8_t>;

class FileSys
{
public:
  static void SetHomeDirectory(const std::wstring& _path)
  {
    sm_homeDir = _path + L"\\Assets\\";
  }
  [[nodiscard]] static std::wstring GetHomeDirectory()
  {
    return sm_homeDir;
  }

  [[nodiscard]] static std::string GetHomeDirectoryA()
  {
    if (sm_homeDir.empty())
      return {};

    const int size =
      ::WideCharToMultiByte(CP_UTF8, 0, sm_homeDir.data(), static_cast<int>(sm_homeDir.size()), nullptr, 0, nullptr, nullptr);

    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, sm_homeDir.data(), static_cast<int>(sm_homeDir.size()), result.data(), size, nullptr, nullptr);

    return result;
  }

  // A name that already carries a drive or a root is taken as it stands; anything else is
  // relative to the home directory.
  [[nodiscard]] static std::wstring ResolvePath(const std::wstring& _fileName);

protected:
  // Set once by the composition root before any thread that reads a file exists (Outpost/Main.cpp),
  // and never written again -- which is the whole of its thread safety. A write after startup would
  // race every reader here, so if a second root ever needs to move it, it moves to that root's state
  // rather than gaining a lock: nothing in the tree wants this to change while the game is running.
  inline static std::wstring sm_homeDir;
};

class BinaryFile : public FileSys
{
public:
  [[nodiscard]] static ByteBuffer ReadFile(const std::wstring& _fileName);
  static bool WriteFile(const std::wstring& _fileName, const ByteBuffer& _data);
};

// Text on disk is UTF-8. The wide overloads convert; the narrow ones hand the bytes over as they
// stand, which is what a parser working in std::string wants.
class TextFile : public FileSys
{
public:
  [[nodiscard]] static std::wstring ReadFile(const std::wstring& _fileName);
  [[nodiscard]] static std::string ReadFileA(const std::wstring& _fileName);
  static bool WriteFile(const std::wstring& _fileName, std::wstring_view _text);
  static bool WriteFileA(const std::wstring& _fileName, std::string_view _text);
};
} // namespace Neuron
