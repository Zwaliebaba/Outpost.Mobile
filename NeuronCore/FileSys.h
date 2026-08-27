#pragma once

namespace Neuron
{
  using ByteBuffer = std::vector<uint8_t>;

  class FileSys
  {
    public:
      static void SetHomeDirectory(const std::wstring& _path) { m_homeDir = _path + L"\\Assets\\"; }
      [[nodiscard]] static std::wstring GetHomeDirectory() { return m_homeDir; }

      [[nodiscard]] static std::string GetHomeDirectoryA()
      {
          if (m_homeDir.empty())
              return {};

          const int size = ::WideCharToMultiByte(
              CP_UTF8, 0,
              m_homeDir.data(), static_cast<int>(m_homeDir.size()),
              nullptr, 0, nullptr, nullptr);

          std::string result(static_cast<size_t>(size), '\0');
          ::WideCharToMultiByte(
              CP_UTF8, 0,
              m_homeDir.data(), static_cast<int>(m_homeDir.size()),
              result.data(), size, nullptr, nullptr);

          return result;
      }

      // A name that already carries a drive or a root is taken as it stands; anything else is
      // relative to the home directory.
      [[nodiscard]] static std::wstring ResolvePath(const std::wstring& _fileName);

    protected:
      inline static std::wstring m_homeDir;
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
}
