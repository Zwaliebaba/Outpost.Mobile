#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace Outpost
{
// The last few things worth telling the player, newest first. A fixed ring of fixed-width rows,
// because it is read on every frame by a draw path that must not allocate, and because a log that
// could grow without bound is a log nobody prunes.
//
// Presentation state: nothing here is simulation, and the simulation never writes to it. The view
// pushes what the player did, the composition root pushes what the game did, and the HUD reads.
class EventLog
{
public:
  enum class Severity : std::uint8_t
  {
    Info,     // grey
    Friendly, // green
    Alert     // amber: orders and alerts
  };

  struct Entry
  {
    Severity severity = Severity::Info;
    float timeSec = 0.0f; // simulation time, so a replay reads the same clock
    char text[64] = {};
  };

  static constexpr int CAPACITY = 8;

  void Push(Severity _severity, float _timeSec, const char* _text) noexcept
  {
    Entry& entry = m_entries[m_head];
    entry.severity = _severity;
    entry.timeSec = _timeSec;
    std::snprintf(entry.text, sizeof(entry.text), "%s", _text); // truncates rather than overruns
    m_head = (m_head + 1) % CAPACITY;
    m_count = std::min(m_count + 1, CAPACITY);
  }

  template <class... Ts> void PushFormat(Severity _severity, float _timeSec, const char* _fmt, Ts... _args) noexcept
  {
    char text[sizeof(Entry::text)] = {};
    std::snprintf(text, sizeof(text), _fmt, _args...);
    Push(_severity, _timeSec, text);
  }

  [[nodiscard]] int Count() const noexcept
  {
    return m_count;
  }

  // _back is 0 for the newest entry, 1 for the one before it, and so on up to Count() - 1.
  [[nodiscard]] const Entry& Newest(int _back) const noexcept
  {
    const int index = ((m_head - 1 - _back) % CAPACITY + CAPACITY) % CAPACITY;
    return m_entries[index];
  }

private:
  Entry m_entries[CAPACITY];
  int m_head = 0;
  int m_count = 0;
};
} // namespace Outpost
