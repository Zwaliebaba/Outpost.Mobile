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

  // Whether there is a file at that name at all.
  //
  // It exists because a reader cannot tell "there is no file" from "there is a file I could not
  // read" -- ReadFile answers both with an empty buffer, and for content that is the right answer,
  // since a missing texture and an unreadable one are equally missing. For a save it is not: absent
  // means start a new universe and unreadable must NOT, so the caller has to be able to ask the two
  // questions separately (Design/Archive/Universe-slice-5.md 7, ADR 0057).
  [[nodiscard]] static bool Exists(const std::wstring& _fileName);

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

  // The same write, except that the file at _fileName is only ever the whole of one version or the
  // whole of the previous one. The bytes go to a sibling temporary, are flushed to the disk, and
  // the temporary is then renamed over the target -- one directory operation, so an interruption
  // anywhere in it leaves the old file intact rather than half of the new one.
  //
  // Here rather than at the one caller because there is nothing game-shaped about it: "replace a
  // file without ever being able to lose both versions" is a filesystem primitive, and the day a
  // second thing is worth not corrupting it is already written (AGENTS.md 2).
  //
  // It is crash-atomic, not concurrency-atomic. Two writers racing on one name is not a case this
  // tree has, and the rename would settle it arbitrarily rather than safely.
  static bool WriteFileAtomic(const std::wstring& _fileName, const ByteBuffer& _data);
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
