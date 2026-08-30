#include "pch.h"
#include "ServerConfig.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace Outpost
{
namespace
{
// Space and tab only. Not std::isspace, which is locale-dependent and takes an int whose negative
// values are undefined for a signed char -- neither of which a configuration file should be able to
// reach. A carriage return is handled where the line is cut, so a CRLF file reads the same as an LF
// one without every trim having to know about it.
[[nodiscard]] constexpr bool IsBlank(char _c) noexcept
{
  return _c == ' ' || _c == '\t' || _c == '\r';
}

[[nodiscard]] std::string_view Trim(std::string_view _text) noexcept
{
  std::size_t first = 0;
  while (first < _text.size() && IsBlank(_text[first]))
    ++first;
  std::size_t last = _text.size();
  while (last > first && IsBlank(_text[last - 1]))
    --last;
  return _text.substr(first, last - first);
}

// The message every refusal is built from, so a reader gets the line number, the key if there was
// one, and what was actually wrong -- rather than "bad config", which sends them to read the parser.
[[nodiscard]] std::string Refuse(std::size_t _line, std::string_view _what, std::string_view _detail)
{
  std::string message = "Server.cfg line " + std::to_string(_line) + ": " + std::string(_what);
  if (!_detail.empty())
    message += " '" + std::string(_detail) + "'";
  return message;
}

// from_chars, not stoul or atoi: it does not throw, does not read a locale, does not accept a
// leading '+' or '-' for an unsigned type, and reports where it stopped -- which is what catches
// `port = 30081x` rather than silently taking the 30081.
[[nodiscard]] bool ReadUnsigned(std::string_view _value, std::uint64_t& _out) noexcept
{
  if (_value.empty())
    return false;
  const char* const begin = _value.data();
  const char* const end = begin + _value.size();
  const std::from_chars_result result = std::from_chars(begin, end, _out);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool ReadFloat(std::string_view _value, float& _out) noexcept
{
  if (_value.empty())
    return false;
  const char* const begin = _value.data();
  const char* const end = begin + _value.size();
  const std::from_chars_result result = std::from_chars(begin, end, _out);
  if (result.ec != std::errc{} || result.ptr != end)
    return false;
  // Rejected here rather than by a range test at the call site, because "not a number" and "outside
  // the range" are different messages and infinity would pass a `> 0` check.
  return _out == _out && _out > -3.0e38f && _out < 3.0e38f;
}
} // namespace

bool ParseServerConfig(std::string_view _text, ServerConfig& _outConfig, std::string& _outError)
{
  // Parsed into a local and copied out only on success. That is the whole of "fails closed": a
  // refusal on line nine cannot leave lines one to eight applied, so there is no state in which half
  // the file is in force and nothing says which half.
  ServerConfig parsed;
  _outError.clear();

  // One flag per key, so a duplicate is a refusal rather than a last-one-wins. Two lines setting the
  // same key means the author believes one of them is in force and has even odds of being right.
  bool seenPort = false;
  bool seenBacklog = false;
  bool seenShard = false;
  bool seenRadius = false;
  bool seenUpdateTicks = false;
  bool seenOrders = false;

  std::size_t line = 0;
  std::size_t at = 0;
  while (at <= _text.size())
  {
    const std::size_t newline = _text.find('\n', at);
    const std::string_view raw = _text.substr(at, (newline == std::string_view::npos) ? std::string_view::npos : newline - at);
    at = (newline == std::string_view::npos) ? _text.size() + 1 : newline + 1;
    ++line;

    const std::string_view content = Trim(raw);
    if (content.empty() || content.front() == '#')
      continue;

    const std::size_t equals = content.find('=');
    if (equals == std::string_view::npos)
    {
      _outError = Refuse(line, "expected key = value, found", content);
      return false;
    }

    const std::string_view key = Trim(content.substr(0, equals));
    const std::string_view value = Trim(content.substr(equals + 1));
    if (key.empty())
    {
      _outError = Refuse(line, "a value with no key", value);
      return false;
    }
    if (value.empty())
    {
      _outError = Refuse(line, "no value for key", key);
      return false;
    }

    // A key this reader does not know is a refusal and not a shrug. Ignoring it turns `prot = 40000`
    // into a silent revert to the default, which looks exactly like the file working
    // (Design/Archive/ServerConfig-work-order.md 2.2).
    std::uint64_t whole = 0;
    if (key == "port")
    {
      if (seenPort)
      {
        _outError = Refuse(line, "key set twice:", key);
        return false;
      }
      if (!ReadUnsigned(value, whole) || whole > 65535)
      {
        _outError = Refuse(line, "port must be 0 to 65535, found", value);
        return false;
      }
      parsed.port = static_cast<std::uint16_t>(whole);
      seenPort = true;
    }
    else if (key == "backlog")
    {
      if (seenBacklog)
      {
        _outError = Refuse(line, "key set twice:", key);
        return false;
      }
      if (!ReadUnsigned(value, whole) || whole < 1 || whole > 0xFFFFFFFFull)
      {
        _outError = Refuse(line, "backlog must be at least 1, found", value);
        return false;
      }
      parsed.backlog = static_cast<std::uint32_t>(whole);
      seenBacklog = true;
    }
    else if (key == "shard")
    {
      if (seenShard)
      {
        _outError = Refuse(line, "key set twice:", key);
        return false;
      }
      if (!ReadUnsigned(value, whole) || whole > 65535)
      {
        _outError = Refuse(line, "shard must be 0 to 65535, found", value);
        return false;
      }
      parsed.shard = static_cast<Game::ShardId>(whole);
      seenShard = true;
    }
    else if (key == "interestRadiusMetres")
    {
      if (seenRadius)
      {
        _outError = Refuse(line, "key set twice:", key);
        return false;
      }
      float radius = 0.0f;
      if (!ReadFloat(value, radius) || radius <= 0.0f)
      {
        _outError = Refuse(line, "interestRadiusMetres must be a positive number, found", value);
        return false;
      }
      parsed.interestRadiusMetres = radius;
      seenRadius = true;
    }
    else if (key == "interestUpdateEveryTicks")
    {
      if (seenUpdateTicks)
      {
        _outError = Refuse(line, "key set twice:", key);
        return false;
      }
      if (!ReadUnsigned(value, whole) || whole < 1 || whole > 0xFFFFFFFFull)
      {
        _outError = Refuse(line, "interestUpdateEveryTicks must be at least 1, found", value);
        return false;
      }
      parsed.interestUpdateEveryTicks = static_cast<std::uint32_t>(whole);
      seenUpdateTicks = true;
    }
    else if (key == "ordersPerTick")
    {
      if (seenOrders)
      {
        _outError = Refuse(line, "key set twice:", key);
        return false;
      }
      if (!ReadUnsigned(value, whole) || whole < 1 || whole > 0xFFFFFFFFull)
      {
        _outError = Refuse(line, "ordersPerTick must be at least 1, found", value);
        return false;
      }
      parsed.ordersPerTick = static_cast<std::uint32_t>(whole);
      seenOrders = true;
    }
    else
    {
      _outError = Refuse(line, "unknown key", key);
      return false;
    }
  }

  _outConfig = parsed;
  return true;
}
} // namespace Outpost
